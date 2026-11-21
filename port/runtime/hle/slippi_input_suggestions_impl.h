// The pieces of the input suggestions port behind the global API in slippi_input_suggestions.h, exposed so unit
// tests can drive one game (or a sender and a receiver) without the global state. Port of slippi-rust-extensions'
// input-suggestions crate at 934f373d: pad.rs (adapter report -> pad, how far apart two pads are), remote.rs (one
// remote player's suggestions and predictions) and lib.rs (one game).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

namespace slippi::input_suggestions::detail {

// ---------------------------------------------------------------- pad.rs
// Reproduces the processing between a GameCube adapter report and the 8 input bytes Slippi sends over the network
// for a frame, plus how far apart two of those inputs are:
// 1. GCAdapter::Input turns the adapter's bytes for a port into buttons, sticks and triggers.
// 2. The game's PADRead centers the sticks and subtracts the origin the controller reported, clamping so nothing
//    wraps around. Melee polls in analog mode 3, where every stick and trigger arrives at full precision.
// 3. TriggerSendInput.asm snaps a stick resting within 2 units of center to zero.
// The first 8 bytes of the game's 12-byte pad report are the inputs: buttons (big endian), stick x/y, c-stick x/y,
// L, R. Suggestions send only these.

constexpr size_t PAD_SIZE = 8;
using Pad = std::array<uint8_t, PAD_SIZE>;

// The adapter's bytes for one port: status, two button bytes, stick x/y, c-stick x/y, L, R.
constexpr size_t ADAPTER_PORT_DATA_SIZE = 9;
using AdapterPortData = std::array<uint8_t, ADAPTER_PORT_DATA_SIZE>;

// START_SYNC_FRAME in Online.s. The ASM zeroes every input polled before START_SYNC_FRAME - delay.
constexpr int32_t START_SYNC_FRAME = 78;
// Triggers at or below this value are treated as released by the ASM's rollback comparison.
constexpr uint8_t TRIGGER_DEADZONE = 42;
// Sticks with both axes within this many units of center are snapped to center by the ASM.
constexpr int8_t STICK_REST_UNITS = 2;
// The analog mode Melee polls the controller in.
constexpr uint8_t MELEE_ANALOG_MODE = 3;

// The origin the controller reported to the game and the analog mode the game set.
struct Calibration {
  std::array<uint8_t, 6> origin{};   // stick x/y, c-stick x/y, L, R as reported (unsigned, centered on 128)
  uint8_t analog_mode = 0;
  bool operator==(const Calibration& o) const { return origin == o.origin && analog_mode == o.analog_mode; }
};

// Converts the adapter's bytes for a port into the pad the game sends to the opponent, or nothing if the game isn't
// polling in Melee's analog mode.
std::optional<Pad> adapter_to_pad(const AdapterPortData& port, const Calibration& calibration);
// The digital buttons that `predicted` has wrong compared with `real`, as a bit mask.
uint16_t wrong_buttons(const Pad& predicted, const Pad& real);
// How far predicted's sticks and triggers are from real's: the largest difference on either axis of each stick plus
// the largest on either trigger, where two triggers inside the deadzone count as equal.
uint32_t analog_error(const Pad& predicted, const Pad& real);
// Whether `next` is worth suggesting after `prev` was sent: only changes that can start a new action in Melee.
bool is_meaningful_change(const Pad& prev, const Pad& next);

// ---------------------------------------------------------------- remote.rs
// The number of frames after a player's latest received frame that the ASM can ask a prediction for.
constexpr size_t PREDICTED_FRAME_COUNT = 7;
using Report = std::array<uint8_t, 12>;
using Predictions = std::array<Report, PREDICTED_FRAME_COUNT>;

// Suggestions for frames further than this past the latest real input are treated as garbage.
constexpr int32_t MAX_SUGGESTION_LEAD_FRAMES = 120;
// Caps memory if a peer floods suggestions.
constexpr size_t MAX_STORED_SUGGESTIONS = 512;
// A player whose suggestions don't match their real inputs only makes the game look worse. Each suggestion is checked
// once, against the real input for the frame it targets, and a mistake the player's previous input didn't make adds
// its cost to a bucket that drains one point every FRAMES_PER_DRAINED_POINT frames. Past IGNORE_AFTER_POINTS the
// player's suggestions are ignored for the rest of the game.
constexpr uint32_t IGNORE_AFTER_POINTS = 15;
constexpr uint32_t FRAMES_PER_DRAINED_POINT = 200;
constexpr uint32_t WRONG_BUTTON_COST = 3;     // a wrong button the previous input had right
constexpr uint32_t FARTHER_ANALOG_COST = 1;   // a stick or trigger further off than the previous input

// What a suggestion costs for being further from the real input than the player's previous input was.
uint32_t worse_cost(const Pad& previous, const Pad& suggested, const Pad& real);

class Remote {
 public:
  explicit Remote(uint8_t port) : port(port) {}
  void on_suggestion(int32_t target_frame, uint32_t seq, const Pad& pad);
  // Fills `out` with the input the ASM should use for each of the frames after latest_frame: the newest applicable
  // suggestion, or `latest` (the latest received input) when there is none.
  void fill_predictions(int32_t latest_frame, const Report& latest, Predictions& out) const;
  // Returns true when this input is the one that got the player's suggestions ignored.
  bool on_real_input(int32_t frame, const Pad& pad);

