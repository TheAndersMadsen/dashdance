// Port of Slippi's jukebox (slippi-rust-extensions, GPLv2 with no version stated; its HPS/DSP-ADPCM decoding comes from
// hps_decode 0.3.0, MIT, Copyright (c) 2023 Daryl Pinto) onto the host audio mixer. See THIRD_PARTY_NOTICES.md.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "jukebox.h"
#include "host.h"
#include "fatal_boundary.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace slippi::jukebox {
namespace {
constexpr uint32_t OUTPUT_RATE = 32000;
constexpr double VOLUME_REDUCTION = 0.8;   // Slippi plays music a little under the game's level

struct Song {
  std::vector<int16_t> samples;   // interleaved stereo at OUTPUT_RATE
  size_t loop_frame = SIZE_MAX;   // frame index the song restarts at, or SIZE_MAX for one-shot
  size_t position = 0;            // frames played
};

std::mutex g_mutex;
std::shared_ptr<Song> g_song;     // replaced atomically under the mutex; the mixer holds a copy
std::atomic<uint32_t> g_song_generation{0};   // a stop() or a newer start_song() cancels an in-flight decode
std::atomic<int> g_melee_volume{254}, g_user_volume{100};
struct DecodeRequest { uint32_t disc_offset, size, generation; };
std::mutex g_worker_mutex;
std::condition_variable g_work_ready;
std::optional<DecodeRequest> g_pending;
std::thread g_worker;
bool g_stopping = false;   // protected by g_worker_mutex

bool cancelled(uint32_t generation) {
  return g_song_generation.load() != generation || host::background_failed();
}

uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
int16_t be16(const uint8_t* p) { return (int16_t)((p[0] << 8) | p[1]); }

// Decodes one channel's DSP-ADPCM frames (8 bytes: header + 7 data bytes = 14 samples).
bool decode_frames(const uint8_t* data, size_t frames, int16_t hist1, int16_t hist2, const int16_t coef[16], std::vector<int16_t>& out, uint32_t generation) {
  static const int8_t nibble_to_i8[16] = {0, 1, 2, 3, 4, 5, 6, 7, -8, -7, -6, -5, -4, -3, -2, -1};
  for (size_t f = 0; f < frames; ++f) {
    if ((f % 256) == 0 && cancelled(generation)) return false;
    const uint8_t* frame = data + f * 8;
    int scale = 1 << (frame[0] & 0xF);
    int ci = (frame[0] >> 4) & 7;
    int c1 = coef[ci * 2], c2 = coef[ci * 2 + 1];
    for (int b = 1; b < 8; ++b) {
      for (int n = 0; n < 2; ++n) {
        int nib = nibble_to_i8[n == 0 ? (frame[b] >> 4) & 0xF : frame[b] & 0xF];
        int32_t s = (int32_t)(((int64_t)nib * scale * 2048 + 1024 +
                              (int64_t)c1 * hist1 + (int64_t)c2 * hist2) >> 11);
        int16_t sample = (int16_t)std::clamp(s, -32768, 32767);
        hist2 = hist1; hist1 = sample;
        out.push_back(sample);
      }
    }
  }
  return true;
}

std::shared_ptr<Song> decode_hps(const std::vector<uint8_t>& file, uint32_t generation) {
  if (cancelled(generation)) return nullptr;
  if (file.size() < 0x80 || std::memcmp(file.data(), " HALPST\0", 8) != 0) { host::log("jukebox: not an HPS file"); return nullptr; }
  uint32_t rate = be32(file.data() + 8), channels = be32(file.data() + 12);
  if (channels != 2) { host::log("jukebox: %u channels unsupported", channels); return nullptr; }
  int16_t coef[2][16];
  for (int ch = 0; ch < 2; ++ch) {
    const uint8_t* info = file.data() + 0x10 + ch * 0x38;
    for (int k = 0; k < 16; ++k) coef[ch][k] = be16(info + 0x10 + k * 2);
  }
  // Blocks: length, (unused), next offset, two decoder states (8 bytes each), pad, then frames.
  struct Block { uint32_t offset, next; std::vector<int16_t> pcm; };
  std::vector<Block> blocks;
  std::vector<int16_t> left, right;
  uint32_t off = 0x80;
  while (off <= file.size() && file.size() - off >= 0x20) {
    if (cancelled(generation)) return nullptr;
    const uint8_t* b = file.data() + off;
    uint32_t len = be32(b), next = be32(b + 8);
    // Validate by subtraction: a hostile u32 block length must not wrap the
    // offset sum and turn an out-of-bounds block into an apparently valid one.
    if (len > file.size() - off - 0x20 || (len % 16) != 0) {
      host::log("jukebox: invalid HPS block length");
      return nullptr;
    }
    int16_t h1l = be16(b + 0x0C + 2), h2l = be16(b + 0x0C + 4), h1r = be16(b + 0x14 + 2), h2r = be16(b + 0x14 + 4);
    size_t frames = len / 8, half = frames / 2;
    left.clear(); right.clear();
    if (!decode_frames(b + 0x20, half, h1l, h2l, coef[0], left, generation) ||
        !decode_frames(b + 0x20 + half * 8, frames - half, h1r, h2r, coef[1], right, generation)) return nullptr;
    Block blk{off, next, {}};
    size_t n = std::min(left.size(), right.size());
    blk.pcm.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
      if ((i % 4096) == 0 && cancelled(generation)) return nullptr;
      blk.pcm.push_back(left[i]); blk.pcm.push_back(right[i]);
    }
    blocks.push_back(std::move(blk));
    if (next == 0xFFFFFFFFu || next <= off || next >= file.size()) break;
    off = next;
  }
  if (blocks.empty()) { host::log("jukebox: no blocks"); return nullptr; }
  auto song = std::make_shared<Song>();
  size_t loop_frame = SIZE_MAX, frame_index = 0;
  uint32_t loop_target = blocks.back().next;
  for (auto& blk : blocks) {
    if (blk.offset == loop_target) loop_frame = frame_index;
    frame_index += blk.pcm.size() / 2;
  }
  // Resample to the mixer rate if the track is not 32 kHz (Melee's are).
  if (rate == OUTPUT_RATE || rate == 0) {
    for (auto& blk : blocks) {
      if (cancelled(generation)) return nullptr;
      song->samples.insert(song->samples.end(), blk.pcm.begin(), blk.pcm.end());
    }
    song->loop_frame = loop_frame;
  } else {
    std::vector<int16_t> all;
    for (auto& blk : blocks) {
      if (cancelled(generation)) return nullptr;
      all.insert(all.end(), blk.pcm.begin(), blk.pcm.end());
    }
    size_t in_frames = all.size() / 2, out_frames = (size_t)((uint64_t)in_frames * OUTPUT_RATE / rate);
    song->samples.resize(out_frames * 2);
    for (size_t i = 0; i < out_frames; ++i) {
      if ((i % 4096) == 0 && cancelled(generation)) return nullptr;
      double src = (double)i * rate / OUTPUT_RATE;
      size_t a = std::min((size_t)src, in_frames - 1), bb = std::min(a + 1, in_frames - 1);
      double t = src - (double)a;
      for (int ch = 0; ch < 2; ++ch) song->samples[i * 2 + ch] = (int16_t)(all[a * 2 + ch] * (1.0 - t) + all[bb * 2 + ch] * t);
    }
    song->loop_frame = loop_frame == SIZE_MAX ? SIZE_MAX : (size_t)((uint64_t)loop_frame * OUTPUT_RATE / rate);
  }
  host::log("jukebox: song %u Hz, %zu frames, %s", rate, song->samples.size() / 2, song->loop_frame == SIZE_MAX ? "no loop" : "loops");
  return song;
}

