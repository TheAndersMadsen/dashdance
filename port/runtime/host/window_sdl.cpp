// SDL3 host: the game window with its CAMetalLayer, events, keyboard and gamepads.
// Works on macOS, iOS and visionOS. Scripted input and the MELEE_PAD_FILE harness
// replace physical devices when present.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "host.h"
#include "controller_rate.h"
#include "game_menu.h"
#include "input_config.h"
#include "input_script.h"
#include "overlay.h"
#include "window.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#if defined(__APPLE__) && TARGET_OS_IPHONE
bool window_fold_division_apple(void* uiwindow, float pixels_per_point, float* out);   // window_fold_apple.mm
#endif

namespace host {
namespace {
enum : uint16_t {
  GC_LEFT = 0x0001, GC_RIGHT = 0x0002, GC_DOWN = 0x0004, GC_UP = 0x0008, GC_Z = 0x0010, GC_R = 0x0020, GC_L = 0x0040,
  GC_A = 0x0100, GC_B = 0x0200, GC_X = 0x0400, GC_Y = 0x0800, GC_START = 0x1000,
};
SDL_Window* g_window = nullptr;
SDL_MetalView g_view = nullptr;
ResizeCallback g_resize;
MessageCallback g_message;
std::atomic<bool> g_closed{false};
std::atomic<bool> g_fullscreen_toggle{false};
std::atomic<int> g_last_key_press{-1};   // scancode of the latest key press, for remapping in the in-game menu
std::atomic<bool> g_capture{false};
std::mutex g_ui_mutex;
PadState g_ui_pad{};
bool g_ui_gamecube = false;
int g_client_w = 0, g_client_h = 0;
std::atomic<float> g_safe_top{0.0f}, g_safe_left{0.0f}, g_safe_right{0.0f}, g_safe_bottom{0.0f};   // pixels; read by the renderer thread
// iPhone Duo: the fold's active division region in client pixels (0/absent when flat or unsupported).
std::atomic<bool> g_div_active{false};
std::atomic<float> g_div_x0{0.0f}, g_div_y0{0.0f}, g_div_x1{0.0f}, g_div_y1{0.0f};
std::vector<SDL_Gamepad*> g_gamepads;
std::string gamepad_guid(SDL_Gamepad* pad) {
  char buf[64];
  SDL_GUIDToString(SDL_GetGamepadGUIDForID(SDL_GetGamepadID(pad)), buf, sizeof buf);
  return buf;
}
SDL_JoystickID g_gamepad_port[4] = {0, 0, 0, 0};   // GameCube port -> gamepad instance id (set by input_poll; 0 = none)
InputScript g_script;
bool g_scripted = false;
std::atomic<uint32_t> g_match_start{0};

// Touch controls (iPhone, iPad, Apple Vision Pro). Layout and touch model follow
// VirtualFriend's on-screen controller (Copyright (c) 2024 Adam Gastineau, MIT; notice in THIRD_PARTY_NOTICES.md): two side columns of
// translucent monochrome shapes (trigger capsule, pad, two round buttons), every
// finger is tested against every button on each touch event so presses slide
// naturally between buttons. Adapted to a GameCube pad: analog stick on the left,
// the A/B/X/Y cluster on the right, Z and the C-stick below it.
enum Label : uint32_t { LB_NONE, LB_A, LB_B, LB_X, LB_Y, LB_Z, LB_L, LB_R, LB_START, LB_TAUNT, LB_C };   // kOverlayLabels order
static_assert(LB_C + 1 == kOverlayLabelCount, "Label enum must match kOverlayLabels");
struct Rect { float x0, y0, x1, y1; bool contains(float x, float y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; } };
struct TouchButton {
  uint16_t button; bool trigger_l, trigger_r; uint32_t label; Rect rect; bool capsule; bool pressed;
  bool hit(float x, float y) const {   // capsules by their box, round buttons by their inscribed circle
    if (capsule) return rect.contains(x, y);
    const float cx = (rect.x0 + rect.x1) * 0.5f, cy = (rect.y0 + rect.y1) * 0.5f, r = (rect.x1 - rect.x0) * 0.5f;
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
  }
};
struct TouchStick { Rect rect; float radius; SDL_FingerID finger; bool owned, active; float dx, dy; bool c; };
struct TouchLayout { std::vector<TouchButton> buttons; TouchStick stick{}, cstick{}; int w = 0, h = 0; float pt = 1.0f; };
TouchLayout g_touch;
struct Finger { SDL_FingerID id; float x, y; };
std::vector<Finger> g_fingers;
bool g_touch_seen = false, g_touch_forced = false;
float g_touch_opacity = 1.0f, g_touch_alpha = 0.0f, g_pixels_per_point = 1.0f, g_touch_aspect = 4.0f / 3.0f, g_touch_scale = 1.0f;
std::mutex g_touch_mutex;   // events arrive on the main thread; input_poll and the overlay run on others

Rect circle(float cx, float cy, float r) { return {cx - r, cy - r, cx + r, cy + r}; }
void touch_layout() {
  TouchLayout& t = g_touch;
  if (t.w == g_client_w && t.h == g_client_h && t.pt == g_pixels_per_point && !t.buttons.empty()) return;
  t.w = g_client_w; t.h = g_client_h; t.pt = g_pixels_per_point;
  std::vector<bool> pressed;
  for (const TouchButton& b : t.buttons) pressed.push_back(b.pressed);
  t.buttons.clear();
  const float pad = 24.0f * t.pt, W = (float)t.w, H_full = (float)t.h;
  // Portrait (iPhone, iPad held upright): the game sits at the top, the controls fill the rest.
  const bool portrait = H_full > W * 1.05f;
  const float safe_t = g_safe_top.load(), safe_l = g_safe_left.load(), safe_r = g_safe_right.load(), safe_b = g_safe_bottom.load();
  const bool div_active = g_div_active.load();
  const float div_x0 = g_div_x0.load(), div_y0 = g_div_y0.load(), div_x1 = g_div_x1.load(), div_y1 = g_div_y1.load();
  const bool div_horizontal = div_active && div_y1 - div_y0 < div_x1 - div_x0;
  const GameRect game = window_game_rect(W, H_full, g_touch_aspect);
  float top = portrait ? game.y + game.h : g_safe_top.load();   // below the island and the game
  float bottom_inset = safe_b;                                   // above the home indicator
  if (div_horizontal) {
    // Partially folded (book / tabletop): the curved band is hard to see and to tap, so the
    // controls live entirely in the bottom region (thumb reach); only when that region is the
    // cramped one do they move above the fold instead.
    const float below = H_full - div_y1 - safe_b;
    if (below >= 220.0f * t.pt || below >= div_y0 - safe_t) top = std::max(top, div_y1);
    else { top = safe_t; bottom_inset = std::max(safe_b, H_full - div_y0); }
  }
  const float H = std::max(H_full - top - bottom_inset, 120.0f);
  const float col_h = std::min(H - 2 * pad, 440.0f * t.pt);   // same size on every display (iPhone Duo outer and inner), sitting low within thumb reach
  const float y_base = top + (H - 2 * pad - col_h);
  const float usable = std::max(W - pad * 2 - safe_l - safe_r, 160.0f * t.pt);
  float col_w = (portrait ? std::min(W * 0.42f, 260.0f * t.pt) : std::min(std::max(200.0f * t.pt, col_h * 0.46f), W * 0.28f)) * g_touch_scale;
  col_w = std::min(col_w, usable / 2);   // the two columns must never overlap, whatever the safe insets
  if (div_active && !div_horizontal)   // flat book: a vertical fold band; the columns must stay clear of it
    col_w = std::min(col_w, std::max(std::min(div_x0 - pad - safe_l, W - pad - safe_r - div_x1), 80.0f * t.pt));
  const float trig_h = col_h * 0.13f, mid = std::min(col_w, col_h * 0.60f), row_h = col_h * 0.27f;
  const float gap = 16.0f * t.pt, btn_d = std::max(std::min(row_h, col_w * 0.42f), 8.0f);
  auto column = [&](float x0, bool left) {
    const float cx = x0 + col_w * 0.5f;
    const float y_trig = y_base + pad, y_mid = y_trig + trig_h + gap, y_row = y_trig + trig_h + col_h * 0.60f + (row_h - btn_d) * 0.5f;
    const float mid_d = std::max(mid - 2 * gap, 24.0f);
    if (left) {
      t.buttons.push_back({GC_L, true, false, LB_L, {x0, y_trig, x0 + col_w, y_trig + trig_h}, true, false});
      t.stick.rect = circle(cx, y_mid + mid_d * 0.5f, mid_d * 0.5f); t.stick.radius = mid_d * 0.5f; t.stick.c = false;
      t.buttons.push_back({GC_START, false, false, LB_START, circle(x0 + btn_d * 0.5f, y_row + btn_d * 0.5f, btn_d * 0.5f), false, false});
      t.buttons.push_back({GC_UP, false, false, LB_TAUNT, circle(x0 + col_w - btn_d * 0.5f, y_row + btn_d * 0.5f, btn_d * 0.5f), false, false});
    } else {
      t.buttons.push_back({GC_R, false, true, LB_R, {x0, y_trig, x0 + col_w, y_trig + trig_h}, true, false});
      // GameCube face cluster inside the middle square: big A, B low-left, X right, Y above.
      const float sx = cx - mid_d * 0.5f, sy = y_mid;
      t.buttons.push_back({GC_A, false, false, LB_A, circle(sx + mid_d * 0.56f, sy + mid_d * 0.62f, mid_d * 0.25f), false, false});
      t.buttons.push_back({GC_B, false, false, LB_B, circle(sx + mid_d * 0.15f, sy + mid_d * 0.82f, mid_d * 0.14f), false, false});
      t.buttons.push_back({GC_X, false, false, LB_X, circle(sx + mid_d * 0.88f, sy + mid_d * 0.34f, mid_d * 0.14f), false, false});
      t.buttons.push_back({GC_Y, false, false, LB_Y, circle(sx + mid_d * 0.44f, sy + mid_d * 0.14f, mid_d * 0.14f), false, false});
      t.buttons.push_back({GC_Z, false, false, LB_Z, circle(x0 + btn_d * 0.5f, y_row + btn_d * 0.5f, btn_d * 0.5f), false, false});
      t.cstick.rect = circle(x0 + col_w - btn_d * 0.5f, y_row + btn_d * 0.5f, btn_d * 0.5f); t.cstick.radius = btn_d * 0.5f; t.cstick.c = true;
    }
  };
  column(pad + safe_l, true);                 // clear of the island in landscape
  column(W - pad - col_w - safe_r, false);
  for (size_t i = 0; i < t.buttons.size() && i < pressed.size(); ++i) t.buttons[i].pressed = pressed[i];
}
bool physical_gamepad_connected() {
#if defined(TARGET_OS_SIMULATOR) && TARGET_OS_SIMULATOR
  return false;   // the Simulator always exposes a virtual Apple "Gamepad"
#else
  return !g_gamepads.empty();
#endif
}
bool touch_controls_visible() { return (g_touch_seen || g_touch_forced) && !physical_gamepad_connected() && g_touch_opacity > 0.0f; }

void stick_update(TouchStick& st) {
  st.active = false;
  if (!st.owned) return;
  for (const Finger& f : g_fingers) {
    if (f.id != st.finger) continue;
    const float cx = (st.rect.x0 + st.rect.x1) * 0.5f, cy = (st.rect.y0 + st.rect.y1) * 0.5f;
    const float lim = std::max(st.radius * 0.72f, 1.0f);   // full deflection well inside the track
    float dx = f.x - cx, dy = f.y - cy;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len > lim) { dx *= lim / len; dy *= lim / len; }
    st.dx = dx / lim; st.dy = dy / lim; st.active = true;
  }
}
void touch_reevaluate() {
  touch_layout();
  for (TouchButton& b : g_touch.buttons) {
    bool pressed = false;
    for (const Finger& f : g_fingers) if (b.hit(f.x, f.y)) pressed = true;
    if (pressed && !b.pressed) haptic_tap(b.button == GC_A);
    b.pressed = pressed;
  }
  stick_update(g_touch.stick);
  stick_update(g_touch.cstick);
}
void touch_event(const SDL_TouchFingerEvent& e) {
  if (SDL_GetTouchDeviceType(e.touchID) != SDL_TOUCH_DEVICE_DIRECT) return;   // trackpads are not touch screens
  std::lock_guard<std::mutex> lock(g_touch_mutex);
  g_touch_seen = true;
  touch_layout();
  const float px = e.x * (float)g_client_w, py = e.y * (float)g_client_h;
  if (e.type == SDL_EVENT_FINGER_DOWN && menu_touch(px, py)) return;   // the menu (or its button) took the tap
  if (e.type == SDL_EVENT_FINGER_DOWN) {
    g_fingers.push_back({e.fingerID, px, py});
    // A finger that lands on a stick owns it until it lifts, even when it wanders off.
    for (TouchStick* st : {&g_touch.stick, &g_touch.cstick})
      if (!st->owned && st->rect.contains(px, py)) { st->finger = e.fingerID; st->owned = true; haptic_tap(false); break; }
  } else if (e.type == SDL_EVENT_FINGER_MOTION) {
    for (Finger& f : g_fingers) if (f.id == e.fingerID) { f.x = px; f.y = py; }
  } else {
    for (auto it = g_fingers.begin(); it != g_fingers.end();) { if (it->id == e.fingerID) it = g_fingers.erase(it); else ++it; }
    for (TouchStick* st : {&g_touch.stick, &g_touch.cstick}) if (st->owned && st->finger == e.fingerID) st->owned = false;
  }
  touch_reevaluate();
}

void read_touch(PadState& p) {
  std::lock_guard<std::mutex> lock(g_touch_mutex);
  if (!g_touch_seen) return;
  if (!touch_controls_visible()) {   // a gamepad took over: drop any latched touch state
    g_fingers.clear();
    g_touch.stick.owned = g_touch.stick.active = g_touch.cstick.owned = g_touch.cstick.active = false;
    for (TouchButton& b : g_touch.buttons) b.pressed = false;
    return;
  }
  p.err = 0;
  if (g_touch.stick.active) {
    p.stick_x = (int8_t)std::clamp((int)std::lround(g_touch.stick.dx * 127.0f), -127, 127);
    p.stick_y = (int8_t)std::clamp((int)std::lround(-g_touch.stick.dy * 127.0f), -127, 127);
  }
  if (g_touch.cstick.active) {
    p.sub_x = (int8_t)std::clamp((int)std::lround(g_touch.cstick.dx * 127.0f), -127, 127);
    p.sub_y = (int8_t)std::clamp((int)std::lround(-g_touch.cstick.dy * 127.0f), -127, 127);
  }
  for (const TouchButton& b : g_touch.buttons) {
    if (!b.pressed) continue;
    p.button |= b.button;
    if (b.trigger_l) p.trig_l = 255;
    if (b.trigger_r) p.trig_r = 255;
  }
}
}  // namespace