  uint8_t port;
  struct Stored { int32_t target_frame; uint32_t seq; Pad pad; };
  std::deque<Stored> suggestions;
  std::optional<std::pair<int32_t, Pad>> latest_real;   // the newest real input received, and its frame
  uint32_t worse_bucket = 0;                            // in frames' worth of draining
  bool ignored = false;

 private:
  const Stored* best_suggestion(int32_t latest_frame, int32_t frame) const;
};

// ---------------------------------------------------------------- lib.rs
struct GameConfig {
  bool enabled = false;            // send suggestions and use the opponent's for predictions
  uint8_t local_player_idx = 0;
  uint8_t remote_player_count = 0;
  uint8_t delay = 0;
  uint8_t source_port = 0;         // the controller port the game reads the local player's inputs from
  bool source_is_adapter = false;  // suggestions are only produced for a GameCube adapter port
};

// Facts about a local frame, reported right after the game polled and sent its input.
struct LocalFrame {
  int32_t frame = 0;
  bool is_skip = false;            // skipped for time sync: the input was not sent and the same frame is polled again
  bool is_advancing = false;
  std::optional<uint64_t> poll_count;   // game polls of the local port so far, this frame's included
  std::optional<Calibration> calibration;
  Pad sent_pad{};                  // the input the game sent (or would have, on a skipped frame)
};

struct Suggestion {
  int32_t target_frame = 0;        // the first frame (sender's numbering, delay included) the input applies to
  uint32_t seq = 0;
  Pad pad{};
  bool operator==(const Suggestion& o) const { return target_frame == o.target_frame && seq == o.seq && pad == o.pad; }
};

class Game {
 public:
  explicit Game(const GameConfig& config);
  // poll_counts are the game's poll counts of each port when the report was stored.
  std::optional<Suggestion> on_adapter_report(const uint64_t poll_counts[4], const uint8_t* payload, size_t payload_len);
  std::optional<Suggestion> on_local_frame(const LocalFrame& info);
  void on_remote_input(size_t remote_idx, int32_t frame, const Pad& pad);
  Predictions fill_predictions(size_t remote_idx, int32_t latest_frame, bool is_disconnected, const Report& latest) const;

  GameConfig config;
  bool sending;                    // enabled and the local player is on a GameCube adapter port
  std::optional<Calibration> calibration;
  std::optional<uint64_t> handled_poll_count;   // as of the frame most recently handled by on_local_frame
  int32_t next_poll_frame = 1;     // the frame the next game poll will be sent as (before delay is added)
  std::optional<Pad> last_communicated;   // the newest input the opponents know about from us
  struct PendingReport { uint64_t poll_count; AdapterPortData port_data; };
  std::optional<PendingReport> pending;   // stored after a game poll not handled yet; it is for the frame after it
  uint32_t next_seq = 0;
  std::vector<Remote> remotes;

 private:
  int32_t delay() const { return (int32_t)config.delay; }
  bool can_send() const { return sending && calibration.has_value() && handled_poll_count.has_value(); }
  std::optional<Pad> transform(const AdapterPortData& port_data) const;
  std::optional<Suggestion> suggest(const Pad& pad);
};

}  // namespace slippi::input_suggestions::detail
