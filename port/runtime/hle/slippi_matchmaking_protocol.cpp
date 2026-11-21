// Port of slippi-rust-extensions matchmaking/src/stun.rs and protocol.rs (Slippi 3.7.0) and of the
// match-result parse in Slippi Ishiiruka's SlippiMatchmaking::handleMatchmaking.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "json_safe.h"
#include "slippi_matchmaking_protocol.h"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <random>

namespace slippi::stun {
namespace {
constexpr uint16_t BINDING_REQUEST = 0x0001;
constexpr uint16_t BINDING_SUCCESS = 0x0101;
constexpr uint16_t ATTR_MAPPED_ADDRESS = 0x0001;
constexpr uint16_t ATTR_XOR_MAPPED_ADDRESS = 0x0020;
constexpr uint8_t FAMILY_IPV4 = 0x01;

uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

bool decode_address(const uint8_t* value, size_t len, bool xored, Observation& out) {
  if (len < 8 || value[1] != FAMILY_IPV4) return false;
  uint16_t port = be16(value + 2);
  uint32_t ip = be32(value + 4);
  if (xored) { port ^= uint16_t(MAGIC_COOKIE >> 16); ip ^= MAGIC_COOKIE; }
  out.answered = true;
  out.ip[0] = uint8_t(ip >> 24); out.ip[1] = uint8_t(ip >> 16); out.ip[2] = uint8_t(ip >> 8); out.ip[3] = uint8_t(ip);
  out.port = port;
  return true;
}
}  // namespace

bool Observation::same_address(const Observation& o) const {
  return ip[0] == o.ip[0] && ip[1] == o.ip[1] && ip[2] == o.ip[2] && ip[3] == o.ip[3] && port == o.port;
}

void build_binding_request(uint8_t out[REQUEST_LEN]) {
  out[0] = uint8_t(BINDING_REQUEST >> 8); out[1] = uint8_t(BINDING_REQUEST);
  out[2] = 0; out[3] = 0;   // no attributes
  out[4] = uint8_t(MAGIC_COOKIE >> 24); out[5] = uint8_t(MAGIC_COOKIE >> 16); out[6] = uint8_t(MAGIC_COOKIE >> 8); out[7] = uint8_t(MAGIC_COOKIE);
  // The transaction ID only needs to be unpredictable enough that a stale or foreign response is
  // not mistaken for ours: a process-random seed plus a counter, mixed.
  static std::atomic<uint64_t> counter{0};
  static const uint64_t seed = [] {
    std::random_device rd;
    return (uint64_t(rd()) << 32 ^ rd()) ^ uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
  }();
  std::mt19937_64 mix(seed ^ (counter.fetch_add(1, std::memory_order_relaxed) * 0x9E3779B97F4A7C15ull));
  for (int i = 0; i < 12; i += 4) {
    uint32_t r = uint32_t(mix());
    for (int b = 0; b < 4; ++b) out[8 + i + b] = uint8_t(r >> (8 * b));
  }
}

bool parse_binding_response(const uint8_t request[REQUEST_LEN], const uint8_t* response, size_t response_len, Observation& out) {
  if (!request || !response || response_len < REQUEST_LEN) return false;
  if (be16(response) != BINDING_SUCCESS) return false;
  const size_t length = be16(response + 2);
  if (be32(response + 4) != MAGIC_COOKIE) return false;
  for (size_t i = 8; i < REQUEST_LEN; ++i) if (response[i] != request[i]) return false;
  const uint8_t* body = response + REQUEST_LEN;
  const size_t body_len = std::min(response_len, REQUEST_LEN + length) - REQUEST_LEN;

  Observation fallback;
  size_t offset = 0;
  while (offset + 4 <= body_len) {
    const uint16_t type = be16(body + offset);
    const size_t attr_len = be16(body + offset + 2);
    const size_t value_start = offset + 4, value_end = value_start + attr_len;
    if (value_end > body_len) break;
    if (type == ATTR_XOR_MAPPED_ADDRESS) {
      Observation found;
      if (decode_address(body + value_start, attr_len, true, found)) { out = found; return true; }
    } else if (type == ATTR_MAPPED_ADDRESS && !fallback.answered) {
      decode_address(body + value_start, attr_len, false, fallback);
    }
    offset = value_end + (4 - attr_len % 4) % 4;   // attributes are padded to four-byte boundaries
  }
  if (!fallback.answered) return false;
  out = fallback;
  return true;
}

NatType classify_nat(const Observation& first, const Observation& second) {
  if (!first.answered || !second.answered) return NatType::UNKNOWN;
  return first.same_address(second) ? NatType::CONE : NatType::SYMMETRIC;
}
const char* nat_type_name(NatType t) {
  switch (t) {
    case NatType::CONE: return "cone";
    case NatType::SYMMETRIC: return "symmetric";
    default: return "unknown";
  }
}
std::string ip_string(const Observation& o) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "%u.%u.%u.%u", o.ip[0], o.ip[1], o.ip[2], o.ip[3]);
  return buf;
}
}  // namespace slippi::stun