bool touch_overlay(OverlayFrame& out) {
  std::lock_guard<std::mutex> lock(g_touch_mutex);
  const float target = touch_controls_visible() ? g_touch_opacity : 0.0f;
  g_touch_alpha += std::clamp(target - g_touch_alpha, -0.08f, 0.08f);
  out.shapes.clear();
  out.alpha = g_touch_alpha;
  if (g_touch_alpha <= 0.001f) return false;
  touch_layout();
  // VirtualFriend's palette on a dark background: buttons white 0.4 at 50%, touched white 0.6 at 50%.
  const float base = 0.40f, touched = 0.60f, alpha = 0.50f;
  for (const TouchButton& b : g_touch.buttons) {
    const float shade = b.pressed ? touched : base;
    const float h = b.rect.y1 - b.rect.y0, w = b.rect.x1 - b.rect.x0;
    const float label_h = b.capsule ? h * 0.62f : h * 0.34f;
    out.shapes.push_back({b.rect.x0, b.rect.y0, b.rect.x1, b.rect.y1, shade, shade, shade, alpha, b.capsule ? h * 0.5f : w * 0.5f, 0.0f, b.pressed ? 1.0f : 0.0f, b.label, w * 0.8f, label_h});
  }
  for (const TouchStick* st : {&g_touch.stick, &g_touch.cstick}) {
    const float cx = (st->rect.x0 + st->rect.x1) * 0.5f, cy = (st->rect.y0 + st->rect.y1) * 0.5f;
    out.shapes.push_back({st->rect.x0, st->rect.y0, st->rect.x1, st->rect.y1, base, base, base, alpha, st->radius, 0.0f, 0.0f, (uint32_t)LB_NONE, 0.0f, 0.0f});
    const float kr = st->radius * (st->c ? 0.42f : 0.36f), lim = st->radius * 0.72f;
    const float kx = cx + (st->active ? st->dx * lim : 0.0f), ky = cy + (st->active ? st->dy * lim : 0.0f);
    const float shade = st->active ? touched + 0.15f : touched;
    out.shapes.push_back({kx - kr, ky - kr, kx + kr, ky + kr, shade, shade, shade, alpha + 0.2f, kr, 0.0f, st->active ? 1.0f : 0.0f, st->c ? (uint32_t)LB_C : (uint32_t)LB_NONE, kr * 1.2f, kr * 0.9f});
  }
  return true;
}
void touch_set_opacity(float opacity) { std::lock_guard<std::mutex> lock(g_touch_mutex); g_touch_opacity = std::clamp(opacity, 0.0f, 1.0f); }
void touch_force_visible(bool visible) { std::lock_guard<std::mutex> lock(g_touch_mutex); g_touch_forced = visible; }
void touch_set_scale(float scale) { std::lock_guard<std::mutex> lock(g_touch_mutex); g_touch_scale = std::clamp(scale, 0.7f, 1.4f); g_touch.w = 0; }
void touch_set_game_aspect(float aspect) { std::lock_guard<std::mutex> lock(g_touch_mutex); g_touch_aspect = aspect > 0.0f ? aspect : 4.0f / 3.0f; g_touch.w = 0; }

