// The headless gate accepts deterministic scripts and exposes no physical devices.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "host.h"
#include "input_script.h"
#include "window.h"
#include <atomic>
#include <cstdio>
#include <fstream>

namespace host {
namespace {
InputScript script;
std::atomic<uint32_t> match_start{0};
}
bool input_load_script(const char* path) {
  std::ifstream file(path);
  if (!file) return false;
  std::string error;
  if (!script.load(file, &error)) { std::fprintf(stderr, "input script: %s\n", error.c_str()); return false; }
  return true;
}
void input_mark_match_start() { match_start.store(retrace_count()); }
void input_poll(PadState out[4]) {
  auto pads = script.sample(retrace_count(), match_start.load(),
                            rd8(0x80479D30), rd8(0x80479D33));
  for (unsigned port = 0; port < pads.size(); ++port) {
    out[port] = {};
    out[port].err = pads[port].connected ? 0 : -1;
    out[port].button = pads[port].buttons;
    out[port].stick_x = pads[port].sx; out[port].stick_y = pads[port].sy;
    out[port].sub_x = pads[port].cx; out[port].sub_y = pads[port].cy;
  }
}
uint32_t gcadapter_poll(PadState[4]) { return 0; }
void gcadapter_rumble(int, bool) {}
void gcadapter_shutdown() {}
}  // namespace host
