// Host audio output: 32 kHz 16-bit stereo blocks from the emulated AI DMA. Primary path is WASAPI
// shared mode on the default render endpoint (the device Windows and browsers use, with the
// mixer's own resampling); WinMM waveOut is the fallback. Optional WAV dump of everything played.
// SPDX-License-Identifier: GPL-2.0-or-later
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include "audio.h"
#include "host.h"
#include "jukebox.h"

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ole32.lib")

namespace host {
namespace {
constexpr int SAMPLE_RATE = 32000;
constexpr int BLOCK_BYTES = 640;      // one 5 ms AI DMA frame: 160 stereo samples
constexpr int BLOCKS = 24;            // 120 ms of queue; more than that is dropped (fast mode)
std::atomic<int> g_volume{0};
std::mutex g_mutex;
uint64_t g_frames = 0, g_dropped = 0;
bool g_open = false;
FILE* g_wav = nullptr;
uint32_t g_wav_bytes = 0;

// ---- WinMM fallback
HWAVEOUT g_out = nullptr;
WAVEHDR g_headers[BLOCKS];
int16_t g_blocks[BLOCKS][BLOCK_BYTES / 2];
int g_next = 0;

// ---- WASAPI: single-producer single-consumer ring, written by the simulation and read by an
// event-driven thread. No lock: the audio thread must never wait on the simulation thread.
//
// The two run off different clocks (the game's timebase against the sound card's crystal), so the
// consumer resamples with a ratio nudged by how full the ring is, the way Dolphin's mixer does.
// Without that the ring slowly fills or empties until it drops a block or runs dry, which is heard
// as a click or a gap.
constexpr size_t RING_FRAMES = 8192;                            // 256 ms, power of two
constexpr size_t RING_MASK = RING_FRAMES - 1;
// Sized from the device once it is open: one full device request plus a simulation frame, so a
// single large callback cannot outrun the ring and the 5 ms blocks arriving in per-frame bursts
// always have somewhere to land.
size_t g_target_frames = SAMPLE_RATE * 35 / 1000;
bool g_priming = true;                                          // fill the ring before the first sample goes out
constexpr double MAX_RATE_SHIFT = 0.015;                        // at most 1.5%: enough range to track a simulation that runs a little under 60 Hz, and still far below a pitch change anyone notices on game audio
int16_t g_ring[RING_FRAMES][2];
std::atomic<uint64_t> g_ring_write{0}, g_ring_read{0};          // frame counters, never wrap in practice
double g_ring_phase = 0.0;                                      // consumer only: position inside the current frame
double g_rate = 1.0;                                            // consumer only: input frames consumed per output frame
double g_fill_average = 0.0;                                    // consumer only: slow average of the fill level
int16_t g_last_output[2] = {0, 0};                              // held through a starved moment instead of silence
std::atomic<uint64_t> g_underruns{0}, g_underrun_frames{0};
std::atomic<double> g_rate_min{1.0}, g_rate_max{1.0};
IAudioClient* g_client = nullptr;
IAudioRenderClient* g_render = nullptr;
HANDLE g_event = nullptr;
std::thread g_thread;
std::atomic<bool> g_running{false};
UINT32 g_buffer_frames = 0;

void wav_header(FILE* f, uint32_t data_bytes) {
  auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
  auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
  std::fwrite("RIFF", 1, 4, f); u32(36 + data_bytes); std::fwrite("WAVE", 1, 4, f);
  std::fwrite("fmt ", 1, 4, f); u32(16); u16(1); u16(2); u32(SAMPLE_RATE); u32(SAMPLE_RATE * 4); u16(4); u16(16);
  std::fwrite("data", 1, 4, f); u32(data_bytes);
}

// Catmull-Rom through four consecutive samples: smooth enough that a continuously varying rate
// introduces no audible artefacts (Dolphin uses the same shape).
inline double cubic(double a, double b, double c, double d, double t) {
  return b + 0.5 * t * (c - a + t * (2.0 * a - 5.0 * b + 4.0 * c - d + t * (3.0 * (b - c) + d - a)));
}

void wasapi_thread() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  while (g_running.load()) {
    if (WaitForSingleObject(g_event, 200) != WAIT_OBJECT_0) continue;
    UINT32 padding = 0;
    if (FAILED(g_client->GetCurrentPadding(&padding))) continue;
    UINT32 want = g_buffer_frames > padding ? g_buffer_frames - padding : 0;
    if (!want) continue;
    BYTE* dst = nullptr;
    if (FAILED(g_render->GetBuffer(want, &dst))) continue;
    int16_t* out = (int16_t*)dst;
    const int volume = g_volume.load();
    uint64_t read = g_ring_read.load(std::memory_order_relaxed);

    // Rate control: steer the ring towards the target fill instead of letting it drift into an
    // overflow (a dropped block) or a gap.
    const uint64_t buffered = g_ring_write.load(std::memory_order_acquire) - read;
    if (g_priming) {
      if (buffered < g_target_frames) { std::memset(dst, 0, (size_t)want * 4); g_render->ReleaseBuffer(want, 0); continue; }
      g_priming = false;
    }
    // The simulation delivers a frame's worth of audio in one burst, so the instantaneous fill
    // swings by 16 ms either way. Steer on a slow average of it, which leaves only the real drift
    // between the game's clock and the sound card's.
    if (g_fill_average == 0.0) g_fill_average = (double)buffered;
    g_fill_average += ((double)buffered - g_fill_average) * 0.02;
    const double error = (g_fill_average - (double)g_target_frames) / (double)g_target_frames;
    const double target_rate = 1.0 + std::clamp(error * 0.25, -MAX_RATE_SHIFT, MAX_RATE_SHIFT);
    g_rate += (target_rate - g_rate) * 0.05;   // ease in, so the pitch never steps
    if (g_rate < g_rate_min.load()) g_rate_min.store(g_rate);
    if (g_rate > g_rate_max.load()) g_rate_max.store(g_rate);

    uint32_t starved = 0;
    for (UINT32 i = 0; i < want; ++i) {
      const uint64_t write = g_ring_write.load(std::memory_order_acquire);
      if (write - read < 4) {
        // Nothing to read: hold the last sample. A repeated sample for a moment is far less
        // audible than silence, and the rate control refills the ring within a few callbacks.
        out[i * 2] = (int16_t)((int32_t)g_last_output[0] * volume / 100);
        out[i * 2 + 1] = (int16_t)((int32_t)g_last_output[1] * volume / 100);
        ++starved;
        continue;
      }
      for (int channel = 0; channel < 2; ++channel) {
        const double a = g_ring[(read - 1) & RING_MASK][channel];
        const double b = g_ring[read & RING_MASK][channel];
        const double c = g_ring[(read + 1) & RING_MASK][channel];
        const double d = g_ring[(read + 2) & RING_MASK][channel];
        const double v = cubic(a, b, c, d, g_ring_phase);
        g_last_output[channel] = (int16_t)std::clamp(v, -32768.0, 32767.0);
        out[i * 2 + channel] = (int16_t)((int32_t)g_last_output[channel] * volume / 100);
      }
      g_ring_phase += g_rate;
      while (g_ring_phase >= 1.0) { g_ring_phase -= 1.0; ++read; }
    }
    g_ring_read.store(read, std::memory_order_release);
    if (starved) { g_underruns.fetch_add(1); g_underrun_frames.fetch_add(starved); }
    if (volume > 0) slippi::jukebox::mix(out, want, volume);
    g_render->ReleaseBuffer(want, 0);
  }
  CoUninitialize();
}

bool wasapi_open() {
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) { log("audio: CoInitialize failed (%08X)", (unsigned)hr); return false; }
  IMMDeviceEnumerator* enumerator = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator))) return false;
  IMMDevice* device = nullptr;
  hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
  enumerator->Release();
  if (FAILED(hr)) { log("audio: no default render device (%08X)", (unsigned)hr); return false; }
  hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&g_client);
  device->Release();
  if (FAILED(hr)) { log("audio: IAudioClient activate failed (%08X)", (unsigned)hr); return false; }
  WAVEFORMATEX fmt{};
  fmt.wFormatTag = WAVE_FORMAT_PCM; fmt.nChannels = 2; fmt.nSamplesPerSec = SAMPLE_RATE;
  fmt.wBitsPerSample = 16; fmt.nBlockAlign = 4; fmt.nAvgBytesPerSec = SAMPLE_RATE * 4;
  const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
  hr = g_client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 40 * 10000 /* 40 ms, 100 ns units */, 0, &fmt, nullptr);
  if (FAILED(hr)) { log("audio: IAudioClient initialize failed (%08X)", (unsigned)hr); g_client->Release(); g_client = nullptr; return false; }
  g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (FAILED(g_client->SetEventHandle(g_event)) || FAILED(g_client->GetBufferSize(&g_buffer_frames)) ||
      FAILED(g_client->GetService(__uuidof(IAudioRenderClient), (void**)&g_render))) {
    log("audio: IAudioClient setup failed"); g_client->Release(); g_client = nullptr; return false;
  }
  g_target_frames = (size_t)g_buffer_frames + SAMPLE_RATE * 17 / 1000;
  if (g_target_frames > RING_FRAMES / 2) g_target_frames = RING_FRAMES / 2;
  g_priming = true;
  g_fill_average = 0.0;
  g_running.store(true);
  g_thread = std::thread(wasapi_thread);
  if (FAILED(g_client->Start())) { log("audio: IAudioClient start failed"); g_running.store(false); g_thread.join(); g_render->Release(); g_render = nullptr; g_client->Release(); g_client = nullptr; return false; }
  return true;
}

