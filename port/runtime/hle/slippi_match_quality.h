// Match quality (Slippi 3.7.0): judges an online match from stalls, rollback depth and frame timing, and decides when a
// ranked match is too degraded to continue. Port of slippi-rust-extensions' match-quality crate; the API mirrors its
// FFI (slprs_match_quality_*).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace slippi::match_quality {

struct GameConfig {
  bool can_terminate = false;   // whether the match may be terminated (ranked); other modes are tracked and logged only
  uint8_t delay = 0;            // the local player's delay frames
};

// One pass through the online frame handler.
struct Frame {
  int32_t frame = 0;
  uint64_t time_us = 0;
  bool is_stall = false;           // halted because the opponent's inputs are past the rollback limit
  int32_t latest_remote_frame = 0; // the newest frame we have the opponent's input for
  uint64_t ping_us = 0;            // the opponent's most recent ping measurement, or 0 if none yet
};

void start_game(const GameConfig& config);
void end_game();
// Returns true once, when the match should be terminated for poor quality.
bool on_frame(const Frame& frame);

}  // namespace slippi::match_quality
