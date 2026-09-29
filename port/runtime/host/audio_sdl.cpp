// Host audio output through SDL3: 32 kHz 16-bit stereo blocks from the emulated
// AI DMA, resampled against the device clock exactly like the Windows WASAPI path
// (cubic interpolation, ring fill feedback, held sample through starvation).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "audio.h"
#include "audio_headless.h"
#include "host.h"
#include "jukebox.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <mutex>

namespace host {
namespace {
constexpr int SAMPLE_RATE = 32000;
constexpr int BLOCK_BYTES = 640;      // one 5 ms AI DMA frame: 160 stereo samples
constexpr size_t RING_FRAMES = 8192;  // 256 ms, power of two
constexpr size_t RING_MASK = RING_FRAMES - 1;
constexpr double MAX_RATE_SHIFT = 0.015;

std::atomic<int> g_volume{0};
std::mutex g_mutex;
uint64_t g_frames = 0, g_dropped = 0;
bool g_open = false, g_device = false, g_output_ok = true;
FILE* g_wav = nullptr;
uint32_t g_wav_bytes = 0;

int16_t g_ring[RING_FRAMES][2];
std::atomic<uint64_t> g_ring_write{0}, g_ring_read{0};
size_t g_target_frames = SAMPLE_RATE * 35 / 1000;
bool g_priming = true;
double g_ring_phase = 0.0, g_rate = 1.0, g_fill_average = 0.0;
int16_t g_last_output[2] = {0, 0};
std::atomic<uint64_t> g_underruns{0}, g_underrun_frames{0};
std::atomic<double> g_rate_min{1.0}, g_rate_max{1.0};
SDL_AudioStream* g_stream = nullptr;
bool g_audio_subsystem = false;

void wav_header(FILE* f, uint32_t data_bytes) {
  auto u32 = [&](uint32_t v) { uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; if (std::fwrite(b, 1, 4, f) != 4) g_output_ok = false; };
  auto u16 = [&](uint16_t v) { uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)}; if (std::fwrite(b, 1, 2, f) != 2) g_output_ok = false; };
  std::fwrite("RIFF", 1, 4, f); u32(36 + data_bytes); std::fwrite("WAVE", 1, 4, f);
  std::fwrite("fmt ", 1, 4, f); u32(16); u16(1); u16(2); u32(SAMPLE_RATE); u32(SAMPLE_RATE * 4); u16(4); u16(16);
  std::fwrite("data", 1, 4, f); u32(data_bytes);
}

inline double cubic(double a, double b, double c, double d, double t) {
  return b + 0.5 * t * (c - a + t * (2.0 * a - 5.0 * b + 4.0 * c - d + t * (3.0 * (b - c) + d - a)));
}

// Device thread: produce `want` output frames from the ring.
void render(int16_t* out, uint32_t want) {
  const int volume = g_volume.load();
  uint64_t read = g_ring_read.load(std::memory_order_relaxed);
  const uint64_t buffered = g_ring_write.load(std::memory_order_acquire) - read;
  if (g_priming) {
    if (buffered < g_target_frames) { std::memset(out, 0, size_t(want) * 4); return; }
    g_priming = false;
  }
  if (g_fill_average == 0.0) g_fill_average = double(buffered);
  g_fill_average += (double(buffered) - g_fill_average) * 0.02;
  const double error = (g_fill_average - double(g_target_frames)) / double(g_target_frames);
  const double target_rate = 1.0 + std::clamp(error * 0.25, -MAX_RATE_SHIFT, MAX_RATE_SHIFT);
  g_rate += (target_rate - g_rate) * 0.05;
  if (g_rate < g_rate_min.load()) g_rate_min.store(g_rate);
  if (g_rate > g_rate_max.load()) g_rate_max.store(g_rate);
  uint32_t starved = 0;
  for (uint32_t i = 0; i < want; ++i) {
    const uint64_t write = g_ring_write.load(std::memory_order_acquire);
    if (write - read < 4) {
      out[i * 2] = int16_t(int32_t(g_last_output[0]) * volume / 100);
      out[i * 2 + 1] = int16_t(int32_t(g_last_output[1]) * volume / 100);
      ++starved;
      continue;
    }
    for (int channel = 0; channel < 2; ++channel) {
      const double a = g_ring[(read - 1) & RING_MASK][channel];
      const double b = g_ring[read & RING_MASK][channel];
      const double c = g_ring[(read + 1) & RING_MASK][channel];
      const double d = g_ring[(read + 2) & RING_MASK][channel];
      const double v = cubic(a, b, c, d, g_ring_phase);
      g_last_output[channel] = int16_t(std::clamp(v, -32768.0, 32767.0));
      out[i * 2 + channel] = int16_t(int32_t(g_last_output[channel]) * volume / 100);
    }
    g_ring_phase += g_rate;
    while (g_ring_phase >= 1.0) { g_ring_phase -= 1.0; ++read; }
  }
  g_ring_read.store(read, std::memory_order_release);
  if (starved) {
    const uint64_t n = g_underruns.fetch_add(1) + 1; g_underrun_frames.fetch_add(starved);
    if (n <= 5 || (n % 100) == 0) log("audio: underrun %llu (%u frames of silence); raise MELEE_AUDIO_SLACK_MS if this repeats", (unsigned long long)n, starved);
  }
  if (volume > 0) slippi::jukebox::mix(out, want, volume);
}