namespace {
std::string narrow(const wchar_t* text) {
  std::string out;
  for (; text && *text; ++text) {
    const uint32_t c = (uint32_t)*text;
    if (c < 0x80) out += (char)c;
    else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
    else { out += (char)(0xF0 | (c >> 18)); out += (char)(0x80 | ((c >> 12) & 0x3F)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
  }
  return out;
}

void refresh_client_size() {
  if (!g_window) return;
  int w = 0, h = 0;
  SDL_GetWindowSizeInPixels(g_window, &w, &h);
  int pw = 0, ph = 0;
  SDL_GetWindowSize(g_window, &pw, &ph);
  std::lock_guard<std::mutex> lock(g_touch_mutex);   // the touch layout reads these from other threads
  g_client_w = std::max(w, 1); g_client_h = std::max(h, 1);
  g_pixels_per_point = pw > 0 ? (float)g_client_w / (float)pw : 1.0f;
  SDL_Rect safe{};
  float top = 0, left = 0, right = 0, bottom = 0;
  if (SDL_GetWindowSafeArea(g_window, &safe) && safe.w > 0 && safe.h > 0) {
    top = std::max(safe.y, 0) * g_pixels_per_point; left = std::max(safe.x, 0) * g_pixels_per_point;
    right = std::max(pw - safe.x - safe.w, 0) * g_pixels_per_point; bottom = std::max(ph - safe.y - safe.h, 0) * g_pixels_per_point;
  }
  if (top != g_safe_top.load() || left != g_safe_left.load() || right != g_safe_right.load() || bottom != g_safe_bottom.load()) {
    g_safe_top.store(top); g_safe_left.store(left); g_safe_right.store(right); g_safe_bottom.store(bottom);
    g_touch.w = 0;   // rotation moves the island and the home indicator: lay the controls out again
  }
  // iPhone Duo: the fold moves with every open/close/pose change, so re-query it with the
  // safe areas. MELEE_FAKE_FOLD=x0,y0,x1,y1 overrides for layout testing on other devices.
  bool div_active = false; float div[4] = {0, 0, 0, 0};
  static const char* fake_fold = std::getenv("MELEE_FAKE_FOLD");
  if (fake_fold && *fake_fold && std::sscanf(fake_fold, "%f,%f,%f,%f", &div[0], &div[1], &div[2], &div[3]) == 4) {
    div_active = true;
  } else {
#if defined(__APPLE__) && TARGET_OS_IPHONE
    SDL_PropertiesID props = SDL_GetWindowProperties(g_window);
    void* uiwindow = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
    if (uiwindow) div_active = window_fold_division_apple(uiwindow, g_pixels_per_point, div);
#endif
  }
  if (!div_active || div[2] <= div[0] || div[3] <= div[1]) div_active = false;
  if (div_active != g_div_active.load() || (div_active && (div[0] != g_div_x0.load() || div[1] != g_div_y0.load() ||
                                                           div[2] != g_div_x1.load() || div[3] != g_div_y1.load()))) {
    g_div_active.store(div_active);
    g_div_x0.store(div[0]); g_div_y0.store(div[1]); g_div_x1.store(div[2]); g_div_y1.store(div[3]);
    g_touch.w = 0;   // the fold moved: lay the controls out again
  }
}

void open_gamepad(SDL_JoystickID id) {
  if (SDL_Gamepad* pad = SDL_OpenGamepad(id)) {
    g_gamepads.push_back(pad);
    log("input: gamepad connected: %s (vendor %04x product %04x)", SDL_GetGamepadName(pad), SDL_GetGamepadVendor(pad), SDL_GetGamepadProduct(pad));
  }
}
void close_gamepad(SDL_JoystickID id) {
  for (auto it = g_gamepads.begin(); it != g_gamepads.end(); ++it) {
    if (SDL_GetGamepadID(*it) == id) { SDL_CloseGamepad(*it); g_gamepads.erase(it); log("input: gamepad disconnected"); return; }
  }
}

int8_t axis_to_stick(Sint16 v, int deadzone) {
  if (v > -deadzone && v < deadzone) return 0;
  int a = v / 258;
  return (int8_t)(a > 127 ? 127 : a < -127 ? -127 : a);
}

// Xbox/PlayStation-layout gamepads mapped the way Dolphin's default profile does.
void read_gamepad(SDL_Gamepad* pad, PadState& p) {
  p.err = 0;
  const ControllerConfig* cfg = controller_config_for(gamepad_guid(pad));
  const ControllerMap map = cfg ? cfg->map : ControllerMap::defaults();
  static const uint16_t gc_bits[GC_CTL_COUNT] = {GC_A, GC_B, GC_X, GC_Y, GC_Z, GC_L, GC_R, GC_START, GC_UP, GC_DOWN, GC_LEFT, GC_RIGHT};
  const int lt = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) / 128, rt = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 128;
  auto pressed = [&](int input, uint8_t* analog) -> bool {
    if (input == kUnbound) return false;
    if (input == kTriggerLeft || input == kTriggerRight) {
      const int v = input == kTriggerLeft ? lt : rt;
      if (analog && v > 30) *analog = (uint8_t)std::min(v, 255);
      return v >= std::max(1, map.trigger_press * 255 / 100);   // the press point is per controller
    }
    return SDL_GetGamepadButton(pad, (SDL_GamepadButton)input);
  };
  for (int i = 0; i < GC_CTL_COUNT; ++i) {
    uint8_t* analog = i == GC_CTL_L ? &p.trig_l : i == GC_CTL_R ? &p.trig_r : nullptr;
    if (pressed(map.binding[i], analog)) {
      p.button |= gc_bits[i];
      if (i == GC_CTL_L && map.binding[i] != kTriggerLeft && map.binding[i] != kTriggerRight) p.trig_l = 255;   // digital L/R press = full trigger
      if (i == GC_CTL_R && map.binding[i] != kTriggerLeft && map.binding[i] != kTriggerRight) p.trig_r = 255;
    }
  }
  Sint16 lx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), ly = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
  Sint16 rx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX), ry = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY);
  if (map.swap_sticks) { std::swap(lx, rx); std::swap(ly, ry); }
  const int dz = map.stick_deadzone * 32767 / 100, cdz = map.cstick_deadzone * 32767 / 100;
  if (std::abs(lx) > dz || std::abs(ly) > dz) { p.stick_x = axis_to_stick(lx, 0); p.stick_y = axis_to_stick((Sint16)std::clamp(-(int)ly, -32767, 32767), 0); }
  if (std::abs(rx) > cdz || std::abs(ry) > cdz) { p.sub_x = axis_to_stick(rx, 0); p.sub_y = axis_to_stick((Sint16)std::clamp(-(int)ry, -32767, 32767), 0); }
}

