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
}  // namespace host
