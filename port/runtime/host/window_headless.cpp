// No native window in the offline bring-up executable. GX decoding still runs.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "host.h"
#include "window.h"

namespace host {
void* window_create(int, int, const wchar_t*, bool) { die("native window unavailable in the headless executable"); }
void window_set_message_callback(MessageCallback) {}
void window_input_capture(bool) {}
bool window_ui_gamecube_pad(PadState& pad) { pad = {}; pad.err = -1; return false; }
void window_set_resize_callback(ResizeCallback) {}
void window_pump() {}
void window_set_fullscreen(bool) {}
void window_gamepad_rumble(int, bool) {}
bool window_take_fullscreen_toggle() { return false; }
double window_refresh_rate() { return 60.0; }
void window_destroy() {}
void window_set_title(const wchar_t*) {}
bool window_closed() { return false; }
void window_client_size(int* w, int* h) { if (w) *w = 0; if (h) *h = 0; }
}  // namespace host

