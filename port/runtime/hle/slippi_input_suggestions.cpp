// Input suggestions for Slippi Online: port of slippi-rust-extensions' input-suggestions crate and its FFI glue
// (ffi/src/input_suggestions.rs) at 934f373d.
//
// The game only reads the controller once per frame, but the GameCube adapter delivers new reports several times per
// frame. Whenever a report changes the local input by enough to matter, it is sent to the opponent right away as a
// suggestion tagged with the frame it will first apply to. A client that has to predict an opponent's input uses the
// newest applicable suggestion instead of repeating the last input it received. Suggestions only ever feed
// predictions: the ASM records every prediction and rolls back when the real input differs, so the game always ends
// up running on the real inputs on every client.
//
// All state lives behind one mutex because it is reached from three threads: the adapter read thread
// (on_adapter_report), the CPU thread (frames and predictions) and the netplay thread (inputs and suggestions
// received from peers).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "slippi_input_suggestions.h"
#include "slippi_input_suggestions_impl.h"
#include "host.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace slippi::input_suggestions {
namespace detail {

// ---------------------------------------------------------------- pad.rs
namespace {

// The button bits GCAdapter::Input sets from the adapter's two button bytes.
uint16_t adapter_buttons(uint8_t byte1, uint8_t byte2) {
  // Byte 1: A B X Y Left Right Down Up. Byte 2: Start Z R L.
  static constexpr uint16_t BYTE1[8] = {0x0100, 0x0200, 0x0400, 0x0800, 0x0001, 0x0002, 0x0004, 0x0008};
  static constexpr uint16_t BYTE2[4] = {0x1000, 0x0010, 0x0020, 0x0040};
  uint16_t button = 0;
  for (int bit = 0; bit < 8; ++bit) if (byte1 & (1 << bit)) button |= BYTE1[bit];
  for (int bit = 0; bit < 4; ++bit) if (byte2 & (1 << bit)) button |= BYTE2[bit];
  return button;
}

// Subtracts the origin from a centered stick axis, first clamping the value so the result can't wrap around.
int8_t clamp_s8(int8_t value, int8_t origin) {
  if (origin > 0) {
    const int8_t min = (int8_t)(-128 + origin);
    if (value < min) value = min;
  } else if (origin < 0) {
    const int8_t max = (int8_t)(127 + origin);
    if (value > max) value = max;
  }
  return (int8_t)(uint8_t)((uint8_t)value - (uint8_t)origin);   // wrapping_sub
}

// Subtracts the origin from a trigger, stopping at zero.
uint8_t clamp_u8(uint8_t value, uint8_t origin) { return (uint8_t)(std::max(value, origin) - origin); }

// Section 2 of TriggerSendInput.asm.
void rest_clamp(int8_t& x, int8_t& y) {
  auto at_rest = [](int8_t v) { return v >= -STICK_REST_UNITS && v <= STICK_REST_UNITS; };
  if (at_rest(x) && at_rest(y)) { x = 0; y = 0; }
}

int8_t center(uint8_t value) { return (int8_t)(uint8_t)(value - 128); }

// The control stick and c-stick (x, y, x, y) as Melee sees them, in units of 1/80, with each stick pulled back inside
// the unit circle. Melee's own rounding is left out; it only matters right at a range's edge.
std::array<float, 4> melee_sticks(const Pad& pad) {
  auto clamp = [](uint8_t xb, uint8_t yb, float& ox, float& oy) {
    const float x = (float)(int8_t)xb, y = (float)(int8_t)yb;
    const float scale = std::min(80.0f / std::hypot(x, y), 1.0f);
    ox = x * scale; oy = y * scale;
  };
  std::array<float, 4> out{};
  clamp(pad[2], pad[3], out[0], out[1]);
  clamp(pad[4], pad[5], out[2], out[3]);
  return out;
}

}  // namespace

std::optional<Pad> adapter_to_pad(const AdapterPortData& port, const Calibration& calibration) {
  if (calibration.analog_mode != MELEE_ANALOG_MODE) return std::nullopt;

  // A port with nothing plugged in reads as a centered pad
  uint16_t button = 0;
  uint8_t raw[6] = {0x80, 0x80, 0x80, 0x80, 0, 0};
  if (port[0] >> 4 != 0) {
    button = adapter_buttons(port[1], port[2]);
    for (int i = 0; i < 6; ++i) raw[i] = port[3 + i];
  }

  const auto& origin = calibration.origin;
  int8_t stick_x = clamp_s8(center(raw[0]), center(origin[0]));
  int8_t stick_y = clamp_s8(center(raw[1]), center(origin[1]));
  int8_t c_x = clamp_s8(center(raw[2]), center(origin[2]));
  int8_t c_y = clamp_s8(center(raw[3]), center(origin[3]));
  const uint8_t trigger_l = clamp_u8(raw[4], origin[4]);
  const uint8_t trigger_r = clamp_u8(raw[5], origin[5]);

  rest_clamp(stick_x, stick_y);
  rest_clamp(c_x, c_y);

  return Pad{(uint8_t)(button >> 8), (uint8_t)button, (uint8_t)stick_x, (uint8_t)stick_y,
             (uint8_t)c_x, (uint8_t)c_y, trigger_l, trigger_r};
}

uint16_t wrong_buttons(const Pad& predicted, const Pad& real) {
  // The digital buttons the ASM's rollback comparison looks at
  const uint8_t p0 = predicted[0] & 0x1F, p1 = predicted[1] & 0x7F;
  const uint8_t r0 = real[0] & 0x1F, r1 = real[1] & 0x7F;
  return (uint16_t)(((p0 ^ r0) << 8) | (p1 ^ r1));
}

uint32_t analog_error(const Pad& predicted, const Pad& real) {
  auto axis = [&](int i) { return (uint32_t)std::abs((int32_t)(int8_t)predicted[i] - (int32_t)(int8_t)real[i]); };
  auto trigger = [&](int i) -> uint32_t {
    const uint8_t a = predicted[i], b = real[i];
    if (a <= TRIGGER_DEADZONE && b <= TRIGGER_DEADZONE) return 0;
    return (uint32_t)(a > b ? a - b : b - a);
  };
  return std::max(axis(2), axis(3)) + std::max(axis(4), axis(5)) + std::max(trigger(6), trigger(7));
}

// Only changes that can start a new action in Melee, from SSBM Arte's list (leaving out tap jump from a dash and
// shield drop): pressing or releasing A, B, X, Y, L, R, Z or Start; an analog trigger crossing 0.307 (light shield)
// either way; either stick leaving the deadzone on either axis; the control stick reaching dash (x ±0.8), tap jump
// (y 0.6625) or crouch (y -0.7) range; and the c-stick reaching a smash (x ±0.8, y ±0.6625). Moving back out of a
// range isn't sent; the real input brings that.
bool is_meaningful_change(const Pad& prev, const Pad& next) {
  // Byte 0 is Start (0x10), Y, X, B, A. Byte 1 is L (0x40), R, Z (0x10), then the d-pad.
  if (((prev[0] ^ next[0]) & 0x1F) != 0 || ((prev[1] ^ next[1]) & 0x70) != 0) return true;

  // 0.307 of a trigger (which Melee scales by 140) is where the ASM's trigger deadzone ends
  for (int i = 6; i < 8; ++i)
    if ((prev[i] > TRIGGER_DEADZONE) != (next[i] > TRIGGER_DEADZONE)) return true;

  // Ranges of an axis in Melee units of 1/80 (see melee_sticks)
  enum { X = 0, Y = 1, CX = 2, CY = 3 };
  using Inside = bool (*)(float);
  const Inside live = [](float v) { return std::fabs(v) >= 23.0f; };   // 0.2875, past the deadzone
  const struct { int axis; Inside inside; } ranges[12] = {
      {X, live},
      {Y, live},
      {X, [](float x) { return x >= 64.0f; }},    // Dash right, 0.8
      {X, [](float x) { return x <= -64.0f; }},   // Dash left
      {Y, [](float y) { return y >= 53.0f; }},    // Tap jump, 0.6625
      {Y, [](float y) { return y <= -56.0f; }},   // Crouch, -0.7
      {CX, live},
      {CY, live},
      {CX, [](float x) { return x >= 64.0f; }},   // Side smashes, ±0.8
      {CX, [](float x) { return x <= -64.0f; }},
      {CY, [](float y) { return y >= 53.0f; }},   // Up and down smash, ±0.6625
      {CY, [](float y) { return y <= -53.0f; }},
  };

  const auto before = melee_sticks(prev), after = melee_sticks(next);
  for (const auto& r : ranges)
    if (!r.inside(before[r.axis]) && r.inside(after[r.axis])) return true;
  return false;
}

// ---------------------------------------------------------------- remote.rs
uint32_t worse_cost(const Pad& previous, const Pad& suggested, const Pad& real) {
  const uint16_t previous_wrong = wrong_buttons(previous, real);
  const uint16_t suggested_wrong = wrong_buttons(suggested, real);
  if ((suggested_wrong & ~previous_wrong) != 0) return WRONG_BUTTON_COST;
  if (suggested_wrong == previous_wrong && analog_error(suggested, real) > analog_error(previous, real)) return FARTHER_ANALOG_COST;
  return 0;
}

void Remote::on_suggestion(int32_t target_frame, uint32_t seq, const Pad& pad) {
  const int32_t latest = latest_real ? latest_real->first : 0;
  if (ignored || target_frame <= latest || target_frame > latest + MAX_SUGGESTION_LEAD_FRAMES) return;
  if (suggestions.size() >= MAX_STORED_SUGGESTIONS) suggestions.pop_front();
  suggestions.push_back({target_frame, seq, pad});
}

// The newest suggestion that applies to `frame` when the latest real input is for latest_frame. A suggestion for
// frame T describes the input from T onward, so it applies when T <= frame, and it is only newer than the real
// inputs when T > latest_frame.
const Remote::Stored* Remote::best_suggestion(int32_t latest_frame, int32_t frame) const {
  const Stored* best = nullptr;
  for (const Stored& s : suggestions) {
    if (s.target_frame <= latest_frame || s.target_frame > frame) continue;
    if (!best || s.seq >= best->seq) best = &s;   // Iterator::max_by_key keeps the last of equal keys
  }
  return best;
}

void Remote::fill_predictions(int32_t latest_frame, const Report& latest, Predictions& out) const {
  for (size_t k = 0; k < out.size(); ++k) {
    out[k] = latest;
    if (const Stored* s = best_suggestion(latest_frame, latest_frame + 1 + (int32_t)k)) {
      // Suggestions only have the inputs; the rest of the report is zero
      out[k].fill(0);
      std::memcpy(out[k].data(), s->pad.data(), PAD_SIZE);
    }
  }
}

bool Remote::on_real_input(int32_t frame, const Pad& pad) {
  const auto previous = latest_real;
  if (previous && frame <= previous->first) return false;
  latest_real = std::make_pair(frame, pad);

  // Check the suggestion in effect for this frame against what the player actually did
  worse_bucket = worse_bucket > 0 ? worse_bucket - 1 : 0;
  const Stored* suggested = best_suggestion(frame - 1, frame);
  if (suggested && previous && previous->first == frame - 1)
    worse_bucket += worse_cost(previous->second, suggested->pad, pad) * FRAMES_PER_DRAINED_POINT;

  // Nothing at or before the latest real input is needed for predictions any more
  suggestions.erase(std::remove_if(suggestions.begin(), suggestions.end(), [&](const Stored& s) { return s.target_frame <= frame; }),
                    suggestions.end());

  if (ignored || worse_bucket <= IGNORE_AFTER_POINTS * FRAMES_PER_DRAINED_POINT) return false;
  ignored = true;
  suggestions.clear();
  return true;
}

// ---------------------------------------------------------------- lib.rs
Game::Game(const GameConfig& cfg) : config(cfg), sending(cfg.enabled && cfg.source_is_adapter) {
  for (uint8_t i = 0; i < cfg.remote_player_count; ++i) remotes.emplace_back((uint8_t)(i >= cfg.local_player_idx ? i + 1 : i));
  host::log("slippi: [InputSuggestions] Game started (enabled %d, delay %u, source port %u, source is adapter %d)",
            (int)cfg.enabled, (unsigned)cfg.delay, (unsigned)cfg.source_port + 1, (int)cfg.source_is_adapter);
}

std::optional<Pad> Game::transform(const AdapterPortData& port_data) const {
  if (!calibration) return std::nullopt;
  return adapter_to_pad(port_data, *calibration);
}

// Sends `pad` as a suggestion for the next polled frame if it says something the opponents don't know yet.
std::optional<Suggestion> Game::suggest(const Pad& pad) {
  // The ASM zeroes inputs polled before this frame, so there is nothing to suggest
  if (next_poll_frame < START_SYNC_FRAME - delay()) return std::nullopt;
  if (!last_communicated || !is_meaningful_change(*last_communicated, pad)) return std::nullopt;
  next_seq += 1;
  last_communicated = pad;
  return Suggestion{next_poll_frame + delay(), next_seq, pad};
}

std::optional<Suggestion> Game::on_adapter_report(const uint64_t poll_counts[4], const uint8_t* payload, size_t payload_len) {
  if (!can_send()) return std::nullopt;
  const size_t port = config.source_port;
  const size_t start = 1 + ADAPTER_PORT_DATA_SIZE * port;
  if (start + ADAPTER_PORT_DATA_SIZE > payload_len || port >= 4) return std::nullopt;
  AdapterPortData port_data;
  std::memcpy(port_data.data(), payload + start, ADAPTER_PORT_DATA_SIZE);
  const uint64_t handled_count = *handled_poll_count;
  const uint64_t poll_count = poll_counts[port];

  // A poll we already handled read this report or a newer one, and that input has been sent
  if (poll_count < handled_count) return std::nullopt;

  // The game polled after this report was stored, so the report is for the frame after that poll's, which we only
  // learn once the poll is handled. Keep only the newest such report.
  if (poll_count > handled_count) {
    pending = PendingReport{poll_count, port_data};
    return std::nullopt;
  }

  const auto pad = transform(port_data);
  if (!pad) return std::nullopt;
  return suggest(*pad);
}

std::optional<Suggestion> Game::on_local_frame(const LocalFrame& info) {
  if (info.calibration) calibration = info.calibration;
  if (info.poll_count) handled_poll_count = info.poll_count;

  // A skipped frame is polled again. An advanced frame runs on this frame's poll, so the next poll is for the frame
  // after it. Otherwise the next poll is for the following frame
  next_poll_frame = info.is_skip ? info.frame : info.is_advancing ? info.frame + 2 : info.frame + 1;
  if (!info.is_skip) last_communicated = info.sent_pad;

  // Release a report stored after this frame's poll, now that we know which frame that poll was
  std::optional<Pad> candidate;
  const std::optional<PendingReport> taken = pending;   // Option::take: the report is dropped unless kept below
  pending.reset();
  if (taken && handled_poll_count) {
    const PendingReport p = *taken;
    if (p.poll_count > *handled_poll_count) pending = p;   // yet another poll is in flight
    else if (p.poll_count == *handled_poll_count) candidate = transform(p.port_data);
  }

  // The skipped frame's input was never sent, so it is news if it differs from what was
  if (!candidate && info.is_skip) candidate = info.sent_pad;

  if (candidate && can_send()) return suggest(*candidate);
  return std::nullopt;
}

void Game::on_remote_input(size_t remote_idx, int32_t frame, const Pad& pad) {
  if (remote_idx >= remotes.size()) return;
  Remote& remote = remotes[remote_idx];
  if (remote.on_real_input(frame, pad))
    host::log("slippi: [InputSuggestions] Ignoring P%u's suggestions for the rest of the game from frame %d: they kept "
              "disagreeing with their real inputs", (unsigned)remote.port + 1, frame);
}

Predictions Game::fill_predictions(size_t remote_idx, int32_t latest_frame, bool is_disconnected, const Report& latest) const {
  Predictions out;
  out.fill(latest);
  if (!config.enabled || is_disconnected) return out;
  if (remote_idx < remotes.size()) remotes[remote_idx].fill_predictions(latest_frame, latest, out);
  return out;
}

}  // namespace detail

