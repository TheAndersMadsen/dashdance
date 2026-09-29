// Actual EXI worker/cache path, with isolated synthetic host dependencies only.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "exi_slippi.h"
#include "gecko_data.h"
#include "host.h"
#include "jukebox.h"
#include "slippi_playback.h"
#include "vcdiff.h"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <thread>

namespace {
using namespace std::chrono_literals;
const auto simulation_thread = std::this_thread::get_id();
std::array<uint8_t, 4096> guest{};
std::atomic<unsigned> background_costs{0}, simulation_costs{0}, jukebox_stops{0};
std::mutex read_mutex;
std::condition_variable read_changed;
bool read_entered = false, read_release = false;
unsigned background_reads = 0;
std::string background_name;
uint32_t module_bytes = 0;
std::string module_sha256, module_name;
unsigned module_records = 0;
int failures = 0;

void check(bool ok, const char* message) {
  if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
void request_file(const std::string& name, bool load) {
  guest.fill(0);
  guest[0] = load ? 0xD2 : 0xD1;
  std::memcpy(guest.data() + 1, name.data(), name.size());
  slippi::dma_write(0, 65);
}
}  // namespace

namespace host {
Options options;
void log(const char*, ...) {}
double now_seconds() { return 0; }
void sim_cost_add(int slot, double) {
  if (slot != SIM_EXI) std::abort();
  if (std::this_thread::get_id() == simulation_thread) ++simulation_costs;
  else ++background_costs;
}
uint8_t* ptr(uint32_t addr, uint32_t bytes) {
  if (addr > guest.size() || bytes > guest.size() - addr) std::abort();
  return guest.data() + addr;
}
void input_mark_match_start() {}
bool disc_find_file(const std::string& name, uint32_t* offset, uint32_t* bytes) {
  *offset = 0; *bytes = 3;
  if (std::this_thread::get_id() != simulation_thread) {
    std::lock_guard<std::mutex> lock(read_mutex);
    background_name = name;
  }
  return true;
}
bool disc_read(uint32_t, void* out, uint32_t bytes) {
  if (std::this_thread::get_id() != simulation_thread) {
    std::unique_lock<std::mutex> lock(read_mutex);
    ++background_reads;
    read_entered = true;
    read_changed.notify_all();
    read_changed.wait(lock, [] { return read_release; });
  }
  if (bytes != 3) std::abort();
  std::memcpy(out, "abc", bytes);
  return true;
}
bool vcdiff_decode(const uint8_t* source, size_t bytes, const uint8_t*, size_t,
                   std::vector<uint8_t>& out, std::string*) {
  out.assign(source, source + bytes);
  return true;
}
}  // namespace host
namespace ppc {
void record_loaded_module(const char* name, uint32_t, uint32_t bytes, const char* digest) {
  ++module_records; module_name = name; module_bytes = bytes; module_sha256 = digest;
}
}
namespace gecko {
const uint8_t slippi_gct[] = {0};
const size_t slippi_gct_size = 0;
const uint32_t gct_base_used = 0, optional_gct_offset = 0;
const CodeTable slippi_gct_options[4] = {};
const OptionalWrite optional_writes[] = {{0, 0, nullptr, nullptr, nullptr}};
const size_t optional_writes_count = 0;
bool option_widescreen = false;
bool option_flash_failed_lcancel = false;
}
namespace slippi::playback {
void prepare_game_info(const uint8_t*, std::vector<uint8_t>&) {}
void prepare_frame_data(const uint8_t*, std::vector<uint8_t>&) {}
void prepare_is_stock_steal(const uint8_t*, std::vector<uint8_t>&) {}
void prepare_is_file_ready(std::vector<uint8_t>&) {}
void prepare_gecko_codes(std::vector<uint8_t>&) {}
void note_gecko_list_dma(uint32_t, uint32_t) {}
}
namespace slippi::jukebox {
void init() {}
void shutdown() { ++jukebox_stops; }
void start_song(uint32_t, uint32_t) {}
void stop() {}
void set_melee_volume(uint8_t) {}
}

int main() {
  namespace fs = std::filesystem;
  fs::path fixture;
  for (unsigned attempt = 0; attempt < 100; ++attempt) {
    auto candidate = fs::temp_directory_path() / ("slippi-preload-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (fs::create_directory(candidate)) { fixture = candidate; break; }
  }
  check(!fixture.empty(), "test owns an isolated fixture directory");
  if (fixture.empty()) return 1;
  struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; fs::remove_all(path, ec); } } cleanup{fixture};
  auto sys = fixture / "Sys", files = sys / "GameFiles" / "GALE01";
  fs::create_directories(files);
  std::ofstream(files / "First.dat.diff") << "synthetic patch";
  std::ofstream(files / "Second.dat.diff") << "synthetic patch";
  host::options.sys_dir = sys.string();
  host::options.replay_dir = (fixture / "replays").string();
  host::options.offline = true;
  slippi::init();
  std::string name;
  {
    std::unique_lock<std::mutex> lock(read_mutex);
    if (!read_changed.wait_for(lock, 2s, [] { return read_entered; })) std::abort();
    name = background_name;
  }
  // Foreground cache miss races the blocked preload for the exact same key.
  // It must build independently instead of waiting for background decoding.
  request_file(name, false);
  slippi::dma_read(256, 4);
  check(guest[259] == 3, "foreground cache miss completes during blocked preload");
  request_file(name, true);
  slippi::dma_read(256, 5);
  check(std::memcmp(guest.data() + 256, "abc\0\0", 5) == 0,
        "cached foreground bytes are served with DMA-only zero padding");
  check(module_bytes == 3 && module_sha256 ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "actual EXI records the loaded guest digest, excluding DMA padding");
  // A later no-response command must not detach the module name from the file
  // bytes, either in the same batch or in a subsequent command-only DMA write.
  request_file(name, true);
  guest[65] = 0xD7;  // STOP_MUSIC has no response
  slippi::dma_write(0, 66);
  slippi::dma_read(256, 3);
  check(module_records == 2 && module_name == name,
        "batched no-response command preserves file response identity");
  request_file(name, true);
  guest[100] = 0xD7;
  slippi::dma_write(100, 1);
  slippi::dma_read(256, 3);
  check(module_records == 3 && module_name == name,
        "separate no-response write preserves file response identity");
  const auto records_before_replacement = module_records;
  request_file(name, true);
  guest[65] = 0xD5;  // GET_DELAY replaces the file response
  slippi::dma_write(0, 66);
  slippi::dma_read(256, 2);
  check(guest[256] == 1 && guest[257] == 2 && module_records == records_before_replacement,
        "later response replaces file bytes and metadata together");
  auto stopping = std::async(std::launch::async, [] { slippi::shutdown(); });
  check(stopping.wait_for(20ms) == std::future_status::timeout,
        "shutdown waits for the in-flight owned preload read");
  {
    std::lock_guard<std::mutex> lock(read_mutex);
    read_release = true;
  }
  read_changed.notify_all();
  stopping.get();
  check(simulation_costs == 1, "only the foreground cache miss records simulation cost");
  check(background_costs == 0, "preload never touches simulation-thread accounting");
  check(background_reads == 1, "shutdown cancels queued preload files");
  check(jukebox_stops == 1, "EXI shutdown quiesces the jukebox owner");
  slippi::shutdown();
  return failures ? 1 : 0;
}