void read_keyboard(PadState& p) {
  int count = 0;
  const bool* keys = SDL_GetKeyboardState(&count);
  if (!keys) return;
  const KeyboardMap& km = keyboard_map();
  auto key = [&](int control) { const int code = km.key[control]; return code > 0 && code < count && keys[code]; };
  static const uint16_t gc_bits[GC_CTL_COUNT] = {GC_A, GC_B, GC_X, GC_Y, GC_Z, GC_L, GC_R, GC_START, GC_UP, GC_DOWN, GC_LEFT, GC_RIGHT};
  for (int i = 0; i < GC_CTL_COUNT; ++i) {
    if (!key(i)) continue;
    p.button |= gc_bits[i];
    if (i == GC_CTL_L) p.trig_l = 255;
    if (i == GC_CTL_R) p.trig_r = 255;
  }
  const int full = key(KB_MODIFIER) ? 127 * km.modifier_percent / 100 : 127;   // the modifier walks, tilts and light-shields
  int sx = 0, sy = 0, cx = 0, cy = 0;
  if (key(KB_STICK_LEFT)) sx -= full; if (key(KB_STICK_RIGHT)) sx += full;
  if (key(KB_STICK_UP)) sy += full; if (key(KB_STICK_DOWN)) sy -= full;
  if (key(KB_CSTICK_LEFT)) cx -= 127; if (key(KB_CSTICK_RIGHT)) cx += 127;
  if (key(KB_CSTICK_UP)) cy += 127; if (key(KB_CSTICK_DOWN)) cy -= 127;
  if (sx || sy) { p.stick_x = (int8_t)sx; p.stick_y = (int8_t)sy; }
  if (cx || cy) { p.sub_x = (int8_t)cx; p.sub_y = (int8_t)cy; }
}

