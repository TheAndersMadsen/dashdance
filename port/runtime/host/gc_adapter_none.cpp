// No physical GameCube adapter support in this executable.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "host.h"
#include "window.h"
namespace host {
uint32_t gcadapter_poll(PadState[4]) { return 0; }
void gcadapter_rumble(int port, bool on) { window_gamepad_rumble(port, on); }
bool gcadapter_status(GcAdapterStatus&) { return false; }
void gcadapter_recalibrate(int) {}
void gcadapter_shutdown() {}
void gcadapter_set_report_callback(GcAdapterReportCallback) {}
uint64_t gcadapter_game_poll_count(int) { return 0; }
bool gcadapter_feeds_game_port(int) { return false; }
bool gcadapter_game_origin(int, uint8_t[6]) { return false; }
void gcadapter_set_game_input_replaced(bool) {}
}  // namespace host
