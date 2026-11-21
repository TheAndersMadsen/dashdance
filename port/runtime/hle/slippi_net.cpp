// Slippi Online networking (see slippi_net.h). Wire-compatible port of Dolphin's
// SlippiNetplayClient, SlippiMatchmaking and the user record.
// SPDX-License-Identifier: GPL-2.0-or-later
#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#endif
#include "slippi_net.h"
#if !defined(MELEE_PORT_OFFLINE) || !MELEE_PORT_OFFLINE
#include "host.h"
#include "slippi_net_diag.h"
#if defined(_WIN32)
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif
#include <enet/enet.h>
#include <nlohmann/json.hpp>
#include "json_safe.h"
#include "slippi_matchmaking_protocol.h"
#include "slippi_ws_apple.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>

using json = nlohmann::json;

#ifdef __APPLE__
#include <sys/socket.h>
#endif
namespace {
// Apple Wi-Fi and cellular stacks map the socket's service type to a traffic class; voice
// (NET_SERVICE_TYPE_VO) gets the lowest-latency queue, which is what rollback input packets want.
void prioritize_socket(ENetSocket socket) {
#ifdef __APPLE__
  int service = NET_SERVICE_TYPE_VO;
  setsockopt(socket, SOL_SOCKET, SO_NET_SERVICE_TYPE, &service, sizeof service);
#else
  (void)socket;
#endif
}
}  // namespace
namespace slippi {

const char* const SLIPPI_SEMVER = "3.7.0";

uint64_t time_us() { return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
uint64_t time_ms() { return time_us() / 1000; }

// ---------------------------------------------------------------- ENet
static bool g_enet_ready = false;
bool enet_ready() {
  if (!g_enet_ready) { if (enet_initialize() < 0) { host::log("slippi: enet_initialize failed"); return false; } g_enet_ready = true; }
  return true;
}
static void wakeup_thread(ENetHost* host) {
  if (!host) return;
  ENetAddress address;
  if (host->address.port != 0) address.port = host->address.port;
  else enet_socket_get_address(host->socket, &address);
  address.host = 0x0100007f;
  uint8_t byte = 0;
  ENetBuffer buf; buf.data = &byte; buf.dataLength = 1;
  enet_socket_send(host->socket, &address, &buf, 1);
}
// MELEE_NET_DROP_CONNECT=1: test aid for one-way NATs. Incoming ENet CONNECT commands are discarded, so the opponent's
// outgoing attempt never completes and only this side's outgoing connection forms (docs/MAC_FIXES.md issue 9).
static int ENET_CALLBACK drop_connect_callback(ENetHost* host, ENetEvent*) {
  const enet_uint8* d = host->receivedData; const size_t n = host->receivedDataLength;
  if (n < 6) return 0;
  const enet_uint16 peer_id = (enet_uint16)(d[0] << 8 | d[1]);
  const size_t header = (peer_id & ENET_PROTOCOL_HEADER_FLAG_SENT_TIME) ? 4 : 2;
  return (d[header] & ENET_PROTOCOL_COMMAND_MASK) == ENET_PROTOCOL_COMMAND_CONNECT ? 1 : 0;   // 1 = swallow the datagram
}
static int ENET_CALLBACK intercept_callback(ENetHost* host, ENetEvent* event) {
  if (host->receivedDataLength == 1 && host->receivedData[0] == 0) { event->type = (ENetEventType)42; return 1; }
  static const bool drop_connects = std::getenv("MELEE_NET_DROP_CONNECT") != nullptr;
  return drop_connects ? drop_connect_callback(host, event) : 0;
}
// One line per ENet peer event. Every mid-game drop seen so far came 30 to 32 s after connecting, which is ENet's cap
// for a peer whose reliable commands were never acknowledged; these lines say which peer that was (the outgoing
// attempt or the opponent's incoming connection), whether it ever connected, and whether the opponent asked for it.
static void log_peer(const char* what, ENetHost* host, ENetPeer* p, bool in_use, unsigned data) {
  host::log("slippi: peer %s: slot %d %x:%u state %d ids in %u out %u connect %08x rtt %u ms lost %u last-recv %u ms ago data %u%s",
            what, (int)(p - host->peers), p->address.host, p->address.port, (int)p->state, p->incomingPeerID, p->outgoingPeerID, p->connectID,
            p->roundTripTime, p->packetsLost, p->lastReceiveTime ? host->serviceTime - p->lastReceiveTime : 0, data, in_use ? " (the connection in use)" : "");
}
static std::string peer_key(ENetPeer* p) { std::stringstream s; s << p->address.host << "-" << p->address.port; return s.str(); }

// ---------------------------------------------------------------- Packet
void Packet::append(const void* data, size_t size) { const uint8_t* p = (const uint8_t*)data; buf_.insert(buf_.end(), p, p + size); }
Packet& Packet::operator<<(uint8_t v) { buf_.push_back(v); return *this; }
Packet& Packet::operator<<(uint16_t v) { buf_.push_back((uint8_t)(v >> 8)); buf_.push_back((uint8_t)v); return *this; }
Packet& Packet::operator<<(uint32_t v) { for (int i = 3; i >= 0; --i) buf_.push_back((uint8_t)(v >> (8 * i))); return *this; }
Packet& Packet::operator<<(const std::string& s) { *this << (uint32_t)s.size(); append(s.data(), s.size()); return *this; }
bool Packet::check(size_t n) { if (!ok_ || pos_ + n > buf_.size()) { ok_ = false; return false; } return true; }
Packet& Packet::operator>>(uint8_t& v) { if (check(1)) v = buf_[pos_++]; return *this; }
Packet& Packet::operator>>(bool& v) { uint8_t b = 0; *this >> b; v = b != 0; return *this; }
Packet& Packet::operator>>(uint16_t& v) { if (check(2)) { v = (uint16_t)((buf_[pos_] << 8) | buf_[pos_ + 1]); pos_ += 2; } return *this; }
Packet& Packet::operator>>(uint32_t& v) {
  if (check(4)) { v = ((uint32_t)buf_[pos_] << 24) | ((uint32_t)buf_[pos_ + 1] << 16) | ((uint32_t)buf_[pos_ + 2] << 8) | buf_[pos_ + 3]; pos_ += 4; }
  return *this;
}
Packet& Packet::operator>>(int32_t& v) { uint32_t u = 0; *this >> u; v = (int32_t)u; return *this; }
Packet& Packet::operator>>(std::string& s) {
  uint32_t n = 0; *this >> n;
  if (check(n)) { s.assign((const char*)&buf_[pos_], n); pos_ += n; }
  return *this;
}

// ---------------------------------------------------------------- selections
void PlayerSelections::Merge(const PlayerSelections& s) {
  rng_offset = s.rng_offset;
  if (s.is_stage_selected) { stage_id = s.stage_id; is_stage_selected = true; alt_stage_mode = s.alt_stage_mode; }
  if (s.is_character_selected) { character_id = s.character_id; character_color = s.character_color; team_id = s.team_id; is_character_selected = true; }
}
void PlayerSelections::Reset() { character_id = character_color = team_id = 0; is_character_selected = false; stage_id = 0; is_stage_selected = false; rng_offset = 0; }

// ---------------------------------------------------------------- user
User::User(std::string user_dir) : dir_(std::move(user_dir)) { AttemptLogin(); }
bool User::AttemptLogin() {
  std::ifstream f(dir_ + "/user.json");
  if (!f) { logged_in_ = false; return false; }
  try {
    json j = json::parse(f);
    info_.uid = jget(j, "uid", "");
    info_.play_key = jget(j, "playKey", "");
    info_.display_name = jget(j, "displayName", "");
    info_.connect_code = jget(j, "connectCode", "");
    info_.latest_version = jget(j, "latestVersion", "");
    info_.chat_messages.clear();
    if (j.count("chatMessages") && j["chatMessages"].is_array()) for (auto& m : j["chatMessages"]) info_.chat_messages.push_back(m.get<std::string>());
    if (info_.chat_messages.size() != 16) info_.chat_messages = GetDefaultChatMessages();
    bool was = logged_in_;
    logged_in_ = !info_.uid.empty() && !info_.play_key.empty();
    if (logged_in_ && !was) host::log("slippi: logged in as %s (%s)", info_.display_name.c_str(), info_.connect_code.c_str());
  } catch (const std::exception& e) {
    host::log("slippi: cannot parse %s/user.json: %s", dir_.c_str(), e.what());
    logged_in_ = false;
  }
  return logged_in_;
}
void User::LogOut() { logged_in_ = false; info_ = UserInfo(); host::log("slippi: logged out (user.json left in place)"); }
std::vector<std::string> User::GetUserChatMessages() const { return info_.chat_messages.size() == 16 ? info_.chat_messages : GetDefaultChatMessages(); }
std::vector<std::string> User::GetDefaultChatMessages() {
  return {"ggs", "one more", "brb", "good luck", "well played", "that was fun", "thanks", "too good",
          "sorry", "my b", "lol", "wow", "gotta go", "one sec", "let's play again later", "bad connection"};
}

DirectCodes::DirectCodes(std::string path) : path_(std::move(path)) { Load(); }
void DirectCodes::Load() {
  codes_.clear();
  std::ifstream f(path_);
  if (!f) return;
  try {
    json j = json::parse(f);
    if (j.is_array()) for (auto& e : j) { std::string c = e.is_object() ? jget(e, "connectCode", "") : e.is_string() ? e.get<std::string>() : ""; if (!c.empty()) codes_.push_back(c); }
  } catch (...) {}
}
void DirectCodes::Save() {
  json j = json::array();
  for (auto& c : codes_) j.push_back({{"connectCode", c}, {"lastPlayed", ""}});
  std::ofstream f(path_);
  if (f) f << j.dump(2);
}
std::string DirectCodes::get(int index) const { return index >= 0 && index < (int)codes_.size() ? codes_[index] : "1"; }
void DirectCodes::AddOrUpdateCode(const std::string& code) {
  codes_.erase(std::remove(codes_.begin(), codes_.end(), code), codes_.end());
  codes_.insert(codes_.begin(), code);
  Save();
}

// ---------------------------------------------------------------- netplay client
NetplayClient::NetplayClient(std::vector<std::string> addrs, std::vector<uint16_t> ports, uint8_t remote_player_count, uint16_t local_port,
                             bool is_decider, uint8_t player_idx, const std::string& bind_address) {
  host::log("slippi: netplay client: local port %u, decider %d, player index %u, %u remote players", local_port, is_decider, player_idx, remote_player_count);
  if (remote_player_count == 0 || remote_player_count > REMOTE_PLAYER_MAX ||
      player_idx >= PLAYER_COUNT_MAX || addrs.size() != remote_player_count || ports.size() != remote_player_count) {
    host::log("slippi: invalid netplay peer configuration");
    status_.store(ConnectStatus::FAILED);
    return;
  }
  is_decider_ = is_decider;
  remote_player_count_ = remote_player_count;
  player_idx_ = player_idx;
  for (int i = 0, j = 0; i < REMOTE_PLAYER_MAX; ++i, ++j) {
    if (j == player_idx) ++j;
    match_info_.remote[i] = PlayerSelections();
    match_info_.remote[i].player_idx = (uint8_t)j;
    last_frame_acked_[i] = 0;
  }
  ENetAddress local_addr{};
  ENetAddress* local = nullptr;
  if (local_port > 0 || !bind_address.empty()) {
    local_addr.host = ENET_HOST_ANY;
    local_addr.port = local_port;
    // A numeric local bind address supports confined loopback validation without
    // DNS, a launcher profile, a matchmaker or a production-facing listener.
    if (!bind_address.empty()) {
      in_addr bind_ip{};
      if (inet_pton(AF_INET, bind_address.c_str(), &bind_ip) != 1) {
        host::log("slippi: invalid local bind address");
        status_.store(ConnectStatus::FAILED);
        return;
      }
      local_addr.host = bind_ip.s_addr;
    }
    local = &local_addr;
  }
  client_ = enet_host_create(local, 10, 3, 0, 0);
  if (client_ && std::getenv("MELEE_NET_DROP_CONNECT")) { client_->intercept = drop_connect_callback; host::log("slippi: test aid: incoming ENet connects are dropped"); }
  if (!client_) { host::log("slippi: cannot create ENet client"); status_.store(ConnectStatus::FAILED); return; }
  prioritize_socket(client_->socket);
  for (int i = 0; i < remote_player_count; ++i) {
    ENetAddress addr{};
    in_addr peer_ip{};
    const bool numeric = inet_pton(AF_INET, addrs[i].c_str(), &peer_ip) == 1;
    if (numeric) addr.host = peer_ip.s_addr;
    else if (enet_address_set_host(&addr, addrs[i].c_str()) < 0) {
      host::log("slippi: cannot resolve configured peer address");
      status_.store(ConnectStatus::FAILED);
      return;
    }
    addr.port = ports[i];
    ENetPeer* peer = enet_host_connect(client_, &addr, 3, 0);
    if (!peer) { host::log("slippi: cannot create configured peer"); status_.store(ConnectStatus::FAILED); return; }
    server_.push_back(peer);
    ActiveConnectionInfo info;
    info.player_idx = match_info_.remote[i].player_idx;
    active_connections_[peer_key(peer)][peer] = info;
    player_active_[info.player_idx].store(true, std::memory_order_release);
  }
  status_.store(ConnectStatus::INITIATED, std::memory_order_release);
  thread_ = std::thread(&NetplayClient::ThreadFunc, this);
}

NetplayClient::~NetplayClient() {
  do_loop_.store(false);
  if (client_) wakeup_thread(client_);
  if (thread_.joinable()) thread_.join();
  if (!server_.empty()) Disconnect();
  if (client_) { enet_host_destroy(client_); client_ = nullptr; }
  host::log("slippi: netplay client cleanup complete");
}

void NetplayClient::OnData(Packet& packet, ENetPeer* peer) {
  uint8_t mid = 0;
  if (!(packet >> mid)) { host::log("slippi: empty netplay packet"); return; }
  switch (mid) {
    case NP_MSG_SLIPPI_PAD: {
      uint64_t cur_time = time_us();
      int32_t frame, checksum_frame; uint32_t checksum; uint8_t packet_player_port;
      if (!(packet >> frame) || !(packet >> packet_player_port) || !(packet >> checksum_frame) || !(packet >> checksum)) { host::log("slippi: pad packet too small"); break; }
      uint8_t pidx = PlayerIdxFromPort(packet_player_port);
      if (pidx >= remote_player_count_) { host::log("slippi: pad packet with invalid player idx %u", pidx); break; }
      const int pad_data_offset = 14;
      int conn_idx = 0;
      for (int i = 0; i < (int)server_.size(); ++i)
        if (peer->address.host == server_[i]->address.host && peer->address.port == server_[i]->address.port) { conn_idx = i; break; }
      std::string key = peer_key(peer);
      int live = 0; bool current_active = false;
      for (auto& c : active_connections_[key]) { if (c.second.is_disconnected) continue; if (c.first == peer) current_active = true; ++live; }
      if (current_active && live > 1 && player_idx_ < packet_player_port) {
        server_[conn_idx] = peer;
        for (auto& c : active_connections_[key]) {
          if (c.first == peer || c.second.is_disconnected) continue;
          log_peer("closing the duplicate of the connection in use", client_, c.first, false, 0);
          enet_peer_disconnect(c.first, 0);
          c.second.is_disconnected = true;
        }
      }
      FrameTiming timing = last_frame_timing_[pidx];
      if (!has_game_started_) { timing.frame = 0; timing.time_us = cur_time; }
      int64_t opponent_send_time_us = (int64_t)cur_time - (int64_t)(ping_us_[pidx] / 2);
      int64_t frame_diff_offset_us = 16683 * (int64_t)(timing.frame - frame);
      int64_t time_offset_us = opponent_send_time_us - (int64_t)timing.time_us + frame_diff_offset_us;
      auto& fod = frame_offset_data_[pidx];
      if ((int)fod.buf.size() < ONLINE_LOCKSTEP_INTERVAL) fod.buf.push_back((int32_t)time_offset_us);
      else fod.buf[fod.idx] = (int32_t)time_offset_us;
      fod.idx = (fod.idx + 1) % ONLINE_LOCKSTEP_INTERVAL;
      int64_t inputs_to_copy;
      {
        std::lock_guard<std::mutex> lk(pad_mutex_);
        const uint8_t* data = packet.data();
        int32_t head_frame = remote_pad_queue_[pidx].empty() ? 0 : remote_pad_queue_[pidx].front()->frame;
        inputs_to_copy = (int64_t)frame - head_frame;
        if (pad_data_offset + inputs_to_copy * PAD_DATA_SIZE > (int64_t)packet.size()) { host::log("slippi: pad packet too small for %lld inputs", (long long)inputs_to_copy); break; }
        if (inputs_to_copy > 128) { host::log("slippi: pad packet with too many frames (%lld)", (long long)inputs_to_copy); break; }
        for (int64_t i = inputs_to_copy - 1; i >= 0; --i)
          remote_pad_queue_[pidx].push_front(std::make_unique<Pad>((int32_t)(frame - i), &data[pad_data_offset + i * PAD_DATA_SIZE]));
        remote_checksums_[pidx] = {checksum_frame, checksum};
      }
      if (inputs_to_copy > 0) {
        Packet ack;
        ack << (uint8_t)NP_MSG_SLIPPI_PAD_ACK << frame << player_idx_;
        ENetPacket* epac = enet_packet_create(ack.data(), ack.size(), ENET_PACKET_FLAG_UNSEQUENCED);
        enet_peer_send(peer, 2, epac);
        net_diag::on_pad_received(time_us(), (int)inputs_to_copy);
      }
      break;
    }
    case NP_MSG_SLIPPI_PAD_ACK: {
      std::lock_guard<std::mutex> lk(ack_mutex_);
      int32_t frame; uint8_t packet_player_port;
      if (!(packet >> frame) || !(packet >> packet_player_port)) { host::log("slippi: ack packet too small"); break; }
      uint8_t pidx = PlayerIdxFromPort(packet_player_port);
      if (pidx >= remote_player_count_) break;
      last_frame_acked_[pidx] = std::max(last_frame_acked_[pidx], frame);
      auto& timers = ack_timers_[pidx];
      while (!timers.empty() && timers.front().frame < frame) timers.pop_front();
      if (timers.empty() || timers.front().frame != frame) break;
      uint64_t send_time = timers.front().time_us;
      timers.pop_front();
      ping_us_[pidx] = time_us() - send_time;
      net_diag::on_ack(ping_us_[pidx]);
      ping_sample_sum_us_.fetch_add(ping_us_[pidx], std::memory_order_relaxed);
      ping_sample_count_.fetch_add(1, std::memory_order_relaxed);
      if (pidx == 0) host::set_online_ping_ms((int)(ping_us_[0] / 1000));
      if (frame % 600 == 0 && pidx == 0) host::log("slippi: ping %llu ms", (unsigned long long)(ping_us_[0] / 1000));
      break;
    }
    case NP_MSG_SLIPPI_MATCH_SELECTIONS: {
      auto s = ReadSelections(packet);
      if (!s->error) {
        uint8_t idx = PlayerIdxFromPort(s->player_idx);
        if (idx >= remote_player_count_) break;
        host::log("slippi: received selections from player %u (char %u color %u stage %u)", s->player_idx, s->character_id, s->character_color, s->stage_id);
        match_info_.remote[idx].Merge(*s);
        has_game_started_ = false;
        remote_pad_queue_[idx].clear();
      }
      break;
    }
    case NP_MSG_SLIPPI_CHAT_MESSAGE: {
      auto s = ReadChatMessage(packet);
      if (!s->error) remote_chat_message_selection_ = std::move(s);
      break;
    }
    case NP_MSG_SLIPPI_CONN_SELECTED: break;
    case NP_MSG_SLIPPI_COMPLETE_STEP: {
      GamePrepStepResults r;
      packet >> r.step_idx >> r.char_selection >> r.char_color_selection >> r.stage_selections[0] >> r.stage_selections[1];
      game_prep_step_queue_.push_back(r);
      break;
    }
    case NP_MSG_SLIPPI_SYNCED_STATE: {
      uint8_t packet_player_port;
      if (!(packet >> packet_player_port)) break;
      uint8_t pidx = PlayerIdxFromPort(packet_player_port);
      if (pidx >= remote_player_count_) break;
      SyncedGameState r;
      packet >> r.match_id >> r.game_index >> r.tiebreak_index >> r.seconds_remaining;
      for (int i = 0; i < 4; ++i) packet >> r.fighters[i].stocks_remaining >> r.fighters[i].current_health;
      remote_sync_states_[pidx] = r;
      break;
    }
    default: host::log("slippi: unknown netplay message %u", mid); break;
  }
}

void NetplayClient::WriteSelections(Packet& p, const PlayerSelections& s) {
  p << (uint8_t)NP_MSG_SLIPPI_MATCH_SELECTIONS << s.character_id << s.character_color << s.is_character_selected << s.player_idx
    << s.stage_id << s.is_stage_selected << s.rng_offset << s.team_id << s.alt_stage_mode;
}
std::unique_ptr<PlayerSelections> NetplayClient::ReadSelections(Packet& p) {
  auto s = std::make_unique<PlayerSelections>();
  if (!(p >> s->character_id >> s->character_color >> s->is_character_selected >> s->player_idx >> s->stage_id >> s->is_stage_selected
        >> s->rng_offset >> s->team_id >> s->alt_stage_mode)) { host::log("slippi: invalid selection packet"); s->error = true; }
  return s;
}
std::unique_ptr<PlayerSelections> NetplayClient::ReadChatMessage(Packet& p) {
  auto s = std::make_unique<PlayerSelections>();
  if (!(p >> s->message_id) || !(p >> s->player_idx)) { s->error = true; return s; }
  static const int allowed[] = {136, 129, 130, 132, 34, 40, 33, 36, 72, 66, 68, 65, 24, 18, 20, 17, CHAT_MSG_CHAT_DISABLED};
  bool ok = false;
  for (int a : allowed) if (a == s->message_id) ok = true;
  if (!ok) { host::log("slippi: invalid chat message %d", s->message_id); s->error = true; }
  return s;
}

void NetplayClient::Send(Packet& packet) {
  for (size_t i = 0; i < server_.size(); ++i) {
    auto conn = active_connections_.find(peer_key(server_[i]));
    if (conn != active_connections_.end()) {
      auto p = conn->second.find(server_[i]);
      if (p != conn->second.end() && p->second.is_disconnected) continue;
    }
    uint8_t mid = packet.data()[0];
    enet_uint32 flags = ENET_PACKET_FLAG_RELIABLE;
    uint8_t channel = 0;
    if (mid == NP_MSG_SLIPPI_PAD || mid == NP_MSG_SLIPPI_PAD_ACK) { flags = ENET_PACKET_FLAG_UNSEQUENCED; channel = 1; }
    ENetPacket* epac = enet_packet_create(packet.data(), packet.size(), flags);
    enet_peer_send(server_[i], channel, epac);
  }
}

void NetplayClient::Disconnect() {
  status_.store(ConnectStatus::DISCONNECTED, std::memory_order_release);
  if (active_connections_.empty()) return;
  for (auto& conn : active_connections_)
    for (auto& peer : conn.second) enet_peer_disconnect(peer.first, pending_disconnect_reason_.load(std::memory_order_acquire));
  ENetEvent ev;
  while (enet_host_service(client_, &ev, 3000) > 0) {
    if (ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
    else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) host::log("slippi: got disconnect from peer %u", ev.peer->address.port);
  }
  for (auto& conn : active_connections_) for (auto& peer : conn.second) enet_peer_reset(peer.first);
  active_connections_.clear();
  for (auto& a : player_active_) a.store(false, std::memory_order_release);
  server_.clear();
}

void NetplayClient::SendAsync(std::unique_ptr<Packet> packet) {
  if (status_.load(std::memory_order_acquire) == ConnectStatus::DISCONNECTED) return;
  { std::lock_guard<std::mutex> lk(async_mutex_); async_queue_.push_back(std::move(packet)); }
  wakeup_thread(client_);
}

void NetplayClient::ThreadFunc() {
#if defined(__APPLE__)
  // Opponent inputs are handled the moment they arrive: keep this thread on performance cores (an efficiency core can
  // take milliseconds to wake on an iPhone), whatever thread happened to create it.
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
  uint64_t start_time = time_ms();
  const uint64_t timeout = 8000;
  std::vector<bool> connections(remote_player_count_, false);
  std::vector<ENetAddress> remote_addrs;
  for (int i = 0; i < remote_player_count_; ++i) remote_addrs.push_back(server_[i]->address);

  while (status_.load(std::memory_order_acquire) == ConnectStatus::INITIATED) {
    ENetEvent ev;
    int net = enet_host_service(client_, &ev, 500);
    if (net > 0) {
      switch (ev.type) {
        case ENET_EVENT_TYPE_RECEIVE: {
          if (!ev.peer) break;
          Packet rpac(ev.packet->data, ev.packet->dataLength);
          OnData(rpac, ev.peer);
          enet_packet_destroy(ev.packet);
          break;
        }
        case ENET_EVENT_TYPE_DISCONNECT:
          if (ev.peer) log_peer("dropped while connecting", client_, ev.peer, false, ev.data);
          break;
        case ENET_EVENT_TYPE_CONNECT: {
          if (!ev.peer) break;
          log_peer("connected", client_, ev.peer, false, ev.data);
          int early_idx = 0;
          for (int i = 0; i < (int)remote_addrs.size(); ++i)
            if (remote_addrs[i].host == ev.peer->address.host && remote_addrs[i].port == ev.peer->address.port) { early_idx = i; break; }
          ActiveConnectionInfo info;
          info.player_idx = match_info_.remote[early_idx].player_idx;
          active_connections_[peer_key(ev.peer)][ev.peer] = info;
          player_active_[info.player_idx].store(true, std::memory_order_release);
          bool already = false;
          for (size_t i = 0; i < server_.size(); ++i)
            if (connections[i] && ev.peer->address.host == server_[i]->address.host && ev.peer->address.port == server_[i]->address.port) { server_[i] = ev.peer; already = true; break; }
          if (already) break;
          for (size_t i = 0; i < server_.size(); ++i) {
            if (remote_addrs[i].host == ev.peer->address.host && !connections[i]) {
              host::log("slippi: connected to %x:%u", ev.peer->address.host, ev.peer->address.port);
              server_[i] = ev.peer;
              connections[i] = true;
              break;
            }
          }
          break;
        }
        default: break;
      }
    }
    bool all = true;
    for (int i = 0; i < remote_player_count_; ++i) if (!connections[i]) all = false;
    if (all) {
      client_->intercept = intercept_callback;
      host::log("slippi: online connection successful");
      status_.store(ConnectStatus::CONNECTED, std::memory_order_release);
      break;
    }
    if (time_ms() - start_time >= timeout || !do_loop_.load()) {
      for (int i = 0; i < remote_player_count_; ++i) if (!connections[i]) failed_connections_.push_back(i);
      status_.store(ConnectStatus::FAILED, std::memory_order_release);
      host::log("slippi: online connection failed");
      return;
    }
  }

  while (do_loop_.load()) {
    if (status_.load(std::memory_order_acquire) == ConnectStatus::DISCONNECTED) break;
    for (auto& conn : active_connections_) {
      for (auto& peer : conn.second) {
        if (peer.second.is_disconnected || player_active_[peer.second.player_idx].load(std::memory_order_acquire)) continue;
        enet_peer_disconnect(peer.first, pending_disconnect_reason_.load(std::memory_order_acquire));
        peer.second.is_disconnected = true;
      }
    }
    ENetEvent ev;
    const uint64_t service_t0 = time_us();
    int net = enet_host_service(client_, &ev, 250);
    net_diag::on_service(time_us() - service_t0);
    for (;;) {
      std::unique_ptr<Packet> p;
      { std::lock_guard<std::mutex> lk(async_mutex_); if (async_queue_.empty()) break; p = std::move(async_queue_.front()); async_queue_.pop_front(); }
      Send(*p);
      net_diag::on_queue_depth(async_queue_.size());
    }
    net_diag::maybe_sample(time_us());
    {   // every 5 s: is each peer's reliable channel being acknowledged? (issue 9: the connection in use times out at 30 s)
      static uint64_t last_health_ms = 0;
      const uint64_t now_ms = time_ms();
      if (now_ms - last_health_ms >= 5000) {
        last_health_ms = now_ms;
        for (size_t i = 0; i < client_->peerCount; ++i) {
          ENetPeer* p = &client_->peers[i];
          if (p->state == ENET_PEER_STATE_DISCONNECTED) continue;
          bool in_use = false; for (auto* sp : server_) if (sp == p) in_use = true;
          host::log("slippi: peer health: slot %zu state %d rtt %u ms lost %u unacked reliable %zu (oldest %u ms) queued reliable %zu last-recv %u ms ago%s",
                    i, (int)p->state, p->roundTripTime, p->packetsLost, enet_list_size(&p->sentReliableCommands),
                    p->earliestTimeout ? client_->serviceTime - p->earliestTimeout : 0, enet_list_size(&p->outgoingReliableCommands),
                    p->lastReceiveTime ? client_->serviceTime - p->lastReceiveTime : 0, in_use ? " (in use)" : "");
        }
      }
    }
    if (net <= 0) continue;
    switch (ev.type) {
      case ENET_EVENT_TYPE_RECEIVE: {
        Packet rpac(ev.packet->data, ev.packet->dataLength);
        OnData(rpac, ev.peer);
        enet_packet_destroy(ev.packet);
        break;
      }
      case ENET_EVENT_TYPE_DISCONNECT: {
        std::string key = peer_key(ev.peer);
        { bool in_use = false; for (auto* sp : server_) if (sp == ev.peer) in_use = true; log_peer(ev.data ? "disconnected by the opponent" : "timed out or closed", client_, ev.peer, in_use, ev.data); }
        if (active_connections_.count(key) && active_connections_[key].count(ev.peer)) active_connections_[key][ev.peer].is_disconnected = true;
        // A player is gone when no connected peer of theirs is left under any address, not when the peers under this
        // one address are. Both sides dial each other, and the opponent's real port can differ from the one matchmaking
        // advertised (their router remaps it): our own attempt to the advertised port then sits alone under its own
        // address, never connects, and expires at ENet's 30 s cap. Judged by address, that expiry marked the player
        // inactive and the loop above closed the healthy connection 30 s into the game (docs/MAC_FIXES.md issue 9).
        bool all_peers_gone = AreAllPeersDisconnectedForKey(key);
        if (all_peers_gone && active_connections_.count(key) && active_connections_[key].count(ev.peer)) {
          const uint8_t player = active_connections_[key][ev.peer].player_idx;
          for (auto& conn : active_connections_)
            for (auto& other : conn.second)
              if (other.second.player_idx == player && !other.second.is_disconnected && other.first->state == ENET_PEER_STATE_CONNECTED) all_peers_gone = false;
          if (!all_peers_gone) host::log("slippi: a connection attempt to %x:%u expired; the player is still connected on another address", ev.peer->address.host, ev.peer->address.port);
        }
        if (all_peers_gone && active_connections_.count(key) && active_connections_[key].count(ev.peer))
          player_active_[active_connections_[key][ev.peer].player_idx].store(false, std::memory_order_release);
        bool connected_client = false;
        for (size_t i = 0; i < server_.size(); ++i)
          if (ev.peer->address.host == server_[i]->address.host && ev.peer->address.port == server_[i]->address.port) connected_client = true;
        if (connected_client && ev.data != 0) disconnect_reason_.store(ev.data, std::memory_order_release);
        host::log("slippi: disconnect from %x:%u (all peers gone: %d, connected client: %d)", ev.peer->address.host, ev.peer->address.port, all_peers_gone, connected_client);
        if (connected_client && all_peers_gone && AreAllConnectionsDisconnected()) do_loop_.store(false);
        break;
      }
      case ENET_EVENT_TYPE_CONNECT: {
        log_peer("connected late", client_, ev.peer, false, ev.data);
        int late_idx = 0;
        for (int i = 0; i < (int)server_.size(); ++i)
          if (server_[i]->address.host == ev.peer->address.host && server_[i]->address.port == ev.peer->address.port) { late_idx = i; break; }
        ActiveConnectionInfo info;
        info.player_idx = match_info_.remote[late_idx].player_idx;
        active_connections_[peer_key(ev.peer)][ev.peer] = info;
        player_active_[info.player_idx].store(true, std::memory_order_release);
        break;
      }
      default: break;
    }
  }
  Disconnect();
}

void NetplayClient::StartSlippiGame() {
  has_game_started_ = false;
  local_pad_queue_.clear();
  for (int i = 0; i < remote_player_count_; ++i) {
    last_frame_timing_[i] = {0, time_us()};
    last_frame_acked_[i] = 0;
    ack_timers_[i].clear();
  }
  is_desync_recovery_ = false;
  game_prep_step_queue_.clear();
  match_info_.Reset();
}

void NetplayClient::SendSlippiPad(std::unique_ptr<Pad> pad) {
  ConnectStatus st = status_.load(std::memory_order_acquire);
  if (st == ConnectStatus::FAILED || st == ConnectStatus::DISCONNECTED) return;
  if (pad) {
    local_pad_queue_.push_front(std::move(pad));
    net_diag::on_pad_sent(time_us());
  }
  int min_ack = INT_MAX;
  for (int i = 0; i < remote_player_count_; ++i) {
    if (!player_active_[match_info_.remote[i].player_idx].load(std::memory_order_acquire)) continue;
    min_ack = std::min(min_ack, last_frame_acked_[i]);
  }
  if (!local_pad_queue_.empty()) min_ack = std::max(min_ack, local_pad_queue_.front()->frame - 128);
  while (!local_pad_queue_.empty() && local_pad_queue_.back()->frame < min_ack) local_pad_queue_.pop_back();
  if (local_pad_queue_.empty()) return;
  int32_t frame = local_pad_queue_.front()->frame;
  auto spac = std::make_unique<Packet>();
  *spac << (uint8_t)NP_MSG_SLIPPI_PAD << frame << player_idx_ << local_pad_queue_.front()->checksum_frame << local_pad_queue_.front()->checksum;
  for (auto& p : local_pad_queue_) spac->append(p->buf, PAD_DATA_SIZE);
  SendAsync(std::move(spac));
  uint64_t t = time_us();
  has_game_started_ = true;
  for (int i = 0; i < remote_player_count_; ++i) {
    last_frame_timing_[i] = {frame, t};
    std::lock_guard<std::mutex> lk(ack_mutex_);
    ack_timers_[i].push_back({frame, t});
  }
}

void NetplayClient::SetMatchSelections(PlayerSelections& s) {
  match_info_.local.Merge(s);
  match_info_.local.player_idx = player_idx_;
  auto spac = std::make_unique<Packet>();
  WriteSelections(*spac, match_info_.local);
  SendAsync(std::move(spac));
}
void NetplayClient::SendGamePrepStep(const GamePrepStepResults& s) {
  auto spac = std::make_unique<Packet>();
  *spac << (uint8_t)NP_MSG_SLIPPI_COMPLETE_STEP << s.step_idx << s.char_selection << s.char_color_selection << s.stage_selections[0] << s.stage_selections[1];
  SendAsync(std::move(spac));
}
void NetplayClient::SendSyncedGameState(const SyncedGameState& s) {
  is_desync_recovery_ = true;
  local_sync_state_ = s;
  auto spac = std::make_unique<Packet>();
  *spac << (uint8_t)NP_MSG_SLIPPI_SYNCED_STATE << player_idx_ << s.match_id << s.game_index << s.tiebreak_index << s.seconds_remaining;
  for (int i = 0; i < 4; ++i) *spac << s.fighters[i].stocks_remaining << s.fighters[i].current_health;
  SendAsync(std::move(spac));
}
bool NetplayClient::GetGamePrepResults(uint8_t step_idx, GamePrepStepResults& res) {
  while (!game_prep_step_queue_.empty()) {
    if (game_prep_step_queue_.front().step_idx == step_idx) { res = game_prep_step_queue_.front(); return true; }
    game_prep_step_queue_.pop_front();
  }
  return false;
}
void NetplayClient::SendChatMessage(int message_id) {
  remote_sent_chat_message_id = (uint8_t)message_id;
  auto spac = std::make_unique<Packet>();
  *spac << (uint8_t)NP_MSG_SLIPPI_CHAT_MESSAGE << message_id << player_idx_;
  SendAsync(std::move(spac));
}
PlayerSelections NetplayClient::GetSlippiRemoteChatMessage(bool chat_enabled) {
  PlayerSelections copied;
  if (remote_chat_message_selection_ && chat_enabled) {
    copied.message_id = remote_chat_message_selection_->message_id;
    copied.player_idx = remote_chat_message_selection_->player_idx;
    remote_chat_message_selection_->message_id = 0;
    remote_chat_message_selection_->player_idx = 0;
  } else {
    copied.message_id = 0; copied.player_idx = 0;
    if (remote_chat_message_selection_ && !chat_enabled && remote_chat_message_selection_->message_id > 0 &&
        remote_chat_message_selection_->message_id != CHAT_MSG_CHAT_DISABLED) {
      auto spac = std::make_unique<Packet>();
      *spac << (uint8_t)NP_MSG_SLIPPI_CHAT_MESSAGE << (int)CHAT_MSG_CHAT_DISABLED << player_idx_;
      SendAsync(std::move(spac));
      remote_sent_chat_message_id = 0;
      remote_chat_message_selection_ = nullptr;
    }
  }
  return copied;
}
uint8_t NetplayClient::GetSlippiRemoteSentChatMessage(bool chat_enabled) {
  if (!chat_enabled) return 0;
  uint8_t id = remote_sent_chat_message_id;
  remote_sent_chat_message_id = 0;
  return id;
}

std::unique_ptr<RemotePadOutput> NetplayClient::GetSlippiRemotePad(int index, int max_frame_count) {
  std::lock_guard<std::mutex> lk(pad_mutex_);
  auto out = std::make_unique<RemotePadOutput>();
  if (remote_pad_queue_[index].empty()) {
    Pad empty(0);
    out->latest_frame = 0;
    out->data.insert(out->data.end(), empty.buf, empty.buf + PAD_FULL_SIZE);
    return out;
  }
  out->latest_frame = 0;
  out->checksum_frame = remote_checksums_[index].frame;
  out->checksum = remote_checksums_[index].value;
  out->player_idx = (uint8_t)(index >= player_idx_ ? index + 1 : index);
  out->is_disconnected = !player_active_[out->player_idx].load(std::memory_order_acquire);
  int count = 0;
  for (auto it = remote_pad_queue_[index].rbegin(); it != remote_pad_queue_[index].rend(); ++it) {
    out->latest_frame = std::max(out->latest_frame, (*it)->frame);
    out->data.insert(out->data.begin(), (*it)->buf, (*it)->buf + PAD_FULL_SIZE);
    if (++count >= max_frame_count) break;
  }
  return out;
}

void NetplayClient::DropOldRemoteInputs(int32_t finalized_frame) {
  std::lock_guard<std::mutex> lk(pad_mutex_);
  for (int i = 0; i < remote_player_count_; ++i)
    while (remote_pad_queue_[i].size() > 1 && remote_pad_queue_[i].back()->frame < finalized_frame) remote_pad_queue_[i].pop_back();
}
std::unordered_map<uint8_t, bool> NetplayClient::GetActivePlayerIndices() const {
  std::unordered_map<uint8_t, bool> r;
  for (uint8_t i = 0; i < PLAYER_COUNT_MAX; ++i) if (player_active_[i].load(std::memory_order_acquire)) r[i] = true;
  return r;
}
void NetplayClient::ForceDisconnectPlayer(uint8_t player_idx) {
  if (player_idx >= PLAYER_COUNT_MAX) return;
  player_active_[player_idx].store(false, std::memory_order_release);
  bool any = false;
  for (uint8_t i = 0; i < remote_player_count_; ++i) if (player_active_[match_info_.remote[i].player_idx].load(std::memory_order_acquire)) any = true;
  if (!any && status_.load(std::memory_order_acquire) == ConnectStatus::CONNECTED) {
    host::log("slippi: all remote players force-disconnected");
    status_.store(ConnectStatus::DISCONNECTED, std::memory_order_release);
  }
  if (client_) wakeup_thread(client_);
}
void NetplayClient::ForceDisconnect(DisconnectReason reason) {
  pending_disconnect_reason_.store((uint32_t)reason, std::memory_order_release);
  disconnect_reason_.store((uint32_t)reason, std::memory_order_release);
  for (uint8_t i = 0; i < remote_player_count_; ++i) ForceDisconnectPlayer(match_info_.remote[i].player_idx);
}
bool NetplayClient::AreAllPeersDisconnectedForKey(const std::string& key) {
  if (!active_connections_.count(key)) return true;
  for (auto& p : active_connections_[key]) if (!p.second.is_disconnected) return false;
  return true;
}
bool NetplayClient::AreAllConnectionsDisconnected() {
  for (auto& c : active_connections_) for (auto& p : c.second) if (!p.second.is_disconnected) return false;
  return true;
}
double NetplayClient::GetAndResetAvgPingMs() {
  uint64_t sum = ping_sample_sum_us_.exchange(0, std::memory_order_relaxed);
  uint64_t count = ping_sample_count_.exchange(0, std::memory_order_relaxed);
  return count ? (double)sum / (double)count / 1000.0 : 0.0;
}
int32_t NetplayClient::CalcTimeOffsetUs() {
  std::vector<int> offsets;
  for (int i = 0; i < remote_player_count_; ++i) {
    if (!player_active_[match_info_.remote[i].player_idx].load(std::memory_order_acquire)) continue;
    if (frame_offset_data_[i].buf.empty()) continue;
    std::vector<int32_t> buf = frame_offset_data_[i].buf;
    std::sort(buf.begin(), buf.end());
    int n = (int)buf.size(), off = (int)((1.0f / 3.0f) * n), end = n - off;
    int sum = 0;
    for (int k = off; k < end; ++k) sum += buf[k];
    int count = end - off;
    if (count <= 0) return 0;
    offsets.push_back(sum / count);
  }
  if (offsets.empty()) return 0;
  return *std::min_element(offsets.begin(), offsets.end());
}
bool NetplayClient::IsWaitingForDesyncRecovery() {
  if (!is_desync_recovery_) return false;
  for (int i = 0; i < remote_player_count_; ++i)
    if (local_sync_state_.game_index != remote_sync_states_[i].game_index || local_sync_state_.tiebreak_index != remote_sync_states_[i].tiebreak_index) return true;
  return false;
}
DesyncRecoveryResp NetplayClient::GetDesyncRecoveryState() {
  DesyncRecoveryResp r;
  r.is_recovering = is_desync_recovery_;
  r.is_waiting = IsWaitingForDesyncRecovery();
  if (!r.is_recovering || r.is_waiting) return r;
  r.state = local_sync_state_;
  for (int i = 0; i < remote_player_count_; ++i) {
    auto& s = remote_sync_states_[i];
    if (std::abs((int)r.state.seconds_remaining - (int)s.seconds_remaining) > 1) { r.is_error = true; return r; }
    if (s.seconds_remaining > r.state.seconds_remaining) r.state.seconds_remaining = s.seconds_remaining;
    for (int j = 0; j < 4; ++j) {
      auto& f = r.state.fighters[i]; auto& g = s.fighters[i];   // (index i, as in Dolphin)
      if (f.stocks_remaining != g.stocks_remaining) { r.is_error = true; return r; }
      if (std::abs((int)f.current_health - (int)g.current_health) > 25) { r.is_error = true; return r; }
      if (g.current_health < f.current_health) f.current_health = g.current_health;
    }
  }
  return r;
}

// ---------------------------------------------------------------- matchmaking
#if !defined(__APPLE__)
// The websocket transport is Apple-only (slippi_ws_apple.mm); elsewhere every connect fails cleanly.
struct ws::Connection::State {};
ws::Connection::Connection(std::shared_ptr<State> state) : state_(std::move(state)) {}
ws::Connection::~Connection() = default;
std::unique_ptr<ws::Connection> ws::Connection::open(const std::string&, int, int, const std::atomic<bool>*, std::string* error) {
  if (error) *error = "websocket transport not available on this platform";
  return nullptr;
}
bool ws::Connection::send_text(const std::string&, std::string*) { return false; }
ws::Event ws::Connection::receive(std::string*, int, std::string*) { return ws::Event::FAILED; }
void ws::Connection::close() {}
#endif
// Slippi 3.7.0 matchmaking. The conversation with the service runs over a websocket on a worker
// of its own (MmSession, a port of slippi-rust-extensions matchmaking/src/client.rs); this class
// owns the UDP socket that will be used for netplay, learns its public address through STUN,
// keeps its NAT mapping alive while queued, and connects to the opponents once a match arrives.
namespace {
constexpr const char* MM_PROD_URL = "wss://matchmaking.slippi.gg/v1/queue";
constexpr const char* MM_DEV_URL = "wss://matchmaking-dev.slippi.gg/v1/queue";
constexpr const char* MM_URL_ENV_VAR = "SLIPPI_MM_URL";   // overrides the service URL, for local testing
constexpr int MM_CONNECT_TIMEOUT_MS = 5000;
constexpr int MM_HANDSHAKE_READ_TIMEOUT_MS = 10000;
constexpr int MM_POLL_READ_TIMEOUT_MS = 250;
constexpr int MM_CONNECT_ATTEMPTS = 3;
// How long after losing a queued ticket's connection we keep trying to resume. The service drops a
// silent ticket after thirty seconds.
constexpr auto MM_RESUME_WINDOW = std::chrono::seconds(15);
constexpr int MM_RETRY_DELAY_MS = 1000;
// How long a connection may go without the service acknowledging its join or resume.
constexpr auto MM_JOIN_TIMEOUT = std::chrono::seconds(5);
// The service sends a status message every few seconds while a ticket is queued. After this long
// without any frame we send a ping, and after the liveness timeout the connection counts as lost.
constexpr auto MM_PING_AFTER = std::chrono::seconds(5);
constexpr auto MM_LIVENESS_TIMEOUT = std::chrono::seconds(15);
enum MmState { MM_IDLE = 0, MM_CONNECTING = 1, MM_QUEUED = 2, MM_MATCHED = 3, MM_FAILED = 4 };

std::string mm_url() {
  const char* env = std::getenv(MM_URL_ENV_VAR);
  if (env && *env) return env;
  return std::string(SLIPPI_SEMVER).find("dev") != std::string::npos ? MM_DEV_URL : MM_PROD_URL;
}
}  // namespace

struct MmSession {
  std::atomic<int> state{MM_CONNECTING};
  std::atomic<bool> cancel{false};
  std::mutex mutex;
  bool has_result = false;
  std::string result_json, error, latest_version;
  bool cancelled() const { return cancel.load(); }
  void fail(const std::string& message) {
    host::log("slippi: [Matchmaking] %s", message.c_str());
    { std::lock_guard<std::mutex> lock(mutex); error = message; }
    state = MM_FAILED;
  }
};

namespace {
// One session runs at a time across the process (the Rust client lives on the EXI device): starting
// a new one cancels the previous, which then leaves the queue on its own.
std::mutex g_mm_session_mutex;
std::weak_ptr<MmSession> g_mm_current_session;

class MmSessionRun {
 public:
  MmSessionRun(std::shared_ptr<MmSession> shared, std::string url, std::string join)
      : shared_(std::move(shared)), url_(std::move(url)), join_(std::move(join)) {}

  void run() {
    int connect_failures = 0;
    for (;;) {
      if (shared_->cancelled()) { shared_->state = MM_IDLE; return; }
      std::string err;
      auto socket = ws::Connection::open(url_, MM_CONNECT_TIMEOUT_MS, MM_CONNECT_TIMEOUT_MS + MM_HANDSHAKE_READ_TIMEOUT_MS,
                                         &shared_->cancel, &err);
      if (!socket) {
        if (shared_->cancelled()) { shared_->state = MM_IDLE; return; }
        host::log("slippi: [Matchmaking] Connect failed: %s", err.c_str());
        if (give_up(++connect_failures) || sleep_unless_cancelled(MM_RETRY_DELAY_MS)) return;
        continue;
      }
      connect_failures = 0;
      const std::string first = has_ticket_ ? protocol::resume(ticket_id_, token_).dump() : join_;
      if (!socket->send_text(first, &err)) {
        host::log("slippi: [Matchmaking] Failed to send first message: %s", err.c_str());
        socket->close();
        if (give_up(++connect_failures) || sleep_unless_cancelled(MM_RETRY_DELAY_MS)) return;
        continue;
      }
      switch (serve(*socket)) {
        case Ended::DONE: return;
        case Ended::JOIN_TIMEOUT: shared_->fail("Failed to join mm queue"); return;
        case Ended::LOST:
          if (!has_ticket_) { shared_->fail("Lost connection to the mm server"); return; }
          if (!has_deadline_) { has_deadline_ = true; resume_deadline_ = Clock::now() + MM_RESUME_WINDOW; }
          else if (Clock::now() >= resume_deadline_) { shared_->fail("Lost connection to the mm server"); return; }
          host::log("slippi: [Matchmaking] Connection lost, resuming ticket");
          if (sleep_unless_cancelled(MM_RETRY_DELAY_MS)) return;
          break;
      }
    }
  }

 private:
  using Clock = std::chrono::steady_clock;
  enum class Ended { DONE, LOST, JOIN_TIMEOUT };

  // A failed connect ends the session past the resume window when resuming, after a few attempts
  // when first joining.
  bool give_up(int connect_failures) {
    const bool give_up = has_deadline_ ? Clock::now() >= resume_deadline_ : connect_failures >= MM_CONNECT_ATTEMPTS;
    if (give_up) shared_->fail(has_ticket_ ? "Lost connection to the mm server" : "Failed to connect to mm server");
    return give_up;
  }

  // Runs one connection until the session ends or the connection drops.
  Ended serve(ws::Connection& socket) {
    const auto opened = Clock::now();
    auto last_frame = opened;
    bool pinged = false;
    acknowledged_ = false;
    for (;;) {
      if (shared_->cancelled()) {
        socket.send_text(protocol::simple("cancel").dump());
        socket.close();
        shared_->state = MM_IDLE;
        return Ended::DONE;
      }
      std::string text, detail;
      switch (socket.receive(&text, MM_POLL_READ_TIMEOUT_MS, &detail)) {
        case ws::Event::TEXT: {
          last_frame = Clock::now();
          pinged = false;
          Ended ended;
          if (handle(text, socket, ended)) return ended;
          break;
        }
        case ws::Event::CLOSED:
          host::log("slippi: [Matchmaking] Server closed connection: %s", detail.c_str());
          return Ended::LOST;
        case ws::Event::CONTROL:   // binary, ping and pong frames carry nothing but prove the service is there
          last_frame = Clock::now();
          pinged = false;
          break;
        case ws::Event::TIMEOUT: break;
        case ws::Event::FAILED:
          host::log("slippi: [Matchmaking] Read failed: %s", detail.c_str());
          return Ended::LOST;
      }
      if (!acknowledged_ && Clock::now() - opened >= MM_JOIN_TIMEOUT) {
        host::log("slippi: [Matchmaking] Service did not acknowledge the connection in time");
        socket.close();
        return has_ticket_ ? Ended::LOST : Ended::JOIN_TIMEOUT;
      }
      const auto idle = Clock::now() - last_frame;
      if (idle >= MM_LIVENESS_TIMEOUT) {
        host::log("slippi: [Matchmaking] Nothing from the service for %lld ms, connection lost",
                  (long long)std::chrono::duration_cast<std::chrono::milliseconds>(idle).count());
        socket.close();
        return Ended::LOST;
      }
      if (idle >= MM_PING_AFTER && !pinged) {
        pinged = true;
        socket.send_text(protocol::simple("ping").dump());
      }
    }
  }

  // Handles one server message. Returns true (with `ended`) when the connection is done.
  bool handle(const std::string& text, ws::Connection& socket, Ended& ended) {
    const auto msg = protocol::parse_server_message(text);
    switch (msg.kind) {
      case protocol::ServerMessage::MALFORMED:
        host::log("slippi: [Matchmaking] Ignoring malformed message");
        return false;
      case protocol::ServerMessage::JOINED:
        if (msg.fields_ok) {
          host::log("slippi: [Matchmaking] Ticket %s accepted", msg.ticket_id.c_str());
          has_ticket_ = true; ticket_id_ = msg.ticket_id; token_ = msg.token;
          acknowledged_ = true;
          has_deadline_ = false;   // the service took us back, so the next drop gets a full resume window
          shared_->state = MM_QUEUED;
        }
        return false;
      case protocol::ServerMessage::MATCH:
        if (msg.fields_ok) host::log("slippi: [Matchmaking] Match found: %s (host: %s)", msg.match_id.c_str(), msg.is_host ? "true" : "false");
        else host::log("slippi: [Matchmaking] Match message unreadable");
        { std::lock_guard<std::mutex> lock(shared_->mutex); shared_->result_json = text; shared_->has_result = true; }
        shared_->state = MM_MATCHED;
        socket.close();
        ended = Ended::DONE;
        return true;
      case protocol::ServerMessage::ERROR: {
        // The service knows a newer build exists when it says so; remembered for players whose update check is broken.
        if (msg.fields_ok && !msg.latest_version.empty()) { std::lock_guard<std::mutex> lock(shared_->mutex); shared_->latest_version = msg.latest_version; }
        shared_->fail(msg.fields_ok ? msg.error : "Received an error from the mm server");
        socket.close();
        ended = Ended::DONE;
        return true;
      }
      default: return false;   // status and anything newer
    }
  }

  // Sleeps in small steps so a cancel is noticed. Returns true if cancelled.
  bool sleep_unless_cancelled(int total_ms) {
    const auto start = Clock::now();
    while (Clock::now() - start < std::chrono::milliseconds(total_ms)) {
      if (shared_->cancelled()) { shared_->state = MM_IDLE; return true; }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
  }

  std::shared_ptr<MmSession> shared_;
  std::string url_, join_;
  bool has_ticket_ = false;
  std::string ticket_id_, token_;
  bool has_deadline_ = false;
  Clock::time_point resume_deadline_;
  bool acknowledged_ = false;
};

// Begins a session (cancelling any previous one) on a worker that is never joined, so cancelling
// does not wait on a connect or handshake in progress; once flagged it touches only its own state.
std::shared_ptr<MmSession> mm_start_session(const UserInfo& me, const protocol::MatchRequest& request) {
  const auto nat = stun::classify_nat(request.stun[0], request.stun[1]);
  host::log("slippi: [Matchmaking] NAT type: %s", stun::nat_type_name(nat));   // the addresses themselves stay out of the log
  std::string join = protocol::join(me, request, SLIPPI_SEMVER).dump();
  std::string url = mm_url();
  auto session = std::make_shared<MmSession>();
  {
    std::lock_guard<std::mutex> lock(g_mm_session_mutex);
    if (auto previous = g_mm_current_session.lock()) previous->cancel = true;
    g_mm_current_session = session;
  }
  host::log("slippi: [Matchmaking] Starting session against %s", url.c_str());
  std::thread([session, url = std::move(url), join = std::move(join)]() mutable {
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif
    MmSessionRun(std::move(session), std::move(url), std::move(join)).run();
  }).detach();
  return session;
}
}  // namespace

Matchmaking::LocalPeer Matchmaking::local_peer;
uint16_t Matchmaking::forced_port = 0;

Matchmaking::Matchmaking(User* user) : user_(user) {}
Matchmaking::~Matchmaking() {
  is_mm_terminated_ = true;
  state_ = ERROR_ENCOUNTERED;
  error_msg_ = "Matchmaking shut down";
  if (thread_.joinable()) thread_.join();
  terminateMmConnection();
}
void Matchmaking::FindMatch(MatchSearchSettings settings) {
  search_settings_ = settings;
  error_msg_.clear();
  state_ = INITIALIZING;
  host::log("slippi: matchmaking started (mode %d, code '%s')", settings.mode, settings.connect_code.c_str());
  thread_ = std::thread(&Matchmaking::MatchmakeThread, this);
}
void Matchmaking::MatchmakeThread() {
#if defined(__APPLE__)
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);   // the handshake that starts a match should not wait behind background work
#endif
  while (IsSearching()) {
    if (is_mm_terminated_) break;
    switch (state_.load()) {
      case INITIALIZING: startMatchmaking(); break;
      case MATCHMAKING: handleMatchmaking(); break;
      case OPPONENT_CONNECTING: handleConnecting(); break;
      default: break;
    }
  }
  terminateMmConnection();   // leave the queue if still in it and free the port
}
void Matchmaking::terminateMmConnection() {
  in_queue_ = false;
  if (session_) { session_->cancel = true; session_.reset(); }
  if (client_) { enet_host_destroy(client_); client_ = nullptr; }
}

// Set up and connect a UDP socket (no packets are sent) so the OS picks the interface it would
// use to reach `remote`; that interface's address is the LAN address opponents behind the same
// public IP can use.
static std::string local_address_string(ENetAddress* remote) {
  ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
  if (s == ENET_SOCKET_NULL) return "";
  ENetAddress a;
  std::string out;
  if (enet_socket_connect(s, remote) != -1 && enet_socket_get_address(s, &a) != -1) {
    struct in_addr in; in.s_addr = a.host;
    char buf[INET_ADDRSTRLEN]{};
    if (inet_ntop(AF_INET, &in, buf, sizeof buf)) out = buf;
  }
  enet_socket_destroy(s);
  return out;
}

// Sends a STUN binding request to the given server from the netplay socket and reads back the
// public address the server saw it come from: three tries of 500 ms each.
bool Matchmaking::stunBindingRequest(uint32_t host_addr, uint16_t port, stun::Observation& result) {
  uint8_t request[stun::REQUEST_LEN];
  stun::build_binding_request(request);
  ENetAddress server; server.host = host_addr; server.port = port;
  ENetBuffer send_buf; send_buf.data = request; send_buf.dataLength = sizeof request;
  for (int attempt = 0; attempt < 3; ++attempt) {
    const int sent = enet_socket_send(client_->socket, &server, &send_buf, 1);
    if (sent != (int)sizeof request) { host::log("slippi: [Matchmaking] STUN send failed with %d", sent); return false; }
    const uint64_t start = time_ms();
    while (time_ms() - start < 500) {
      if (is_mm_terminated_) return false;
      enet_uint32 condition = ENET_SOCKET_WAIT_RECEIVE;
      if (enet_socket_wait(client_->socket, &condition, 100) != 0) { host::log("slippi: [Matchmaking] STUN wait failed"); return false; }
      if (!(condition & ENET_SOCKET_WAIT_RECEIVE)) continue;
      uint8_t response[512];
      ENetBuffer recv_buf; recv_buf.data = response; recv_buf.dataLength = sizeof response;
      ENetAddress from;
      const int received = enet_socket_receive(client_->socket, &from, &recv_buf, 1);
      if (received <= 0) continue;
      if (stun::parse_binding_response(request, response, (size_t)received, result)) return true;
    }
  }
  return false;
}

// Keeps the NAT mapping for the netplay socket alive while queued. Any reply is drained so the
// netplay host does not find it later.
void Matchmaking::sendStunKeepalive() {
  last_keepalive_ms_ = time_ms();
  if (!client_ || !has_stun_addr_) return;
  uint8_t request[stun::REQUEST_LEN];
  stun::build_binding_request(request);
  ENetAddress server; server.host = stun_host_; server.port = stun_port_;
  ENetBuffer send_buf; send_buf.data = request; send_buf.dataLength = sizeof request;
  enet_socket_send(client_->socket, &server, &send_buf, 1);
  enet_uint32 condition = ENET_SOCKET_WAIT_RECEIVE;
  if (enet_socket_wait(client_->socket, &condition, 100) == 0 && (condition & ENET_SOCKET_WAIT_RECEIVE)) {
    uint8_t response[512];
    ENetBuffer recv_buf; recv_buf.data = response; recv_buf.dataLength = sizeof response;
    ENetAddress from;
    enet_socket_receive(client_->socket, &from, &recv_buf, 1);
  }
}

void Matchmaking::startMatchmaking() {
  client_ = nullptr;
  UserInfo me = user_->GetUserInfo();
  static std::mt19937 rng((uint32_t)time_ms());

  if (local_peer.enabled) {
    // Two local instances peer directly; the "match" is fabricated the way the server would report it.
    host_port_ = local_peer.local_port;
    local_player_index_ = local_peer.local_index;
    is_host_ = local_player_index_ == 0;
    player_info_.clear();
    remote_ips_.clear();
    for (int i = 0; i < 2; ++i) {
      UserInfo p;
      if (i == local_player_index_) p = me;
      else { p.uid = "local-peer"; p.display_name = "Peer"; p.connect_code = "PEER#001"; }
      p.port = i + 1;
      p.chat_messages = User::GetDefaultChatMessages();
      player_info_.push_back(p);
    }
    remote_ips_.push_back(local_peer.remote_ip + ":" + std::to_string(local_peer.remote_port));
    allowed_stages_ = {0x2, 0x3, 0x8, 0x1C, 0x1F, 0x20};
    mm_result_.id = "mode." + std::string(search_settings_.mode == DIRECT ? "direct" : "unranked") + "-local-test";
    mm_result_.players = player_info_;
    mm_result_.stages = allowed_stages_;
    mm_result_.items = 0;
    state_ = OPPONENT_CONNECTING;
    host::log("slippi: local peer test: player %d on port %d, peer %s", local_player_index_, host_port_, remote_ips_[0].c_str());
    return;
  }

  // The host is bound to the port we will play on. Talking to the STUN servers through its socket
  // creates the NAT mapping opponents will use and tells us what it is.
  for (int retry = 0; !client_ && retry < 15; ++retry) {
    host_port_ = forced_port ? forced_port : 41000 + (int)(rng() % 10000);
    ENetAddress addr; addr.host = ENET_HOST_ANY; addr.port = (enet_uint16)host_port_;
    client_ = enet_host_create(&addr, 1, 3, 0, 0);
  }
  if (!client_) { state_ = ERROR_ENCOUNTERED; error_msg_ = "Failed to create mm client"; host::log("slippi: [Matchmaking] Failed to create client"); return; }
  prioritize_socket(client_->socket);
  host::log("slippi: [Matchmaking] Port to use: %d", host_port_);

  std::vector<ENetAddress> stun_addrs;
  for (const auto& server : STUN_SERVERS) {
    ENetAddress addr;
    if (enet_address_set_host(&addr, server[0]) != 0) { host::log("slippi: [Matchmaking] Failed to resolve STUN server %s", server[0]); continue; }
    addr.port = (enet_uint16)std::atoi(server[1]);
    stun_addrs.push_back(addr);
  }
  has_stun_addr_ = !stun_addrs.empty();
  if (has_stun_addr_) { stun_host_ = stun_addrs[0].host; stun_port_ = stun_addrs[0].port; }

  // Determine the local IP address. Opponents behind the same public IP (same LAN, same VPN node,
  // same CGNAT) can reach us on it.
  std::string lan = has_stun_addr_ ? local_address_string(&stun_addrs[0]) : "";
  std::string lan_addr = lan.empty() ? "" : lan + ":" + std::to_string(host_port_);
  host::log("slippi: [Matchmaking] Sending LAN address: %s", lan_addr.c_str());

  // Ask two servers what public address they see, walking the list in order so a network that
  // blocks the first entries still gets an answer. The addresses themselves are never logged.
  protocol::MatchRequest request;
  for (const auto& addr : stun_addrs) {
    if (!request.stun[0].answered) {
      if (stunBindingRequest(addr.host, addr.port, request.stun[0])) { stun_host_ = addr.host; stun_port_ = addr.port; }   // keepalives go to a server known to answer
    } else if (stunBindingRequest(addr.host, addr.port, request.stun[1])) {
      break;
    }
  }
  last_keepalive_ms_ = time_ms();
  const int answers = (request.stun[0].answered ? 1 : 0) + (request.stun[1].answered ? 1 : 0);
  if (answers == 0) host::log("slippi: [Matchmaking] STUN lookup failed, the service will use the address it observes");
  else host::log("slippi: [Matchmaking] STUN servers answered: %d of 2", answers);

  request.mode = (uint8_t)search_settings_.mode;
  request.connect_code = search_settings_.connect_code;
  request.netplay_port = (uint16_t)host_port_;
  request.lan_addr = lan_addr;
  // A cancel that arrived during STUN must not start a session nobody will poll.
  if (is_mm_terminated_) return;
  session_ = mm_start_session(me, request);
  state_ = MATCHMAKING;
  host::log("slippi: [Matchmaking] Search started");
}

void Matchmaking::handleMatchmaking() {
  if (state_ != MATCHMAKING) return;
  const int session_state = session_ ? session_->state.load() : MM_IDLE;
  if (session_state != MM_CONNECTING && session_state != MM_QUEUED) in_queue_ = false;
  switch (session_state) {
    case MM_CONNECTING:
    case MM_QUEUED:
      in_queue_ = session_state == MM_QUEUED;
      if (has_stun_addr_ && time_ms() - last_keepalive_ms_ >= STUN_KEEPALIVE_INTERVAL_MS) sendStunKeepalive();
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      return;
    case MM_FAILED: {
      std::string message, latest;
      { std::lock_guard<std::mutex> lock(session_->mutex); message = session_->error; latest = session_->latest_version; }
      session_.reset();
      if (!latest.empty()) user_->OverwriteLatestVersion(latest);   // force the latest version for people whose file updates don't work
      error_msg_ = message;
      host::log("slippi: [Matchmaking] Received error from server: %s", message.c_str());
      state_ = ERROR_ENCOUNTERED;
      return;
    }
    case MM_MATCHED: break;
    default:
      // The session ended without a result, which only happens if it was cancelled out from under us.
      session_.reset();
      state_ = ERROR_ENCOUNTERED;
      error_msg_ = "Matchmaking was cancelled";
      return;
  }

  std::string result_text;
  bool has_result;
  { std::lock_guard<std::mutex> lock(session_->mutex); has_result = session_->has_result; result_text = std::move(session_->result_json); session_->has_result = false; }
  session_.reset();
  if (!has_result) {
    host::log("slippi: [Matchmaking] Match result was missing");
    state_ = ERROR_ENCOUNTERED; error_msg_ = "Invalid response when getting mm status";
    return;
  }
  json resp = json::parse(result_text, nullptr, false);
  protocol::MatchResult match;
  if (resp.is_discarded() || !protocol::match_result(resp, match)) {
    host::log("slippi: [Matchmaking] Match result could not be parsed");
    state_ = ERROR_ENCOUNTERED; error_msg_ = "Invalid response when getting mm status";
    return;
  }

  netplay_client_ = nullptr;
  host::log("slippi: match id %s", match.result.id.c_str());
  for (auto& p : match.result.players) if (p.chat_messages.size() != 16) p.chat_messages = User::GetDefaultChatMessages();
  player_info_ = match.result.players;
  remote_ips_ = match.remote_ips;
  local_player_index_ = match.local_player_index;
  is_host_ = match.is_host;
  allowed_stages_ = match.result.stages;
  mm_result_ = match.result;
  terminateMmConnection();   // free the port so the netplay client can bind it
  state_ = OPPONENT_CONNECTING;
  host::log("slippi: opponent found (decider: %d)", is_host_);
}

void Matchmaking::handleConnecting() {
  netplay_client_ = nullptr;
  std::vector<std::string> addrs;
  std::vector<uint16_t> ports;
  for (auto& ip : remote_ips_) {
    size_t colon = ip.find(':');
    addrs.push_back(ip.substr(0, colon));
    ports.push_back((uint16_t)std::atoi(ip.substr(colon + 1).c_str()));
  }
  auto client = std::make_unique<NetplayClient>(addrs, ports, (uint8_t)remote_ips_.size(), (uint16_t)host_port_, is_host_, (uint8_t)local_player_index_);
  while (!netplay_client_) {
    auto st = client->GetSlippiConnectStatus();
    if (st == NetplayClient::ConnectStatus::INITIATED) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      if (state_ != OPPONENT_CONNECTING) return;
      continue;
    }
    if (st != NetplayClient::ConnectStatus::CONNECTED) {
      if (local_peer.enabled) { state_ = ERROR_ENCOUNTERED; error_msg_ = "Could not connect to the local peer"; return; }
      if (search_settings_.mode == TEAMS) { state_ = ERROR_ENCOUNTERED; error_msg_ = "Timed out waiting for other players to connect"; return; }
      host::log("slippi: connection attempt failed, searching again");
      state_ = INITIALIZING;
      return;
    }
    netplay_client_ = std::move(client);
  }
  state_ = CONNECTION_SUCCESS;
}

int Matchmaking::GetPlayerRank(uint8_t port) const {
  if (port >= player_info_.size()) return 0;
  const UserInfo& info = player_info_[port];
  return protocol::player_rank(info.ranked_rating, info.ranked_update_count, info.ranked_global_placement, info.ranked_regional_placement);
}

}  // namespace slippi
#endif  // online transport is deliberately absent from the diagnostic target
