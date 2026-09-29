// In-game settings menu and performance HUD, drawn over the game (the decomp-style overlay that
// Ship of Harkinian and friends have): open it with L+R+Start held for half a second on any
// controller, F1 or Escape on the keyboard, or the MENU button next to the touch controls. While it
// is open the game receives neutral inputs. Changes apply immediately and are saved for next time.
// A Controls page configures each controller and the keyboard, with a step-by-step remap.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "host.h"
#include "overlay.h"
#include <functional>
#include <string>

namespace host {
struct RuntimeSettings {
  int scale = 0;                 // 0 auto, 1..8
  int anisotropy = 16;           // 1, 4, 16
  float sharpness = 0.0f;        // 0..1
  int upscaler = 0;              // 0 off, 1 MetalFX spatial (balanced), 2 (quality); Metal only, ignored where unsupported
  bool widescreen = false;       // takes full effect on the next launch (the game's own code table reads it at boot)
  bool flash_failed_lcancel = false;
  bool vsync = true;
  int volume = 70;               // 0..100
  float overlay_opacity = 1.0f;  // touch controls
  float overlay_scale = 1.0f;
  bool hud = false;              // performance HUD
  bool fullscreen = false;       // macOS
  int online_delay = 2;          // Slippi Online input delay frames (1..9), used from the next match; each frame adds 16.7 ms
};
enum class MenuChange { Graphics, Volume, TouchControls, Fullscreen, Hud, Widescreen, OnlineDelay, FailedLCancelFlash };
// `apply` runs on the simulation thread whenever the player changes a value.
void menu_init(const RuntimeSettings& initial, std::function<void(const RuntimeSettings&, MenuChange)> apply);
bool menu_is_open();
bool menu_capturing();                     // a remap is waiting for an input: Escape belongs to it, not to the menu toggle
bool menu_changed();                       // any value changed since menu_init (for saving at exit)
RuntimeSettings menu_settings();
void menu_toggle();                        // from the event thread (keyboard)
// Per frame from input_poll: handles the open combo and navigation, neutralises pads while open.
void menu_frame(PadState pads[4]);
// Touch: returns true when the point hit the menu (or its MENU button) and was consumed.
bool menu_touch(float px, float py);
// Text and shapes for the open menu and the HUD; appended to `out`.
void menu_overlay(OverlayFrame& out, int window_w, int window_h, bool touch_controls_visible);
}  // namespace host