namespace slippi::protocol {
using nlohmann::json;

json join(const UserInfo& identity, const MatchRequest& request, const std::string& app_version) {
  const stun::NatType nat = stun::classify_nat(request.stun[0], request.stun[1]);
  json candidates = json::array();
  uint16_t port = request.netplay_port;
  for (const auto& reflexive : request.stun) {
    if (!reflexive.answered) continue;
    // The STUN-mapped port, not the local one, is what opponents should send to.
    candidates.push_back({{"ip", stun::ip_string(reflexive)}, {"port", reflexive.port}, {"type", "srflx"}});
    port = reflexive.port;
    break;
  }
  const auto colon = request.lan_addr.rfind(':');
  if (colon != std::string::npos) {
    const char* first = request.lan_addr.data() + colon + 1;
    const char* last = request.lan_addr.data() + request.lan_addr.size();
    uint16_t lan_port = 0;
    const auto parsed = std::from_chars(first, last, lan_port);
    if (parsed.ec == std::errc{} && parsed.ptr == last && first != last)
      candidates.push_back({{"ip", request.lan_addr.substr(0, colon)}, {"port", lan_port}, {"type", "host"}});
  }
  json out = {
      {"type", "join"},
      {"user", {{"uid", identity.uid}, {"playKey", identity.play_key},
                {"connectCode", identity.connect_code}, {"displayName", identity.display_name}}},
      {"search", {{"mode", request.mode},
                  {"connectCode", std::vector<uint8_t>(request.connect_code.begin(), request.connect_code.end())}}},
      {"appVersion", app_version},
      {"port", port},
      {"ipAddressLan", request.lan_addr},
  };
  if (!candidates.empty()) out["candidates"] = std::move(candidates);
  out["natType"] = stun::nat_type_name(nat);   // never empty, so always sent
  return out;
}

json resume(const std::string& ticket_id, const std::string& token) {
  return {{"type", "resume"}, {"ticketId", ticket_id}, {"token", token}};
}
json simple(const char* type) { return {{"type", type}}; }

namespace {
bool string_field(const json& j, const char* key, std::string& out) {
  auto it = j.find(key);
  if (it == j.end() || !it->is_string()) return false;
  out = it->get<std::string>();
  return true;
}
}  // namespace

ServerMessage parse_server_message(const std::string& text) {
  ServerMessage m;
  json j = json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object() || !string_field(j, "type", m.type)) return m;   // MALFORMED: ignored
  if (m.type == "joined") {
    m.kind = ServerMessage::JOINED;
    m.fields_ok = string_field(j, "ticketId", m.ticket_id) && string_field(j, "token", m.token);
  } else if (m.type == "status") {
    m.kind = ServerMessage::STATUS;
    auto it = j.find("secondsInQueue");
    m.fields_ok = it != j.end() && it->is_number();
    if (m.fields_ok) m.seconds_in_queue = it->get<double>();
  } else if (m.type == "match") {
    m.kind = ServerMessage::MATCH;
    m.fields_ok = string_field(j, "matchId", m.match_id);
    auto it = j.find("isHost");
    if (it != j.end()) { if (it->is_boolean()) m.is_host = it->get<bool>(); else m.fields_ok = false; }
  } else if (m.type == "error") {
    m.kind = ServerMessage::ERROR;
    m.fields_ok = string_field(j, "error", m.error);
    if (j.count("latestVersion") && !string_field(j, "latestVersion", m.latest_version)) m.fields_ok = false;
    if (!m.fields_ok) { m.error.clear(); m.latest_version.clear(); }
  } else {
    m.kind = ServerMessage::OTHER;
    m.fields_ok = true;
  }
  return m;
}

