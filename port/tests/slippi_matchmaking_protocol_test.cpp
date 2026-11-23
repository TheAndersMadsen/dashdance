// Slippi 3.7.0 matchmaking, pure parts: STUN binding request/response (RFC 5389, with the RFC 5769
// sample response), NAT classification, the websocket messages, the match-result parse and the
// rank thresholds. Synthetic fixtures only: no socket, profile, HTTP or service activation.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "slippi_matchmaking_protocol.h"
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
int failures = 0;
void check(bool value, const char* message) { if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); } }

using Bytes = std::vector<uint8_t>;
void put16(Bytes& b, uint16_t v) { b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v)); }
void put32(Bytes& b, uint32_t v) { put16(b, uint16_t(v >> 16)); put16(b, uint16_t(v)); }

Bytes response_for(const uint8_t* request, const Bytes& attrs, uint16_t type = 0x0101) {
  Bytes out;
  put16(out, type);
  put16(out, uint16_t(attrs.size()));
  put32(out, slippi::stun::MAGIC_COOKIE);
  out.insert(out.end(), request + 8, request + 20);
  out.insert(out.end(), attrs.begin(), attrs.end());
  return out;
}
Bytes xor_mapped(uint32_t ip, uint16_t port) {
  Bytes a = {0x00, 0x20, 0x00, 0x08, 0x00, 0x01};
  put16(a, port ^ uint16_t(slippi::stun::MAGIC_COOKIE >> 16));
  put32(a, ip ^ slippi::stun::MAGIC_COOKIE);
  return a;
}
bool is(const slippi::stun::Observation& o, uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint16_t port) {
  return o.answered && o.ip[0] == a && o.ip[1] == b && o.ip[2] == c && o.ip[3] == d && o.port == port;
}

