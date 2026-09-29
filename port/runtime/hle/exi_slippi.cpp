// SPDX-License-Identifier: GPL-2.0-or-later
#include "exi_slippi.h"
#include "jukebox.h"
#include "slippi_playback.h"
#include "slippi_online.h"
#include "slippi_game_file.h"
#include "gecko_data.h"
#include "discord_rpc.h"
#include "host.h"
#include "slippi_login.h"
#include "fatal_boundary.h"
#include "sha256.h"
#include "window.h"
#include "vcdiff.h"
#include <cstdio>
#include <atomic>
#include <cstring>
#include <ctime>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <thread>
#include <map>
#include <unordered_map>
#include <vector>

namespace slippi {
namespace {

enum Cmd : uint8_t {
  CMD_RECEIVE_COMMANDS = 0x35, CMD_RECEIVE_GAME_INFO = 0x36, CMD_RECEIVE_POST_FRAME_UPDATE = 0x38, CMD_RECEIVE_GAME_END = 0x39,
  CMD_RECEIVE_INITIAL_RNG = 0x3A, CMD_RECEIVE_ITEM = 0x3B, CMD_FRAME_BOOKEND = 0x3C, CMD_GECKO_LIST = 0x3D, CMD_MENU_FRAME = 0x3E,
  CMD_RECEIVE_FOD_INFO = 0x3F, CMD_RECEIVE_DL_INFO = 0x40, CMD_RECEIVE_PS_INFO = 0x41, CMD_RECEIVE_BONES = 0x60,
  CMD_PREPARE_REPLAY = 0x75, CMD_READ_FRAME = 0x76, CMD_GET_LOCATION = 0x77, CMD_IS_FILE_READY = 0x88, CMD_IS_STOCK_STEAL = 0x89, CMD_GET_GECKO_CODES = 0x8A,
  CMD_ONLINE_INPUTS = 0xB0, CMD_CAPTURE_SAVESTATE = 0xB1, CMD_LOAD_SAVESTATE = 0xB2, CMD_GET_MATCH_STATE = 0xB3, CMD_FIND_OPPONENT = 0xB4,
  CMD_SET_MATCH_SELECTIONS = 0xB5, CMD_OPEN_LOGIN = 0xB6, CMD_LOGOUT = 0xB7, CMD_UPDATE = 0xB8, CMD_GET_ONLINE_STATUS = 0xB9,
  CMD_CLEANUP_CONNECTION = 0xBA, CMD_SEND_CHAT_MESSAGE = 0xBB, CMD_GET_NEW_SEED = 0xBC, CMD_REPORT_GAME = 0xBD, CMD_FETCH_CODE_SUGGESTION = 0xBE,
  CMD_OVERWRITE_SELECTIONS = 0xBF, CMD_GP_COMPLETE_STEP = 0xC0, CMD_GP_FETCH_STEP = 0xC1, CMD_REPORT_SET_COMPLETE = 0xC2,
  CMD_GET_PLAYER_SETTINGS = 0xC3, CMD_REPORT_MATCH_STATUS_UPDATE = 0xC4,
  CMD_LOG_MESSAGE = 0xD0, CMD_FILE_LENGTH = 0xD1, CMD_FILE_LOAD = 0xD2, CMD_GCT_LENGTH = 0xD3, CMD_GCT_LOAD = 0xD4, CMD_GET_DELAY = 0xD5,
  CMD_PLAY_MUSIC = 0xD6, CMD_STOP_MUSIC = 0xD7, CMD_CHANGE_MUSIC_VOLUME = 0xD8, CMD_PREMADE_TEXT_LENGTH = 0xE1, CMD_PREMADE_TEXT_LOAD = 0xE2,
  CMD_GET_RANK = 0xE3, CMD_FETCH_RANK = 0xE4, CMD_GET_RANK_VISIBILITY = 0xE5,
};

// Fixed payload sizes (bytes after the command byte), from CEXISlippi::payloadSizes.
std::unordered_map<uint8_t, uint32_t> g_payload_sizes = {
    {CMD_RECEIVE_COMMANDS, 1}, {CMD_PREPARE_REPLAY, 0xFFFF}, {CMD_READ_FRAME, 4}, {CMD_IS_STOCK_STEAL, 5}, {CMD_GET_LOCATION, 6},
    {CMD_IS_FILE_READY, 0}, {CMD_GET_GECKO_CODES, 0}, {CMD_ONLINE_INPUTS, 25}, {CMD_CAPTURE_SAVESTATE, 32}, {CMD_LOAD_SAVESTATE, 32},
    {CMD_GET_MATCH_STATE, 0}, {CMD_FIND_OPPONENT, 19}, {CMD_SET_MATCH_SELECTIONS, 9}, {CMD_SEND_CHAT_MESSAGE, 2}, {CMD_OPEN_LOGIN, 0},
    {CMD_LOGOUT, 0}, {CMD_UPDATE, 0}, {CMD_GET_ONLINE_STATUS, 0}, {CMD_CLEANUP_CONNECTION, 0}, {CMD_GET_NEW_SEED, 0},
    {CMD_REPORT_GAME, 368}, {CMD_FETCH_CODE_SUGGESTION, 31}, {CMD_OVERWRITE_SELECTIONS, 2 + 12},
    {CMD_GP_COMPLETE_STEP, 5}, {CMD_GP_FETCH_STEP, 1}, {CMD_REPORT_SET_COMPLETE, 1}, {CMD_GET_PLAYER_SETTINGS, 0}, {CMD_REPORT_MATCH_STATUS_UPDATE, 1},
    {CMD_LOG_MESSAGE, 0xFFFF}, {CMD_FILE_LENGTH, 0x40}, {CMD_FILE_LOAD, 0x40}, {CMD_GCT_LENGTH, 0}, {CMD_GCT_LOAD, 4}, {CMD_GET_DELAY, 0},
    {CMD_PLAY_MUSIC, 8}, {CMD_STOP_MUSIC, 0}, {CMD_CHANGE_MUSIC_VOLUME, 1}, {CMD_PREMADE_TEXT_LENGTH, 2}, {CMD_PREMADE_TEXT_LOAD, 2},
    {CMD_GET_RANK, 0}, {CMD_FETCH_RANK, 0}, {CMD_GET_RANK_VISIBILITY, 0},
};
// Sizes of the recording commands are configured by CMD_RECEIVE_COMMANDS at game start.
std::unordered_map<uint8_t, uint32_t> g_record_sizes;

// One response owns both its bytes and their provenance. The last response in
// a command batch wins; commands without a response cannot disturb this pair.
struct DmaResponse {
  std::vector<uint8_t> bytes;
  std::string module_name;
  bool gecko_list = false;
};
DmaResponse g_response;
uint32_t g_gct_address = 0;
uint64_t g_commands = 0;
RecordingEvents g_recording_events;
std::string g_replay_dir = "replays";
uint8_t g_frame_delay = 2;   // Slippi Online input delay setting (frames)

inline void append_u32(std::vector<uint8_t>& q, uint32_t v) { q.push_back((uint8_t)(v >> 24)); q.push_back((uint8_t)(v >> 16)); q.push_back((uint8_t)(v >> 8)); q.push_back((uint8_t)v); }
inline uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

void observe_recording_event(const uint8_t* data, size_t size) {
#if !defined(MELEE_PORT_OFFLINE) || !MELEE_PORT_OFFLINE   // the offline executables have no Discord presence
  slippi::discord::on_replay_event(data, size);   // stage, characters, live stocks, set score
#endif
  const uint8_t transition = g_recording_events.observe(data, size);
  if (transition & RecordingEvents::Commands) host::log("accept: slippi event=commands frame=na");
  if (transition & RecordingEvents::GameStart) host::log("accept: slippi event=game_start frame=na");
  if (transition & RecordingEvents::GameEnd) {
    if (g_recording_events.has_frames) host::log("accept: slippi event=game_end frame=%d", g_recording_events.max_frame);
    else host::log("accept: slippi event=game_end frame=na");
  }
  if ((transition & RecordingEvents::MatchInputStart) && !online::is_online_match()) host::input_mark_match_start();
}

// ---- .slp replay recording (UBJSON container, exactly as CEXISlippi::writeToFile) ----
FILE* g_file = nullptr;
uint32_t g_written = 0;
int32_t g_last_frame = -123;
time_t g_start_time = 0;
std::map<uint8_t, std::map<uint8_t, uint32_t>> g_char_usage;   // player index -> internal character -> frames
std::string g_replay_path;
uint64_t g_replays_written = 0;

std::vector<uint8_t> generate_metadata() {
  std::vector<uint8_t> m({'U', 8, 'm', 'e', 't', 'a', 'd', 'a', 't', 'a', '{'});
  char stamp[32];
  std::strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&g_start_time));
  std::string date(stamp);
  m.insert(m.end(), {'U', 7, 's', 't', 'a', 'r', 't', 'A', 't', 'S', 'U', (uint8_t)date.size()});
  m.insert(m.end(), date.begin(), date.end());
  m.insert(m.end(), {'U', 9, 'l', 'a', 's', 't', 'F', 'r', 'a', 'm', 'e', 'l'});
  append_u32(m, (uint32_t)g_last_frame);
  m.insert(m.end(), {'U', 7, 'p', 'l', 'a', 'y', 'e', 'r', 's', '{'});
  for (auto& [player, usage] : g_char_usage) {
    std::string idx = std::to_string(player);
    m.push_back('U'); m.push_back((uint8_t)idx.size()); m.insert(m.end(), idx.begin(), idx.end()); m.push_back('{');
    m.insert(m.end(), {'U', 5, 'n', 'a', 'm', 'e', 's', '{', '}'});
    m.insert(m.end(), {'U', 10, 'c', 'h', 'a', 'r', 'a', 'c', 't', 'e', 'r', 's', '{'});
    for (auto& [character, frames] : usage) {
      std::string cid = std::to_string(character);
      m.push_back('U'); m.push_back((uint8_t)cid.size()); m.insert(m.end(), cid.begin(), cid.end());
      m.push_back('l'); append_u32(m, frames);
    }
    m.push_back('}'); m.push_back('}');
  }
  m.push_back('}');
  m.insert(m.end(), {'U', 8, 'p', 'l', 'a', 'y', 'e', 'd', 'O', 'n', 'S', 'U', 7, 'd', 'o', 'l', 'p', 'h', 'i', 'n'});
  m.push_back('}');
  return m;
}

