// Input suggestions: the unit tests of slippi-rust-extensions' input-suggestions crate (pad.rs, remote.rs, lib.rs at
// 934f373d), ported one for one, plus the FFI-shaped global API.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "slippi_input_suggestions.h"
#include "slippi_input_suggestions_impl.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace host { void log(const char*, ...) {} }

using namespace slippi::input_suggestions::detail;

namespace {
int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #cond); } } while (0)

// ---------------------------------------------------------------- pad.rs
const Calibration CENTERED{{128, 128, 128, 128, 0, 0}, 3};

AdapterPortData port(uint8_t b1, uint8_t b2, std::array<uint8_t, 4> sticks, uint8_t l, uint8_t r) {
  return {0x10, b1, b2, sticks[0], sticks[1], sticks[2], sticks[3], l, r};
}
Pad P(std::initializer_list<int> v) { Pad p{}; size_t i = 0; for (int x : v) p[i++] = (uint8_t)x; return p; }

void neutral_and_unplugged_ports_read_as_zero() {
  CHECK(adapter_to_pad(port(0, 0, {128, 128, 128, 128}, 0, 0), CENTERED) == Pad{});
  // A port with a zero status nibble has nothing plugged in and reads centered, whatever its other bytes
  CHECK(adapter_to_pad({0x00, 0xFF, 0xFF, 0, 0, 0, 0, 255, 255}, CENTERED) == Pad{});
}

void buttons_map_to_pad_status_bits() {
  // A and Start set, d-pad up and L set
  const Pad pad = *adapter_to_pad(port(0x01 | 0x80, 0x01 | 0x08, {128, 128, 128, 128}, 0, 0), CENTERED);
  CHECK(pad[0] == 0x11);   // Start | A
  CHECK(pad[1] == 0x48);   // L | Up
}

void sticks_are_centered_and_rest_clamped() {
  Pad pad = *adapter_to_pad(port(0, 0, {200, 56, 129, 127}, 0, 0), CENTERED);
  CHECK((int8_t)pad[2] == 72);
  CHECK((int8_t)pad[3] == -72);
  // c-stick at (1, -1) rests at center
  CHECK(pad[4] == 0 && pad[5] == 0);
  pad = *adapter_to_pad(port(0, 0, {131, 128, 128, 128}, 0, 0), CENTERED);
  CHECK((int8_t)pad[2] == 3);
}

void origin_is_subtracted_and_clamped() {
  const Calibration calibration{{130, 126, 128, 128, 30, 0}, 3};
  Pad pad = *adapter_to_pad(port(0, 0, {140, 120, 128, 128}, 100, 20), calibration);
  CHECK((int8_t)pad[2] == 10);
  CHECK((int8_t)pad[3] == -6);
  CHECK(pad[6] == 70);
  CHECK(pad[7] == 20);
  // Triggers below the origin clamp to zero
  pad = *adapter_to_pad(port(0, 0, {128, 128, 128, 128}, 20, 50), calibration);
  CHECK(pad[6] == 0);
  CHECK(pad[7] == 50);
  // ClampS8 with a positive origin limits the low end before subtracting
  pad = *adapter_to_pad(port(0, 0, {0, 128, 128, 128}, 0, 0), calibration);
  CHECK((int8_t)pad[2] == -128);
}

void other_analog_modes_are_not_converted() {
  Calibration calibration = CENTERED;
  calibration.analog_mode = 0;
  CHECK(!adapter_to_pad(port(0, 0, {128, 128, 128, 128}, 0, 0), calibration));
}

void analog_error_adds_the_worst_axis_of_each_input() {
  const Pad real = P({0x01, 0x00, 80, 0xF0, 0, 0, 100, 10});
  const Pad predicted = P({0x00, 0x00, 70, 0x10, 3, 0, 90, 30});
  // Stick y goes from -16 to 16 (32), c-stick x is off by 3, L by 10 and R is in the deadzone on both
  CHECK(analog_error(predicted, real) == 45);
  CHECK(analog_error(real, real) == 0);
}

void wrong_buttons_ignores_bits_the_asm_does_not_compare() {
  CHECK(wrong_buttons(P({0xE0, 0x80}), Pad{}) == 0);
  CHECK(wrong_buttons(P({0x01}), P({0x02})) == 0x0300);
}

void only_action_triggers_are_worth_suggesting() {
  auto sent = [](Pad prev, Pad next) { return is_meaningful_change(prev, next); };
  auto stick = [](int x, int y) { return P({0, 0, (uint8_t)(int8_t)x, (uint8_t)(int8_t)y}); };
  auto c_stick = [](int x, int y) { return P({0, 0, 0, 0, (uint8_t)(int8_t)x, (uint8_t)(int8_t)y}); };
  auto trigger = [](int l) { return P({0, 0, 0, 0, 0, 0, l, 0}); };

  // Leaving the deadzone (0.2875 is 23) is sent; moving inside it, or back into it, is not
  CHECK(!sent(stick(0, 0), stick(22, 0)));
  CHECK(sent(stick(0, 0), stick(0, -23)));
  CHECK(!sent(stick(23, 0), stick(0, 0)));
  // Dash at 0.8 (64) either way, but leaving dash range is not sent
  CHECK(!sent(stick(30, 0), stick(63, 0)));
  CHECK(sent(stick(30, 0), stick(66, 0)));
  CHECK(sent(stick(-30, 0), stick(-64, 0)));
  CHECK(!sent(stick(70, 0), stick(40, 0)));
  // At a diagonal the unit circle pulls x back: (66, 50) is about (63.8, 48.3), short of a dash
  CHECK(!sent(stick(30, 50), stick(66, 50)));
  // Tap jump at 0.6625 (53), crouch at -0.7 (-56)
  CHECK(sent(stick(0, 30), stick(0, 53)));
  CHECK(!sent(stick(0, -30), stick(0, -55)));
  CHECK(sent(stick(0, -30), stick(0, -56)));
  // C-stick smashes at ±0.8 sideways and ±0.6625 up and down
  CHECK(sent(c_stick(30, 0), c_stick(-64, 0)));
  CHECK(sent(c_stick(0, -30), c_stick(0, -53)));
  CHECK(!sent(c_stick(0, 30), c_stick(0, 52)));
  // Light shield: a trigger crossing 0.307 (43) either way, but not moving further in
  CHECK(sent(trigger(42), trigger(43)));
  CHECK(sent(trigger(43), trigger(42)));
  CHECK(!sent(trigger(50), trigger(120)));
  // Presses and releases of A, B, X, Y, L, R, Z and Start, but never the d-pad
  CHECK(sent(Pad{}, P({0x01})));
  CHECK(sent(P({0x01}), Pad{}));
  CHECK(sent(P({0x02}), Pad{}));
  CHECK(sent(Pad{}, P({0, 0x40})));
  CHECK(sent(P({0, 0x10}), Pad{}));
  CHECK(!sent(Pad{}, P({0, 0x08})));
}

// ---------------------------------------------------------------- remote.rs
Pad pad1(uint8_t first) { return P({first}); }
Report report(const Pad& p) { Report r{}; std::memcpy(r.data(), p.data(), 8); return r; }

void newest_applicable_suggestion_fills_each_slot() {
  Remote remote(1);
  remote.on_real_input(100, pad1(0));
  remote.on_suggestion(102, 1, pad1(1));
  remote.on_suggestion(104, 2, pad1(2));
  // Arrives out of order but is older
  remote.on_suggestion(102, 0, pad1(9));
  Predictions out{};
  remote.fill_predictions(100, report(pad1(0)), out);
  const uint8_t expected[7] = {0, 1, 1, 2, 2, 2, 2};   // frames 101..=107
  for (int i = 0; i < 7; ++i) CHECK(out[i][0] == expected[i]);
}

void late_and_stale_suggestions_are_rejected() {
  Remote remote(1);
  remote.on_real_input(100, pad1(0));
  remote.on_suggestion(100, 1, pad1(1));
  remote.on_suggestion(5000, 2, pad1(1));
  CHECK(remote.suggestions.empty());
}

// Suggests `suggested` for `frame`, then delivers the real input for it. The previous real input is always neutral.
bool suggest_and_receive(Remote& remote, int32_t frame, const Pad& suggested, const Pad& real) {
  remote.on_suggestion(frame, (uint32_t)frame, suggested);
  return remote.on_real_input(frame, real);
}
// Suggests a press that never happens on each frame, returning the frames that started ignoring.
std::vector<int32_t> suggest_wrong_presses(Remote& remote, int32_t first, int32_t last) {
  std::vector<int32_t> out;
  for (int32_t frame = first; frame <= last; ++frame) if (suggest_and_receive(remote, frame, pad1(1), pad1(0))) out.push_back(frame);
  return out;
}

void worse_cost_counts_only_what_the_suggestion_made_worse() {
  const Pad real_a = P({0x01, 0, 10}), nothing = P({0x00, 0, 10});
  // Fixing a button outweighs a farther stick
  CHECK(worse_cost(nothing, P({0x01, 0, 40}), real_a) == 0);
  // B instead of A is worse than nothing, even though both miss the A
  CHECK(worse_cost(nothing, P({0x02, 0, 10}), real_a) == WRONG_BUTTON_COST);
  // Same buttons, farther or closer stick
  CHECK(worse_cost(real_a, P({0x01, 0, 30}), real_a) == FARTHER_ANALOG_COST);
  CHECK(worse_cost(P({0x01, 0, 30}), P({0x01, 0, 12}), real_a) == 0);
}

void suggestions_are_judged_only_at_their_target_frame() {
  Remote remote(1);
  remote.on_real_input(100, pad1(0));
  // A tapped for frames 101 and 102 with the release never suggested. During a lag spike every frame up to the real
  // inputs is predicted with A held, but the suggestion was true for the frame it targeted.
  remote.on_suggestion(101, 1, pad1(1));
  Predictions out{};
  remote.fill_predictions(100, report(pad1(0)), out);
  for (const Report& slot : out) CHECK(slot[0] == 1);
  const std::pair<int32_t, uint8_t> reals[] = {{101, 1}, {102, 1}, {103, 0}, {104, 0}, {105, 0}};
  for (const auto& [frame, real] : reals) remote.on_real_input(frame, pad1(real));
  // Only the newest suggestion for a frame counts: A pressed and released again before the poll
  remote.on_suggestion(106, 2, pad1(1));
  remote.on_suggestion(106, 3, pad1(0));
  remote.on_real_input(106, pad1(0));
  CHECK(remote.worse_bucket == 0);
}

void wrong_suggestions_are_ignored_past_the_limit() {
  Remote remote(1);
  remote.on_real_input(100, pad1(0));
  // The sixth wrong press passes 15
  CHECK(suggest_wrong_presses(remote, 101, 110) == std::vector<int32_t>{106});
  // Later suggestions are dropped, so predictions fall back to the latest input
  remote.on_suggestion(112, 1000, pad1(1));
  CHECK(remote.suggestions.empty());
  Predictions out{};
  remote.fill_predictions(110, report(pad1(0)), out);
  for (const Report& slot : out) CHECK(slot[0] == 0);
}

void occasional_misses_never_add_up_in_a_long_game() {
  Remote remote(1);
  remote.on_real_input(100, pad1(0));
  // Twice the worst rate an honest game showed, for about half an hour of frames: a farther stick every 400 frames
  // and a wrong button every 2000
  uint32_t peak = 0;
  for (int32_t frame = 101; frame <= 100100; ++frame) {
    Pad suggested = pad1(0);
    if (frame % 400 == 0) suggested[2] = 5;
    if (frame % 2000 == 1000) suggested[0] = 1;
    CHECK(!suggest_and_receive(remote, frame, suggested, pad1(0)));
    peak = std::max(peak, remote.worse_bucket);
  }
  CHECK(peak < 5 * FRAMES_PER_DRAINED_POINT);
}

void a_clean_stretch_does_not_bank_allowance() {
  Remote remote(1);
  remote.on_real_input(100, pad1(0));
  for (int32_t frame = 101; frame <= 20100; ++frame) suggest_and_receive(remote, frame, pad1(0), pad1(0));
  CHECK(suggest_wrong_presses(remote, 20101, 20110) == std::vector<int32_t>{20106});
}

// ---------------------------------------------------------------- lib.rs
GameConfig config() {
  GameConfig c;
  c.enabled = true; c.local_player_idx = 0; c.remote_player_count = 1; c.delay = 2; c.source_port = 0; c.source_is_adapter = true;
  return c;
}
const Calibration CALIBRATION{{128, 128, 128, 128, 0, 0}, 3};

AdapterPortData port_data(uint8_t stick_x, bool a) { return {0x10, (uint8_t)(a ? 1 : 0), 0, stick_x, 128, 128, 128, 0, 0}; }
std::array<uint8_t, 37> payload(const AdapterPortData& port) {
  std::array<uint8_t, 37> p{};
  p[0] = 0x21;
  std::memcpy(&p[1], port.data(), port.size());
  return p;
}
std::optional<Suggestion> adapter(Game& game, std::array<uint64_t, 4> counts, const AdapterPortData& data) {
  const auto p = payload(data);
  return game.on_adapter_report(counts.data(), p.data(), p.size());
}
LocalFrame local_frame(int32_t frame, uint64_t poll_count, const AdapterPortData& data) {
  LocalFrame f;
  f.frame = frame; f.poll_count = poll_count; f.calibration = CALIBRATION;
  f.sent_pad = *adapter_to_pad(data, CALIBRATION);
  return f;
}
std::optional<Suggestion> frame(Game& game, int32_t f, uint64_t poll_count, const AdapterPortData& data, bool skip) {
  LocalFrame info = local_frame(f, poll_count, data);
  info.is_skip = skip;
  return game.on_local_frame(info);
}

void change_between_polls_targets_next_frame() {
  Game game(config());
  CHECK(!frame(game, 100, 1, port_data(128, false), false));
  // Noise is not sent
  CHECK(!adapter(game, {1, 0, 0, 0}, port_data(129, false)));
  const auto s = adapter(game, {1, 0, 0, 0}, port_data(128, true));
  CHECK(s && s->target_frame == 103 && s->seq == 1);
  // The same input again says nothing new
  CHECK(!adapter(game, {1, 0, 0, 0}, port_data(128, true)));
}

// A report stored at the same poll count as the last handled poll is for the next frame. An older count means a later
// poll already read it, and a newer one means a poll is in flight that has to be handled first.
void reports_from_other_polls_wait_for_or_skip_them() {
  {
    // Poll 5 is handled, so a report stored at count 4 was read by it and its input already sent
    Game game(config());
    frame(game, 100, 5, port_data(128, false), false);
    CHECK(!adapter(game, {4, 0, 0, 0}, port_data(200, false)));
  }
  {
    // This report was stored after poll 2, which hasn't been handled yet, so it waits for that poll
    Game game(config());
    frame(game, 100, 1, port_data(128, false), false);
    CHECK(!adapter(game, {2, 0, 0, 0}, port_data(200, false)));
    // Once poll 2 is handled as frame 101, the report is for frame 102 (+2 delay)
    const auto s = frame(game, 101, 2, port_data(128, false), false);
    CHECK(s && s->target_frame == 104);
  }
  {
    // A report stored after poll 3 waits through poll 2 as well
    Game game(config());
    frame(game, 100, 1, port_data(128, false), false);
    adapter(game, {3, 0, 0, 0}, port_data(200, false));
    CHECK(!frame(game, 101, 2, port_data(128, false), false));
    const auto s = frame(game, 102, 3, port_data(128, false), false);
    CHECK(s && s->target_frame == 105);
  }
}

void skipped_frame_input_is_suggested_for_the_same_frame() {
  Game game(config());
  frame(game, 100, 1, port_data(128, false), false);
  const auto s = frame(game, 101, 2, port_data(128, true), true);
  // The frame is polled again, so the input applies from 101 + delay
  CHECK(s && s->target_frame == 103);
}

// On a time sync advance the game runs the next frame right away on the same poll, so a change made after that poll
// is first read for the frame after the advanced one.
void change_during_advance_targets_frame_after_advanced_one() {
  const AdapterPortData neutral = port_data(128, false);
  auto advancing = [&](int32_t f, uint64_t poll_count) { LocalFrame info = local_frame(f, poll_count, neutral); info.is_advancing = true; return info; };
  {
    // Stored after poll 1 but before frame 100 was handled
    Game game(config());
    frame(game, 99, 0, neutral, false);
    CHECK(!adapter(game, {1, 0, 0, 0}, port_data(128, true)));
    const auto s = game.on_local_frame(advancing(100, 1));
    CHECK(s && s->target_frame == 104);
  }
  {
    // Stored after frame 100 was handled
    Game game(config());
    game.on_local_frame(advancing(100, 1));
    auto s = adapter(game, {1, 0, 0, 0}, port_data(128, true));
    CHECK(s && s->target_frame == 104);
    // The advanced frame 101 doesn't poll, and the next poll is still frame 102's
    CHECK(!frame(game, 101, 1, neutral, false));
    s = adapter(game, {1, 0, 0, 0}, port_data(200, false));
    CHECK(s && s->target_frame == 104);
  }
}

void nothing_is_sent_for_frozen_frames() {
  Game game(config());
  frame(game, 10, 1, port_data(128, false), false);
  CHECK(!adapter(game, {1, 0, 0, 0}, port_data(128, true)));
}

void nothing_is_sent_when_disabled_or_off_the_adapter() {
  GameConfig disabled = config(); disabled.enabled = false;
  GameConfig keyboard = config(); keyboard.source_is_adapter = false;
  for (const GameConfig& c : {disabled, keyboard}) {
    Game game(c);
    frame(game, 100, 1, port_data(128, false), false);
    CHECK(!adapter(game, {1, 0, 0, 0}, port_data(128, true)));
  }
}

void suggestions_are_only_used_when_enabled_and_connected() {
  auto first_slot = [](bool enabled, bool is_disconnected) {
    GameConfig c = config(); c.enabled = enabled;
    Game game(c);
    game.remotes[0].on_real_input(100, Pad{});
    game.remotes[0].on_suggestion(101, 1, P({0x01}));
    return game.fill_predictions(0, 100, is_disconnected, Report{})[0][0];
  };
  CHECK(first_slot(true, false) == 0x01);
  // Otherwise every slot holds the latest input, as before suggestions existed
  CHECK(first_slot(false, false) == 0);
  CHECK(first_slot(true, true) == 0);
}

// A sender and a receiver that is 3 frames behind on the sender's real inputs: a button press made between polls
// reaches the receiver as a suggestion and is predicted correctly.
void suggestion_reaches_receiver_prediction() {
  Game sender(config());
  GameConfig rc = config(); rc.local_player_idx = 1;
  Game receiver(rc);
  const Pad neutral = *adapter_to_pad(port_data(128, false), CALIBRATION);
  const Pad pressed = *adapter_to_pad(port_data(128, true), CALIBRATION);

  frame(sender, 100, 1, port_data(128, false), false);
  receiver.on_remote_input(0, 100, neutral);

  // A pressed between polls 100 and 101, so it first applies to 101 + delay = 103
  const auto s = adapter(sender, {1, 0, 0, 0}, port_data(128, true));
  CHECK(s.has_value());
  if (!s) return;
  receiver.remotes[0].on_suggestion(s->target_frame, s->seq, s->pad);

  // The receiver has real inputs up to 100 and must predict 101..=103
  const Report latest = report(neutral);
  const Predictions out = receiver.fill_predictions(0, 100, false, latest);
  CHECK(std::memcmp(out[0].data(), neutral.data(), 8) == 0);
  CHECK(std::memcmp(out[1].data(), neutral.data(), 8) == 0);
  CHECK(std::memcmp(out[2].data(), pressed.data(), 8) == 0);
}

// ---------------------------------------------------------------- the global API, as the call sites use it
void global_api_round_trip() {
  namespace is = slippi::input_suggestions;
  // No game: predictions are the latest input and nothing is suggested
  uint8_t latest[12] = {0x05};
  uint8_t out[7 * 12];
  std::memset(out, 0xAA, sizeof out);
  is::fill_predictions(0, 100, false, latest, out, sizeof out);
  for (int k = 0; k < 7; ++k) CHECK(out[k * 12] == 0x05 && out[k * 12 + 1] == 0);

  is::GameConfig c;
  c.enabled = true; c.remote_player_count = 1; c.delay = 2; c.source_is_adapter = true;
  is::start_game(c);
  is::LocalFrame f;
  f.frame = 100; f.has_poll = true; f.poll_count = 1; f.has_origin = true;
  const uint8_t origin[6] = {128, 128, 128, 128, 0, 0};
  std::memcpy(f.origin, origin, 6);
  f.analog_mode = 3;
  CHECK(!is::on_local_frame(f).is_valid);
  const uint64_t counts[4] = {1, 0, 0, 0};
  const auto p = payload(port_data(128, true));
  const is::Suggestion s = is::on_adapter_report(counts, p.data(), p.size());
  CHECK(s.is_valid && s.target_frame == 103 && s.seq == 1 && s.pad[0] == 0x01);
  CHECK(!is::on_adapter_report(nullptr, p.data(), p.size()).is_valid);

  // A remote suggestion feeds that player's predictions; a too-small buffer is left alone
  const uint8_t zero[8] = {}, a_pressed[8] = {0x01};
  is::on_remote_input(0, 100, zero);
  is::on_remote_suggestion(0, 102, 1, a_pressed);
  uint8_t neutral[12] = {};
  is::fill_predictions(0, 100, false, neutral, out, sizeof out);
  CHECK(out[0] == 0 && out[12] == 0x01 && out[6 * 12] == 0x01);
  uint8_t small[12] = {0x77};
  is::fill_predictions(0, 100, false, neutral, small, sizeof small);
  CHECK(small[0] == 0x77);

  // A reset forgets the remote's suggestions
  is::reset_remote(0);
  is::fill_predictions(0, 100, false, neutral, out, sizeof out);
  CHECK(out[12] == 0);

  is::end_game();
  CHECK(!is::on_adapter_report(counts, p.data(), p.size()).is_valid);
}
}  // namespace

