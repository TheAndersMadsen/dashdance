// On-screen controller overlay (touch devices): shapes the renderer draws over the
// presented frame, produced by the host's touch layout each frame.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace host {
struct OverlayShape {
  float x0, y0, x1, y1;   // window pixels
  float r, g, b, a;       // colour and opacity
  float corner;           // corner radius in pixels (== half extent for circles)
  float ring;             // ring thickness in pixels, 0 = filled
  float pressed;          // 0..1 highlight
  uint32_t label;         // 0 = none, else index into kOverlayLabels
  float label_w, label_h; // label box in pixels (text is fitted inside, aspect kept)
};
constexpr const char* kOverlayLabels[] = {"", "A", "B", "X", "Y", "Z", "L", "R", "Start", "Taunt", "C"};
constexpr int kOverlayLabelCount = (int)(sizeof kOverlayLabels / sizeof kOverlayLabels[0]);
// Free text drawn from an ASCII glyph atlas (menus, HUD). `size` is the line height in window pixels.
struct OverlayText {
  float x, y, size;       // top-left of the line box, window pixels
  float r, g, b, a;
  int align;              // 0 left, 1 centre, 2 right (about x)
  std::string text;
  float max_w = 0.0f;     // > 0: the line shrinks to fit this width instead of running past its box
};
struct OverlayFrame {
  std::vector<OverlayShape> shapes;
  std::vector<OverlayText> texts;
  float alpha = 0.0f;     // whole-overlay opacity after fade (shapes only; texts carry their own alpha)
};
// Everything drawn over the game: touch controls, the in-game menu and the performance HUD.
bool game_overlay(OverlayFrame& out);
// Menu and HUD only (no touch controls, no letterbox artwork) at an explicit client size; the
// Vision Pro mirror uses it, because its "window" is the game picture itself.
void game_overlay_menu_only(OverlayFrame& out, int ww, int wh);
// Fills `out` with the current overlay; returns false when nothing should be drawn.
bool touch_overlay(OverlayFrame& out);
void touch_set_opacity(float opacity);    // 0..1, user setting
void touch_force_visible(bool visible);   // show even without a touch screen (development)
void touch_set_game_aspect(float aspect);  // 4:3 or 16:9; portrait layouts put the controls below the game
void touch_set_scale(float scale);         // control size multiplier, 0.7..1.4
void haptic_tap(bool strong);             // physical feedback on supported devices
}  // namespace host