void close_file() {
  if (!g_file) return;
  std::fclose(g_file); g_file = nullptr;
  ++g_replays_written;
  host::log("slippi: replay written: %s (%u raw bytes, last frame %d)", g_replay_path.c_str(), g_written, g_last_frame);
}

void create_file() {
  close_file();
  std::error_code error;
  std::filesystem::create_directories(g_replay_dir, error);
  if (error) { host::log("slippi: cannot create replay directory: %s", error.message().c_str()); return; }
  char stamp[32];
  std::strftime(stamp, sizeof stamp, "%Y%m%dT%H%M%S", std::localtime(&g_start_time));
  g_replay_path = (std::filesystem::path(g_replay_dir) / (std::string("Game_") + stamp + ".slp")).string();
  g_file = std::fopen(g_replay_path.c_str(), "wb");
  if (!g_file) { host::log("slippi: cannot create %s", g_replay_path.c_str()); return; }
  const uint8_t header[] = {'{', 'U', 3, 'r', 'a', 'w', '[', '$', 'U', '#', 'l', 0, 0, 0, 0};
  std::fwrite(header, 1, sizeof header, g_file);
  g_written = 0;
  g_char_usage.clear();
  g_last_frame = -123;
}

void write_to_file(const uint8_t* payload, uint32_t length, const char* option) {
  if (std::strcmp(option, "create") == 0) create_file();
  if (!g_file) return;
  if (length > 0 && payload[0] == CMD_RECEIVE_POST_FRAME_UPDATE && length >= 8) {
    g_last_frame = (int32_t)be32(payload + 1);
    g_char_usage[payload[5]][payload[7]] += 1;
  }
  std::fwrite(payload, 1, length, g_file);
  g_written += length;
  if (std::strcmp(option, "close") == 0) {
    std::vector<uint8_t> closing = generate_metadata();
    closing.push_back('}');
    std::fwrite(closing.data(), 1, closing.size(), g_file);
    uint8_t size_bytes[4] = {(uint8_t)(g_written >> 24), (uint8_t)(g_written >> 16), (uint8_t)(g_written >> 8), (uint8_t)g_written};
    std::fseek(g_file, 11, SEEK_SET);
    std::fwrite(size_bytes, 1, 4, g_file);
    close_file();
  }
}