void decode_loop() {
  for (;;) {
    DecodeRequest request{};
    {
      std::unique_lock<std::mutex> lock(g_worker_mutex);
      g_work_ready.wait(lock, [] { return g_stopping || g_pending.has_value(); });
      if (g_stopping || host::background_failed()) return;
      request = *g_pending;
      g_pending.reset();
    }
    if (cancelled(request.generation)) continue;
    std::vector<uint8_t> file(request.size);
    if (!host::disc_read(request.disc_offset, file.data(), request.size)) {
      host::log("jukebox: cannot read song at %08X", request.disc_offset);
      continue;
    }
    if (cancelled(request.generation)) continue;
    auto song = decode_hps(file, request.generation);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!cancelled(request.generation)) g_song = std::move(song);
  }
}
}  // namespace

void init() {
  shutdown();
  std::lock_guard<std::mutex> lock(g_worker_mutex);
  g_stopping = false;
}

void shutdown() {
  {
    std::lock_guard<std::mutex> lock(g_worker_mutex);
    g_stopping = true;
    ++g_song_generation;
    g_pending.reset();
  }
  g_work_ready.notify_one();
  // The worker only relays fatal exceptions; it never runs observer/teardown.
  if (g_worker.joinable()) g_worker.join();
  std::lock_guard<std::mutex> lock(g_mutex);
  g_song.reset();
}

