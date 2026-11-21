// A small blocking WebSocket client for the Slippi matchmaking service, shared by macOS, iOS and
// visionOS. Built on Network.framework's WebSocket protocol so the connection can be held to IPv4,
// as slippi-rust-extensions' matchmaking client does (netplay is IPv4, so the address the service
// observes must be the IPv4 one); NSURLSessionWebSocketTask has no way to pin the IP family.
// Every call is synchronous and meant for one worker thread; nothing here blocks forever.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <memory>
#include <string>

namespace slippi::ws {

enum class Event {
  TEXT,      // a text message; `text` holds it
  CONTROL,   // a binary, ping or pong frame: proof the service is there, nothing to read
  TIMEOUT,   // nothing arrived within the timeout
  CLOSED,    // the service closed the connection (close frame or orderly end)
  FAILED,    // the connection broke; `detail` says why
};

class Connection {
 public:
  ~Connection();
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  // Opens `url` (ws:// or wss://) over IPv4 and completes the WebSocket handshake. TCP gets
  // `tcp_timeout_ms` and the whole open `total_timeout_ms`. Returns null with `error` filled on
  // failure; also returns null (error "cancelled") once `*cancel` becomes true while waiting.
  static std::unique_ptr<Connection> open(const std::string& url, int tcp_timeout_ms, int total_timeout_ms,
                                          const std::atomic<bool>* cancel, std::string* error);
  // Sends one text message and waits (up to 5 s) for the stack to take it.
  bool send_text(const std::string& text, std::string* error = nullptr);
  // Waits up to `timeout_ms` for the next message or connection event.
  Event receive(std::string* text, int timeout_ms, std::string* detail = nullptr);
  // Sends a close frame (best effort, short wait) and tears the connection down. Idempotent.
  void close();

  struct State;
 private:
  explicit Connection(std::shared_ptr<State> state);
  std::shared_ptr<State> state_;
};

}  // namespace slippi::ws