void stun_tests() {
  using namespace slippi::stun;
  uint8_t a[REQUEST_LEN], b[REQUEST_LEN];
  build_binding_request(a);
  build_binding_request(b);
  const uint8_t header[8] = {0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42};
  check(std::memcmp(a, header, 8) == 0, "binding request: type 0x0001, no attributes, magic cookie");
  check(std::memcmp(a + 8, b + 8, 12) != 0, "every request carries a fresh transaction ID");

  // RFC 5769 section 2.2, "Sample IPv4 Response": XOR-MAPPED-ADDRESS 192.0.2.1 port 32853, after a
  // SOFTWARE attribute of odd length (padding) and followed by MESSAGE-INTEGRITY and FINGERPRINT.
  const uint8_t rfc5769[] = {
      0x01, 0x01, 0x00, 0x3c, 0x21, 0x12, 0xa4, 0x42, 0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae,
      0x80, 0x22, 0x00, 0x0b, 0x74, 0x65, 0x73, 0x74, 0x20, 0x76, 0x65, 0x63, 0x74, 0x6f, 0x72, 0x20,
      0x00, 0x20, 0x00, 0x08, 0x00, 0x01, 0xa1, 0x47, 0xe1, 0x12, 0xa6, 0x43,
      0x00, 0x08, 0x00, 0x14, 0x2b, 0x91, 0xf5, 0x99, 0xfd, 0x9e, 0x90, 0xc3, 0x8c, 0x74, 0x89, 0xf9, 0x2a, 0xf9, 0xba, 0x53,
      0xf0, 0x6b, 0xe7, 0xd7, 0x80, 0x28, 0x00, 0x04, 0xc0, 0x7d, 0x4c, 0x96};
  uint8_t rfc_request[REQUEST_LEN] = {0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xa4, 0x42};
  std::memcpy(rfc_request + 8, rfc5769 + 8, 12);
  Observation got;
  check(parse_binding_response(rfc_request, rfc5769, sizeof rfc5769, got) && is(got, 192, 0, 2, 1, 32853),
        "RFC 5769 sample IPv4 response decodes to 192.0.2.1:32853");
  check(ip_string(got) == "192.0.2.1", "dotted IPv4 text");
  Observation untouched;
  check(!parse_binding_response(a, rfc5769, sizeof rfc5769, untouched) && !untouched.answered,
        "a response to another transaction is rejected and leaves the result alone");

  got = {};
  check(parse_binding_response(a, response_for(a, xor_mapped(0xCB007107u, 41234)).data(), 32, got) && is(got, 203, 0, 113, 7, 41234),
        "XOR-MAPPED-ADDRESS (203.0.113.7:41234)");

  // SOFTWARE with odd length, then a plain MAPPED-ADDRESS somewhere else, then XOR-MAPPED-ADDRESS: XOR wins.
  Bytes attrs = {0x80, 0x22, 0x00, 0x05, 'h', 'e', 'l', 'l', 'o', 0, 0, 0, 0x00, 0x01, 0x00, 0x08, 0x00, 0x01};
  put16(attrs, 1000); put32(attrs, 0x0A000001u);
  const Bytes xm = xor_mapped(0xC6336402u, 5000);
  attrs.insert(attrs.end(), xm.begin(), xm.end());
  auto resp = response_for(a, attrs);
  got = {};
  check(parse_binding_response(a, resp.data(), resp.size(), got) && is(got, 198, 51, 100, 2, 5000),
        "XOR-MAPPED-ADDRESS is preferred over MAPPED-ADDRESS and unknown attributes are skipped");

  Bytes mapped_only = {0x00, 0x01, 0x00, 0x08, 0x00, 0x01};
  put16(mapped_only, 1000); put32(mapped_only, 0x0A000001u);
  resp = response_for(a, mapped_only);
  got = {};
  check(parse_binding_response(a, resp.data(), resp.size(), got) && is(got, 10, 0, 0, 1, 1000), "MAPPED-ADDRESS is the fallback");

  Bytes ipv6 = {0x00, 0x20, 0x00, 0x14, 0x00, 0x02};
  ipv6.resize(ipv6.size() + 18, 0);
  resp = response_for(a, ipv6);
  check(!parse_binding_response(a, resp.data(), resp.size(), got = {}), "an IPv6-only answer gives no IPv4 address");

  resp = response_for(b, xor_mapped(0xCB007107u, 1));
  check(!parse_binding_response(a, resp.data(), resp.size(), got = {}), "foreign transaction ID is rejected");
  resp = response_for(a, xor_mapped(0xCB007107u, 1), 0x0111);
  check(!parse_binding_response(a, resp.data(), resp.size(), got = {}), "binding error response is rejected");
  resp = response_for(a, xor_mapped(0xCB007107u, 1));
  resp[4] ^= 0xFF;
  check(!parse_binding_response(a, resp.data(), resp.size(), got = {}), "wrong magic cookie is rejected");
  const uint8_t short_buf[5] = {};
  check(!parse_binding_response(a, short_buf, sizeof short_buf, got = {}), "short datagram is rejected");
  resp = response_for(a, xor_mapped(0xCB007107u, 1));
  resp.resize(resp.size() - 2);
  check(!parse_binding_response(a, resp.data(), resp.size(), got = {}), "truncated attribute is not read past the datagram");

  Observation x, y, none;
  x.answered = y.answered = true;
  x.ip[0] = y.ip[0] = 1; x.port = 40000; y.port = 40001;
  check(classify_nat(x, x) == NatType::CONE && std::strcmp(nat_type_name(NatType::CONE), "cone") == 0, "same mapping twice: cone");
  check(classify_nat(x, y) == NatType::SYMMETRIC && std::strcmp(nat_type_name(NatType::SYMMETRIC), "symmetric") == 0, "two mappings: symmetric");
  check(classify_nat(x, none) == NatType::UNKNOWN && classify_nat(none, none) == NatType::UNKNOWN &&
        std::strcmp(nat_type_name(NatType::UNKNOWN), "unknown") == 0, "a missing answer: unknown");
}