void wasapi_close() {
  if (!g_client) return;
  g_running.store(false);
  if (g_thread.joinable()) g_thread.join();
  g_client->Stop();
  if (g_render) { g_render->Release(); g_render = nullptr; }
  g_client->Release(); g_client = nullptr;
  if (g_event) { CloseHandle(g_event); g_event = nullptr; }
}

bool winmm_open() {
  WAVEFORMATEX fmt{};
  fmt.wFormatTag = WAVE_FORMAT_PCM; fmt.nChannels = 2; fmt.nSamplesPerSec = SAMPLE_RATE;
  fmt.wBitsPerSample = 16; fmt.nBlockAlign = 4; fmt.nAvgBytesPerSec = SAMPLE_RATE * 4;
  if (waveOutOpen(&g_out, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
    log("audio: waveOutOpen failed; audio output disabled");
    g_out = nullptr;
    return false;
  }
  for (int i = 0; i < BLOCKS; ++i) {
    std::memset(&g_headers[i], 0, sizeof(WAVEHDR));
    g_headers[i].lpData = (LPSTR)g_blocks[i];
    g_headers[i].dwBufferLength = BLOCK_BYTES;
    waveOutPrepareHeader(g_out, &g_headers[i], sizeof(WAVEHDR));
    g_headers[i].dwFlags |= WHDR_DONE;   // free
  }
  return true;
}
}  // namespace

