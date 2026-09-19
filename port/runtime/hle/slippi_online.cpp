// Slippi Online command handling (see slippi_online.h): port of the online half of Dolphin's
// CEXISlippi plus SlippiSavestate. The game-side Slippi codes drive everything: they ask for
// the match state each frame in the lobby, send local inputs and fetch remote ones each frame
// in a match, and capture/load savestates around rollbacks.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "slippi_online.h"
#include "slippi_net_diag.h"
#include "slippi_offline.h"

#if defined(MELEE_PORT_OFFLINE) && MELEE_PORT_OFFLINE
// The diagnostic target deliberately does not link identity, HTTP, ENet or
// reporting implementations. Asking it to enable online cannot load a profile.
namespace slippi::online {
namespace {
Config g_config;
bool g_initialized = false;
}
Config& config() { return g_config; }
bool available() { return false; }
void init() {
  if (g_initialized) return;
  offline::reset(g_config);
  g_initialized = true;
}
void shutdown() { g_initialized = false; }
uint64_t rollback_count() { return 0; }
int32_t current_online_frame() { return 0; }
bool is_online_match() { return false; }
bool handle(uint8_t cmd, const uint8_t* payload, uint32_t payload_len, std::vector<uint8_t>& q) {
  if (!g_initialized) init();
  return offline::handle(cmd, payload, payload_len, q);
}
}  // namespace slippi::online
#else
#include "slippi_net.h"
#include "slippi_report.h"
#include "exi_slippi.h"
#include "discord_rpc.h"
#include "host.h"
#include "window.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <random>
#include <thread>
#include <unordered_map>

namespace slippi::online {
namespace {

Config g_config;
bool g_initialized = false;
// Local two-instance peering is a test harness: never report those games or fetch ranks for them.
static bool reporting_enabled() { return !Matchmaking::local_peer.enabled; }
bool g_offline = true;

enum Cmd : uint8_t {
  CMD_ONLINE_INPUTS = 0xB0, CMD_CAPTURE_SAVESTATE = 0xB1, CMD_LOAD_SAVESTATE = 0xB2, CMD_GET_MATCH_STATE = 0xB3, CMD_FIND_OPPONENT = 0xB4,
  CMD_SET_MATCH_SELECTIONS = 0xB5, CMD_OPEN_LOGIN = 0xB6, CMD_LOGOUT = 0xB7, CMD_UPDATE = 0xB8, CMD_GET_ONLINE_STATUS = 0xB9,
  CMD_CLEANUP_CONNECTION = 0xBA, CMD_SEND_CHAT_MESSAGE = 0xBB, CMD_GET_NEW_SEED = 0xBC, CMD_REPORT_GAME = 0xBD, CMD_FETCH_CODE_SUGGESTION = 0xBE,
  CMD_OVERWRITE_SELECTIONS = 0xBF, CMD_GP_COMPLETE_STEP = 0xC0, CMD_GP_FETCH_STEP = 0xC1, CMD_REPORT_SET_COMPLETE = 0xC2,
  CMD_GET_PLAYER_SETTINGS = 0xC3, CMD_REPORT_MATCH_STATUS_UPDATE = 0xC4, CMD_GET_DELAY = 0xD5,
  CMD_GET_RANK = 0xE3, CMD_FETCH_RANK = 0xE4, CMD_GET_RANK_VISIBILITY = 0xE5,
};

inline uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
inline uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
inline void append_u32(std::vector<uint8_t>& q, uint32_t v) { for (int i = 3; i >= 0; --i) q.push_back((uint8_t)(v >> (8 * i))); }

// ---------------------------------------------------------------- savestates
// Exactly Dolphin's SlippiSavestate: full RAM regions minus the sound/VI exclusions, with the
// main heap bounds read from the game (0x804d76b8/0x804d76bc).
struct PreserveBlock { uint32_t address, length; bool operator==(const PreserveBlock& o) const { return address == o.address && length == o.length; } };
struct PreserveHash { size_t operator()(const PreserveBlock& b) const { return b.address ^ b.length; } };

class Savestate {
 public:
  struct Loc { uint32_t start, end; std::vector<uint8_t> data; };
  static bool force_init;
  Savestate() { initBackupLocs(); for (auto& l : locs_) l.data.resize(l.end - l.start); }
  void Capture() { for (auto& l : locs_) std::memcpy(l.data.data(), host::ptr(l.start, l.end - l.start), l.end - l.start); }
  void Load(const std::vector<PreserveBlock>& blocks) {
    for (auto& b : blocks) {
      auto& keep = preservation_[b];
      keep.resize(b.length);
      std::memcpy(keep.data(), host::ptr(b.address, b.length), b.length);
    }
    for (auto& l : locs_) std::memcpy(host::ptr(l.start, l.end - l.start), l.data.data(), l.end - l.start);
    for (auto& b : blocks) std::memcpy(host::ptr(b.address, b.length), preservation_[b].data(), b.length);
  }
 private:
  static std::vector<Loc> processed_;
  std::vector<Loc> locs_;
  std::unordered_map<PreserveBlock, std::vector<uint8_t>, PreserveHash> preservation_;