void message_tests() {
  using namespace slippi;
  using nlohmann::json;
  UserInfo me;
  me.uid = "u"; me.play_key = "k"; me.connect_code = "ME#1"; me.display_name = "Me";
  protocol::MatchRequest req;
  req.mode = 2;
  req.connect_code = std::string("\x82\x60", 2);
  req.netplay_port = 41000;
  req.lan_addr = "192.168.1.5:41000";
  req.stun[0].answered = req.stun[1].answered = true;
  req.stun[0].ip[0] = req.stun[1].ip[0] = 1; req.stun[0].ip[1] = req.stun[1].ip[1] = 2;
  req.stun[0].ip[2] = req.stun[1].ip[2] = 3; req.stun[0].ip[3] = req.stun[1].ip[3] = 4;
  req.stun[0].port = req.stun[1].port = 41001;
  json j = protocol::join(me, req, "3.7.0");
  check(j["type"] == "join" && j["user"]["uid"] == "u" && j["user"]["playKey"] == "k" && j["user"]["connectCode"] == "ME#1" &&
        j["user"]["displayName"] == "Me", "join carries the identity under the service's field names");
  check(j["search"]["mode"] == 2 && j["search"]["connectCode"] == json::array({130, 96}), "connect code goes out as raw Shift-JIS bytes");
  check(j["appVersion"] == "3.7.0" && j["ipAddressLan"] == "192.168.1.5:41000", "version and LAN address");
  check(j["port"] == 41001, "opponents are told the STUN-mapped port, not the local one");
  check(j["candidates"].size() == 2 && j["candidates"][0] == json({{"ip", "1.2.3.4"}, {"port", 41001}, {"type", "srflx"}}) &&
        j["candidates"][1] == json({{"ip", "192.168.1.5"}, {"port", 41000}, {"type", "host"}}), "srflx then host candidates");
  check(j["natType"] == "cone" && !j.count("debug"), "NAT type sent, debug overrides never");

  protocol::MatchRequest bare;
  bare.mode = 1;
  bare.netplay_port = 41020;
  j = protocol::join(me, bare, "3.7.0");
  check(!j.count("candidates") && j["natType"] == "unknown" && j["port"] == 41020 && j["ipAddressLan"] == "" &&
        j["search"]["connectCode"] == json::array(), "no STUN and no LAN: no candidates, local port, unknown NAT");
  bare.stun[1].answered = true; bare.stun[1].port = 5;
  bare.lan_addr = "10.0.0.2:notaport";
  j = protocol::join(me, bare, "3.7.0");
  check(j["candidates"].size() == 1 && j["candidates"][0]["type"] == "srflx" && j["port"] == 5 && j["natType"] == "unknown",
        "the first STUN answer is used even from the second server; an unparseable LAN port adds no host candidate");

  check(protocol::resume("t1", "tok") == json({{"type", "resume"}, {"ticketId", "t1"}, {"token", "tok"}}), "resume message");
  check(protocol::simple("cancel") == json({{"type", "cancel"}}) && protocol::simple("ping").dump() == R"({"type":"ping"})", "cancel and ping");

  using M = protocol::ServerMessage;
  auto m = protocol::parse_server_message(R"({"type":"joined","ticketId":"abc","token":"xyz"})");
  check(m.kind == M::JOINED && m.fields_ok && m.ticket_id == "abc" && m.token == "xyz", "joined");
  m = protocol::parse_server_message(R"({"type":"joined","ticketId":"abc"})");
  check(m.kind == M::JOINED && !m.fields_ok, "joined without a token is not an acknowledgement");
  m = protocol::parse_server_message(R"({"type":"status","secondsInQueue":4.5})");
  check(m.kind == M::STATUS && m.fields_ok && m.seconds_in_queue == 4.5, "status");
  m = protocol::parse_server_message(R"({"type":"match","matchId":"mode.unranked-1","isHost":true,"players":[]})");
  check(m.kind == M::MATCH && m.fields_ok && m.match_id == "mode.unranked-1" && m.is_host, "match summary");
  m = protocol::parse_server_message(R"({"type":"error","error":"A new version has been released","latestVersion":"3.7.1"})");
  check(m.kind == M::ERROR && m.fields_ok && m.error == "A new version has been released" && m.latest_version == "3.7.1", "error with latest version");
  m = protocol::parse_server_message(R"({"type":"error","message":"no error field"})");
  check(m.kind == M::ERROR && !m.fields_ok && m.error.empty(), "error without an error field falls back to the generic message");
  check(protocol::parse_server_message("not json").kind == M::MALFORMED && protocol::parse_server_message(R"({"kind":"x"})").kind == M::MALFORMED &&
        protocol::parse_server_message(R"({"type":5})").kind == M::MALFORMED, "malformed messages are ignored");
  check(protocol::parse_server_message(R"({"type":"pong"})").kind == M::OTHER, "unknown types are ignored");
}

