// Placeholder until the port of slippi-rust-extensions' match-quality crate lands: tracks nothing, never terminates.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "slippi_match_quality.h"

namespace slippi::match_quality {
void start_game(const GameConfig&) {}
void end_game() {}
bool on_frame(const Frame&) { return false; }
}  // namespace slippi::match_quality