void configure_commands(const uint8_t* payload, uint8_t length) {
  // payload[0] is this command's own size byte; then (command, u16 size) triples.
  for (uint32_t i = 1; i + 2 < (uint32_t)length + 1; i += 3) {
    uint8_t cmd = payload[i];
    uint32_t size = ((uint32_t)payload[i + 1] << 8) | payload[i + 2];
    g_record_sizes[cmd] = size;
  }
  host::log("slippi: recording command sizes configured (%zu commands)", g_record_sizes.size());
}

const gecko::CodeTable& selected_gct() {
  return gecko::slippi_gct_options[(gecko::option_widescreen ? 1 : 0) |
                                    (gecko::option_flash_failed_lcancel ? 2 : 0)];
}

std::atomic<int> g_widescreen_request{-1};

void apply_widescreen(bool on) {
  gecko::option_widescreen = on;
  if (g_gct_address) {
    uint8_t* table = host::ptr(g_gct_address, (uint32_t)gecko::slippi_gct_size);
    const gecko::CodeTable& selected = selected_gct();
    std::memcpy(table + gecko::optional_gct_offset, selected.data + gecko::optional_gct_offset,
                selected.size - gecko::optional_gct_offset);
  }
  for (size_t i = 0; i < gecko::optional_writes_count; ++i) {
    const gecko::OptionalWrite& w = gecko::optional_writes[i];
    if (w.enabled == &gecko::option_widescreen)
      std::memcpy(host::ptr(w.addr, w.size), on ? w.patched : w.original, w.size);
  }
  host::log("slippi: widescreen 16:9 %s", on ? "on" : "off");
}

