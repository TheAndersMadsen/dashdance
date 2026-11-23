// One game of the match quality port behind the global API in slippi_match_quality.h, exposed for unit tests.
// Port of slippi-rust-extensions' match-quality crate at 934f373d.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include "slippi_match_quality.h"

namespace slippi::match_quality::detail {

// The game halts once the opponent's latest input is further behind than this.
constexpr int32_t ROLLBACK_MAX_FRAMES = 7;
constexpr uint64_t FRAME_TIME_US = 16683;
// Nothing before this frame is judged, since the start of a game halts while both sides finish loading and time sync
// lines them up. Stalls up to here are ignored, and the first debt interval starts here.
constexpr int32_t WARMUP_FRAMES = 120;
// The debt is updated every 150 frames (2.5 seconds) after the warmup.
constexpr int32_t INTERVAL_FRAMES = 150;
// Once a game has gone for a minute, every debt threshold is raised by this factor.
constexpr int32_t LATE_GAME_FRAME = 3600;
constexpr double LATE_GAME_MODIFIER = 1.15;
constexpr int32_t TERMINATE_DEBT = 30;
// Debt added for an interval in the mild, bad and severe tiers. A healthy interval pays one back.
constexpr int32_t MILD_DEBT = 4;
constexpr int32_t BAD_DEBT = 8;
constexpr int32_t SEVERE_DEBT = 15;
constexpr int32_t HEALTHY_DEBT = -1;
// How far over the expected real time an interval ran (0.1 is 10% over) for each tier.
constexpr double OVERRUN_TIERS[3] = {0.1, 0.5, 0.75};
// Average rollback depth in frames for each tier. With a local delay of 2 these land around 105, 140 and 200 ms of
// ping on a clean connection, and each extra delay frame adds about 33 ms.
constexpr double DEPTH_TIERS[3] = {2.0, 3.0, 4.75};
// Rollback depth is judged as if the local player has at least this much delay.
constexpr int32_t MIN_JUDGED_DELAY = 2;
// Stalled frames closer together than this belong to the same stall event.
constexpr uint64_t STALL_MERGE_GAP_US = 500000;
// A stall event only counts once it has been halted for this many passes in total, which is about 80 ms.
constexpr uint32_t MIN_STALL_PASSES = 5;
// Debt for an interval holding at least one new stall event, however long it was. The late game modifier doesn't apply.
constexpr int32_t STALL_DEBT = 9;
// Debt for an interval whose only stalls were under the minimum length.
constexpr int32_t SHORT_STALL_DEBT = 3;

// The debt for a value measured against mild, bad and severe thresholds, each scaled by `modifier`.
int32_t tier_debt(double value, const double tiers[3], double modifier);

class Game {
 public:
  explicit Game(const GameConfig& config) : config(config) {}
  // Returns true once, when the match should be terminated.
  bool on_frame(const Frame& frame);

  GameConfig config;
  bool verdict_reached = false;        // judged too degraded to continue; only reported once
  int32_t debt = 0;
  std::optional<uint64_t> interval_start_us;   // unset until the first interval boundary
  int32_t last_interval_frame = 0;     // a frame held back for time sync is reported more than once
  uint64_t depth_sum = 0, depth_samples = 0;
  uint64_t ping_sum_us = 0, ping_samples = 0;
  std::optional<uint64_t> last_stall_us;
  uint32_t current_stall_passes = 0;   // halted passes so far in the stall event in progress, or the most recent one
  uint32_t interval_stall_events = 0;  // stall events that reached the minimum length in the current interval
  uint32_t interval_stalled_passes = 0;   // halted passes in the current interval, short stalls included
  uint32_t stall_events = 0;           // stall events so far this game

 private:
  void on_stall(const Frame& frame);
  std::optional<std::string> on_interval(const Frame& frame);
};

}  // namespace slippi::match_quality::detail