// Development aid: MELEE_PAD_FILE names a text file re-read every poll. Each line is
// "[p=N] BUTTON+BUTTON [sx=N] [sy=N] [cx=N] [cy=N]"; the state holds until the file changes.
bool pad_file(PadState out[4]) {
  static const char* path = std::getenv("MELEE_PAD_FILE");
  if (!path) return false;
  std::ifstream file(path);
  if (!file) return false;
  bool any = false;
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') continue;
    int port = 0;
    PadState p{};
    size_t pos = 0;
    while (pos < line.size()) {
      while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t' || line[pos] == '+')) ++pos;
      size_t end = pos;
      while (end < line.size() && line[end] != ' ' && line[end] != '\t' && line[end] != '+') ++end;
      if (end == pos) break;
      const std::string tok = line.substr(pos, end - pos);
      pos = end;
      if (tok == "A") p.button |= GC_A; else if (tok == "B") p.button |= GC_B;
      else if (tok == "X") p.button |= GC_X; else if (tok == "Y") p.button |= GC_Y;
      else if (tok == "Z") p.button |= GC_Z; else if (tok == "L") { p.button |= GC_L; p.trig_l = 255; }
      else if (tok == "R") { p.button |= GC_R; p.trig_r = 255; } else if (tok == "START") p.button |= GC_START;
      else if (tok == "DU") p.button |= GC_UP; else if (tok == "DD") p.button |= GC_DOWN;
      else if (tok == "DL") p.button |= GC_LEFT; else if (tok == "DR") p.button |= GC_RIGHT;
      else if (tok.rfind("sx=", 0) == 0) p.stick_x = (int8_t)std::atoi(tok.c_str() + 3);
      else if (tok.rfind("sy=", 0) == 0) p.stick_y = (int8_t)std::atoi(tok.c_str() + 3);
      else if (tok.rfind("cx=", 0) == 0) p.sub_x = (int8_t)std::atoi(tok.c_str() + 3);
      else if (tok.rfind("cy=", 0) == 0) p.sub_y = (int8_t)std::atoi(tok.c_str() + 3);
      else if (tok.rfind("p=", 0) == 0) { port = std::atoi(tok.c_str() + 2) - 1; if (port < 0 || port > 3) port = 0; }
    }
    p.err = 0;
    out[port] = p;
    any = true;
  }
  if (!any) out[0].err = 0;
  return true;
}
}  // namespace

void* window_create(int w, int h, const wchar_t* title, bool visible) {
  if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) die("SDL: %s", SDL_GetError());
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  SDL_SetHint(SDL_HINT_IOS_HIDE_HOME_INDICATOR, "2");   // hidden, and the first swipe only shows it
  Uint32 flags = SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
  if (!visible) flags |= SDL_WINDOW_HIDDEN;