void prepare_gct_length() {
  DmaResponse response;
  append_u32(response.bytes, (uint32_t)selected_gct().size);
  g_response = std::move(response);
}

void prepare_gct_load(const uint8_t* payload) {
  DmaResponse response;
  g_gct_address = be32(payload);
  const gecko::CodeTable& selected = selected_gct();
  host::log("slippi: game loads the GCT (%zu bytes) at %08X%s", selected.size, g_gct_address,
            gecko::gct_base_used == g_gct_address ? "" : " (recompile with --gct-base to translate C0 caves at this address)");
  response.bytes.insert(response.bytes.end(), selected.data, selected.data + selected.size);
  g_response = std::move(response);
}

void log_message(const uint8_t* payload, uint32_t max) {
  std::string s;
  for (uint32_t i = 0; i < max && payload[i]; ++i) s += (char)payload[i];
  host::log("slippi[game]: %s", s.c_str());
}

// Game files: Sys/GameFiles/GALE01/<name> served as-is, or <name>.diff (VCDIFF) applied to the
// file of the same name from the ISO. Port of SlippiGameFileLoader::LoadFile.
std::unordered_map<std::string, std::vector<uint8_t>> g_file_cache;
std::mutex g_file_cache_mutex;   // the boot-time preload thread and the simulation thread share it
std::optional<files::GameFileRoot> g_game_file_root;
std::thread g_preload_thread;
std::atomic<bool> g_preload_cancel{false};

enum class FileLoadContext { Simulation, Preload };
bool preload_cancelled(FileLoadContext context) {
  return context == FileLoadContext::Preload &&
         (g_preload_cancel.load() || host::background_failed());
}

void stop_preload() {
  g_preload_cancel.store(true);
  // Lifecycle operations run on the owner, never in the wrapped worker below.
  if (g_preload_thread.joinable()) g_preload_thread.join();
}

