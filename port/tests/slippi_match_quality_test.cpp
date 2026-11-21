// Match quality: the unit tests of slippi-rust-extensions' match-quality crate (lib.rs at 934f373d), ported one for one.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "slippi_match_quality.h"
#include "slippi_match_quality_impl.h"

#include <cstdio>

namespace host { void log(const char*, ...) {} }

using namespace slippi::match_quality;
using namespace slippi::match_quality::detail;

namespace {
int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #cond); } } while (0)

// Drives a game one pass at a time, a frame time apart.
struct Sim {
  Game game;
  int32_t frame = 1;
  uint64_t time_us = 1000000;
  int32_t depth = 1;   // how far behind the opponent's latest input is when the game isn't stalled
  bool terminated = false;

  explicit Sim(bool can_terminate) : game(GameConfig{can_terminate, 2}) {}

  void pass(bool is_stall) {
    const int32_t d = is_stall ? ROLLBACK_MAX_FRAMES + 1 : depth;
    Frame f;
    f.frame = frame; f.time_us = time_us; f.is_stall = is_stall; f.latest_remote_frame = frame - d; f.ping_us = 0;
    const bool terminate = game.on_frame(f);
    CHECK(!(terminate && terminated));   // termination reported twice
    terminated |= terminate;
    time_us += FRAME_TIME_US;
    if (!is_stall) frame += 1;
  }
  void run(int32_t frames) { for (int32_t i = 0; i < frames; ++i) pass(false); }
  void run_seconds(int32_t seconds) { run(seconds * 60); }
  void stall(uint32_t passes) { for (uint32_t i = 0; i < passes; ++i) pass(true); }
};

Sim warmed_up(bool can_terminate) {
  Sim sim(can_terminate);
  sim.run(WARMUP_FRAMES);
  return sim;
}

void healthy_match_is_not_terminated() {
  Sim sim = warmed_up(true);
  sim.run_seconds(480);
  CHECK(!sim.terminated);
  CHECK(sim.game.debt == 0);
}

// Runs a stall followed by healthy play until the match terminates, returning how many stalls that took (0: never).
int stalls_to_terminate(int32_t gap_seconds, int max_stalls) {
  Sim sim = warmed_up(true);
  for (int stalls = 1; stalls <= max_stalls; ++stalls) {
    sim.stall(10);
    sim.run_seconds(gap_seconds);
    if (sim.terminated) return stalls;
  }
  return 0;
}

void five_stall_events_in_a_minute_terminate() { CHECK(stalls_to_terminate(12, 20) == 5); }

void slower_stall_events_take_more_of_them() {
  // Roughly 10 in 3 minutes
  const int stalls = stalls_to_terminate(18, 20);
  CHECK(stalls >= 8 && stalls <= 10);
}

void stall_events_the_leak_keeps_up_with_survive() { CHECK(stalls_to_terminate(26, 20) == 0); }

void close_stalls_are_one_event() {
  Sim sim = warmed_up(true);
  // Halting and releasing every few frames during one spike
  for (int i = 0; i < 20; ++i) { sim.stall(2); sim.run(5); }
  CHECK(sim.game.stall_events == 1);
  CHECK(sim.game.interval_stalled_passes == 40);
  CHECK(!sim.terminated);
}

void stall_events_in_one_interval_add_debt_once() {
  Sim sim = warmed_up(true);
  // More than the merge gap apart, so these are separate events
  for (int i = 0; i < 3; ++i) { sim.stall(MIN_STALL_PASSES); sim.run(40); }
  CHECK(sim.game.stall_events == 3);
  sim.run(INTERVAL_FRAMES - 3 * 40);
  CHECK(sim.game.debt == STALL_DEBT);
}

void stalls_under_the_minimum_length_add_less_debt() {
  Sim sim = warmed_up(true);
  // More than the merge gap apart, so these don't add up to one event
  for (int i = 0; i < 3; ++i) { sim.stall(MIN_STALL_PASSES - 1); sim.run(40); }
  sim.run(INTERVAL_FRAMES - 3 * 40);
  CHECK(sim.game.stall_events == 0);
  CHECK(sim.game.debt == SHORT_STALL_DEBT);
}

void only_frequent_short_stalls_terminate() {
  auto terminates = [](int32_t gap_seconds) {
    Sim sim = warmed_up(true);
    for (int i = 0; i < 60; ++i) { sim.stall(MIN_STALL_PASSES - 1); sim.run_seconds(gap_seconds); }
    return sim.terminated;
  };
  CHECK(terminates(5));
  CHECK(!terminates(10));
}

void stall_debt_is_the_worst_signal_not_a_sum() {
  // A 60 pass stall also scores bad depth debt, and a 120 pass stall scores severe speed debt
  const std::pair<uint32_t, int32_t> cases[] = {{MIN_STALL_PASSES, STALL_DEBT}, {60, STALL_DEBT}, {120, SEVERE_DEBT}};
  for (const auto& [passes, debt] : cases) {
    Sim sim = warmed_up(true);
    sim.stall(passes);
    sim.run(INTERVAL_FRAMES);
    CHECK(sim.game.debt == debt);
  }
}

void halts_during_warmup_leave_no_debt() {
  Sim sim(true);
  sim.run(60);
  // A second of halting to wait for the opponent to finish loading. If these samples carried into the first interval
  // they would push its average depth into the mild tier
  sim.stall(60);
  sim.run(WARMUP_FRAMES - 60);
  sim.run(4 * INTERVAL_FRAMES);
  CHECK(sim.game.debt == 0);
  CHECK(sim.game.stall_events == 0);
}

void constant_bad_depth_terminates_after_four_intervals() {
  Sim sim(true);
  sim.depth = 3;
  // The first boundary at the end of the warmup only starts the clock, and four bad intervals reach 32
  sim.run(WARMUP_FRAMES + 3 * INTERVAL_FRAMES);
  CHECK(!sim.terminated);
  sim.run(INTERVAL_FRAMES);
  CHECK(sim.terminated);
}

void mild_depth_needs_the_late_game_bar() {
  {
    Sim sim(true);
    sim.depth = 2;
    sim.run(WARMUP_FRAMES + 4 * INTERVAL_FRAMES);
    CHECK(sim.game.debt == 16);
  }
  {
    // Past a minute, 2 frames is under the raised mild threshold of 2.3
    Sim sim(true);
    sim.depth = 2;
    sim.frame = LATE_GAME_FRAME;
    sim.run(10 * INTERVAL_FRAMES);
    CHECK(sim.game.debt == 0);
    CHECK(!sim.terminated);
  }
}

void delay_below_the_minimum_is_judged_as_the_minimum() {
  // 3 frames at delay 1 is the ping that reads as 2 frames at delay 2, so it scores mild instead of bad
  Sim sim(true);
  sim.game.config.delay = 1;
  sim.depth = 3;
  sim.run(WARMUP_FRAMES + INTERVAL_FRAMES);
  CHECK(sim.game.debt == MILD_DEBT);
}

void delay_above_the_minimum_is_not_adjusted() {
  Sim sim(true);
  sim.game.config.delay = 4;
  sim.depth = 3;
  sim.run(WARMUP_FRAMES + INTERVAL_FRAMES);
  CHECK(sim.game.debt == BAD_DEBT);
}

void stalls_on_delay_one_still_add_debt() {
  Sim sim = warmed_up(true);
  sim.game.config.delay = 1;
  sim.stall(MIN_STALL_PASSES);
  sim.run(INTERVAL_FRAMES);
  CHECK(sim.game.debt == STALL_DEBT);
}

void slow_intervals_add_debt() {
  Sim sim(true);
  sim.run(WARMUP_FRAMES);
  // Each interval takes twice as long as it should
  for (int i = 0; i < 2 * INTERVAL_FRAMES; ++i) { sim.time_us += FRAME_TIME_US; sim.pass(false); }
  CHECK(sim.game.debt == 2 * SEVERE_DEBT);
  CHECK(sim.terminated);
}

void first_boundary_held_by_time_sync_adds_no_debt() {
  Sim sim(true);
  sim.depth = 3;
  sim.run(WARMUP_FRAMES - 1);
  // Time sync only holds frames back this early in a game, which makes this the one boundary it can repeat
  for (int i = 0; i < 5; ++i) {
    Frame f;
    f.frame = sim.frame; f.time_us = sim.time_us; f.is_stall = false; f.latest_remote_frame = sim.frame - sim.depth; f.ping_us = 0;
    sim.game.on_frame(f);
    sim.time_us += FRAME_TIME_US;
  }
  CHECK(sim.game.debt == 0);
}

void unranked_matches_are_only_logged() {
  Sim sim = warmed_up(false);
  for (int i = 0; i < 6; ++i) { sim.stall(10); sim.run_seconds(5); }
  CHECK(!sim.terminated);
  CHECK(sim.game.verdict_reached);
  // The verdict comes before the last stalls, and they are still tracked
  CHECK(sim.game.stall_events == 6);
}

void global_api_terminates_once() {
  start_game(GameConfig{true, 2});
  int terminations = 0;
  uint64_t t = 1000000;
  for (int32_t frame = 1; frame <= WARMUP_FRAMES + 5 * INTERVAL_FRAMES; ++frame, t += FRAME_TIME_US) {
    Frame f;
    f.frame = frame; f.time_us = t; f.latest_remote_frame = frame - 5;   // severe depth every interval
    terminations += on_frame(f) ? 1 : 0;
  }
  CHECK(terminations == 1);
  end_game();
  Frame f;
  f.frame = 1000;
  CHECK(!on_frame(f));
}
}  // namespace

int main() {
  healthy_match_is_not_terminated();
  five_stall_events_in_a_minute_terminate();
  slower_stall_events_take_more_of_them();
  stall_events_the_leak_keeps_up_with_survive();
  close_stalls_are_one_event();
  stall_events_in_one_interval_add_debt_once();
  stalls_under_the_minimum_length_add_less_debt();
  only_frequent_short_stalls_terminate();
  stall_debt_is_the_worst_signal_not_a_sum();
  halts_during_warmup_leave_no_debt();
  constant_bad_depth_terminates_after_four_intervals();
  mild_depth_needs_the_late_game_bar();
  delay_below_the_minimum_is_judged_as_the_minimum();
  delay_above_the_minimum_is_not_adjusted();
  stalls_on_delay_one_still_add_debt();
  slow_intervals_add_debt();
  first_boundary_held_by_time_sync_adds_no_debt();
  unranked_matches_are_only_logged();
  global_api_terminates_once();
  if (failures) { std::fprintf(stderr, "%d match quality check(s) failed\n", failures); return 1; }
  std::printf("match quality: all checks passed\n");
  return 0;
}