#if defined(__APPLE__) && !TARGET_OS_OSX
  flags |= SDL_WINDOW_FULLSCREEN;
  g_touch_seen = true;   // touch devices show the on-screen controller until a gamepad connects
#endif
  if (const char* force = std::getenv("MELEE_TOUCH_OVERLAY")) g_touch_forced = *force && *force != '0';
  g_window = SDL_CreateWindow(narrow(title).c_str(), w, h, flags);
  if (!g_window) die("SDL window: %s", SDL_GetError());
  g_view = SDL_Metal_CreateView(g_window);
  if (!g_view) die("SDL Metal view: %s", SDL_GetError());
  refresh_client_size();
  int count = 0;
  if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
    for (int i = 0; i < count; ++i) open_gamepad(ids[i]);
    SDL_free(ids);
  }
  return SDL_Metal_GetLayer(g_view);
}

void window_set_message_callback(MessageCallback cb) { g_message = std::move(cb); }
void window_input_capture(bool capture) { g_capture.store(capture); }
bool window_ui_gamecube_pad(PadState& pad) { std::lock_guard<std::mutex> lock(g_ui_mutex); pad = g_ui_pad; return g_ui_gamecube; }
void window_set_resize_callback(ResizeCallback cb) { g_resize = std::move(cb); }

void window_pump() {
  if (!g_window) return;
  SDL_Event event;
  bool resized = false;
  while (SDL_PollEvent(&event)) {
    switch (event.type) {
      case SDL_EVENT_QUIT: case SDL_EVENT_WINDOW_CLOSE_REQUESTED: g_closed.store(true); request_exit(0); break;
      case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: case SDL_EVENT_WINDOW_RESIZED: resized = true; break;
      case SDL_EVENT_GAMEPAD_ADDED: open_gamepad(event.gdevice.which); break;
      case SDL_EVENT_GAMEPAD_REMOVED: close_gamepad(event.gdevice.which); break;
      case SDL_EVENT_KEY_DOWN:
        if (event.key.key == SDLK_RETURN && (event.key.mod & SDL_KMOD_ALT) && !event.key.repeat) g_fullscreen_toggle.store(true);
        if (event.key.key == SDLK_Q && (event.key.mod & SDL_KMOD_GUI) && !event.key.repeat) { g_closed.store(true); request_exit(0); }   // Cmd+Q in full screen
        if (!event.key.repeat) g_last_key_press.store((int)event.key.scancode);
        if ((event.key.key == SDLK_F1 || event.key.key == SDLK_ESCAPE) && !event.key.repeat && !menu_capturing()) menu_toggle();
        break;
      case SDL_EVENT_FINGER_DOWN: case SDL_EVENT_FINGER_MOTION: case SDL_EVENT_FINGER_UP: case SDL_EVENT_FINGER_CANCELED:
        touch_event(event.tfinger);
        break;
      default: break;
    }
  }
  if (resized) {
    refresh_client_size();
    if (g_resize) g_resize(g_client_w, g_client_h);
  }
}

float window_safe_top_pixels() { return g_safe_top.load(); }
float window_pixels_per_point() { return g_pixels_per_point; }
GameRect window_game_rect(float ww, float wh, float aspect) {
  aspect = aspect > 0.0f ? aspect : 4.0f / 3.0f;
  const float st = g_safe_top.load(), sb = g_safe_bottom.load();
  if (wh > ww * 1.05f) {   // upright
    float w = ww, h = ww / aspect;
#if defined(__APPLE__) && TARGET_OS_IPHONE   // iPhone, iPad, Vision Pro: touch controls below, and iPhone Duo's fold
    float cap = std::max(wh * 0.5f - st, wh * 0.25f);   // keep the fold (and room for controls) below the game
    if (g_div_active.load()) {   // iPhone Duo: the real fold region; cross it visually rather than shrink below a fifth of the screen
      cap = std::max(g_div_y0.load() - st, wh * 0.2f);
    }
    if (h > cap) { h = cap; w = h * aspect; }
    return {(ww - w) * 0.5f, std::min(st, std::max(0.0f, wh - sb - h)), w, h};
#else
    (void)sb;
    return {(ww - w) * 0.5f, (wh - h) * 0.5f, w, h};   // a tall Mac window or portrait display: centred at full width
#endif
  }
  float h = wh, w = wh * aspect;
  if (w > ww) { w = ww; h = ww / aspect; }
  return {(ww - w) * 0.5f, (wh - h) * 0.5f, w, h};
}
void window_safe_insets(float& top, float& left, float& right, float& bottom) { top = g_safe_top.load(); left = g_safe_left.load(); right = g_safe_right.load(); bottom = g_safe_bottom.load(); }
void window_set_fullscreen(bool enabled) { if (g_window) SDL_SetWindowFullscreen(g_window, enabled); }