bool read_whole_file(const std::string& path, std::vector<uint8_t>& out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
  out.resize(n > 0 ? (size_t)n : 0);
  bool ok = n <= 0 || std::fread(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  return ok;
}

// Reads and patches one file. Runs without the cache lock held: the preload worker must never
// make the simulation thread wait behind a multi-megabyte read plus VCDIFF.
std::vector<uint8_t> build_game_file(const std::string& name, FileLoadContext context) {
  std::vector<uint8_t> out;
  if (preload_cancelled(context) || !g_game_file_root || !files::valid_name(name)) return out;
  std::vector<uint8_t> blob;
  const auto direct = g_game_file_root->file(name, false);
  if (name != "MxDt.dat" && direct && read_whole_file(direct->string(), blob)) {
    out = std::move(blob);
    host::log("slippi: served %s (%zu bytes)", name.c_str(), out.size());
    return out;
  }
  if (preload_cancelled(context)) return out;
  const auto diff = g_game_file_root->file(name, true);
  if (diff && read_whole_file(diff->string(), blob)) {
    if (preload_cancelled(context)) return out;
    uint32_t off = 0, size = 0;
    std::vector<uint8_t> source;
    if (host::disc_find_file(name, &off, &size)) {
      source.resize(size);
      if (!host::disc_read(off, source.data(), size)) source.clear();
    }
    if (preload_cancelled(context)) return out;
    std::string err;
    if (source.empty() || !host::vcdiff_decode(source.data(), source.size(), blob.data(), blob.size(), out, &err)) {
      host::log("slippi: cannot apply %s.diff (%s)", name.c_str(), source.empty() ? "file not on disc" : err.c_str());
      out.clear();
    } else {
      host::log("slippi: served %s (%zu bytes from ISO + %zu byte diff)", name.c_str(), out.size(), blob.size());
    }
    return out;
  }
  host::log("slippi: game file %s not found in %s", name.c_str(), host::options.sys_dir.c_str());
  return out;
}

const std::vector<uint8_t>& load_game_file(const std::string& name, FileLoadContext context) {
  static const std::vector<uint8_t> cancelled;
  if (preload_cancelled(context)) return cancelled;
  {
    std::lock_guard<std::mutex> lock(g_file_cache_mutex);   // node-based map: element references survive later inserts
    auto it = g_file_cache.find(name);
    if (it != g_file_cache.end()) return it->second;
  }
  // Simulation cost arrays are owned by the simulation thread. Background
  // warming must neither race those arrays nor count decode time as frame work.
  std::optional<host::SimCostScope> cost;
  if (context == FileLoadContext::Simulation) cost.emplace(host::SIM_EXI);
  std::vector<uint8_t> built = build_game_file(name, context);
  if (preload_cancelled(context)) return cancelled;
  std::lock_guard<std::mutex> lock(g_file_cache_mutex);
  return g_file_cache.emplace(name, std::move(built)).first->second;   // a racing preload already inserted: keep that copy
}

void prepare_file(const uint8_t* payload, uint32_t available, bool load) {
  DmaResponse response;
  const auto name = files::guest_name(payload, available);
  if (!name) {
    if (!load) append_u32(response.bytes, 0);
    host::log("slippi: rejected invalid game-file request");
    g_response = std::move(response);
    return;
  }
  const std::vector<uint8_t>& data = load_game_file(*name, FileLoadContext::Simulation);
  if (!load) append_u32(response.bytes, (uint32_t)data.size());
  else {
    response.bytes = data;
    if (!data.empty()) response.module_name = *name;
  }
  g_response = std::move(response);
}


}  // namespace