bool match_result(const json& message, MatchResult& out) {
  if (!message.is_object()) return false;
  MatchResult r;
  r.result.id = jget(message, "matchId", "");
  auto players = message.find("players");
  if (players != message.end() && players->is_array()) {
    std::string local_external_ip;
    for (const auto& el : *players) {
      UserInfo p;
      const bool is_local = jget(el, "isLocalPlayer", false);
      p.uid = jget(el, "uid", "");
      p.display_name = jget(el, "displayName", "");
      p.connect_code = jget(el, "connectCode", "");
      p.port = jget(el, "port", 0);
      p.is_bot = jget(el, "isBot", false);
      // Kept only when the player sent a full set of 16; the caller substitutes the defaults otherwise.
      if (el.is_object() && el.count("chatMessages") && el["chatMessages"].is_array()) {
        for (const auto& msg : el["chatMessages"]) if (msg.is_string()) p.chat_messages.push_back(msg.get<std::string>());
        if (p.chat_messages.size() != 16) p.chat_messages.clear();
      }
      if (el.is_object() && el.count("rank") && el["rank"].is_object()) {
        const auto& rank = el["rank"];
        p.ranked_rating = jget(rank, "rating", 0.0f);
        p.ranked_update_count = jget(rank, "updateCount", 0);
        p.ranked_global_placement = jget(rank, "globalPlacement", 0);
        p.ranked_regional_placement = jget(rank, "regionalPlacement", 0);
        p.ranked_mmr = jget(rank, "mmr", 0.0f);
      }
      r.result.players.push_back(p);
      if (is_local) {
        const std::string ip = jget(el, "ipAddress", "1.1.1.1:123");
        local_external_ip = ip.substr(0, ip.find(':'));
        r.local_player_index = p.port - 1;
      }
    }
    // A second pass picks each opponent's address: the LAN address when both sides share a public IP.
    for (const auto& el : *players) {
      if (jget(el, "port", 0) - 1 == r.local_player_index) continue;
      const std::string ext = jget(el, "ipAddress", "1.1.1.1:123");
      const std::string lan = jget(el, "ipAddressLan", "1.1.1.1:123");
      if (ext.substr(0, ext.find(':')) != local_external_ip || lan.empty()) r.remote_ips.push_back(ext);
      else r.remote_ips.push_back(lan);
    }
  }
  r.is_host = jget(message, "isHost", false);
  auto stages = message.find("stages");
  if (stages != message.end() && stages->is_array())
    for (const auto& s : *stages) if (s.is_number()) r.result.stages.push_back(uint16_t(s.get<int>()));
  if (r.result.stages.empty()) {
    r.result.stages = {0x3, 0x8, 0x1C, 0x1F, 0x20};   // Pokemon, Yoshi's Story, Dream Land, Battlefield, Final Destination
    if (r.result.players.size() == 2) r.result.stages.push_back(0x2);   // FoD in singles
  }
  r.result.items = jget(message, "items", 0u);
  out = std::move(r);
  return true;
}

int player_rank(float rating, int update_count, int global, int regional) {
  if (update_count < 5) return 0;
  // Each threshold is the minimum rating for that rank. Checking from the top down means every
  // rating lands in exactly one rank, including negative ones (Bronze 1). Matches the launcher.
  if (rating >= 2191.75f && (global > 0 || regional > 0)) return 19;   // Grandmaster
  static const float minimum[] = {2350.0f, 2275.0f, 2191.75f, 2136.28f, 2073.67f, 2003.92f, 1927.03f, 1843.0f, 1751.83f,
                                  1653.52f, 1548.07f, 1435.48f, 1315.75f, 1188.88f, 1054.87f, 913.72f, 765.43f};
  for (int i = 0; i < 17; ++i) if (rating >= minimum[i]) return 18 - i;   // Master 3 (18) down to Bronze 2 (2)
  return 1;   // Bronze 1
}
}  // namespace slippi::protocol