// ---- launcher services: controllers without a window
void window_input_init() {
  controller_rate_init();
  static bool done = false;
  if (done) return;
  done = true;
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) { log("input: SDL gamepad init failed: %s", SDL_GetError()); return; }
  int count = 0;
  if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) { for (int i = 0; i < count; ++i) open_gamepad(ids[i]); SDL_free(ids); }
}
std::vector<ControllerInfo> window_list_controllers() {
  std::vector<ControllerInfo> list;
  PadState scratch[4];
  gcadapter_poll(scratch);   // the adapter is opened lazily on first poll; without this the launcher never triggers it before a match starts
  GcAdapterStatus adapter;
  if (gcadapter_status(adapter)) {
    ControllerInfo info;
    info.name = "GameCube Controller Adapter"; info.guid = "gc-adapter"; info.is_gamecube_adapter = true; info.wired = true;
    info.adapter_ports = adapter.ports; info.adapter_interval_ms = adapter.interval_ms; info.report_hz = adapter.report_hz;
    list.push_back(info);
  }
  const std::vector<ControllerReport> reports = controller_reports();
  SDL_PumpEvents();
  SDL_Event event;
  while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_EVENT_GAMEPAD_ADDED, SDL_EVENT_GAMEPAD_REMOVED) > 0) {
    if (event.type == SDL_EVENT_GAMEPAD_ADDED) open_gamepad(event.gdevice.which);
    else if (event.type == SDL_EVENT_GAMEPAD_REMOVED) close_gamepad(event.gdevice.which);
  }
  for (SDL_Gamepad* pad : g_gamepads) {
    ControllerInfo info;
    info.name = SDL_GetGamepadName(pad) ? SDL_GetGamepadName(pad) : "Controller";
    info.guid = gamepad_guid(pad);
    info.instance_id = SDL_GetGamepadID(pad);
    if (const ControllerConfig* cfg = controller_config_for(info.guid)) info.assigned_port = cfg->port;
    for (const ControllerReport& r : reports)
      if (r.name == info.name || info.name.find(r.name) != std::string::npos || r.name.find(info.name) != std::string::npos) { info.report_hz = r.hz; info.wired = r.wired; break; }
    list.push_back(info);
  }
  return list;
}
int window_capture_input(const std::string& guid) {
  SDL_PumpEvents();
  for (SDL_Gamepad* pad : g_gamepads) {
    if (gamepad_guid(pad) != guid) continue;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) if (SDL_GetGamepadButton(pad, (SDL_GamepadButton)b)) return b;
    if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) return kTriggerLeft;
    if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) return kTriggerRight;
  }
  return kUnbound;
}
bool window_controller_state(const std::string& guid, ControllerLiveState& out) {
  SDL_PumpEvents();
  for (SDL_Gamepad* pad : g_gamepads) {
    if (gamepad_guid(pad) != guid) continue;
    auto axis = [&](SDL_GamepadAxis a) { return std::clamp(SDL_GetGamepadAxis(pad, a) / 32767.0f, -1.0f, 1.0f); };
    out.lx = axis(SDL_GAMEPAD_AXIS_LEFTX); out.ly = -axis(SDL_GAMEPAD_AXIS_LEFTY);
    out.rx = axis(SDL_GAMEPAD_AXIS_RIGHTX); out.ry = -axis(SDL_GAMEPAD_AXIS_RIGHTY);
    out.lt = std::max(0.0f, axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER)); out.rt = std::max(0.0f, axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    for (int b = 0; b < 32 && b < SDL_GAMEPAD_BUTTON_COUNT; ++b) out.button[b] = SDL_GetGamepadButton(pad, (SDL_GamepadButton)b);
    return true;
  }
  return false;
}
void window_test_rumble(const std::string& guid) {
  for (SDL_Gamepad* pad : g_gamepads) if (gamepad_guid(pad) == guid) { SDL_RumbleGamepad(pad, 0xC000, 0xC000, 350); return; }
}
int window_take_key_press() { return g_last_key_press.exchange(-1); }
// Rumble for a GameCube port served by an SDL gamepad (DualSense, Xbox, MFi, Switch Pro...).
void window_gamepad_rumble(int port, bool on) {
  if (port < 0 || port >= 4 || !g_gamepad_port[port]) return;
  for (SDL_Gamepad* pad : g_gamepads)
    if (SDL_GetGamepadID(pad) == g_gamepad_port[port]) {
      if (const ControllerConfig* c = controller_config_for(gamepad_guid(pad)); c && !c->map.rumble) on = false;
      SDL_RumbleGamepad(pad, on ? 0xC000 : 0, on ? 0xC000 : 0, on ? 250 : 0); return;
    }
}
bool window_take_fullscreen_toggle() { return g_fullscreen_toggle.exchange(false); }
double window_refresh_rate() {
  if (!g_window) return 60.0;
  const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(g_window));
  return mode && mode->refresh_rate > 0 ? mode->refresh_rate : 60.0;
}
void window_destroy() {
  for (SDL_Gamepad* pad : g_gamepads) SDL_CloseGamepad(pad);
  g_gamepads.clear();
  if (g_view) { SDL_Metal_DestroyView(g_view); g_view = nullptr; }
  if (g_window) { SDL_DestroyWindow(g_window); g_window = nullptr; }
}
void window_set_title(const wchar_t* title) { if (g_window) SDL_SetWindowTitle(g_window, narrow(title).c_str()); }
bool window_closed() { return g_closed.load(); }
void window_client_size(int* w, int* h) { if (w) *w = g_client_w; if (h) *h = g_client_h; }

bool input_load_script(const char* path) {
  std::ifstream file(path);
  if (!file) return false;
  std::string error;
  if (!g_script.load(file, &error)) { std::fprintf(stderr, "input script: %s\n", error.c_str()); return false; }
  g_scripted = true;
  return true;
}
void input_mark_match_start() { g_match_start.store(retrace_count()); }