// The disc read and HPS decode (tens of ms for a full track) run on a worker: music start time
// is not part of the deterministic simulation, and the game's own audio must not hitch for it.
void start_song(uint32_t disc_offset, uint32_t size) {
  host::SimCostScope cost(host::SIM_JUKEBOX);
  if (size == 0 || size > 64u * 1024 * 1024) { host::log("jukebox: bad song size %u", size); return; }
  std::lock_guard<std::mutex> lock(g_worker_mutex);
  if (g_stopping || host::background_failed()) return;
  const uint32_t generation = ++g_song_generation;
  // Keep only the newest pending request; generation cancellation interrupts
  // old decode work without spawning unbounded detached threads.
  g_pending = DecodeRequest{disc_offset, size, generation};
  if (!g_worker.joinable()) g_worker = std::thread([] {
    host::run_background_task([] { decode_loop(); });
  });
  g_work_ready.notify_one();
}

void stop() {
  {
    std::lock_guard<std::mutex> lock(g_worker_mutex);
    ++g_song_generation;
    g_pending.reset();
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  g_song.reset();
}
void set_melee_volume(uint8_t volume) { g_melee_volume.store(volume); }
void set_user_volume(int percent) { g_user_volume.store(std::clamp(percent, 0, 100)); }
int user_volume() { return g_user_volume.load(); }

void mix(int16_t* out, size_t frames, int master_volume) {
  std::shared_ptr<Song> song;
  { std::lock_guard<std::mutex> lk(g_mutex); song = g_song; }
  if (!song || song->samples.empty()) return;
  double gain = (g_melee_volume.load() / 254.0) * (g_user_volume.load() / 100.0) * (std::clamp(master_volume, 0, 100) / 100.0) * VOLUME_REDUCTION;
  if (gain <= 0.0) return;
  size_t total = song->samples.size() / 2;
  for (size_t i = 0; i < frames; ++i) {
    if (song->position >= total) {
      if (song->loop_frame == SIZE_MAX) { std::lock_guard<std::mutex> lk(g_mutex); if (g_song == song) g_song.reset(); return; }
      song->position = std::min(song->loop_frame, total - 1);
    }
    for (int ch = 0; ch < 2; ++ch) {
      int32_t v = out[i * 2 + ch] + (int32_t)(song->samples[song->position * 2 + ch] * gain);
      out[i * 2 + ch] = (int16_t)std::clamp(v, -32768, 32767);
    }
    ++song->position;
  }
}
}  // namespace slippi::jukebox
