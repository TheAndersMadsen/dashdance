#include "host.h"
#include "jukebox.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {
std::array<uint8_t, 0xB0> track{};

void require(bool ok, const char* message) {
  if (!ok) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}

int sample(int master_volume, int game_sample = 0) {
  int16_t output[2] = {int16_t(game_sample), int16_t(game_sample)};
  slippi::jukebox::mix(output, 1, master_volume);
  require(output[0] == output[1], "both music channels have the same gain");
  return output[0];
}
}

namespace host {
void log(const char*, ...) {}
double now_seconds() { return 0; }
void sim_cost_add(int, double) {}
bool disc_read(uint32_t, void* out, uint32_t size) {
  if (size != track.size()) return false;
  std::memcpy(out, track.data(), size);
  return true;
}
}

int main() {
  std::memcpy(track.data(), " HALPST\0", 8);
  track[10] = 0x7d;
  track[15] = 2;
  track[0x83] = 16;
  std::memset(track.data() + 0x88, 0xff, 4);
  for (size_t channel : {size_t(0), size_t(8)}) {
    track[0xA0 + channel] = 0x0A;
    std::memset(track.data() + 0xA1 + channel, 0x11, 7);
  }
  slippi::jukebox::init();
  slippi::jukebox::start_song(0, track.size());
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  int full = 0;
  while (!(full = sample(100)) && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
  require(full == 819, "full-volume music sample");
  require(sample(0) == 0, "mute silences music");
  require(sample(10) == 81, "10% volume scales music");
  require(sample(50) == 409, "50% volume scales music");
  require(sample(100, 500) == 500 + full, "game audio is not scaled twice");
  slippi::jukebox::set_user_volume(50);
  require(sample(100) == 409, "music volume remains independent");
  slippi::jukebox::shutdown();
}