void match_tests() {
  using namespace slippi;
  using nlohmann::json;
  std::vector<std::string> sixteen(16, "gg");
  json match = {{"type", "match"}, {"matchId", "mode.unranked-fixture"}, {"isHost", true}, {"items", 7},
    {"players", json::array({
      {{"uid", "remote"}, {"displayName", "Them"}, {"connectCode", "THEM#2"}, {"port", 2}, {"isBot", true},
       {"ipAddress", "192.0.2.1:34567"}, {"ipAddressLan", "10.0.0.9:34567"}, {"chatMessages", sixteen},
       {"rank", {{"rating", 1500.5}, {"updateCount", 10}, {"globalPlacement", 3}, {"regionalPlacement", 4}, {"mmr", 1612.25}}}},
      {{"uid", "local"}, {"port", 1}, {"isLocalPlayer", true}, {"ipAddress", "192.0.2.1:23456"}, {"chatMessages", {"only one"}}}
    })}};
  protocol::MatchResult r;
  check(protocol::match_result(match, r), "match result decodes");
  check(r.result.id == "mode.unranked-fixture" && r.is_host && r.result.items == 7 && r.local_player_index == 0, "id, host, items, local index");
  check(r.result.players.size() == 2 && r.result.players[0].uid == "remote" && r.result.players[0].is_bot &&
        r.result.players[0].display_name == "Them" && r.result.players[0].connect_code == "THEM#2", "players kept in message order");
  const auto& rank = r.result.players[0];
  check(rank.ranked_rating == 1500.5f && rank.ranked_update_count == 10 && rank.ranked_global_placement == 3 &&
        rank.ranked_regional_placement == 4 && rank.ranked_mmr == 1612.25f, "rank fields including 3.7.0's mmr");
  check(r.result.players[0].chat_messages.size() == 16 && r.result.players[1].chat_messages.empty(), "only a full set of 16 chat messages is kept");
  check(r.remote_ips == std::vector<std::string>{"10.0.0.9:34567"}, "same public IP: the LAN address is used");
  check(r.result.stages == std::vector<uint16_t>({0x3, 0x8, 0x1C, 0x1F, 0x20, 0x2}), "default singles stages include FoD");

  match["players"][0]["ipAddress"] = "198.51.100.7:34567";
  match["stages"] = json::array({31, 32});
  check(protocol::match_result(match, r) && r.remote_ips == std::vector<std::string>{"198.51.100.7:34567"} &&
        r.result.stages == std::vector<uint16_t>({31, 32}), "different public IP: the public address; assigned stages kept");
  match["players"][0]["ipAddressLan"] = "";
  match["players"][0]["ipAddress"] = "192.0.2.1:34567";
  check(protocol::match_result(match, r) && r.remote_ips == std::vector<std::string>{"192.0.2.1:34567"}, "empty LAN address: the public one");
  match["players"][0]["rank"]["mmr"] = nullptr;
  match["players"][0]["isBot"] = "yes";
  check(protocol::match_result(match, r) && r.result.players[0].ranked_mmr == 0 && !r.result.players[0].is_bot,
        "null or mistyped fields fall back instead of throwing");
  check(!protocol::match_result(json::array(), r), "a non-object result is refused");
}

void rank_tests() {
  using slippi::protocol::player_rank;
  check(player_rank(3000, 4, 1, 1) == 0, "fewer than 5 sets: unranked");
  check(player_rank(-20, 5, 0, 0) == 1 && player_rank(765.42f, 5, 0, 0) == 1, "Bronze 1 includes negative ratings");
  check(player_rank(765.425f, 5, 0, 0) == 1 && player_rank(765.43f, 5, 0, 0) == 2, "no gaps between thresholds (765.425 was unranked before)");
  check(player_rank(913.72f, 5, 0, 0) == 3 && player_rank(1054.87f, 5, 0, 0) == 4 && player_rank(1751.83f, 5, 0, 0) == 10 &&
        player_rank(1843.0f, 5, 0, 0) == 11 && player_rank(2136.28f, 5, 0, 0) == 15, "minimum rating for each rank");
  check(player_rank(2191.75f, 5, 0, 0) == 16 && player_rank(2274.99f, 5, 0, 0) == 16 && player_rank(2275.0f, 5, 0, 0) == 17 &&
        player_rank(2350.0f, 5, 0, 0) == 18, "Master 1 to 3, with 2350 now Master 3");
  check(player_rank(2191.75f, 5, 0, 7) == 19 && player_rank(2191.74f, 5, 1, 0) == 15, "Grandmaster needs the rating and a placement");
}
}  // namespace

int main() {
  stun_tests();
  message_tests();
  match_tests();
  rank_tests();
  if (!failures) std::printf("slippi_matchmaking_protocol_test: all checks passed\n");
  return failures ? 1 : 0;
}