// Every game file the Sys folder can serve is read and patched on a worker at boot, so the first
// request from the game (menus, CSS) is a cache hit instead of a multi-megabyte read plus VCDIFF
// on the simulation thread.
static void preload_game_files() {
  std::error_code ec;
  const auto dir = g_game_file_root ? g_game_file_root->directory() : std::nullopt;
  if (!dir) return;
  std::vector<std::string> names;
  for (auto& entry : std::filesystem::directory_iterator(*dir, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    std::string name = entry.path().filename().string();
    if (name.size() > 5 && name.compare(name.size() - 5, 5, ".diff") == 0) name.resize(name.size() - 5);
    if (files::valid_name(name)) names.push_back(name);
  }
  g_preload_cancel.store(false);
  g_preload_thread = std::thread([names = std::move(names)] {
    host::run_background_task([&] {
      for (const auto& n : names) {
        if (preload_cancelled(FileLoadContext::Preload)) break;
        load_game_file(n, FileLoadContext::Preload);
      }
    });
  });
}

void init() {
  stop_preload();
  jukebox::init();
  { std::lock_guard<std::mutex> lock(g_file_cache_mutex); g_file_cache.clear(); }
  g_game_file_root.emplace(host::options.sys_dir);
  g_recording_events = {};
  g_record_sizes.clear();
  g_response = {};
  g_replay_dir = host::options.replay_dir;
  online::config().offline = host::options.offline;
  online::config().offline_seed = static_cast<uint32_t>(host::options.time_base);
  online::init();
  preload_game_files();
}
void request_widescreen(bool on) { g_widescreen_request.store(on ? 1 : 0); }
bool widescreen() { return gecko::option_widescreen; }
void poll_options() {
  int r = g_widescreen_request.exchange(-1);
  if (r >= 0 && (r != 0) != gecko::option_widescreen) apply_widescreen(r != 0);
}
void shutdown() {
  stop_preload();
  jukebox::shutdown();
  if (g_file) { uint8_t empty[1]; write_to_file(empty, 0, "close"); }
  online::shutdown();
}
uint64_t replays_written() { return g_replays_written; }
RecordingEvents recording_events() { return g_recording_events; }
uint32_t gct_load_address() { return g_gct_address; }
uint64_t commands_seen() { return g_commands; }
const std::string& replay_directory() { return g_replay_dir; }
const std::string& last_replay_path() { return g_replay_path; }

void imm_write(uint32_t, uint32_t) {}
uint32_t imm_read(uint32_t) { return 0; }

void dma_write(uint32_t addr, uint32_t size) {
  if (!size) return;
  const uint8_t* mem = host::ptr(addr, size);
  static const bool diag_exi = std::getenv("MELEE_DIAG_EXI") != nullptr;   // isolation aid
  if (diag_exi) host::log("slippi: exi write %02X size %u at %08X (retrace %u)", mem[0], size, addr, host::retrace_count());
  uint32_t loc = 0;
  uint8_t byte = mem[0];
  if (byte == CMD_RECEIVE_COMMANDS) {
    if (size < 2 || mem[1] < 1 || uint32_t(mem[1]) + 1 > size || (mem[1] - 1) % 3 != 0) {
      host::log("slippi: rejected truncated recording command table");
      return;
    }
    std::time(&g_start_time);
    uint8_t len = mem[1];
    g_record_sizes.clear();
    configure_commands(&mem[1], len);
    observe_recording_event(mem, len + 1);
    write_to_file(&mem[0], len + 1, "create");
    loc += len + 1;
    ++g_commands;
  }
  if (byte == CMD_MENU_FRAME) { ++g_commands; return; }
  uint8_t prev = 0;
  while (loc < size) {
    byte = mem[loc];
    uint32_t payload = 0;
    auto fixed = g_payload_sizes.find(byte);
    auto rec = g_record_sizes.find(byte);
    if (fixed != g_payload_sizes.end()) payload = fixed->second;
    else if (rec != g_record_sizes.end()) payload = rec->second;
    else { host::log("slippi: invalid command byte %02X (previous %02X)", byte, prev); return; }
    // Like Dolphin's CEXISlippi::DMAWrite, a command's payload is read from guest RAM at
    // its table size even when the game transferred fewer bytes (file requests send
    // strlen + 2). Only a payload that would leave guest RAM is rejected.
    if (payload != 0xFFFF && payload > size - loc - 1) {
      const uint32_t start = (addr + loc + 1) & 0x01FFFFFFu;
      if (start + payload > 0x01800000u) {
        host::log("slippi: rejected EXI command %02X whose payload leaves guest RAM (addr %08X, dma size %u)", byte, addr, size);
        return;
      }
      mem = host::ptr(addr, loc + 1 + payload);
    }
    if (byte >= 0x36 && byte <= 0x39) observe_recording_event(&mem[loc], payload + 1);
    ++g_commands;
    switch (byte) {
      case CMD_GCT_LENGTH: prepare_gct_length(); break;
      case CMD_GCT_LOAD: prepare_gct_load(&mem[loc + 1]); break;
      case CMD_LOG_MESSAGE: log_message(&mem[loc + 1], size - loc - 1); break;
      case CMD_FILE_LENGTH: prepare_file(&mem[loc + 1], payload, false); break;
      case CMD_FILE_LOAD: prepare_file(&mem[loc + 1], payload, true); break;
      case CMD_PREMADE_TEXT_LENGTH: {
        DmaResponse response;
        append_u32(response.bytes, 0);
        g_response = std::move(response);
        break;
      }
      case CMD_PREMADE_TEXT_LOAD: g_response = {}; break;
      case CMD_PLAY_MUSIC: jukebox::start_song(be32(&mem[loc + 1]), be32(&mem[loc + 5])); break;
      case CMD_STOP_MUSIC: jukebox::stop(); break;
      case CMD_CHANGE_MUSIC_VOLUME: jukebox::set_melee_volume(mem[loc + 1]); break;
      case CMD_RECEIVE_COMMANDS: break;   // handled above
      case CMD_RECEIVE_GAME_END: write_to_file(&mem[loc], payload + 1, "close"); break;
      case CMD_FRAME_BOOKEND: write_to_file(&mem[loc], payload + 1, ""); break;
      case CMD_PREPARE_REPLAY: case CMD_READ_FRAME: case CMD_IS_STOCK_STEAL:
      case CMD_IS_FILE_READY: case CMD_GET_GECKO_CODES: {
        DmaResponse response;
        switch (byte) {
          case CMD_PREPARE_REPLAY: playback::prepare_game_info(&mem[loc + 1], response.bytes); break;
          case CMD_READ_FRAME: playback::prepare_frame_data(&mem[loc + 1], response.bytes); break;
          case CMD_IS_STOCK_STEAL: playback::prepare_is_stock_steal(&mem[loc + 1], response.bytes); break;
          case CMD_IS_FILE_READY: playback::prepare_is_file_ready(response.bytes); break;
          default: playback::prepare_gecko_codes(response.bytes); response.gecko_list = true; break;
        }
        g_response = std::move(response);
        break;
      }
      // These online commands own a response in both full and offline handlers.
      case CMD_ONLINE_INPUTS: case CMD_GET_MATCH_STATE: case CMD_FETCH_CODE_SUGGESTION:
      case CMD_GP_FETCH_STEP: case CMD_GET_DELAY: case CMD_GET_ONLINE_STATUS:
      case CMD_GET_NEW_SEED: case CMD_GET_PLAYER_SETTINGS: case CMD_GET_RANK: case CMD_GET_RANK_VISIBILITY: {
        DmaResponse response;
        online::handle(byte, &mem[loc + 1], payload, response.bytes);
        g_response = std::move(response);
        break;
      }
      case CMD_CAPTURE_SAVESTATE: case CMD_LOAD_SAVESTATE: case CMD_FIND_OPPONENT:
      case CMD_SET_MATCH_SELECTIONS: case CMD_OPEN_LOGIN: case CMD_LOGOUT: case CMD_UPDATE: case CMD_CLEANUP_CONNECTION:
      case CMD_SEND_CHAT_MESSAGE: case CMD_REPORT_GAME: case CMD_OVERWRITE_SELECTIONS:
      case CMD_GP_COMPLETE_STEP: case CMD_REPORT_SET_COMPLETE: case CMD_REPORT_MATCH_STATUS_UPDATE: case CMD_FETCH_RANK: {
        std::vector<uint8_t> no_response;
        online::handle(byte, &mem[loc + 1], payload, no_response);
        break;
      }
      default:
        // Recording payloads (game info, frames, items, bones...) go to the replay file.
        write_to_file(&mem[loc], payload + 1, "");
        break;
    }
    prev = byte;
    loc += payload + 1;
  }
}

void dma_read(uint32_t addr, uint32_t size) {
  if (!size) return;
  DmaResponse response = std::move(g_response);
  g_response = {};
  if (response.gecko_list) playback::note_gecko_list_dma(addr, size);
  if (response.bytes.empty()) { host::log("slippi: DMA read of %u bytes with an empty response queue", size); return; }
  const uint32_t loaded = std::min<uint32_t>(size, static_cast<uint32_t>(response.bytes.size()));
  response.bytes.resize(size, 0);
  std::memcpy(host::ptr(addr, size), response.bytes.data(), size);
  if (loaded && !response.module_name.empty()) {
    // Hash exactly the post-DMA guest span recorded below, including all loaded
    // patches. Source-file hashes and zero padding outside that span are unused.
    const std::string sha256 = host::memory_sha256(host::ptr(addr, loaded), loaded);
    ppc::record_loaded_module(response.module_name.c_str(), addr, loaded, sha256.c_str());
  }
}

}  // namespace slippi
