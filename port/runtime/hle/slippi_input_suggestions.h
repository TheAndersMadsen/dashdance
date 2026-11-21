// Input suggestions (Slippi 3.7.0): mid-frame input messages that let opponents predict our next input before the
// frame's real input arrives, which feels like 16.7 ms less ping. Port of slippi-rust-extensions' input-suggestions
// crate; the API mirrors its FFI (slprs_input_suggestions_*). The ASM checks every prediction against the real input
// once it arrives and rolls back if they differ, so predictions never affect sync.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

namespace slippi::input_suggestions {

struct GameConfig {
  bool enabled = false;
  uint8_t local_player_idx = 0;
  uint8_t remote_player_count = 0;
  uint8_t delay = 0;
  uint8_t source_port = 0;
  bool source_is_adapter = false;
};

// A suggestion the caller should send to the opponents. Ignore it unless is_valid is set.
struct Suggestion {
  bool is_valid = false;
  int32_t target_frame = 0;
  uint32_t seq = 0;
  uint8_t pad[8] = {};
};

// What happened on a local frame: how many times the game has polled the controller port and what it sent.
struct LocalFrame {
  int32_t frame = 0;
  bool is_skip = false;
  bool is_advancing = false;
  bool has_poll = false;
  uint64_t poll_count = 0;
  bool has_origin = false;
  uint8_t origin[6] = {};   // stick x/y, c-stick x/y, L, R as reported to the game
  uint8_t analog_mode = 0;
  uint8_t sent_pad[8] = {};
};

// Starts tracking a game. CPU thread, when the game's first online frame is handled.
void start_game(const GameConfig& config);
// Stops tracking the current game, if any.
void end_game();
// Reports a local frame. CPU thread, right after the frame's input was sent and Slippi decided whether to advance.
Suggestion on_local_frame(const LocalFrame& frame);
// Reports an adapter input report. Adapter read thread. game_poll_counts points at four counts, one per port.
Suggestion on_adapter_report(const uint64_t* game_poll_counts, const uint8_t* payload, size_t payload_len);
// Reports a suggestion received from a remote player. pad points at 8 bytes. Netplay thread.
void on_remote_suggestion(uint8_t remote_idx, int32_t target_frame, uint32_t seq, const uint8_t* pad);
// Reports a real input received from a remote player. pad points at 8 bytes. Netplay thread.
void on_remote_input(uint8_t remote_idx, int32_t frame, const uint8_t* pad);
// Forgets a remote player's suggestions and inputs, for when they start a new game.
void reset_remote(uint8_t remote_idx);
// Writes the input the ASM should use for each of the 7 frames after latest_frame into out (7 pad reports of 12 bytes).
// latest_pad points at the 12-byte latest received input, which every slot already holds unless a suggestion applies.
void fill_predictions(uint8_t remote_idx, int32_t latest_frame, bool is_disconnected, const uint8_t* latest_pad,
                      uint8_t* out, size_t out_len);

// The GameCube adapter as input suggestions sees it. Whether the game's controller on this port comes from the
// adapter; how many times the game has polled that port; and the origin (neutral) the adapter reported for it.
bool adapter_has_port(uint8_t port);
uint64_t adapter_game_poll_count(uint8_t port);
bool adapter_origin(uint8_t port, uint8_t origin[6], uint8_t* analog_mode);

}  // namespace slippi::input_suggestions
