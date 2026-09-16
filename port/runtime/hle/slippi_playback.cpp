// Port of the playback half of Dolphin's CEXISlippi (mode "normal", rollback display off, no
// fast-forward, no seeking): enough to replay a file frame-exact and record it again.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "slippi_playback.h"
#include "slippi_playback_legacy.h"
#include "host.h"
#include "slippilib/SlippiGame.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <unordered_map>

namespace slippi::playback {
namespace {
std::string g_path;
std::unique_ptr<Slippi::SlippiGame> g_game;
bool g_loaded_once = false, g_finished = false;
int32_t g_current_frame = Slippi::GAME_FIRST_FRAME;
std::vector<uint8_t> g_gecko_list;
uint32_t g_gecko_list_addr = 0;
enum : uint8_t { FRAME_RESP_WAIT = 0, FRAME_RESP_CONTINUE = 1, FRAME_RESP_TERMINATE = 2, FRAME_RESP_FASTFORWARD = 3 };

void append_u32(std::vector<uint8_t>& q, uint32_t v) { for (int i = 3; i >= 0; --i) q.push_back((uint8_t)(v >> (8 * i))); }
void append_u16(std::vector<uint8_t>& q, uint16_t v) { q.push_back((uint8_t)(v >> 8)); q.push_back((uint8_t)v); }
void append_f32(std::vector<uint8_t>& q, float f) { uint32_t v; std::memcpy(&v, &f, 4); append_u32(q, v); }

// Semver "a.b.c" >= "x.y.z"
bool version_at_least(const std::string& v, int a, int b, int c) {
  int x = 0, y = 0, z = 0;
  std::sscanf(v.c_str(), "%d.%d.%d", &x, &y, &z);
  if (x != a) return x > a;
  if (y != b) return y > b;
  return z >= c;
}

// Dolphin's denylist: injections that do not affect gameplay (from the Sys/Slippi/InjectionLists
// files, later-numbered files winning) plus a fixed backward-compatibility set.
std::unordered_map<uint32_t, bool> build_denylist() {
  std::unordered_map<uint32_t, bool> deny = {
      {0x802fef88, true}, {0x8006c5d8, true}, {0x8016d30c, true}, {0x8016e9b4, true},
      {0x802f6690, true}, {0x802F71E0, true}, {0x80071960, true}, {0x800CC818, true}, {0x8008A478, true},
  };
  std::filesystem::path dir = std::filesystem::path(host::options.sys_dir) / "Slippi" / "InjectionLists";
  std::vector<std::pair<int, std::filesystem::path>> files;
  std::error_code ec;
  for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
    if (!e.is_regular_file()) continue;
    std::string stem = e.path().stem().string();
    size_t us = stem.find_last_of('_');
    int num = 0;
    if (us != std::string::npos) num = std::atoi(stem.substr(us + 1).c_str());
    files.push_back({num, e.path()});
  }
  std::stable_sort(files.begin(), files.end(), [](auto& a, auto& b) { return a.first < b.first; });
  for (auto& [num, path] : files) {
    std::ifstream f(path);
    std::string contents((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto res = nlohmann::json::parse(contents, nullptr, false);
    if (res.is_discarded() || !res.is_object() || !res.count("Details") || !res["Details"].is_array()) { host::log("playback: injection list %s malformed", path.string().c_str()); continue; }
    for (auto& inj : res["Details"]) {
      if (!inj.is_object()) continue;
      std::string tags = inj.count("Tags") && inj["Tags"].is_string() ? inj["Tags"].get<std::string>() : "";
      std::string addr = inj.count("InjectionAddress") && inj["InjectionAddress"].is_string() ? inj["InjectionAddress"].get<std::string>() : "";
      if (addr.empty()) continue;
      deny[(uint32_t)std::strtoul(addr.c_str(), nullptr, 16)] = tags.find("[affects-gameplay]") == std::string::npos;
    }
  }
  deny[0x8038add0] = true;   // Online/Core/PreventFileAlarms/PreventMusicAlarm.asm (rollback display off)
  deny[0x80023FFC] = true;   // Online/Core/PreventFileAlarms/MuteMusic.asm
  return deny;
}

void prepare_gecko_list() {
  g_gecko_list.clear();
  auto* settings = g_game->GetSettings();
  if (settings->geckoCodes.empty()) {
    g_gecko_list.assign(LEGACY_CODELIST, LEGACY_CODELIST + sizeof LEGACY_CODELIST);
    host::log("playback: replay has no stored codes; serving the legacy list (%zu bytes)", g_gecko_list.size());
    return;
  }
  auto denylist = build_denylist();
  const std::vector<uint8_t>& source = settings->geckoCodes;
  size_t idx = 0, kept = 0, dropped = 0;
  while (idx + 8 <= source.size()) {
    uint8_t type = source[idx] & 0xFE;
    uint32_t address = ((uint32_t)source[idx] << 24 | (uint32_t)source[idx + 1] << 16 | (uint32_t)source[idx + 2] << 8 | source[idx + 3]);
    address = (address & 0x01FFFFFF) | 0x80000000;
    size_t len = 8;
    if (type == 0xC0 || type == 0xC2) { uint32_t lines = (uint32_t)source[idx + 4] << 24 | (uint32_t)source[idx + 5] << 16 | (uint32_t)source[idx + 6] << 8 | source[idx + 7]; len = 8 + (size_t)lines * 8; }
    else if (type == 0x08) len = 16;
    else if (type == 0x06) { uint32_t bytes = (uint32_t)source[idx + 4] << 24 | (uint32_t)source[idx + 5] << 16 | (uint32_t)source[idx + 6] << 8 | source[idx + 7]; len = 8 + ((bytes + 7) & ~7u); }
    if (idx + len > source.size()) break;
    auto it = denylist.find(address);
    if (it != denylist.end() && it->second) { ++dropped; idx += len; continue; }
    g_gecko_list.insert(g_gecko_list.end(), source.begin() + idx, source.begin() + idx + len);
    ++kept; idx += len;
  }
  g_gecko_list.insert(g_gecko_list.end(), {0xFF, 0, 0, 0, 0, 0, 0, 0});
  host::log("playback: gecko list from the replay: %zu codes kept, %zu denylisted, %zu bytes", kept, dropped, g_gecko_list.size());
  // The recompiler bakes this list (see recomp.py --extra-gct) so its caves run as translated code.
  std::string dump = host::options.replay_dir + "/gecko_list.bin";
  if (FILE* f = std::fopen(dump.c_str(), "wb")) { std::fwrite(g_gecko_list.data(), 1, g_gecko_list.size(), f); std::fclose(f); host::log("playback: served code list written to %s", dump.c_str()); }
}

void character_frame_data(const Slippi::FrameData* frame, uint8_t port, bool follower, std::vector<uint8_t>& q) {
  const auto& source = follower ? frame->followers : frame->players;
  auto it = source.find(port);
  if (it == source.end()) { q.insert(q.end(), 52, 0); return; }
  const Slippi::PlayerFrameData& d = it->second;
  append_u32(q, d.randomSeed);
  append_f32(q, d.joystickX); append_f32(q, d.joystickY); append_f32(q, d.cstickX); append_f32(q, d.cstickY); append_f32(q, d.trigger);
  append_u32(q, d.buttons);
  append_f32(q, d.locationX); append_f32(q, d.locationY); append_f32(q, d.facingDirection);
  append_u32(q, d.animation);
  q.push_back(d.joystickXRaw); q.push_back(d.joystickYRaw);
  append_f32(q, d.percent);
  q.push_back(d.cstickXRaw); q.push_back(d.cstickYRaw);
}
// Desync forensics (tools/mac/ramdiff.py): MELEE_RAM_DUMP_FRAMES=1056,1057 and MELEE_RAM_DUMP_DIR=<dir> write
// guest RAM as <dir>/ram_<frame>.bin the first time the game asks for that frame's inputs. Slippi Dolphin is
// dumped at the same point (CEXISlippi::prepareFrameData), so the two files are directly comparable.
void dump_ram_if_requested(int32_t frame) {
  static const std::vector<int32_t> frames = [] {
    std::vector<int32_t> out;
    if (const char* v = std::getenv("MELEE_RAM_DUMP_FRAMES"))
      for (const char* p = v; *p;) { char* end; long n = std::strtol(p, &end, 10); if (end == p) break; out.push_back((int32_t)n); p = *end ? end + 1 : end; }
    return out;
  }();
  static std::vector<int32_t> done;
  const char* dir = std::getenv("MELEE_RAM_DUMP_DIR");
  if (!dir || std::find(frames.begin(), frames.end(), frame) == frames.end() ||
      std::find(done.begin(), done.end(), frame) != done.end()) return;
  done.push_back(frame);
  std::string path = std::string(dir) + "/ram_" + std::to_string(frame) + ".bin";
  if (FILE* f = std::fopen(path.c_str(), "wb")) { std::fwrite(host::ram, 1, 0x01800000u, f); std::fclose(f); host::log("playback: RAM at frame %d written to %s", frame, path.c_str()); }
}
}  // namespace

void set_replay(const std::string& path) { g_path = path; }
bool enabled() { return !g_path.empty(); }

void prepare_is_file_ready(std::vector<uint8_t>& q) {
  q.clear();
  if (!enabled() || g_loaded_once) {
    q.push_back(0);
    if (g_loaded_once && !g_finished) { g_finished = true; host::log("playback: replay finished; exiting"); host::request_exit(0); }
    return;
  }
  g_game = Slippi::SlippiGame::FromFile(g_path);
  g_loaded_once = true;
  if (!g_game) { host::log("playback: cannot open replay %s", g_path.c_str()); q.push_back(0); host::request_exit(2); return; }
  host::log("playback: loaded %s (version %s, last frame %d)", g_path.c_str(), g_game->GetVersionString().c_str(), g_game->GetLatestIndex());
  q.push_back(1);
}

void prepare_game_info(const uint8_t*, std::vector<uint8_t>& q) {
  q.clear();
  if (!g_game) return;
  if (!g_game->AreSettingsLoaded()) { q.push_back(0); return; }
  q.push_back(1);
  Slippi::GameSettings* settings = g_game->GetSettings();
  append_u32(q, settings->randomSeed);
  std::array<uint32_t, Slippi::GAME_INFO_HEADER_SIZE> header = settings->header;
  for (int i = 0; i < 4; ++i) {
    if (!g_game->DoesPlayerExist((int8_t)i)) continue;
    uint8_t ext = settings->players[(uint8_t)i].characterId;
    if (ext != 0x12 && ext != 0x13) continue;   // Sheik/Zelda: overwrite the character in the header
    int pos = 24 + 9 * i;
    header[pos] &= 0x00FFFFFF; header[pos] |= (uint32_t)ext << 24;
  }
  for (int i = 0; i < Slippi::GAME_INFO_HEADER_SIZE; ++i) append_u32(q, header[i]);
  for (int i = 0; i < Slippi::UCF_TOGGLE_SIZE; ++i) append_u32(q, settings->ucfToggles[i]);
  for (int i = 0; i < 4; ++i) { auto player = settings->players[(uint8_t)i]; for (int j = 0; j < Slippi::NAMETAG_SIZE; ++j) append_u16(q, player.nametag[j]); }
  q.push_back(settings->isPAL);
  auto version = g_game->GetVersion();
  bool preload_ps = version[0] > 1 || (version[0] == 1 && version[1] > 2);
  q.push_back(preload_ps ? 1 : 0);
  q.push_back(settings->isFrozenPS);
  q.push_back(0);   // shouldResync
  for (int i = 0; i < 4; ++i) { auto& name = settings->players[(uint8_t)i].displayName; q.insert(q.end(), name.begin(), name.end()); }
  prepare_gecko_list();
  append_u32(q, (uint32_t)g_gecko_list.size());
  g_current_frame = Slippi::GAME_FIRST_FRAME;
}

void prepare_gecko_codes(std::vector<uint8_t>& q) { q.assign(g_gecko_list.begin(), g_gecko_list.end()); }

void note_gecko_list_dma(uint32_t addr, uint32_t size) {
  if (g_gecko_list_addr != addr) { g_gecko_list_addr = addr; host::log("playback: game placed the replay code list at %08X (%u bytes)", addr, size); }
}

void prepare_frame_data(const uint8_t* payload, std::vector<uint8_t>& q) {
  q.clear();
  if (!g_game) return;
  int32_t frame = (int32_t)((uint32_t)payload[0] << 24 | (uint32_t)payload[1] << 16 | (uint32_t)payload[2] << 8 | payload[3]);
  dump_ram_if_requested(frame);
  bool complete = g_game->IsProcessingComplete();
  bool found = g_game->DoesFrameExist(frame);
  bool fully = false;
  if (found) {
    Slippi::FrameData* f = g_game->GetFrame(frame);
    bool finalized = true;
    if (version_at_least(g_game->GetVersionString(), 3, 7, 0)) finalized = g_game->GetLastFinalizedFrame() >= frame;
    fully = f->inputsFullyFetched && finalized;
  }
  bool ready = found && (complete || fully);
  if (!ready) { q.push_back(complete ? FRAME_RESP_TERMINATE : FRAME_RESP_WAIT); if (complete) host::log("playback: game terminates on frame %d", frame); return; }
  g_current_frame = frame;
  q.push_back(FRAME_RESP_CONTINUE);
  Slippi::FrameData* f = g_game->GetFrame(frame);
  q.push_back(0);   // rollback code
  q.push_back(f->randomSeedExists ? 1 : 0);
  append_u32(q, f->randomSeed);
  for (uint8_t port = 0; port < 4; ++port) { character_frame_data(f, port, false, q); character_frame_data(f, port, true, q); }
}

void prepare_is_stock_steal(const uint8_t* payload, std::vector<uint8_t>& q) {
  q.clear();
  if (!g_game) return;
  int32_t frame = (int32_t)((uint32_t)payload[0] << 24 | (uint32_t)payload[1] << 16 | (uint32_t)payload[2] << 8 | payload[3]);
  uint8_t player = payload[4];
  if (!g_game->DoesFrameExist(frame)) { q.push_back(0); return; }
  q.push_back(g_game->GetFrame(frame)->players.count(player) ? 1 : 0);
}
}  // namespace slippi::playback