// ---------------------------------------------------------------- global state and the FFI glue
namespace {
std::mutex g_mutex;
std::optional<detail::Game> g_game;

Suggestion to_ffi(const std::optional<detail::Suggestion>& s) {
  Suggestion out;
  if (!s) return out;
  out.is_valid = true;
  out.target_frame = s->target_frame;
  out.seq = s->seq;
  std::memcpy(out.pad, s->pad.data(), sizeof out.pad);
  return out;
}

detail::Pad pad_from(const uint8_t* p) {
  detail::Pad pad;
  std::memcpy(pad.data(), p, detail::PAD_SIZE);
  return pad;
}
}  // namespace

void start_game(const GameConfig& c) {
  detail::GameConfig config;
  config.enabled = c.enabled;
  config.local_player_idx = c.local_player_idx;
  config.remote_player_count = c.remote_player_count;
  config.delay = c.delay;
  config.source_port = c.source_port;
  config.source_is_adapter = c.source_is_adapter;
  std::lock_guard<std::mutex> lk(g_mutex);
  g_game.emplace(config);
}

void end_game() {
  std::lock_guard<std::mutex> lk(g_mutex);
  g_game.reset();
}

Suggestion on_local_frame(const LocalFrame& frame) {
  detail::LocalFrame info;
  info.frame = frame.frame;
  info.is_skip = frame.is_skip;
  info.is_advancing = frame.is_advancing;
  if (frame.has_poll) info.poll_count = frame.poll_count;
  if (frame.has_origin) {
    detail::Calibration calibration;
    std::memcpy(calibration.origin.data(), frame.origin, 6);
    calibration.analog_mode = frame.analog_mode;
    info.calibration = calibration;
  }
  info.sent_pad = pad_from(frame.sent_pad);
  std::lock_guard<std::mutex> lk(g_mutex);
  if (!g_game) return {};
  return to_ffi(g_game->on_local_frame(info));
}