void SDLCALL stream_callback(void*, SDL_AudioStream* stream, int additional_amount, int) {
  if (additional_amount <= 0) return;
  // SDL asks in bytes of the stream's input format (S16 stereo).
  uint32_t want = uint32_t(additional_amount) / 4;
  if (!want) return;
  static thread_local std::vector<int16_t> scratch;
  scratch.resize(size_t(want) * 2);
  render(scratch.data(), want);
  SDL_PutAudioStreamData(stream, scratch.data(), int(want * 4));
}

// Game-side slack in the ring on top of the device buffer. The game delivers audio once per 60 Hz
// frame, so the ring must hold at least one frame (16.7 ms) plus scheduling jitter: 20 ms measured
// underrun-free; 12 ms starved several times a second. MELEE_AUDIO_SLACK_MS overrides. Underruns are
// logged as they happen.
int ring_slack_ms() { const char* e = std::getenv("MELEE_AUDIO_SLACK_MS"); int v = e ? std::atoi(e) : 20; return std::clamp(v, 4, 200); }
bool device_open() {
  if (!SDL_WasInit(SDL_INIT_AUDIO)) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) { log("audio: SDL audio init failed: %s", SDL_GetError()); return false; }
    g_audio_subsystem = true;
  }
  SDL_AudioSpec spec{};
  spec.format = SDL_AUDIO_S16;
  spec.channels = 2;
  spec.freq = SAMPLE_RATE;
  // A small CoreAudio buffer: the render callback runs on a real-time audio thread and our ring adds
  // its own slack, so the device buffer only needs to cover one callback period. MELEE_AUDIO_FRAMES
  // overrides (default 256 frames, about 5 ms at 48 kHz).
  const char* frames = std::getenv("MELEE_AUDIO_FRAMES");
  SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, frames && *frames ? frames : "256");
  g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, stream_callback, nullptr);
  if (!g_stream) { log("audio: cannot open default playback device: %s", SDL_GetError()); return false; }
  // Aim for the same ~35-55 ms of queued audio the WASAPI path uses; the device
  // buffer is whatever CoreAudio chose, our ring target adds the game-side slack.
  int device_frames = 0;
  SDL_AudioSpec device_spec{};
  if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(g_stream), &device_spec, &device_frames) && device_frames > 0)
    g_target_frames = size_t(device_frames) * SAMPLE_RATE / std::max(device_spec.freq, 1) + SAMPLE_RATE * ring_slack_ms() / 1000;
  if (g_target_frames > RING_FRAMES / 2) g_target_frames = RING_FRAMES / 2;
  log("audio: device buffer %d frames at %d Hz, ring target %zu frames (%.0f ms of queued audio)", device_frames, device_spec.freq, g_target_frames, g_target_frames * 1000.0 / SAMPLE_RATE);
  audio_session_report();   // iPhone, iPad, Vision Pro: the buffer and route the system granted
  g_priming = true;
  g_fill_average = 0.0;
  g_ring_phase = 0.0;
  g_rate = 1.0;
  if (!SDL_ResumeAudioStreamDevice(g_stream)) {
    log("audio: cannot start playback: %s", SDL_GetError());
    SDL_DestroyAudioStream(g_stream); g_stream = nullptr;
    return false;
  }
  return true;
}