  void initBackupLocs() {
    struct Region { uint32_t start, end; };
    std::vector<Region> full = {
        {0x80005520, 0x80005940}, {0x803b7240, 0x804DEC00}, {0x8065c000, 0x8071b000}, {0x80bd5c40, 0x811AD5A0},
    };
    std::vector<PreserveBlock> exclude = {
        {0x804031A0, 0x24}, {0x80407FB4, 0x34C}, {0x80433C64, 0x1EE80}, {0x804A8D78, 0x17A68}, {0x804C28E0, 0x399C}, {0x804D7474, 0x8},
        {0x804D74F0, 0x50}, {0x804D7548, 0x4}, {0x804D7558, 0x24}, {0x804D7580, 0xC}, {0x804D759C, 0x4}, {0x804D7720, 0x4},
        {0x804D7744, 0x4}, {0x804D774C, 0x8}, {0x804D7758, 0x8}, {0x804D7788, 0x10}, {0x804D77C8, 0x4}, {0x804D77D0, 0x4},
        {0x804D77E0, 0x4}, {0x804DE358, 0x80}, {0x804DE800, 0x70},
        {0x804d6030, 0x4}, {0x804d603c, 0x4}, {0x804d7218, 0x4}, {0x804d7228, 0x8}, {0x804d7740, 0x4}, {0x804d7754, 0x4},
        {0x804d77bc, 0x4}, {0x804de7f0, 0x10},
        {0x804c0980, 0x15F8},
    };
    if (!processed_.empty() && !force_init) { locs_ = processed_; for (auto& l : locs_) l.data.clear(); return; }
    force_init = false;
    full[3].start = host::rd32(0x804d76b8);
    full[3].end = host::rd32(0x804d76bc);
    host::log("slippi: savestate heap region %08X-%08X", full[3].start, full[3].end);
    std::sort(exclude.begin(), exclude.end(), [](const PreserveBlock& a, const PreserveBlock& b) { return a.address < b.address; });
    std::vector<Loc> locs;
    for (auto& r : full) locs.push_back({r.start, r.end, {}});
    size_t idx = 0;
    for (PreserveBlock ipb : exclude) {
      while (ipb.length > 0) {
        while (idx < locs.size() && ipb.address >= locs[idx].end) ++idx;
        if (idx >= locs.size()) break;
        if (ipb.address < locs[idx].start) {
          int new_size = (int32_t)ipb.length - ((int32_t)locs[idx].start - (int32_t)ipb.address);
          ipb.length = new_size > 0 ? new_size : 0;
          ipb.address = locs[idx].start;
          continue;
        }
        int new_size = (int32_t)ipb.length - ((int32_t)locs[idx].end - (int32_t)ipb.address);
        if (locs[idx].end > ipb.address + ipb.length) locs.insert(locs.begin() + idx + 1, {ipb.address + ipb.length, locs[idx].end, {}});
        locs[idx].end = ipb.address;
        if (locs[idx].end <= locs[idx].start) locs.erase(locs.begin() + idx);
        new_size = new_size > 0 ? new_size : 0;
        ipb.address = ipb.address + (ipb.length - new_size);
        ipb.length = (uint32_t)new_size;
      }
    }
    processed_ = locs;
    locs_ = locs;
  }
};
bool Savestate::force_init = true;
std::vector<Savestate::Loc> Savestate::processed_;

// ---------------------------------------------------------------- state
std::unique_ptr<User> g_user;
std::unique_ptr<Matchmaking> g_matchmaking;
std::unique_ptr<NetplayClient> g_netplay;
std::unique_ptr<DirectCodes> g_direct_codes, g_teams_codes;
std::map<int32_t, std::unique_ptr<Savestate>> g_active_savestates;
std::deque<std::unique_ptr<Savestate>> g_available_savestates;
PlayerSelections g_local_selections;
Matchmaking::MatchSearchSettings g_last_search;
const char* mode_name(Matchmaking::OnlinePlayMode m) { return m == Matchmaking::RANKED ? "ranked" : m == Matchmaking::UNRANKED ? "unranked" : m == Matchmaking::DIRECT ? "direct" : m == Matchmaking::TEAMS ? "teams" : "online"; }
const char* mode_title(Matchmaking::OnlinePlayMode m) { return m == Matchmaking::RANKED ? "Ranked" : m == Matchmaking::UNRANKED ? "Unranked" : m == Matchmaking::DIRECT ? "Direct" : m == Matchmaking::TEAMS ? "Teams" : "Online"; }
Matchmaking::MatchmakeResult g_recent_mm_result;
std::vector<uint16_t> g_allowed_stages = {0x2, 0x3, 0x8, 0x1C, 0x1F, 0x20};
std::vector<uint16_t> g_stage_pool;
std::vector<PlayerSelections> g_overwrite_selections;
std::string g_forced_error;
bool g_play_session_active = false;
uint8_t g_local_player_index = 0, g_remote_player_index = 1;
uint32_t g_stall_frame_counts[REMOTE_PLAYER_MAX] = {};
// Open remote-input gap per player: the frame it started at (0 = none). A gap
// means the guest has to reuse old pads for the current frames - the usual
// first-domino of an online desync, so it gets named in the log.
int32_t g_input_gap_start[REMOTE_PLAYER_MAX] = {};
uint64_t g_last_interval_time_us = 0;
int32_t g_perf_debt = 0;
int g_frames_to_skip = 0, g_frames_to_advance = 0, g_fall_behind = 0, g_fall_far_behind = 0;
bool g_currently_skipping = false, g_currently_advancing = false;
std::mt19937 g_rng((uint32_t)time_ms());
uint64_t g_rollbacks = 0;
int32_t g_current_online_frame = 0;   // for the rollback log line
bool g_in_online_match = false;
// Determinism oracle: the game hands us a checksum of its finalized state each frame and the
// opponent's client sends theirs; a mismatch is a desync between the two simulations.
std::map<int32_t, uint32_t> g_local_checksums;
uint32_t g_checksums_compared = 0, g_checksums_mismatched = 0;
int32_t g_last_checksum_frame = 0;

bool is_disconnected() { return !g_netplay || g_netplay->GetSlippiConnectStatus() != NetplayClient::ConnectStatus::CONNECTED; }
bool chat_enabled() { return g_last_search.mode == Matchmaking::DIRECT ? (g_config.chat == 0 || g_config.chat == 1) : g_config.chat == 0; }

uint16_t random_stage() {
  if (g_stage_pool.empty()) g_stage_pool.insert(g_stage_pool.end(), g_allowed_stages.begin(), g_allowed_stages.end());
  int i = (int)(g_rng() % g_stage_pool.size());
  uint16_t s = g_stage_pool[i];
  g_stage_pool.erase(g_stage_pool.begin() + i);
  return s;
}

void cleanup_connection() {
  host::log("slippi: connection cleanup");
  if (g_matchmaking || g_netplay) {
    std::thread([mm = std::move(g_matchmaking), nc = std::move(g_netplay)]() mutable { mm.reset(); nc.reset(); }).detach();
  }
  g_matchmaking = std::make_unique<Matchmaking>(g_user.get());
  g_netplay = nullptr;
  discord::set_menus();
  g_local_selections.Reset();
  g_stage_pool.clear();
  g_forced_error.clear();
  g_overwrite_selections.clear();
  g_play_session_active = false;
  g_in_online_match = false;
  net_diag::match_ended("reset");
  host::set_online_ping_ms(-1);
  host::set_emulation_speed(1.0);
}

// ---------------------------------------------------------------- per-frame online flow
void handle_poor_match_performance(int32_t frame) {
  if (g_last_search.mode != Matchmaking::RANKED || !g_netplay) return;
  const uint64_t interval_frames = 150, frame_time_us = 16683;
  if ((frame + (interval_frames - 50)) % interval_frames != 0) return;
  double modifier = frame > 3600 ? 1.15 : 1.0;
  uint64_t cur = time_us();
  if (g_last_interval_time_us == 0) { g_last_interval_time_us = cur; g_netplay->GetAndResetAvgPingMs(); return; }
  double ratio = (double)(cur - g_last_interval_time_us) / (double)(frame_time_us * interval_frames);
  g_last_interval_time_us = cur;
  int32_t speed_debt = ratio >= 1.0 + 0.75 * modifier ? 15 : ratio >= 1.0 + 0.5 * modifier ? 8 : ratio >= 1.0 + 0.1 * modifier ? 4 : -1;
  double ping = g_netplay->GetAndResetAvgPingMs();
  int32_t ping_debt = ping >= 200 * modifier ? 15 : ping >= 120 * modifier ? 8 : ping >= 90 * modifier ? 4 : -1;
  g_perf_debt = std::max(0, g_perf_debt + std::max(speed_debt, ping_debt));
  if (g_perf_debt >= 30) {
    host::log("slippi: match terminated due to poor performance (%d)", g_perf_debt);
    { UserInfo me = g_user->GetUserInfo(); if (reporting_enabled()) report::match_status(me.uid, me.play_key, g_recent_mm_result.id, "poor_performance", true); }
    g_netplay->ForceDisconnect(NetplayClient::DisconnectReason::POOR_PERFORMANCE);
  }
}

bool should_skip_online_frame(int32_t frame, int32_t finalized_frame) {
  auto st = g_netplay->GetSlippiConnectStatus();
  if (st == NetplayClient::ConnectStatus::FAILED || st == NetplayClient::ConnectStatus::DISCONNECTED) return false;
  bool any_needs_inputs = false;
  uint8_t remote_count = g_matchmaking->RemotePlayerCount();
  for (uint8_t i = 0; i < remote_count; ++i) {
    auto pad = g_netplay->GetSlippiRemotePad(i, ROLLBACK_MAX_FRAMES);
    if (pad->is_disconnected) { g_stall_frame_counts[i] = 0; continue; }
    int32_t latest = pad->latest_frame;
    bool enough = latest - finalized_frame >= (frame - finalized_frame - ROLLBACK_MAX_FRAMES);
    if (enough) { g_stall_frame_counts[i] = 0; continue; }
    g_stall_frame_counts[i]++;
    any_needs_inputs = true;
    if (g_stall_frame_counts[i] > 60 * 7) {
      host::log("slippi: force-disconnecting player %u after 7 s stall (frame %d, latest %d)", pad->player_idx, frame, latest);
      g_netplay->ForceDisconnectPlayer(pad->player_idx);
      g_stall_frame_counts[i] = 0;
      continue;
    }
  }
  if (any_needs_inputs) return true;
  const int32_t frame_time = 16683, t1 = 10000, t2 = 2 * frame_time + t1;
  if (frame % ONLINE_LOCKSTEP_INTERVAL == 0 && !g_currently_skipping && frame <= 120) {
    int32_t offset = g_netplay->CalcTimeOffsetUs();
    if (offset > (frame <= 120 ? t1 : t2)) {
      g_currently_skipping = true;
      int max_skip = frame <= 120 ? 5 : 1;
      g_frames_to_skip = std::min(((offset - t1) / frame_time) + 1, max_skip);
      host::log("slippi: halting on frame %d for time sync (offset %d us, %d frames)", frame, offset, g_frames_to_skip);
    }
  }
  if (g_frames_to_skip > 0) { --g_frames_to_skip; return true; }
  g_currently_skipping = false;
  return false;
}

bool opponent_runahead() {
  auto info = g_matchmaking->GetPlayerInfo();
  for (size_t i = 0; i < info.size(); ++i) { if ((int)i == g_matchmaking->LocalPlayerIndex()) continue; if (!info[i].is_bot) return false; }
  return true;
}

bool should_advance_online_frame(int32_t frame) {
  if (opponent_runahead()) return false;
  if (frame % ONLINE_LOCKSTEP_INTERVAL == 0) {
    int32_t offset = g_netplay->CalcTimeOffsetUs();
    float deviation = 0;
    if (offset > -250 && offset < 8000) deviation = 0;
    else if (offset < 0) deviation = std::min(-offset / (3 * 16683.0f), 1.0f) * 0.01f;
    else deviation = std::min(offset / (3 * 16683.0f), 1.0f) * -0.005f;
    host::set_emulation_speed(1.0 + deviation);
    const int32_t frame_time = 16683, t1 = 10000, t2 = frame_time + t1;
    g_fall_behind += offset < -t1 ? 1 : 0;
    g_fall_far_behind += offset < -t2 ? 1 : 0;
    bool slow = (offset < -t1 && g_fall_behind > 50) || (offset < -t2 && g_fall_far_behind > 15);
    if (slow && g_matchmaking->RemotePlayerCount() == 1 && g_last_search.mode != Matchmaking::RANKED)
      host::log("slippi: possible poor match performance detected (offset %d us)", offset);
    if (offset < -t2 && !g_currently_advancing) {
      g_currently_advancing = true;
      int max_adv = frame > 120 ? 3 : 0;
      g_frames_to_advance = std::min(((-offset - t1) / frame_time) + 1, max_adv);
      host::log("slippi: advancing on frame %d for time sync (offset %d us, %d frames)", frame, offset, g_frames_to_advance);
    }
  }
  if (g_frames_to_advance > 0) {
    if (frame % 5 != 0) return false;
    --g_frames_to_advance;
    return true;
  }
  g_currently_advancing = false;
  return false;
}

void prepare_opponent_inputs(int32_t frame, bool should_skip, std::vector<uint8_t>& q) {
  q.clear();
  uint8_t frame_result = 1;
  auto st = g_netplay->GetSlippiConnectStatus();
  if (should_skip) frame_result = 2;
  else if (st != NetplayClient::ConnectStatus::CONNECTED) frame_result = 3;
  else if (should_advance_online_frame(frame)) frame_result = 4;
  q.push_back(frame_result);
  uint8_t remote_count = g_matchmaking->RemotePlayerCount();
  q.push_back(remote_count);
  std::unique_ptr<RemotePadOutput> results[REMOTE_PLAYER_MAX];
  int32_t latest_from_opps = -123 - 1;
  uint32_t last_checksum_frame = 0, last_checksum = 0;
  for (int i = 0; i < remote_count; ++i) {
    results[i] = g_netplay->GetSlippiRemotePad(i, ROLLBACK_MAX_FRAMES);
    if (results[i]->is_disconnected) continue;
    // Delivery-gap watch: if the newest remote input is older than the frame
    // being simulated, the guest reuses pads - name the gap when it opens and
    // again on the next checksum mismatch, so a desync points at its cause.
    if (results[i]->latest_frame < frame) {
      if (g_input_gap_start[i] == 0) {
        g_input_gap_start[i] = frame;
        host::log("slippi: remote input gap opens at frame %d (newest remote input %d, player %u)",
                  frame, results[i]->latest_frame, results[i]->player_idx);
      }
    } else if (g_input_gap_start[i] != 0) {
      host::log("slippi: remote input gap from frame %d closed at frame %d (player %u)",
                g_input_gap_start[i], frame, results[i]->player_idx);
      g_input_gap_start[i] = 0;
    }
    int32_t cf = results[i]->checksum_frame;
    if (cf > g_last_checksum_frame && results[i]->checksum) {
      auto it = g_local_checksums.find(cf);
      if (it != g_local_checksums.end()) {
        g_last_checksum_frame = cf; ++g_checksums_compared;
        if (it->second != results[i]->checksum) {
          ++g_checksums_mismatched;
          if (g_input_gap_start[i] != 0)
            host::log("slippi: DESYNC: checksum mismatch at frame %d (ours %08X, player %u %08X; remote input gap open since frame %d)",
                      cf, it->second, results[i]->player_idx, results[i]->checksum, g_input_gap_start[i]);
          else
            host::log("slippi: DESYNC: checksum mismatch at frame %d (ours %08X, player %u %08X; no remote input gap - suspect input conversion or sim divergence)",
                      cf, it->second, results[i]->player_idx, results[i]->checksum);
        }
        else if (g_checksums_compared % 20 == 0) host::log("slippi: checksums agree through frame %d (%u compared, %u mismatched)", cf, g_checksums_compared, g_checksums_mismatched);
      }
    }
    if (results[i]->latest_frame > latest_from_opps) {
      last_checksum_frame = (uint32_t)results[i]->checksum_frame;
      last_checksum = results[i]->checksum;
      latest_from_opps = results[i]->latest_frame;
    }
  }
  constexpr int32_t DESPAWN_INTERVAL = 30;
  uint8_t should_despawn[REMOTE_PLAYER_MAX] = {0, 0, 0};
  for (int i = 0; i < remote_count; ++i) {
    if (!results[i]->is_disconnected) continue;
    int32_t threshold = results[i]->latest_frame + 2 * ROLLBACK_MAX_FRAMES + 2;
    int32_t despawn = ((threshold + DESPAWN_INTERVAL - 1) / DESPAWN_INTERVAL) * DESPAWN_INTERVAL;
    if (frame >= despawn) should_despawn[i] = 1;
  }
  for (int i = 0; i < remote_count; ++i) {
    if (!results[i]->is_disconnected) { append_u32(q, (uint32_t)results[i]->checksum_frame); append_u32(q, results[i]->checksum); continue; }
    results[i]->latest_frame = latest_from_opps;
    append_u32(q, last_checksum_frame); append_u32(q, last_checksum);
  }
  for (int i = remote_count; i < REMOTE_PLAYER_MAX; ++i) { append_u32(q, 0); append_u32(q, 0); }
  int offset[REMOTE_PLAYER_MAX] = {};
  int32_t latest_read[REMOTE_PLAYER_MAX] = {};
  for (int i = 0; i < remote_count; ++i) {
    offset[i] = std::max(0, (results[i]->latest_frame - frame) * PAD_FULL_SIZE);
    int32_t latest = std::min(results[i]->latest_frame, frame);
    latest_read[i] = latest;
    append_u32(q, (uint32_t)latest);
  }
  for (int i = remote_count; i < REMOTE_PLAYER_MAX; ++i) { latest_read[i] = frame; append_u32(q, (uint32_t)frame); }
  append_u32(q, (uint32_t)*std::min_element(std::begin(latest_read), std::end(latest_read)));
  for (int i = 0; i < REMOTE_PLAYER_MAX; ++i) {
    std::vector<uint8_t> tx;
    if (i < remote_count && offset[i] < (int)results[i]->data.size()) tx.assign(results[i]->data.begin() + offset[i], results[i]->data.end());
    tx.resize(PAD_FULL_SIZE * ROLLBACK_MAX_FRAMES, 0);
    q.insert(q.end(), tx.begin(), tx.end());
  }
  for (int i = 0; i < REMOTE_PLAYER_MAX; ++i) q.push_back(should_despawn[i]);
}

void handle_online_inputs(const uint8_t* payload, std::vector<uint8_t>& q) {
  q.clear();
  int32_t frame = (int32_t)be32(payload), finalized = (int32_t)be32(payload + 4);
  uint32_t finalized_checksum = be32(payload + 8);
  uint8_t delay = payload[12];
  const uint8_t* inputs = payload + 13;
  if (frame == 1) {
    g_available_savestates.clear();
    g_active_savestates.clear();
    for (int i = 0; i < ROLLBACK_MAX_FRAMES; ++i) g_available_savestates.push_back(std::make_unique<Savestate>());
    for (auto& c : g_stall_frame_counts) c = 0;
    g_last_interval_time_us = 0; g_perf_debt = 0;
    g_frames_to_skip = 0; g_currently_skipping = false;
    g_frames_to_advance = 0; g_currently_advancing = false; g_fall_behind = 0; g_fall_far_behind = 0;
    g_local_selections.Reset();
    if (g_netplay) g_netplay->StartSlippiGame();
    g_in_online_match = true;
    net_diag::match_started(host::options.cache_dir.c_str());
    host::input_mark_match_start();
    g_local_checksums.clear(); g_checksums_compared = 0; g_checksums_mismatched = 0; g_last_checksum_frame = 0;
    for (auto& gap : g_input_gap_start) gap = 0;
  }
  if (is_disconnected()) {
    if (g_netplay && g_netplay->GetDisconnectReason() == NetplayClient::DisconnectReason::POOR_PERFORMANCE)
      host::log("slippi: the match has been terminated due to poor network quality");
    q.push_back(3);
    return;
  }
  g_current_online_frame = frame;
  if (frame % 30 == 0) host::log("slippi: online frame %d wall %.3f s retrace %u rollbacks %llu", frame, host::now_seconds(), host::retrace_count(), (unsigned long long)g_rollbacks);
  if (finalized > 0 && finalized_checksum) { g_local_checksums[finalized] = finalized_checksum; while (g_local_checksums.size() > 600) g_local_checksums.erase(g_local_checksums.begin()); }
  g_netplay->DropOldRemoteInputs(finalized);
  bool skip = should_skip_online_frame(frame, finalized);
  if (skip) g_netplay->SendSlippiPad(nullptr);
  else {
    handle_poor_match_performance(frame);
    if (frame == 1) for (int i = 1; i <= delay; ++i) g_netplay->SendSlippiPad(std::make_unique<Pad>(i));
    g_netplay->SendSlippiPad(std::make_unique<Pad>(frame + delay, finalized, finalized_checksum, inputs));
  }
  prepare_opponent_inputs(frame, skip, q);
}

void handle_capture_savestate(const uint8_t* payload) {
  host::SimCostScope cost(host::SIM_SAVESTATE);   // rollback bookkeeping shows up in the frame-timing log
  if (is_disconnected()) return;
  int32_t frame = (int32_t)be32(payload);
  std::unique_ptr<Savestate> ss;
  if (!g_available_savestates.empty()) { ss = std::move(g_available_savestates.back()); g_available_savestates.pop_back(); }
  else { auto it = g_active_savestates.begin(); ss = std::move(it->second); g_active_savestates.erase(it); }
  if (g_active_savestates.count(frame)) { g_available_savestates.push_back(std::move(g_active_savestates[frame])); g_active_savestates.erase(frame); }
  ss->Capture();
  g_active_savestates[frame] = std::move(ss);
}

void handle_load_savestate(const uint8_t* payload) {
  host::SimCostScope cost(host::SIM_SAVESTATE);
  int32_t frame = (int32_t)be32(payload);
  if (!g_active_savestates.count(frame)) { host::log("slippi: savestate for frame %d does not exist", frame); return; }
  std::vector<PreserveBlock> blocks;
  for (int i = 4; be32(payload + i) != 0; i += 8) blocks.push_back({be32(payload + i), be32(payload + i + 4)});
  g_active_savestates[frame]->Load(blocks);
  ++g_rollbacks;
  net_diag::on_rollback();
  // One line per rollback: a crash or desync right after one points at savestate coverage.
  host::log("slippi: rollback to frame %d from frame %d (%zu preserved blocks)", frame, g_current_online_frame, blocks.size());
  for (auto& kv : g_active_savestates) g_available_savestates.push_back(std::move(kv.second));
  g_active_savestates.clear();
}

// ---------------------------------------------------------------- lobby
void start_find_match(const uint8_t* payload) {
  Matchmaking::MatchSearchSettings search;
  search.mode = (Matchmaking::OnlinePlayMode)payload[0];
  std::string sj((const char*)payload + 1, 18);
  sj.erase(std::find(sj.begin(), sj.end(), '\0'), sj.end());
  if (search.mode == Matchmaking::DIRECT) g_direct_codes->AddOrUpdateCode(shiftjis_to_utf8(sj));
  else if (search.mode == Matchmaking::TEAMS) g_teams_codes->AddOrUpdateCode(shiftjis_to_utf8(sj));
  search.connect_code = sj;
  g_last_search = search;
  if (Matchmaking::IsFixedRulesMode(search.mode)) {
    if (g_local_selections.character_id >= 26) { g_forced_error = "The character you selected is not allowed in this mode"; return; }
    if (g_local_selections.is_stage_selected && std::find(g_allowed_stages.begin(), g_allowed_stages.end(), g_local_selections.stage_id) == g_allowed_stages.end()) {
      g_forced_error = "The stage being requested is not allowed in this mode"; return;
    }
  } else if (search.mode == Matchmaking::TEAMS && g_local_selections.character_id >= 26) {
    g_forced_error = "The character you selected is not allowed in this mode"; return;
  }
  if (!enet_ready()) { g_forced_error = "Networking unavailable"; return; }
  discord::set_searching(mode_name(search.mode));
  g_matchmaking->FindMatch(search);
}

bool tag_matches_input(const uint8_t* input, uint8_t len, const std::string& tag) {
  std::string jis = utf8_to_shiftjis(tag);
  for (int i = 0; i < len; ++i) {
    uint8_t a = i * 2 < (int)jis.size() ? (uint8_t)jis[i * 2] : 0, b = i * 2 + 1 < (int)jis.size() ? (uint8_t)jis[i * 2 + 1] : 0;
    if (input[i * 3] != a || input[i * 3 + 1] != b) return false;
  }
  return true;
}

void handle_name_entry_load(const uint8_t* payload, std::vector<uint8_t>& q) {
  uint8_t len = payload[24];
  uint32_t initial = be32(payload + 25);
  uint8_t scroll = payload[29], mode = payload[30];
  DirectCodes* history = mode == Matchmaking::TEAMS ? g_teams_codes.get() : g_direct_codes.get();
  uint32_t cur = initial;
  if (scroll == 1) ++cur;
  else if (scroll == 2) cur = cur > 0 ? cur - 1 : cur;
  else if (scroll == 3) cur = 0;
  std::string tag = "1";
  while (cur < (uint32_t)history->length()) {
    tag = history->get((int)cur);
    if (tag_matches_input(payload, len, tag)) break;
    cur = scroll == 2 ? cur - 1 : cur + 1;
  }
  tag = history->get((int)cur);
  if (tag == "1") {
    std::string init_tag = history->get((int)initial);
    if (tag_matches_input(payload, len, init_tag)) { tag = init_tag; cur = initial; }
  }
  q.clear();
  if (tag == "1") {
    q.push_back(0);
    q.insert(q.end(), payload, payload + 3 * len);
    q.insert(q.end(), 3 * (8 - len), 0);
    q.push_back(len);
    append_u32(q, initial);
    return;
  }
  q.push_back(1);
  std::string jis = utf8_to_shiftjis(tag);
  for (int i = 0; i < 8; ++i) {
    for (int j = i * 2; j < i * 2 + 2; ++j) q.push_back(j < (int)jis.size() ? (uint8_t)jis[j] : 0);
    q.push_back(0);
  }
  q.push_back((uint8_t)(jis.size() / 2));
  append_u32(q, cur);
}

void set_match_selections(const uint8_t* payload) {
  PlayerSelections s;
  s.team_id = payload[0];
  s.character_id = payload[1];
  s.character_color = payload[2];
  s.is_character_selected = payload[3] != 0;
  s.stage_id = be16(payload + 4);
  uint8_t stage_option = payload[6];
  s.alt_stage_mode = payload[8];
  s.is_stage_selected = stage_option == 1 || stage_option == 3;
  if (stage_option == 3) s.stage_id = random_stage();
  s.rng_offset = g_rng() % 0xFFFF;
  g_local_selections.Merge(s);
  if (g_netplay) g_netplay->SetMatchSelections(g_local_selections);
}

void prepare_online_match_state(std::vector<uint8_t>& q);

void prepare_online_match_state(std::vector<uint8_t>& q) {
  host::set_emulation_speed(1.0);
  static std::vector<uint8_t> block = {
      0x32, 0x01, 0x86, 0x4C, 0xC3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x6E, 0x00, 0x1F, 0x00, 0x00,
      0x01, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
      0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0x78, 0x00,
      0xC0, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80,
      0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x05, 0x00, 0x04, 0x01, 0x00, 0x01, 0x00, 0x00, 0x09, 0x00, 0x78, 0x00,
      0xC0, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80,
      0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x15, 0x03, 0x04, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x09, 0x00, 0x78, 0x00,
      0xC0, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80,
      0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x15, 0x03, 0x04, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x09, 0x00, 0x78, 0x00,
      0xC0, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80,
      0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x21, 0x03, 0x04, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x09, 0x00, 0x78, 0x00,
      0x40, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80,
      0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x21, 0x03, 0x04, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x09, 0x00, 0x78, 0x00,
      0x40, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80,
      0x00, 0x00, 0x3F, 0x80, 0x00, 0x00,
  };
  q.clear();
  Matchmaking::ProcessState mm_state = !g_forced_error.empty() ? Matchmaking::ERROR_ENCOUNTERED : g_matchmaking->GetMatchmakeState();
  q.push_back((uint8_t)mm_state);
  uint8_t local_ready = g_local_selections.is_character_selected ? 1 : 0;
  uint8_t remote_ready = 0;
  UserInfo me = g_user->GetUserInfo();
  uint16_t alt_stage_mode = 0;
  if (mm_state == Matchmaking::CONNECTION_SUCCESS) {
    g_local_player_index = (uint8_t)g_matchmaking->LocalPlayerIndex();
    if (!g_netplay) {
      g_netplay = g_matchmaking->GetNetplayClient();
      g_recent_mm_result = g_matchmaking->GetMatchmakeResult();
      {   // presence and a "match found" notification for a Mac that is not in front
        std::string opponent, opponent_with_code; int opponent_rank = 0;
        const std::vector<UserInfo>& players = g_recent_mm_result.players;
        for (size_t i = 0; i < players.size(); ++i) {
          if (players[i].port - 1 == g_local_player_index) continue;
          if (!opponent.empty()) { opponent += ", "; opponent_with_code += ", "; }
          opponent += players[i].display_name;
          opponent_with_code += players[i].display_name + " (" + players[i].connect_code + ")";
          if (!opponent_rank) opponent_rank = g_matchmaking->GetPlayerRank((uint8_t)i);
        }
        discord::set_local_port(g_local_player_index);
        discord::set_opponent(mode_name(g_last_search.mode), opponent, opponent_rank);
        host::notify_local("Match found", "vs " + opponent_with_code + "  ·  " + mode_title(g_last_search.mode));
      }
      g_allowed_stages = g_recent_mm_result.stages;
      if (g_allowed_stages.empty()) g_allowed_stages = {0x2, 0x3, 0x8, 0x1C, 0x1F, 0x20};
      g_stage_pool.clear();
      g_local_selections.stage_id = random_stage();
      g_netplay->SetMatchSelections(g_local_selections);
    }
    bool connected = g_netplay->GetSlippiConnectStatus() == NetplayClient::ConnectStatus::CONNECTED;
    if (g_netplay->GetActivePlayerIndices().size() != g_matchmaking->RemotePlayerCount()) connected = false;
    if (connected) {
      MatchInfo* mi = g_netplay->GetMatchInfo();
      remote_ready = 1;
      uint8_t remote_count = g_matchmaking->RemotePlayerCount();
      for (int i = 0; i < remote_count; ++i) if (!mi->remote[i].is_character_selected) remote_ready = 0;
      if (remote_count == 1) {
        bool decider = g_netplay->IsDecider();
        g_local_player_index = decider ? 0 : 1;
        g_remote_player_index = decider ? 1 : 0;
      }
    } else {
      cleanup_connection();
      prepare_online_match_state(q);
      return;
    }
    if (!g_play_session_active) g_play_session_active = true;
  } else {
    g_netplay = nullptr;
    discord::set_menus();
  }
  uint32_t rng_offset = 0;
  std::string local_name, opp_name;
  int8_t p1_rank = 0, p2_rank = 0;
  uint8_t chat_message_id = 0, chat_message_player_idx = 0, sent_chat_message_id = 0;
  DesyncRecoveryResp desync;
  if (g_netplay) desync = g_netplay->GetDesyncRecoveryState();
  if (desync.is_recovering && desync.is_waiting) remote_ready = 0;
  if (desync.is_error) { cleanup_connection(); prepare_online_match_state(q); return; }
  q.push_back(local_ready);
  q.push_back(remote_ready);
  q.push_back(g_local_player_index);
  q.push_back(g_remote_player_index);
  if (g_netplay) {
    bool single = g_matchmaking && g_matchmaking->RemotePlayerCount() == 1;
    bool chat = chat_enabled();
    sent_chat_message_id = g_netplay->GetSlippiRemoteSentChatMessage(chat);
    if (sent_chat_message_id <= 0) {
      PlayerSelections rm = g_netplay->GetSlippiRemoteChatMessage(chat);
      chat_message_id = (uint8_t)rm.message_id;
      chat_message_player_idx = rm.player_idx;
      if (chat_message_id == CHAT_MSG_CHAT_DISABLED && !single) chat_message_id = chat_message_player_idx = 0;
    } else {
      chat_message_player_idx = g_local_player_index;
    }
    if (single || !g_matchmaking) chat_message_player_idx = sent_chat_message_id > 0 ? g_local_player_index : g_remote_player_index;
    local_name = me.display_name;
  }
  if (local_ready && remote_ready) {
    bool decider = g_netplay->IsDecider();
    uint8_t remote_count = g_matchmaking->RemotePlayerCount();
    MatchInfo* mi = g_netplay->GetMatchInfo();
    PlayerSelections lps = mi->local;
    PlayerSelections rps[REMOTE_PLAYER_MAX];
    for (int i = 0; i < REMOTE_PLAYER_MAX; ++i) rps[i] = mi->remote[i];
    bool local_char_ok = lps.character_id < 26, remote_char_ok = true;
    for (int i = 0; i < remote_count; ++i) if (rps[i].character_id >= 26) remote_char_ok = false;
    std::vector<PlayerSelections*> ordered(remote_count + 1, nullptr);
    if (lps.player_idx < ordered.size()) ordered[lps.player_idx] = &lps;
    for (int i = 0; i < remote_count; ++i) if (rps[i].player_idx < ordered.size()) ordered[rps[i].player_idx] = &rps[i];
    for (auto& o : ordered) if (!o) o = &lps;   // defensive: never dereference a hole
    for (size_t i = 0; i < g_overwrite_selections.size() && i < ordered.size(); ++i) {
      ordered[i]->character_id = g_overwrite_selections[i].character_id;
      ordered[i]->character_color = g_overwrite_selections[i].character_color;
      ordered[i]->stage_id = g_overwrite_selections[i].stage_id;
    }
    uint16_t stage_id = 0x1F;
    for (auto* s : ordered) { if (!s->is_stage_selected) continue; stage_id = s->stage_id; alt_stage_mode = s->alt_stage_mode; break; }
    if (Matchmaking::IsFixedRulesMode(g_last_search.mode)) {
      if (!local_char_ok) { cleanup_connection(); g_forced_error = "The character you selected is not allowed in this mode"; prepare_online_match_state(q); return; }
      if (!remote_char_ok) { cleanup_connection(); prepare_online_match_state(q); return; }
      if (std::find(g_allowed_stages.begin(), g_allowed_stages.end(), stage_id) == g_allowed_stages.end()) { cleanup_connection(); prepare_online_match_state(q); return; }
    } else if (g_last_search.mode == Matchmaking::TEAMS) {
      if (!local_char_ok) { cleanup_connection(); g_forced_error = "The character you selected is not allowed in this mode"; prepare_online_match_state(q); return; }
      if (!remote_char_ok) { cleanup_connection(); prepare_online_match_state(q); return; }
    }
    rng_offset = decider ? lps.rng_offset : rps[0].rng_offset;
    uint8_t first_team = ordered[0]->team_id;
    bool all_same_team = true;
    for (auto* s : ordered) if (s->team_id != first_team) all_same_team = false;
    static const uint8_t perms[6][4] = {{0, 0, 1, 1}, {1, 1, 0, 0}, {0, 1, 1, 0}, {1, 0, 0, 1}, {0, 1, 0, 1}, {1, 0, 1, 0}};
    const uint8_t* team_assign = perms[rng_offset % 6];
    bool teams = g_last_search.mode == Matchmaking::TEAMS;
    for (auto* s : ordered) {
      if (!s->is_character_selected) continue;
      uint8_t team = teams ? s->team_id : 0;
      if (teams && all_same_team) team = team_assign[s->player_idx];
      block[0x60 + s->player_idx * 0x24] = s->character_id;
      block[0x63 + s->player_idx * 0x24] = s->character_color;
      block[0x67 + s->player_idx * 0x24] = 0;
      block[0x69 + s->player_idx * 0x24] = team;
    }
    std::unordered_map<uint16_t, uint8_t> color_counts;
    for (int i = 0; i < PLAYER_COUNT_MAX; ++i) {
      if (block[0x61 + i * 0x24] != 0) continue;
      uint8_t char_id = block[0x60 + i * 0x24], color = block[0x63 + i * 0x24], team = block[0x69 + i * 0x24];
      char_id = char_id == 0x13 ? 0x12 : char_id;
      uint16_t key = (uint16_t)((char_id << 8) | (teams ? team : color));
      uint8_t& count = color_counts[key];
      block[0x67 + 0x24 * i] = count;
      count += 1;
    }
    block[0x8] = teams ? 1 : 0;
    block[0x61 + 2 * 0x24] = remote_count >= 2 ? 0 : 3;
    block[0x61 + 3 * 0x24] = remote_count >= 3 ? 0 : 3;
    block[0xE] = (uint8_t)(stage_id >> 8); block[0xF] = (uint8_t)stage_id;
    bool pause_allowed = g_last_search.mode == Matchmaking::DIRECT;
    block[2] = pause_allowed ? (block[2] & 0xF7) : (block[2] | 0x8);
    bool stage_selection_mode = g_last_search.mode == Matchmaking::DIRECT || g_last_search.mode == Matchmaking::TEAMS;
    if (!stage_selection_mode) alt_stage_mode = 0;
    uint32_t secs = desync.state.seconds_remaining;
    block[0x10] = (uint8_t)(secs >> 24); block[0x11] = (uint8_t)(secs >> 16); block[0x12] = (uint8_t)(secs >> 8); block[0x13] = (uint8_t)secs;
    for (int i = 0; i < 4; ++i) {
      block[0x62 + i * 0x24] = desync.state.fighters[i].stocks_remaining;
      uint16_t hp = desync.state.fighters[i].current_health;
      block[0x70 + i * 0x24] = (uint8_t)(hp >> 8); block[0x71 + i * 0x24] = (uint8_t)hp;
    }
  }
  if (g_last_search.mode == Matchmaking::PARTY) {
    block[0x0] = 0x12; block[0x3] = 0xCC;
    uint32_t t = 5 * 60; block[0x10] = (uint8_t)(t >> 24); block[0x11] = (uint8_t)(t >> 16); block[0x12] = (uint8_t)(t >> 8); block[0x13] = (uint8_t)t;
  } else {
    block[0x0] = 0x32; block[0x3] = 0x4C;
    uint32_t t = 8 * 60; block[0x10] = (uint8_t)(t >> 24); block[0x11] = (uint8_t)(t >> 16); block[0x12] = (uint8_t)(t >> 8); block[0x13] = (uint8_t)t;
  }
  block[0xB] = 0xFF;
  uint64_t items = 0xF80000000F000000ull;
  if (g_recent_mm_result.items != 0) { items |= (uint64_t)g_recent_mm_result.items << 28; block[0xB] = 3; }
  for (int i = 0; i < 8; ++i) block[0x23 + i] = (uint8_t)(items >> (56 - 8 * i));
  append_u32(q, rng_offset);
  q.push_back((uint8_t)g_config.delay);
  q.push_back(sent_chat_message_id);
  q.push_back(chat_message_id);
  q.push_back(chat_message_player_idx);
  if (g_last_search.mode == Matchmaking::RANKED) {
    std::array<int8_t, 2> ranks = {0, 0};
    ranks[g_local_player_index & 1] = g_config.show_local_rank ? (int8_t)g_matchmaking->GetPlayerRank(g_local_player_index) : -1;
    ranks[g_remote_player_index & 1] = g_config.show_opponent_rank ? (int8_t)g_matchmaking->GetPlayerRank(g_remote_player_index) : -1;
    p1_rank = ranks[0]; p2_rank = ranks[1];
  }
  q.push_back((uint8_t)p1_rank);
  q.push_back((uint8_t)p2_rank);
  std::string ln = convert_string_for_game(local_name, 15);
  q.insert(q.end(), ln.begin(), ln.end());
  for (int i = 0; i < 4; ++i) { std::string n = convert_string_for_game(g_matchmaking->GetPlayerName((uint8_t)i), 15); q.insert(q.end(), n.begin(), n.end()); }
  std::vector<std::string> opponent_names;
  int team_idx = block[0x69 + g_local_player_index * 0x24];
  for (int i = 0; i < 4; ++i) {
    bool teams = g_last_search.mode == Matchmaking::TEAMS;
    bool same_team = block[0x69 + i * 0x24] == team_idx;
    bool human = block[0x61 + i * 0x24] == 0;
    if (g_local_player_index == i || !human || (same_team && teams)) continue;
    std::string n = g_matchmaking->GetPlayerName((uint8_t)i);
    if (!n.empty()) opponent_names.push_back(n);
  }
  size_t num_opp = opponent_names.empty() ? 1 : opponent_names.size();
  int chars_per_name = (int)((15 - (num_opp - 1)) / num_opp);
  std::string opp_text;
  for (auto& n : opponent_names) { if (!opp_text.empty()) opp_text += "/"; opp_text += truncate_length_char(n, chars_per_name); }
  opp_name = convert_string_for_game(opp_text, 15);
  q.insert(q.end(), opp_name.begin(), opp_name.end());
  auto players = g_matchmaking->GetPlayerInfo();
  for (int i = 0; i < 4; ++i) { std::string c = convert_connect_code_for_game(i < (int)players.size() ? players[i].connect_code : ""); q.insert(q.end(), c.begin(), c.end()); }
  for (int i = 0; i < 4; ++i) { std::string uid = i < (int)players.size() ? players[i].uid : ""; uid.resize(29); q.insert(q.end(), uid.begin(), uid.end()); }
  std::string err = convert_string_for_game(!g_forced_error.empty() ? g_forced_error : g_matchmaking->GetErrorMessage(), 120);
  q.insert(q.end(), err.begin(), err.end());
  q.insert(q.end(), block.begin(), block.end());
  std::string match_id = g_recent_mm_result.id;
  match_id.resize(51);
  q.insert(q.end(), match_id.begin(), match_id.end());
  q.push_back((uint8_t)alt_stage_mode);
}

void prepare_online_status(std::vector<uint8_t>& q) {
  q.clear();
  g_user->AttemptLogin();
  UserInfo me = g_user->GetUserInfo();
  uint8_t app_state = 0;
  if (g_user->IsLoggedIn()) app_state = 1;   // version check: this port speaks SLIPPI_SEMVER; the server enforces the rest
  q.push_back(app_state);
  std::string name = convert_string_for_game(me.display_name, 15);
  q.insert(q.end(), name.begin(), name.end());
  std::string code = convert_connect_code_for_game(me.connect_code);
  q.insert(q.end(), code.begin(), code.end());
}

void handle_report_game(const uint8_t* p) {
  // ReportGameQuery (packed, big-endian fields): mode, frameLength, gameIndex, tiebreakIndex, winnerIdx, gameEndMethod, lrasInitiator, syncedTimer, players[4], gameInfoBlock[312]
  uint8_t mode = p[0];
  uint32_t frames = be32(p + 1), game_index = be32(p + 5), tiebreak = be32(p + 9);
  int8_t winner = (int8_t)p[13]; uint8_t end_method = p[14]; int8_t lras = (int8_t)p[15];
  uint32_t synced_timer = be32(p + 16);
  const uint8_t* players = p + 20;
  const uint8_t* info_block = players + 4 * 9;
  int stage = be16(info_block + 0xE);
  host::log("slippi: game report: mode %u, %u frames, game %u, tiebreak %u, winner %d, end %u, lras %d, stage %d",
            mode, frames, game_index, tiebreak, winner, end_method, lras, stage);
  {
    // Exactly CEXISlippi::handleReportGame: one report per game with every slot's result.
    UserInfo me = g_user->GetUserInfo();
    report::GameReport r;
    r.uid = me.uid; r.play_key = me.play_key; r.match_id = g_recent_mm_result.id; r.replay_path = slippi::last_replay_path();
    r.online_mode = mode; r.duration_frames = frames; r.game_index = game_index; r.tiebreak_index = tiebreak;
    r.winner_index = winner; r.game_end_method = end_method; r.lras_initiator = lras; r.stage_id = stage;
    for (int i = 0; i < 4; ++i) {
      report::PlayerReport pr;
      pr.uid = g_recent_mm_result.players.size() > (size_t)i ? g_recent_mm_result.players[i].uid : "";
      pr.slot_type = players[i * 9]; pr.stocks_remaining = players[i * 9 + 1];
      uint32_t dmg = be32(players + i * 9 + 2); float dmg_f; std::memcpy(&dmg_f, &dmg, 4); pr.damage_done = dmg_f;
      pr.character_id = info_block[0x60 + 0x24 * i]; pr.color_id = info_block[0x63 + 0x24 * i];
      pr.starting_stocks = info_block[0x62 + 0x24 * i]; pr.starting_percent = be16(info_block + 0x70 + 0x24 * i);
      r.players.push_back(pr);
    }
    if (reporting_enabled()) report::log_game(r);
  }
  if (mode == Matchmaking::RANKED && end_method == 7 && g_netplay) {
    SyncedGameState s;
    s.match_id = g_recent_mm_result.id;
    s.game_index = game_index; s.tiebreak_index = tiebreak; s.seconds_remaining = synced_timer;
    for (int i = 0; i < 4; ++i) { s.fighters[i].stocks_remaining = players[i * 9 + 6]; s.fighters[i].current_health = be16(players + i * 9 + 7); }
    g_netplay->SendSyncedGameState(s);
  }
}

void handle_get_player_settings(std::vector<uint8_t>& q) {
  q.clear();
  std::vector<std::vector<std::string>> by_player(4);
  auto mine = g_user->GetUserChatMessages();
  if (mine.size() == 16) by_player[0] = mine;
  for (auto& p : g_matchmaking->GetPlayerInfo()) if (p.port >= 1 && p.port <= 4) by_player[p.port - 1] = p.chat_messages;
  for (int i = 0; i < 4; ++i) {
    if (by_player[i].size() != 16) by_player[i] = User::GetDefaultChatMessages();
    for (int j = 0; j < 16; ++j) {
      std::string s = convert_string_for_game(by_player[i][j], 25);
      s.resize(51);
      q.insert(q.end(), s.begin(), s.end());
    }
  }
}

}  // namespace

Config& config() { return g_config; }
bool available() { return true; }
uint64_t rollback_count() { return g_rollbacks; }
int32_t current_online_frame() { return g_current_online_frame; }
bool is_online_match() { return g_in_online_match; }

void init() {
  if (g_initialized) return;
  g_initialized = true;
  g_offline = g_config.offline;
  if (g_offline) {
    offline::reset(g_config);
    host::log("slippi: offline services (identity, matchmaking and reporting disabled)");
    return;
  }
  report::init(host::options.iso, g_config.user_dir);
  // Only the caller-owned directory is used. Never discover another installed
  // client's login or silently replace the configured profile on any platform.
  g_user = std::make_unique<User>(g_config.user_dir);
  if (g_user->IsLoggedIn()) if (reporting_enabled()) report::fetch_user_rank(g_user->GetUserInfo().uid);
  g_matchmaking = std::make_unique<Matchmaking>(g_user.get());
  g_direct_codes = std::make_unique<DirectCodes>(g_config.user_dir + "/direct-codes.json");
  g_teams_codes = std::make_unique<DirectCodes>(g_config.user_dir + "/teams-codes.json");
  g_local_selections.Reset();
  Savestate::force_init = true;
  if (!g_user->IsLoggedIn()) host::log("slippi: not logged in (no %s/user.json); log in with the Slippi Launcher and point --user-dir at its Slippi folder", g_config.user_dir.c_str());
}

void shutdown() {
  if (!g_initialized) return;
  g_initialized = false;
  if (g_offline) return;
  // Leaving during a ranked game counts as abandoning it (same as Dolphin).
  if (g_in_online_match && g_recent_mm_result.id.find("mode.ranked") != std::string::npos && g_user) {
    UserInfo me = g_user->GetUserInfo(); if (reporting_enabled()) report::match_status(me.uid, me.play_key, g_recent_mm_result.id, "abandoned", false);
  }
  report::shutdown();
  if (g_matchmaking) {
    std::string id = g_matchmaking->GetMatchmakeResult().id;
    if (id.find("mode.ranked") != std::string::npos) host::log("slippi: exit during ranked match %s", id.c_str());
  }
  g_netplay.reset();
  discord::set_menus();
  g_matchmaking.reset();
  g_active_savestates.clear();
  g_available_savestates.clear();
  g_user.reset();
  g_direct_codes.reset();
  g_teams_codes.reset();
  g_in_online_match = false;
  net_diag::match_ended("shutdown");
}

bool handle(uint8_t cmd, const uint8_t* payload, uint32_t payload_len, std::vector<uint8_t>& q) {
  if (!g_initialized) init();
  if (g_offline) return offline::handle(cmd, payload, payload_len, q);
  switch (cmd) {
    case CMD_ONLINE_INPUTS: handle_online_inputs(payload, q); return true;
    case CMD_CAPTURE_SAVESTATE: handle_capture_savestate(payload); return true;
    case CMD_LOAD_SAVESTATE: handle_load_savestate(payload); return true;
    case CMD_GET_MATCH_STATE: prepare_online_match_state(q); return true;
    case CMD_FIND_OPPONENT: start_find_match(payload); return true;
    case CMD_SET_MATCH_SELECTIONS: set_match_selections(payload); return true;
    case CMD_OPEN_LOGIN:
      if (!g_user->AttemptLogin()) host::log("slippi: login requested: sign in with the Slippi Launcher, then copy its user.json to %s", g_config.user_dir.c_str());
      return true;
    case CMD_LOGOUT: g_user->LogOut(); return true;
    case CMD_UPDATE: host::log("slippi: update requested by the game (ignored)"); return true;
    case CMD_GET_ONLINE_STATUS: prepare_online_status(q); return true;
    case CMD_CLEANUP_CONNECTION: cleanup_connection(); return true;
    case CMD_SEND_CHAT_MESSAGE: if (chat_enabled() && g_netplay) g_netplay->SendChatMessage(payload[0]); return true;
    case CMD_GET_NEW_SEED: q.clear(); append_u32(q, g_rng() % 0xFFFFFFFFu); return true;
    case CMD_REPORT_GAME: handle_report_game(payload); return true;
    case CMD_FETCH_CODE_SUGGESTION: handle_name_entry_load(payload, q); return true;
    case CMD_OVERWRITE_SELECTIONS: {
      g_overwrite_selections.clear();
      uint16_t stage = be16(payload);
      for (int i = 0; i < 4; ++i) {
        const uint8_t* c = payload + 2 + i * 3;
        if (!c[0]) continue;
        PlayerSelections s;
        s.is_character_selected = true; s.character_id = c[1]; s.character_color = c[2];
        s.is_stage_selected = true; s.stage_id = stage; s.player_idx = (uint8_t)i;
        g_overwrite_selections.push_back(s);
      }
      return true;
    }
    case CMD_GP_COMPLETE_STEP: {
      GamePrepStepResults r;
      r.step_idx = payload[0]; r.char_selection = payload[1]; r.char_color_selection = payload[2];
      r.stage_selections[0] = payload[3]; r.stage_selections[1] = payload[4];
      if (g_netplay) g_netplay->SendGamePrepStep(r);
      return true;
    }
    case CMD_GP_FETCH_STEP: {
      q.assign(6, 0);
      GamePrepStepResults r;
      if (g_netplay && g_netplay->GetGamePrepResults(payload[0], r)) {
        q[0] = 1; q[1] = 0; q[2] = r.char_selection; q[3] = r.char_color_selection; q[4] = r.stage_selections[0]; q[5] = r.stage_selections[1];
      }
      return true;
    }
    case CMD_REPORT_SET_COMPLETE: {
      host::log("slippi: set complete (end mode %u)", payload[0]);
      if (g_recent_mm_result.id.find("mode.ranked") != std::string::npos) {
        UserInfo me = g_user->GetUserInfo();
        if (reporting_enabled()) report::match_status(me.uid, me.play_key, g_recent_mm_result.id, payload[0] == 0 ? "normal_completion" : "abnormal_completion", true);
      }
      return true;
    }
    case CMD_REPORT_MATCH_STATUS_UPDATE: {
      if (g_recent_mm_result.id.find("mode.ranked") == std::string::npos) return true;
      static const std::map<uint8_t, const char*> status_names = {
          {1, "connecting"}, {10, "game_setup_1"}, {11, "game_setup_2"}, {12, "game_setup_3"}, {13, "game_setup_4"}, {14, "game_setup_5"},
          {15, "game_setup_6"}, {16, "game_setup_7"}, {20, "game_start_1"}, {21, "game_start_2"}, {22, "game_start_3"}, {23, "game_start_4"},
          {24, "game_start_5"}, {25, "game_start_6"}, {26, "game_start_7"}, {30, "normal_completion"}, {31, "abnormal_completion"}, {40, "abandoned"}};
      auto it = status_names.find(payload[0]);
      if (it == status_names.end()) { host::log("slippi: invalid match status index %u", payload[0]); return true; }
      UserInfo me = g_user->GetUserInfo();
      if (reporting_enabled()) report::match_status(me.uid, me.play_key, g_recent_mm_result.id, it->second, true);
      return true;
    }
    case CMD_GET_PLAYER_SETTINGS: handle_get_player_settings(q); return true;
    case CMD_GET_DELAY: q.clear(); q.push_back(1); q.push_back((uint8_t)g_config.delay); return true;
    case CMD_GET_RANK: {
      // CEXISlippi::handleGetRank: visibility, fetch status, rank, ordinal, update count, change, rank change.
      q.clear();
      q.push_back((uint8_t)((g_config.show_local_rank ? 1 : 0) | (g_config.show_opponent_rank ? 2 : 0)));
      report::RankInfo ri; auto st = report::rank_info(&ri);
      q.push_back((uint8_t)st);
      q.push_back((uint8_t)ri.rank);
      uint32_t ord, chg; std::memcpy(&ord, &ri.rating_ordinal, 4); std::memcpy(&chg, &ri.rating_change, 4);
      append_u32(q, ord); append_u32(q, ri.rating_update_count); append_u32(q, chg); q.push_back((uint8_t)ri.rank_change);
      return true;
    }
    case CMD_FETCH_RANK: { UserInfo me = g_user->GetUserInfo(); if (reporting_enabled()) report::fetch_match_result(g_recent_mm_result.id, me.uid, me.play_key); return true; }
    case CMD_GET_RANK_VISIBILITY: q.clear(); q.push_back((uint8_t)((g_config.show_local_rank ? 1 : 0) | (g_config.show_opponent_rank ? 2 : 0))); return true;
    default: return false;
  }
}

}  // namespace slippi::online
#endif