Suggestion on_adapter_report(const uint64_t* game_poll_counts, const uint8_t* payload, size_t payload_len) {
  if (!game_poll_counts || !payload) return {};
  std::lock_guard<std::mutex> lk(g_mutex);
  if (!g_game) return {};
  return to_ffi(g_game->on_adapter_report(game_poll_counts, payload, payload_len));
}

void on_remote_suggestion(uint8_t remote_idx, int32_t target_frame, uint32_t seq, const uint8_t* pad) {
  if (!pad) return;
  std::lock_guard<std::mutex> lk(g_mutex);
  if (g_game && remote_idx < g_game->remotes.size()) g_game->remotes[remote_idx].on_suggestion(target_frame, seq, pad_from(pad));
}

void on_remote_input(uint8_t remote_idx, int32_t frame, const uint8_t* pad) {
  if (!pad) return;
  std::lock_guard<std::mutex> lk(g_mutex);
  if (g_game) g_game->on_remote_input(remote_idx, frame, pad_from(pad));
}

void reset_remote(uint8_t remote_idx) {
  std::lock_guard<std::mutex> lk(g_mutex);
  if (g_game && remote_idx < g_game->remotes.size()) {
    detail::Remote& remote = g_game->remotes[remote_idx];
    remote = detail::Remote(remote.port);
  }
}

void fill_predictions(uint8_t remote_idx, int32_t latest_frame, bool is_disconnected, const uint8_t* latest_pad,
                      uint8_t* out, size_t out_len) {
  if (!latest_pad || !out || out_len < detail::PREDICTED_FRAME_COUNT * 12) return;
  detail::Report latest;
  std::memcpy(latest.data(), latest_pad, latest.size());
  detail::Predictions predictions;
  {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_game) predictions = g_game->fill_predictions(remote_idx, latest_frame, is_disconnected, latest);
    else predictions.fill(latest);
  }
  for (size_t k = 0; k < predictions.size(); ++k) std::memcpy(out + k * 12, predictions[k].data(), 12);
}

}  // namespace slippi::input_suggestions
