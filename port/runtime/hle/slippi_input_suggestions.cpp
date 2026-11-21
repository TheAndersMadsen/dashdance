// Placeholder until the port of slippi-rust-extensions' input-suggestions crate lands: suggestions are off, so
// predictions stay the latest input (Slippi's behaviour with the option disabled).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "slippi_input_suggestions.h"

namespace slippi::input_suggestions {
void start_game(const GameConfig&) {}
void end_game() {}
Suggestion on_local_frame(const LocalFrame&) { return {}; }
Suggestion on_adapter_report(const uint64_t*, const uint8_t*, size_t) { return {}; }
void on_remote_suggestion(uint8_t, int32_t, uint32_t, const uint8_t*) {}
void on_remote_input(uint8_t, int32_t, const uint8_t*) {}
void reset_remote(uint8_t) {}
void fill_predictions(uint8_t, int32_t, bool, const uint8_t*, uint8_t*, size_t) {}
bool adapter_has_port(uint8_t) { return false; }
uint64_t adapter_game_poll_count(uint8_t) { return 0; }
bool adapter_origin(uint8_t, uint8_t*, uint8_t*) { return false; }
}  // namespace slippi::input_suggestions
