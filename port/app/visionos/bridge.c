// AR Mode: the space-open hooks the game flow (main_mac.cpp) and the Swift app share.
// The Swift side registers the two function pointers at startup; the game flow calls them after
// Play to open the mixed-style immersive space and to wait for it.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <TargetConditionals.h>
#if TARGET_OS_VISION

#include "../../runtime/gx/ar_bridge.h"

static void (*g_request_open)(void);
static int (*g_is_open)(void);
static void (*g_show_dashboard)(void);

void dashdance_vision_set_space_hooks(void (*request_open)(void), int (*is_open)(void)) {
  g_request_open = request_open;
  g_is_open = is_open;
}

void dashdance_vision_set_dashboard_hook(void (*show)(void)) {
  g_show_dashboard = show;
}

void dashdance_vision_request_space(void) {
  if (g_request_open) g_request_open();
}

int dashdance_vision_space_opened(void) {
  return g_is_open ? g_is_open() : 0;
}

void dashdance_vision_show_dashboard(void) {
  if (g_show_dashboard) g_show_dashboard();
}

#endif  // TARGET_OS_VISION
