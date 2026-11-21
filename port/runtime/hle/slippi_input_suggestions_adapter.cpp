// The GameCube adapter side of input suggestions (Slippi 3.7.0), after Dolphin's GCAdapter::GetGamePollCount,
// SerialInterface::GetGCControllerOrigin and GCAdapter::SetReportCallback at v3.7.0. Kept apart from
// slippi_input_suggestions.cpp so that file stays free of host services for its unit test.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "slippi_input_suggestions.h"
#include "slippi_net.h"
#include "host.h"

namespace slippi::input_suggestions {

bool adapter_has_port(uint8_t port) { return host::gcadapter_feeds_game_port(port); }

uint64_t adapter_game_poll_count(uint8_t port) { return host::gcadapter_game_poll_count(port); }

// Dashdance hands the game pads with the origin already subtracted, at full precision: what the game's PADRead does in
// Dolphin with the origin the controller reported and analog mode 3, Melee's.
bool adapter_origin(uint8_t port, uint8_t origin[6], uint8_t* analog_mode) {
  if (!host::gcadapter_game_origin(port, origin)) return false;
  *analog_mode = 3;
  return true;
}

void connect_adapter_reports() { host::gcadapter_set_report_callback(&NetplayClient::SendInputSuggestionForAdapterReport); }

}  // namespace slippi::input_suggestions