void audio_set_volume(int volume) { g_volume.store(std::clamp(volume, 0, 100)); }
int audio_volume() { return g_volume.load(); }

bool audio_open(int volume_percent, const char* wav_dump_path, bool open_device) {
  if (wav_dump_path && *wav_dump_path) {
    g_wav = std::fopen(wav_dump_path, "wb");
    if (g_wav) { wav_header(g_wav, 0); g_wav_bytes = 0; g_open = true; }
    else log("audio: cannot open %s", wav_dump_path);
  }
  if (!open_device) return g_open;
  int v = std::clamp(volume_percent, 0, 100);
  g_volume = v; // software gain applies only to our PCM
  const char* backend = "none";
  if (wasapi_open()) backend = "WASAPI shared mode, default device";
  else if (winmm_open()) backend = "WinMM waveOut";
  else return g_open;
  g_open = true;
  log("audio: %s, 32 kHz stereo, volume %d%%", backend, v);
  return true;
}

void audio_close() {
  if (g_wav) {
    std::fseek(g_wav, 0, SEEK_SET);
    wav_header(g_wav, g_wav_bytes);
    std::fclose(g_wav); g_wav = nullptr;
  }
  wasapi_close();
  if (g_out) {
    waveOutReset(g_out);
    for (int i = 0; i < BLOCKS; ++i) waveOutUnprepareHeader(g_out, &g_headers[i], sizeof(WAVEHDR));
    waveOutClose(g_out);
    g_out = nullptr;
  }
  g_open = false;
}

