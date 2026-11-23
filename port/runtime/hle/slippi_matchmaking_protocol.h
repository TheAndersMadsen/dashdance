// Pure pieces of Slippi 3.7.0's matchmaking: the STUN (RFC 5389) binding request/response
// encoding and NAT classification, the websocket messages exchanged with the matchmaking service,
// and the parse of the match result the service hands back. Ports of slippi-rust-extensions
// matchmaking/src/{stun.rs,protocol.rs} and of SlippiMatchmaking::handleMatchmaking's result
// parse. No profile, DNS, socket, thread or HTTP calls are made by these functions.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "slippi_net.h"
#include <nlohmann/json.hpp>

namespace slippi::stun {
// Size of a binding request with no attributes.
constexpr size_t REQUEST_LEN = 20;
constexpr uint32_t MAGIC_COOKIE = 0x2112A442;

// What one STUN server reported for the netplay socket; `answered` is false when it did not answer.
struct Observation {
  bool answered = false;
  uint8_t ip[4] = {};   // public IPv4 address, network order octets
  uint16_t port = 0;    // host order
  bool same_address(const Observation& o) const;
};

// Fills `out` with a binding request carrying a fresh transaction ID.
void build_binding_request(uint8_t out[REQUEST_LEN]);
// Extracts the mapped address from a binding response to `request`. Returns false (leaving `out`
// untouched) if the bytes are not a success response to that request or carry no IPv4 mapped
// address. XOR-MAPPED-ADDRESS wins over MAPPED-ADDRESS.
bool parse_binding_response(const uint8_t request[REQUEST_LEN], const uint8_t* response, size_t response_len, Observation& out);

enum class NatType { UNKNOWN, CONE, SYMMETRIC };
// Classifies the NAT from the mapped addresses two different STUN servers reported for one socket.
NatType classify_nat(const Observation& first, const Observation& second);
const char* nat_type_name(NatType t);   // "unknown" / "cone" / "symmetric", as the service expects
std::string ip_string(const Observation& o);
}  // namespace slippi::stun

namespace slippi::protocol {
// What the matchmaking thread knows when it asks for a match (MatchRequest in client.rs).
struct MatchRequest {
  uint8_t mode = 0;               // 0 ranked, 1 unranked, 2 direct, 3 teams, 4 party
  std::string connect_code;       // opponent or lobby code as raw Shift-JIS bytes
  uint16_t netplay_port = 0;      // local UDP port the ENet host is bound to
  std::string lan_addr;           // "ip:port" on the local network, or empty
  stun::Observation stun[2];      // in the order the servers were asked
};

// The first message on a fresh connection ("join"). Contains the play key and public address:
// never log it.
nlohmann::json join(const UserInfo& identity, const MatchRequest& request, const std::string& app_version);
// The first message when reattaching to an existing ticket.
nlohmann::json resume(const std::string& ticket_id, const std::string& token);
// A message with only a type, such as "cancel" or "ping".
nlohmann::json simple(const char* type);

// One message from the service, classified the way client.rs Session::handle reads it.
struct ServerMessage {
  enum Kind { MALFORMED, JOINED, STATUS, MATCH, ERROR, OTHER } kind = MALFORMED;
  std::string type;
  bool fields_ok = false;          // the kind-specific fields parsed (serde succeeded)
  std::string ticket_id, token;    // joined
  double seconds_in_queue = 0;     // status
  std::string match_id;            // match (summary only; the full text is the result)
  bool is_host = false;
  std::string error, latest_version;   // error
};
ServerMessage parse_server_message(const std::string& text);

// The match result, parsed exactly as SlippiMatchmaking::handleMatchmaking does.
struct MatchResult {
  bool is_host = false;
  int local_player_index = 0;
  std::vector<std::string> remote_ips;   // "ip:port", LAN address when the public IPs match
  Matchmaking::MatchmakeResult result;   // id, players (in message order), stages, items
};
bool match_result(const nlohmann::json& message, MatchResult& out);

// Rank from rating, update count and placements, with 3.7.0's top-down thresholds (0 unranked,
// 1..18 Bronze 1 to Master 3, 19 Grandmaster).
int player_rank(float rating, int update_count, int global_placement, int regional_placement);
}  // namespace slippi::protocol
