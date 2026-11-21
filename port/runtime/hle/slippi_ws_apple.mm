// SPDX-License-Identifier: GPL-2.0-or-later
#import <Foundation/Foundation.h>
#include "slippi_ws_apple.h"
#include <Network/Network.h>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <utility>

namespace slippi::ws {

struct Connection::State {
  dispatch_queue_t queue = nil;
  nw_connection_t connection = nil;
  std::mutex mutex;
  std::condition_variable changed;
  bool ready = false, ended = false, cancelled = false;
  std::string error;
  std::deque<std::pair<Event, std::string>> inbox;

  void push(Event e, std::string payload) {
    { std::lock_guard<std::mutex> lock(mutex); inbox.emplace_back(e, std::move(payload)); if (e == Event::CLOSED || e == Event::FAILED) ended = true; }
    changed.notify_all();
  }
};

namespace {
std::string describe(nw_error_t error) {
  if (!error) return "unknown error";
  CFErrorRef cf = nw_error_copy_cf_error(error);
  if (!cf) return "network error " + std::to_string(nw_error_get_error_code(error));
  NSString* text = (__bridge_transfer NSString*)CFErrorCopyDescription(cf);
  CFRelease(cf);
  return text ? std::string(text.UTF8String) : "network error " + std::to_string(nw_error_get_error_code(error));
}

std::string bytes_of(dispatch_data_t data) {
  __block std::string out;
  if (!data) return out;
  dispatch_data_apply(data, ^bool(dispatch_data_t, size_t, const void* buffer, size_t size) {
    out.append(static_cast<const char*>(buffer), size);
    return true;
  });
  return out;
}

// Keeps exactly one receive outstanding; each completion queues its message and asks for the next.
void receive_next(std::shared_ptr<Connection::State> state, nw_connection_t connection) {
  nw_connection_receive_message(connection, ^(dispatch_data_t content, nw_content_context_t context, bool, nw_error_t error) {
    bool stop = false;
    if (context) {
      nw_protocol_definition_t ws = nw_protocol_copy_ws_definition();
      nw_protocol_metadata_t metadata = nw_content_context_copy_protocol_metadata(context, ws);
      const nw_ws_opcode_t opcode = metadata ? nw_ws_metadata_get_opcode(metadata) : nw_ws_opcode_invalid;
      if (opcode == nw_ws_opcode_text) state->push(Event::TEXT, bytes_of(content));
      else if (opcode == nw_ws_opcode_close) { state->push(Event::CLOSED, "close frame"); stop = true; }
      else if (opcode == nw_ws_opcode_binary || opcode == nw_ws_opcode_ping || opcode == nw_ws_opcode_pong) state->push(Event::CONTROL, {});
      else if (!content && nw_content_context_get_is_final(context)) { state->push(Event::CLOSED, "connection ended"); stop = true; }
    }
    if (error && !stop) { state->push(Event::FAILED, describe(error)); stop = true; }
    if (!stop) receive_next(state, connection);
  });
}

bool send_message(nw_connection_t connection, nw_ws_opcode_t opcode, const std::string& payload, int timeout_ms, std::string* error) {
  if (!connection) { if (error) *error = "not connected"; return false; }
  nw_protocol_metadata_t metadata = nw_ws_create_metadata(opcode);
  if (opcode == nw_ws_opcode_close) nw_ws_metadata_set_close_code(metadata, nw_ws_close_code_normal_closure);
  nw_content_context_t context = nw_content_context_create(opcode == nw_ws_opcode_close ? "close" : "text");
  nw_content_context_set_metadata_for_protocol(context, metadata);
  dispatch_data_t data = payload.empty() ? dispatch_data_empty
                                         : dispatch_data_create(payload.data(), payload.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  auto done = std::make_shared<std::pair<bool, std::string>>(false, std::string());
  dispatch_semaphore_t sent = dispatch_semaphore_create(0);
  nw_connection_send(connection, data, context, true, ^(nw_error_t send_error) {
    done->first = send_error == nil;
    if (send_error) done->second = describe(send_error);
    dispatch_semaphore_signal(sent);
  });
  if (dispatch_semaphore_wait(sent, dispatch_time(DISPATCH_TIME_NOW, int64_t(timeout_ms) * NSEC_PER_MSEC)) != 0) {
    if (error) *error = "send timed out";
    return false;
  }
  if (!done->first && error) *error = done->second;
  return done->first;
}
}  // namespace

Connection::Connection(std::shared_ptr<State> state) : state_(std::move(state)) {}
Connection::~Connection() { close(); }

std::unique_ptr<Connection> Connection::open(const std::string& url, int tcp_timeout_ms, int total_timeout_ms,
                                             const std::atomic<bool>* cancel, std::string* error) {
  @autoreleasepool {
    nw_endpoint_t endpoint = nw_endpoint_create_url(url.c_str());
    if (!endpoint) { if (error) *error = "invalid URL"; return nullptr; }
    const bool secure = url.rfind("wss://", 0) == 0;
    const int tcp_seconds = std::max(1, tcp_timeout_ms / 1000);
    nw_parameters_t parameters = nw_parameters_create_secure_tcp(
        secure ? NW_PARAMETERS_DEFAULT_CONFIGURATION : NW_PARAMETERS_DISABLE_PROTOCOL,
        ^(nw_protocol_options_t tcp) {
          nw_tcp_options_set_no_delay(tcp, true);
          nw_tcp_options_set_connection_timeout(tcp, (uint32_t)tcp_seconds);
        });
    nw_protocol_stack_t stack = nw_parameters_copy_default_protocol_stack(parameters);
    nw_protocol_options_t ws = nw_ws_create_options(nw_ws_version_13);
    nw_ws_options_set_auto_reply_ping(ws, true);
    nw_protocol_stack_prepend_application_protocol(stack, ws);
    nw_protocol_options_t ip = nw_protocol_stack_copy_internet_protocol(stack);
    nw_ip_options_set_version(ip, nw_ip_version_4);   // netplay is IPv4: the service must observe that address

    auto state = std::make_shared<State>();
    state->queue = dispatch_queue_create("gg.slippi.matchmaking.ws", DISPATCH_QUEUE_SERIAL);
    nw_connection_t connection = nw_connection_create(endpoint, parameters);
    if (!connection) { if (error) *error = "cannot create connection"; return nullptr; }
    state->connection = connection;
    std::shared_ptr<State> captured = state;
    nw_connection_set_queue(connection, state->queue);
    nw_connection_set_state_changed_handler(connection, ^(nw_connection_state_t s, nw_error_t e) {
      if (s == nw_connection_state_ready) {
        { std::lock_guard<std::mutex> lock(captured->mutex); captured->ready = true; }
        captured->changed.notify_all();
        receive_next(captured, connection);
      } else if (s == nw_connection_state_waiting || s == nw_connection_state_failed) {
        // "Waiting" means the stack would retry later (no route, refused); the caller retries itself.
        bool was_ready;
        { std::lock_guard<std::mutex> lock(captured->mutex); was_ready = captured->ready; if (captured->error.empty()) captured->error = describe(e); }
        captured->push(Event::FAILED, describe(e));
        if (!was_ready || s == nw_connection_state_failed) nw_connection_cancel(connection);
      } else if (s == nw_connection_state_cancelled) {
        { std::lock_guard<std::mutex> lock(captured->mutex); captured->cancelled = true; }
        captured->push(Event::CLOSED, "cancelled");
        nw_connection_set_state_changed_handler(connection, nil);   // breaks the handler's reference to itself
      }
    });
    nw_connection_start(connection);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(total_timeout_ms);
    std::unique_lock<std::mutex> lock(state->mutex);
    while (!state->ready && !state->ended) {
      if (cancel && cancel->load()) { lock.unlock(); nw_connection_cancel(connection); if (error) *error = "cancelled"; return nullptr; }
      if (std::chrono::steady_clock::now() >= deadline) {
        lock.unlock(); nw_connection_cancel(connection);
        if (error) *error = "timed out opening the connection";
        return nullptr;
      }
      state->changed.wait_for(lock, std::chrono::milliseconds(50));
    }
    if (!state->ready) {
      std::string why = state->error.empty() ? "connection failed" : state->error;
      lock.unlock();
      nw_connection_cancel(connection);
      if (error) *error = why;
      return nullptr;
    }
    return std::unique_ptr<Connection>(new Connection(std::move(state)));
  }
}

bool Connection::send_text(const std::string& text, std::string* error) {
  nw_connection_t connection;
  { std::lock_guard<std::mutex> lock(state_->mutex); connection = state_->connection; }
  @autoreleasepool { return send_message(connection, nw_ws_opcode_text, text, 5000, error); }
}

Event Connection::receive(std::string* text, int timeout_ms, std::string* detail) {
  std::unique_lock<std::mutex> lock(state_->mutex);
  if (state_->inbox.empty())
    state_->changed.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return !state_->inbox.empty(); });
  if (state_->inbox.empty()) return Event::TIMEOUT;
  auto [event, payload] = std::move(state_->inbox.front());
  state_->inbox.pop_front();
  if (event == Event::TEXT) { if (text) *text = std::move(payload); }
  else if (detail) *detail = std::move(payload);
  return event;
}

void Connection::close() {
  if (!state_) return;
  nw_connection_t connection;
  bool live;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    connection = state_->connection;
    live = state_->ready && !state_->ended && !state_->cancelled;
    state_->connection = nil;
  }
  if (!connection) return;
  @autoreleasepool {
    // Like tungstenite's close(None) followed by dropping the socket: send the close frame and go.
    if (live) send_message(connection, nw_ws_opcode_close, {}, 1000, nullptr);
    nw_connection_cancel(connection);
  }
}

}  // namespace slippi::ws