void audio_push(const uint8_t* be_samples, size_t bytes) {
  if (!g_open) return;
  std::unique_lock<std::mutex> winmm_lock(g_mutex, std::defer_lock);
  if (!g_client) winmm_lock.lock();   // the WASAPI path is lock free; only the fallback needs this
  for (size_t off = 0; off + BLOCK_BYTES <= bytes; off += BLOCK_BYTES) {
    int16_t converted[BLOCK_BYTES / 2];
    const uint8_t* src = be_samples + off;
    for (int i = 0; i < BLOCK_BYTES / 4; ++i) {
      int16_t r = (int16_t)((src[i * 4] << 8) | src[i * 4 + 1]);
      int16_t l = (int16_t)((src[i * 4 + 2] << 8) | src[i * 4 + 3]);
      converted[i * 2] = l; converted[i * 2 + 1] = r;
    }
    if (g_wav) { std::fwrite(converted, 1, BLOCK_BYTES, g_wav); g_wav_bytes += BLOCK_BYTES; }
    if (g_client) {
      constexpr size_t frames = BLOCK_BYTES / 4;
      uint64_t write = g_ring_write.load(std::memory_order_relaxed);
      if (write + frames - g_ring_read.load(std::memory_order_acquire) > RING_FRAMES - 4) { ++g_dropped; continue; }
      for (size_t i = 0; i < frames; ++i) {
        g_ring[(write + i) & RING_MASK][0] = converted[i * 2];
        g_ring[(write + i) & RING_MASK][1] = converted[i * 2 + 1];
      }
      g_ring_write.store(write + frames, std::memory_order_release);
      g_frames += frames;
      continue;
    }
    if (!g_out) { g_frames += BLOCK_BYTES / 4; continue; }
    WAVEHDR& h = g_headers[g_next];
    if (!(h.dwFlags & WHDR_DONE)) { ++g_dropped; continue; }
    for (int i = 0; i < BLOCK_BYTES / 2; ++i)
      g_blocks[g_next][i] = (int16_t)((int32_t)converted[i] * g_volume / 100);
    if (g_volume > 0) slippi::jukebox::mix(g_blocks[g_next], BLOCK_BYTES / 4, g_volume);
    h.dwFlags &= ~WHDR_DONE;
    if (waveOutWrite(g_out, &h, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) { h.dwFlags |= WHDR_DONE; ++g_dropped; continue; }
    g_next = (g_next + 1) % BLOCKS;
    g_frames += BLOCK_BYTES / 4;
  }
}

uint64_t audio_pushed_frames() { return g_frames; }
uint64_t audio_dropped_blocks() { return g_dropped; }
uint64_t audio_underruns(uint64_t* silent_ms) { if (silent_ms) *silent_ms = g_underrun_frames.load() * 1000 / SAMPLE_RATE; return g_underruns.load(); }
void audio_rate_range(double* low, double* high) { if (low) *low = g_rate_min.load(); if (high) *high = g_rate_max.load(); }
uint32_t audio_buffered_ms() { return (uint32_t)((g_ring_write.load() - g_ring_read.load()) * 1000 / SAMPLE_RATE); }

}  // namespace host
