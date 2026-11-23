// Match quality for Slippi Online: port of slippi-rust-extensions' match-quality crate and its FFI glue
// (ffi/src/match_quality.rs) at 934f373d.
//
// Decides when a match has degraded enough that it should be terminated. Instead of ping, it judges what the players
// actually experience. Each interval is scored on three signals, and the worst one feeds a leaky debt accumulator
// that ends the match once it fills up:
// - Stall events. The game halts whenever the opponent's latest input is more than ROLLBACK_MAX_FRAMES behind. A run
//   of halted frames is one stall event, and runs closer together than STALL_MERGE_GAP_US count as the same event.
//   Stalls under MIN_STALL_PASSES in total aren't events and add less debt.
// - Rollback depth. How many frames the game is running ahead of the opponent's latest received input, averaged over
//   an interval.
// - Frame timing. An interval that took longer than it should have in real time.
// Ping is never judged. It is only logged next to rollback depth so the depth tiers can be calibrated from real games.
// All calls come from the CPU thread, but the state still lives behind a mutex like the other global trackers.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "slippi_match_quality.h"
#include "slippi_match_quality_impl.h"
#include "host.h"

#include <algorithm>
#include <cstdio>
#include <mutex>

namespace slippi::match_quality {
namespace detail {

int32_t tier_debt(double value, const double tiers[3], double modifier) {
  const double mild = tiers[0] * modifier, bad = tiers[1] * modifier, severe = tiers[2] * modifier;
  if (value >= severe) return SEVERE_DEBT;
  if (value >= bad) return BAD_DEBT;
  if (value >= mild) return MILD_DEBT;
  return HEALTHY_DEBT;
}

bool Game::on_frame(const Frame& frame) {
  const int32_t delay_shortfall = std::max(MIN_JUDGED_DELAY - (int32_t)config.delay, 0);

  // A stalled frame is past the rollback limit by definition, so cap there in case the input queue is empty and the
  // latest frame reads as 0
  const int32_t depth = std::clamp(frame.frame - frame.latest_remote_frame - delay_shortfall, 0, ROLLBACK_MAX_FRAMES + 1);
  depth_sum += (uint64_t)depth;
  depth_samples += 1;

  if (frame.ping_us > 0) {
    ping_sum_us += frame.ping_us;
    ping_samples += 1;
  }

  std::optional<std::string> reason;
  if (frame.is_stall) {
    on_stall(frame);
  } else if (frame.frame >= WARMUP_FRAMES && (frame.frame - WARMUP_FRAMES) % INTERVAL_FRAMES == 0 &&
             frame.frame != last_interval_frame) {
    last_interval_frame = frame.frame;
    reason = on_interval(frame);
  }

  if (!reason) return false;

  // A match that can't be terminated keeps being tracked, so its log covers the whole game
  if (verdict_reached) return false;
  verdict_reached = true;

  if (!config.can_terminate) {
    host::log("slippi: [MatchQuality] A ranked match would have been terminated here: %s (frame %d)", reason->c_str(), frame.frame);
    return false;
  }

  host::log("slippi: [MatchQuality] Terminating match: %s (frame %d)", reason->c_str(), frame.frame);
  return true;
}

void Game::on_stall(const Frame& frame) {
  if (frame.frame <= WARMUP_FRAMES) return;

  interval_stalled_passes += 1;

  const bool is_new_event = !last_stall_us || (frame.time_us > *last_stall_us ? frame.time_us - *last_stall_us : 0) > STALL_MERGE_GAP_US;
  last_stall_us = frame.time_us;

  if (is_new_event) current_stall_passes = 0;
  current_stall_passes += 1;

  // An event counts once, on the pass that reaches the minimum length
  if (current_stall_passes != MIN_STALL_PASSES) return;

  interval_stall_events += 1;
  stall_events += 1;

  host::log("slippi: [MatchQuality] Stall event. %u this game (frame %d, latest remote frame %d)", stall_events, frame.frame,
            frame.latest_remote_frame);
}

std::optional<std::string> Game::on_interval(const Frame& frame) {
  const uint64_t sum = depth_sum, samples = depth_samples, ping_sum = ping_sum_us, pings = ping_samples;
  const uint32_t events = interval_stall_events, stalled = interval_stalled_passes;
  depth_sum = depth_samples = ping_sum_us = ping_samples = 0;
  interval_stall_events = interval_stalled_passes = 0;

  // The first boundary only starts the clock
  const std::optional<uint64_t> start_us = interval_start_us;
  interval_start_us = frame.time_us;
  if (!start_us) return std::nullopt;

  const uint64_t expected_us = FRAME_TIME_US * (uint64_t)INTERVAL_FRAMES;
  const double ratio = (double)(frame.time_us > *start_us ? frame.time_us - *start_us : 0) / (double)expected_us;
  const double avg_depth = (double)sum / (double)std::max<uint64_t>(samples, 1);
  const double avg_ping_ms = (double)ping_sum / (double)std::max<uint64_t>(pings, 1) / 1000.0;

  const double modifier = frame.frame > LATE_GAME_FRAME ? LATE_GAME_MODIFIER : 1.0;
  const int32_t speed_debt = tier_debt(ratio - 1.0, OVERRUN_TIERS, modifier);
  const int32_t depth_debt = tier_debt(avg_depth, DEPTH_TIERS, modifier);
  const int32_t stall_debt = events > 0 ? STALL_DEBT : stalled > 0 ? SHORT_STALL_DEBT : HEALTHY_DEBT;

  // Take the worst of the three rather than summing: a bad connection stalls, deepens rollbacks and stretches the
  // interval (halting to wait for inputs) all at once, so summing would count one cause several times
  const int32_t change = std::max({speed_debt, depth_debt, stall_debt});
  debt = std::max(debt + change, 0);

  host::log("slippi: [MatchQuality] Debt %+d (speed: %d, depth: %d, stall: %d, ratio: %.2f, avg depth: %.2f, delay: %u, "
            "avg ping: %.1fms, stall events: %u, stalled passes: %u). Now %d/%d (frame %d)",
            change, speed_debt, depth_debt, stall_debt, ratio, avg_depth, (unsigned)config.delay, avg_ping_ms, events, stalled,
            debt, TERMINATE_DEBT, frame.frame);

  if (debt < TERMINATE_DEBT) return std::nullopt;
  char text[256];
  std::snprintf(text, sizeof text,
                "poor performance debt %d/%d (ratio: %.2f, avg depth: %.2f, avg ping: %.1fms, stall events this game: %u)", debt,
                TERMINATE_DEBT, ratio, avg_depth, avg_ping_ms, stall_events);
  return std::string(text);
}

}  // namespace detail

namespace {
std::mutex g_mutex;
std::optional<detail::Game> g_game;
}  // namespace

void start_game(const GameConfig& config) {
  std::lock_guard<std::mutex> lk(g_mutex);
  g_game.emplace(config);
}

void end_game() {
  std::lock_guard<std::mutex> lk(g_mutex);
  g_game.reset();
}

bool on_frame(const Frame& frame) {
  std::lock_guard<std::mutex> lk(g_mutex);
  return g_game && g_game->on_frame(frame);
}

}  // namespace slippi::match_quality
