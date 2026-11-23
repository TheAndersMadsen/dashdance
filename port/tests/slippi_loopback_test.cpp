// Exercises the real NetplayClient against an in-process ENet reference peer.
// Both UDP sockets bind 127.0.0.1 on ephemeral ports. No identity, matchmaking,
// reporting, DNS, installed profile or game image is involved.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "slippi_net.h"
#include <enet/enet.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

#include "slippi_net_test_stubs.h"

namespace host { void log(const char*, ...) {} }

namespace {
using Clock = std::chrono::steady_clock;
int failures = 0;
void check(bool condition, const char* message) {
  if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
struct HostDeleter { void operator()(ENetHost* host) const { if (host) enet_host_destroy(host); } };
using FixtureHost = std::unique_ptr<ENetHost, HostDeleter>;
}

int main() {
  using namespace slippi;
  using Status = NetplayClient::ConnectStatus;
  check(enet_ready(), "real ENet initialization");
  if (failures) return 1;

  // Invalid configuration fails before constructing a UDP host or a worker.
  { NetplayClient invalid({}, {}, 0, 0, true, 0, "127.0.0.1");
    check(invalid.GetSlippiConnectStatus() == Status::FAILED, "zero peers is not a connected match"); }
  { NetplayClient invalid({"127.0.0.1"}, {1}, 1, 0, true, 0, "not-an-ip");
    check(invalid.GetSlippiConnectStatus() == Status::FAILED, "invalid local binding is rejected without DNS"); }

  ENetAddress bind{};
  bind.host = ENET_HOST_TO_NET_32(0x7F000001u);
  bind.port = 0;
  FixtureHost fixture(enet_host_create(&bind, 2, 3, 0, 0));
  check(fixture != nullptr, "loopback fixture binds an ephemeral port");
  if (!fixture) return 1;
  ENetAddress local{};
  check(enet_socket_get_address(fixture->socket, &local) == 0 &&
        local.host == bind.host && local.port != 0, "fixture is confined to IPv4 loopback");
  if (failures) return 1;

  NetplayClient client({"127.0.0.1"}, {local.port}, 1, 0, true, 0, "127.0.0.1");
  ENetPeer* peer = nullptr;
  auto deadline = Clock::now() + std::chrono::seconds(5);
  while (Clock::now() < deadline && (!peer || client.GetSlippiConnectStatus() != Status::CONNECTED)) {
    ENetEvent event{};
    if (enet_host_service(fixture.get(), &event, 10) > 0) {
      if (event.type == ENET_EVENT_TYPE_CONNECT) peer = event.peer;
      if (event.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(event.packet);
    }
  }
  check(peer && client.GetSlippiConnectStatus() == Status::CONNECTED, "real ENet connection completes");
  if (!peer || client.GetSlippiConnectStatus() != Status::CONNECTED) return 1;
  check(peer->address.host == bind.host, "native client is also confined to IPv4 loopback");

  // The game's 12-byte pad report: 8 input bytes, then the RNG bits Slippi 3.7.0 sends with them, then padding.
  const uint8_t inputs[PAD_FULL_SIZE] = {0x01, 0x00, 0x7F, 0x80, 0x01, 0xFF, 0x22, 0x33, 0x5A, 0xEE, 0xEE, 0xEE};
  client.SendSlippiPad(std::make_unique<Pad>(1, 0, 0x12345678u, inputs));
  std::vector<uint8_t> received;
  deadline = Clock::now() + std::chrono::seconds(5);
  while (Clock::now() < deadline && received.empty()) {
    ENetEvent event{};
    if (enet_host_service(fixture.get(), &event, 10) > 0 && event.type == ENET_EVENT_TYPE_RECEIVE) {
      if (event.packet->dataLength && event.packet->data[0] == NP_MSG_SLIPPI_PAD)
        received.assign(event.packet->data, event.packet->data + event.packet->dataLength);
      enet_packet_destroy(event.packet);
    }
  }
  const std::vector<uint8_t> expected = {
    0x80, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0x12, 0x34, 0x56, 0x78,
    0x01, 0x00, 0x7F, 0x80, 0x01, 0xFF, 0x22, 0x33, 0x5A,
  };
  check(received == expected, "native client sends the exact Slippi 3.7.0 pad/checksum wire bytes (9 per frame)");

  // Reference peer sends one valid remote frame through the same ENet channel.
  const std::vector<uint8_t> remote = {
    0x80, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0x87, 0x65, 0x43, 0x21,
    0x02, 0x00, 0x40, 0xC0, 0x00, 0x00, 0x11, 0x44, 0xA5,
  };
  ENetPacket* packet = enet_packet_create(remote.data(), remote.size(), ENET_PACKET_FLAG_UNSEQUENCED);
  check(packet && enet_peer_send(peer, 1, packet) == 0, "reference peer queues a valid pad");
  enet_host_flush(fixture.get());
  bool got_pad = false, got_ack = false;
  deadline = Clock::now() + std::chrono::seconds(5);
  while (Clock::now() < deadline && (!got_pad || !got_ack)) {
    auto pad = client.GetSlippiRemotePad(0, 1);
    got_pad = pad->latest_frame == 1 && pad->checksum == 0x87654321u && pad->player_idx == 1 &&
              pad->data.size() == PAD_FULL_SIZE && std::equal(remote.begin() + 14, remote.end(), pad->data.begin());
    ENetEvent event{};
    if (enet_host_service(fixture.get(), &event, 10) > 0 && event.type == ENET_EVENT_TYPE_RECEIVE) {
      const std::vector<uint8_t> ack(event.packet->data, event.packet->data + event.packet->dataLength);
      if (ack == std::vector<uint8_t>({0x81, 0, 0, 0, 1, 0})) got_ack = true;
      enet_packet_destroy(event.packet);
    }
  }
  check(got_pad, "native client receives exact remote input and checksum");
  check(got_ack, "native client returns the exact Slippi acknowledgement");

  client.ForceDisconnect(NetplayClient::DisconnectReason::POOR_PERFORMANCE);
  check(client.GetSlippiConnectStatus() == Status::DISCONNECTED &&
        client.GetDisconnectReason() == NetplayClient::DisconnectReason::POOR_PERFORMANCE,
        "explicit disconnect keeps its source reason");
  deadline = Clock::now() + std::chrono::seconds(5);
  bool disconnected = false;
  while (Clock::now() < deadline && !disconnected) {
    ENetEvent event{};
    if (enet_host_service(fixture.get(), &event, 10) > 0) {
      if (event.type == ENET_EVENT_TYPE_DISCONNECT) disconnected = event.data == 1;
      if (event.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(event.packet);
    }
  }
  check(disconnected, "disconnect reason reaches the real ENet peer");
  return failures ? 1 : 0;
}