int main() {
  neutral_and_unplugged_ports_read_as_zero();
  buttons_map_to_pad_status_bits();
  sticks_are_centered_and_rest_clamped();
  origin_is_subtracted_and_clamped();
  other_analog_modes_are_not_converted();
  analog_error_adds_the_worst_axis_of_each_input();
  wrong_buttons_ignores_bits_the_asm_does_not_compare();
  only_action_triggers_are_worth_suggesting();
  newest_applicable_suggestion_fills_each_slot();
  late_and_stale_suggestions_are_rejected();
  worse_cost_counts_only_what_the_suggestion_made_worse();
  suggestions_are_judged_only_at_their_target_frame();
  wrong_suggestions_are_ignored_past_the_limit();
  occasional_misses_never_add_up_in_a_long_game();
  a_clean_stretch_does_not_bank_allowance();
  change_between_polls_targets_next_frame();
  reports_from_other_polls_wait_for_or_skip_them();
  skipped_frame_input_is_suggested_for_the_same_frame();
  change_during_advance_targets_frame_after_advanced_one();
  nothing_is_sent_for_frozen_frames();
  nothing_is_sent_when_disabled_or_off_the_adapter();
  suggestions_are_only_used_when_enabled_and_connected();
  suggestion_reaches_receiver_prediction();
  global_api_round_trip();
  if (failures) { std::fprintf(stderr, "%d input suggestion check(s) failed\n", failures); return 1; }
  std::printf("input suggestions: all checks passed\n");
  return 0;
}