void input_poll(PadState out[4]) {
  struct UiSnapshot {
    PadState* pads; bool gamecube = false;
    ~UiSnapshot() {
      std::lock_guard<std::mutex> lock(g_ui_mutex); g_ui_pad = pads[0]; g_ui_gamecube = gamecube;
      if (g_capture.load()) { pads[0] = {}; pads[0].err = 0; }
    }
  } ui{out};
  struct MenuPass { PadState* pads; ~MenuPass() { menu_frame(pads); } } menu_pass{out};   // after every input source, before the UI snapshot
  for (int i = 0; i < 4; ++i) { out[i] = {}; out[i].err = -1; }
  if (pad_file(out)) return;
  if (g_scripted) {
    auto pads = g_script.sample(retrace_count(), g_match_start.load(),
                                rd8(0x80479D30), rd8(0x80479D33));
    for (unsigned port = 0; port < pads.size(); ++port) {
      out[port].err = pads[port].connected ? 0 : -1;
      out[port].button = pads[port].buttons;
      out[port].stick_x = pads[port].sx; out[port].stick_y = pads[port].sy;
      out[port].sub_x = pads[port].cx; out[port].sub_y = pads[port].cy;
    }
    return;
  }
  const uint32_t adapter_mask = gcadapter_poll(out);
  ui.gamecube = (adapter_mask & 1u) != 0;
  // Gamepads fill ports in connection order after the adapter's; the keyboard adds to port 1.
  for (int i = 0; i < 4; ++i) g_gamepad_port[i] = 0;
  // Gamepads with a fixed port take it first; the rest fill free ports in connection order.
  bool taken[4] = {(adapter_mask & 1u) != 0, (adapter_mask & 2u) != 0, (adapter_mask & 4u) != 0, (adapter_mask & 8u) != 0};
  std::vector<SDL_Gamepad*> floating;
  for (SDL_Gamepad* pad : g_gamepads) {
    const ControllerConfig* cfg = controller_config_for(gamepad_guid(pad));
    const int fixed = cfg ? cfg->port : 0;
    if (fixed >= 1 && fixed <= 4 && !taken[fixed - 1]) { taken[fixed - 1] = true; read_gamepad(pad, out[fixed - 1]); g_gamepad_port[fixed - 1] = SDL_GetGamepadID(pad); }
    else floating.push_back(pad);
  }
  int port = 0;
  for (SDL_Gamepad* pad : floating) {
    while (port < 4 && taken[port]) ++port;
    if (port >= 4) break;
    taken[port] = true; read_gamepad(pad, out[port]); g_gamepad_port[port] = SDL_GetGamepadID(pad); ++port;
  }
  if (!(adapter_mask & 1u)) { out[0].err = 0; read_keyboard(out[0]); read_touch(out[0]); }
}

// The letterbox is never bare black: Melee's menu grid continues around the picture, faintly, the way Apple asks games to
// fill the padding when the picture cannot change its aspect ratio (iPhone Duo, iPad, a Mac display that is not 4:3).
void letterbox_artwork(std::vector<OverlayShape>& out, int ww, int wh) {
  const GameRect g = window_game_rect((float)ww, (float)wh, g_touch_aspect);
  const float pt = std::max(g_pixels_per_point, 1.0f), step = 44.0f * pt, line = std::max(1.0f, pt * 0.75f);
  const float gx0 = std::floor(g.x), gy0 = std::floor(g.y), gx1 = std::ceil(g.x + g.w), gy1 = std::ceil(g.y + g.h);
  if (gx0 < 2 && gy0 < 2 && gx1 > ww - 2 && gy1 > wh - 2) return;   // the picture fills the window
  auto add = [&](float x0, float y0, float x1, float y1, float a) {
    // Clip each line against the picture so the artwork only ever sits in the padding.
    if (x1 <= gx0 || x0 >= gx1 || y1 <= gy0 || y0 >= gy1) { out.push_back({x0, y0, x1, y1, 0.55f, 0.62f, 1.0f, a, 0.0f, 0.0f, 0.0f, 0, 0.0f, 0.0f}); return; }
    if (x1 - x0 > y1 - y0) {   // horizontal line crossing the picture: keep the parts left and right of it
      if (x0 < gx0) out.push_back({x0, y0, gx0, y1, 0.55f, 0.62f, 1.0f, a, 0.0f, 0.0f, 0.0f, 0, 0.0f, 0.0f});
      if (x1 > gx1) out.push_back({gx1, y0, x1, y1, 0.55f, 0.62f, 1.0f, a, 0.0f, 0.0f, 0.0f, 0, 0.0f, 0.0f});
    } else {
      if (y0 < gy0) out.push_back({x0, y0, x1, gy0, 0.55f, 0.62f, 1.0f, a, 0.0f, 0.0f, 0.0f, 0, 0.0f, 0.0f});
      if (y1 > gy1) out.push_back({x0, gy1, x1, y1, 0.55f, 0.62f, 1.0f, a, 0.0f, 0.0f, 0.0f, 0, 0.0f, 0.0f});
    }
  };
  const float cx = ww * 0.5f, cy = wh * 0.5f;   // grid centred on the display, so it is symmetric about iPhone Duo's fold
  for (float x = std::fmod(cx, step); x < ww; x += step) add(x - line * 0.5f, 0, x + line * 0.5f, (float)wh, 0.07f);
  for (float y = std::fmod(cy, step); y < wh; y += step) add(0, y - line * 0.5f, (float)ww, y + line * 0.5f, 0.07f);
  // A thin yellow keyline where the picture meets the padding, like the frame around Melee's menus.
  const float k = std::max(1.0f, pt);
  if (gy1 < wh - 2) add(gx0, gy1, gx1, gy1 + k, 0.35f);
  if (gy0 > 2) add(gx0, gy0 - k, gx1, gy0, 0.35f);
  if (gx0 > 2) add(gx0 - k, gy0, gx0, gy1, 0.35f);
  if (gx1 < ww - 2) add(gx1, gy0, gx1 + k, gy1, 0.35f);
  for (size_t i = out.size() >= 4 ? out.size() - 4 : 0; i < out.size(); ++i) if (out[i].a > 0.3f) { out[i].r = 0.97f; out[i].g = 0.79f; out[i].b = 0.28f; }
}
bool game_overlay(OverlayFrame& out) {
  out.texts.clear();
  const bool touch = touch_overlay(out);   // clears shapes, sets alpha to the touch fade
  for (OverlayShape& s : out.shapes) s.a *= out.alpha;   // bake the touch fade: artwork, menu and HUD are drawn at full opacity
  out.alpha = 1.0f;
  int w = 0, h = 0; window_client_size(&w, &h);
  std::vector<OverlayShape> art;
  { std::lock_guard<std::mutex> lock(g_touch_mutex); letterbox_artwork(art, w, h); }
  out.shapes.insert(out.shapes.begin(), art.begin(), art.end());   // under the touch controls
  menu_overlay(out, w, h, touch);
  return !out.shapes.empty() || !out.texts.empty();
}
}  // namespace host