void device_close() {
  if (!g_stream) return;
  SDL_DestroyAudioStream(g_stream);   // unbinds and closes the logical device, joins the callback
  g_stream = nullptr;
  if (g_audio_subsystem) { SDL_QuitSubSystem(SDL_INIT_AUDIO); g_audio_subsystem = false; }
}
}  // namespace

void audio_set_volume(int volume) { g_volume.store(std::clamp(volume, 0, 100)); }
int audio_volume() { return g_volume.load(); }

bool audio_open(int volume_percent, const char* wav_dump_path, bool open_device) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_open) return false;
  g_output_ok = true;
  if (wav_dump_path && *wav_dump_path) {
    g_wav = std::fopen(wav_dump_path, "wb");
    if (g_wav) { wav_header(g_wav, 0); g_wav_bytes = 0; }
    else { log("audio: cannot open %s", wav_dump_path); g_output_ok = false; return false; }
  }
  g_volume = std::clamp(volume_percent, 0, 100);
  g_open = true;
  if (!open_device) return true;
  if (!device_open()) return true;   // WAV/silent operation continues without a device
  g_device = true;
  log("audio: SDL3 default playback device, 32 kHz stereo, volume %d%%", g_volume.load());
  return true;
}

void audio_close() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_device) { device_close(); g_device = false; }
  if (g_wav) {
    if (std::fseek(g_wav, 0, SEEK_SET) != 0) g_output_ok = false;
    wav_header(g_wav, g_wav_bytes);
    if (std::fclose(g_wav) != 0) g_output_ok = false;
    g_wav = nullptr;
  }
  g_open = false;
}

void audio_push(const uint8_t* be_samples, size_t bytes) {
  if (!g_open) return;
  for (size_t off = 0; off + BLOCK_BYTES <= bytes; off += BLOCK_BYTES) {
    int16_t converted[BLOCK_BYTES / 2];
    const uint8_t* src = be_samples + off;
    for (int i = 0; i < BLOCK_BYTES / 4; ++i) {
      int16_t r = int16_t((src[i * 4] << 8) | src[i * 4 + 1]);
      int16_t l = int16_t((src[i * 4 + 2] << 8) | src[i * 4 + 3]);
      converted[i * 2] = l; converted[i * 2 + 1] = r;
    }
    if (g_wav) { if (std::fwrite(converted, 1, BLOCK_BYTES, g_wav) != BLOCK_BYTES) g_output_ok = false; g_wav_bytes += BLOCK_BYTES; }
    constexpr size_t frames = BLOCK_BYTES / 4;
    if (!g_device) { g_frames += frames; continue; }
    uint64_t write = g_ring_write.load(std::memory_order_relaxed);
    if (write + frames - g_ring_read.load(std::memory_order_acquire) > RING_FRAMES - 4) { ++g_dropped; continue; }
    for (size_t i = 0; i < frames; ++i) {
      g_ring[(write + i) & RING_MASK][0] = converted[i * 2];
      g_ring[(write + i) & RING_MASK][1] = converted[i * 2 + 1];
    }
    g_ring_write.store(write + frames, std::memory_order_release);
    g_frames += frames;
  }
}

uint64_t audio_pushed_frames() { return g_frames; }
uint64_t audio_dropped_blocks() { return g_dropped; }
uint64_t audio_underruns(uint64_t* silent_ms) { if (silent_ms) *silent_ms = g_underrun_frames.load() * 1000 / SAMPLE_RATE; return g_underruns.load(); }
void audio_rate_range(double* low, double* high) { if (low) *low = g_rate_min.load(); if (high) *high = g_rate_max.load(); }
uint32_t audio_buffered_ms() { return uint32_t((g_ring_write.load() - g_ring_read.load()) * 1000 / SAMPLE_RATE); }
bool headless_audio_output_ok() { return g_output_ok; }
}  // namespace host
