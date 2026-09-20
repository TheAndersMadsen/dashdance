// See game_menu.h.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "game_menu.h"
#include "host.h"
#include "input_config.h"
#include "window.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace host {
namespace {
constexpr uint16_t BTN_LEFT = 0x0001, BTN_RIGHT = 0x0002, BTN_DOWN = 0x0004, BTN_UP = 0x0008, BTN_R = 0x0020, BTN_L = 0x0040,
                   BTN_A = 0x0100, BTN_B = 0x0200, BTN_START = 0x1000;
constexpr int kEscapeScancode = 41;   // SDL_SCANCODE_ESCAPE
constexpr int kSkipFrames = 6 * 60;   // remap: a control left untouched for 6 s keeps its binding

enum class Page { Main, Controls, Remap };
enum MainRow { ROW_SCALE, ROW_ANISO, ROW_UPSCALE, ROW_SHARPEN, ROW_VSYNC, ROW_WIDESCREEN, ROW_VOLUME, ROW_TOUCH_OPACITY, ROW_TOUCH_SIZE, ROW_FULLSCREEN, ROW_ONLINE_DELAY, ROW_CONTROLS, ROW_HUD, ROW_RESUME, ROW_MAIN_COUNT };
enum ControlsRow { C_DEVICE, C_REMAP, C_STICK_DZ, C_CSTICK_DZ, C_TRIGGER, C_SWAP, C_RUMBLE, C_TEST_RUMBLE, C_MODIFIER, C_RESET, C_BACK };
struct Device { std::string guid, name; };   // an empty guid is the keyboard

// The menu runs on the simulation thread and is drawn on the render thread: everything below except
// the atomics is guarded by g_mutex.
std::mutex g_mutex;
RuntimeSettings g_settings;
std::function<void(const RuntimeSettings&, MenuChange)> g_apply;
std::atomic<bool> g_open{false}, g_toggle_request{false}, g_changed{false}, g_capturing{false};
Page g_page = Page::Main;
int g_selected = 0;
std::vector<Device> g_devices;
int g_device = 0;
int g_step = 0, g_step_frames = 0;
bool g_step_armed = false;
int g_combo_frames = 0;
uint16_t g_prev_buttons = 0;
int g_prev_dir = 0, g_repeat = 0;
bool g_touch_visible = false;
float g_menu_button[4] = {0, 0, 0, 0};   // MENU touch button (window pixels), set by the overlay
struct Layout { float x0, y0, x1, y1, row_h, top; } g_layout{};   // last drawn panel, for touch hits
int g_open_at_frame = -1;

std::string ascii(const std::string& s) { std::string out; for (char c : s) if (c >= 32 && c < 127) out += c; return out; }   // the glyph atlas is ASCII
std::string upper(const std::string& s) { std::string out = ascii(s); for (char& c : out) c = (char)std::toupper((unsigned char)c); return out; }
int index_of(const std::vector<int>& rows, int row) { for (size_t i = 0; i < rows.size(); ++i) if (rows[i] == row) return (int)i; return 0; }

// ---- main page
bool apple_graphics() {
#if defined(__APPLE__)
  return true;
#else
  return false;
#endif
}
std::vector<int> main_rows() {
  std::vector<int> v;
  for (int r = 0; r < ROW_MAIN_COUNT; ++r) {
    if ((r == ROW_TOUCH_OPACITY || r == ROW_TOUCH_SIZE) && !g_touch_visible) continue;
    if ((r == ROW_FULLSCREEN || r == ROW_VSYNC) && g_touch_visible) continue;   // macOS-only controls
    if (r == ROW_UPSCALE && !apple_graphics()) continue;                       // Metal only
    v.push_back(r);
  }
  return v;
}
const char* main_name(int r) {
  switch (r) {
    case ROW_SCALE: return "Internal resolution";
    case ROW_ANISO: return "Anisotropic filtering";
    case ROW_UPSCALE: return "MetalFX upscaling";
    case ROW_SHARPEN: return "Sharpen";
    case ROW_VSYNC: return "Display sync";
    case ROW_WIDESCREEN: return "Widescreen (next launch)";
    case ROW_VOLUME: return "Volume";
    case ROW_TOUCH_OPACITY: return "Touch controls opacity";
    case ROW_TOUCH_SIZE: return "Touch controls size";
    case ROW_FULLSCREEN: return "Full screen";
    case ROW_ONLINE_DELAY: return "Online input delay (next match)";
    case ROW_CONTROLS: return "Controls";
    case ROW_HUD: return "Performance HUD";
    case ROW_RESUME: return "Resume game";
  }
  return "";
}
std::string main_value(int r, const RuntimeSettings& s) {
  char b[48];
  switch (r) {
    case ROW_SCALE: if (s.scale == 0) return "Auto"; std::snprintf(b, sizeof b, "%dx", s.scale); return b;
    case ROW_ANISO: return s.anisotropy >= 16 ? "16x" : s.anisotropy >= 4 ? "4x" : "Off";
    case ROW_UPSCALE: return s.upscaler == 2 ? "MetalFX (quality)" : s.upscaler == 1 ? "MetalFX (balanced)" : "Off";
    case ROW_SHARPEN: std::snprintf(b, sizeof b, "%d%%", (int)std::lround(s.sharpness * 100)); return b;
    case ROW_VSYNC: return s.vsync ? "On" : "Off (lowest latency)";
    case ROW_WIDESCREEN: return s.widescreen ? "16:9" : "4:3";
    case ROW_VOLUME: std::snprintf(b, sizeof b, "%d%%", s.volume); return b;
    case ROW_TOUCH_OPACITY: std::snprintf(b, sizeof b, "%d%%", (int)std::lround(s.overlay_opacity * 100)); return b;
    case ROW_TOUCH_SIZE: std::snprintf(b, sizeof b, "%.2fx", s.overlay_scale); return b;
    case ROW_FULLSCREEN: return s.fullscreen ? "On" : "Off";
    case ROW_ONLINE_DELAY: if (s.online_delay <= 1) return "1 frame (lowest)"; std::snprintf(b, sizeof b, "%d frames", s.online_delay); return b;
    case ROW_CONTROLS: return ">";
    case ROW_HUD: return s.hud ? "On" : "Off";
  }
  return "";
}
// Steps a main-page value by `dir`; returns the change class, or -1 when nothing changed.
int main_step(int r, int dir, RuntimeSettings& s) {
  static const int scales[] = {0, 1, 2, 3, 4, 6, 8};
  switch (r) {
    case ROW_SCALE: { int i = 0; for (int k = 0; k < 7; ++k) if (scales[k] == s.scale) i = k; i = std::clamp(i + dir, 0, 6); s.scale = scales[i]; return (int)MenuChange::Graphics; }
    case ROW_ANISO: { const int a[] = {1, 4, 16}; int i = s.anisotropy >= 16 ? 2 : s.anisotropy >= 4 ? 1 : 0; i = std::clamp(i + dir, 0, 2); s.anisotropy = a[i]; return (int)MenuChange::Graphics; }
    case ROW_UPSCALE: s.upscaler = (s.upscaler + 3 + dir) % 3; return (int)MenuChange::Graphics;
    case ROW_SHARPEN: s.sharpness = std::clamp(s.sharpness + 0.1f * dir, 0.0f, 1.0f); return (int)MenuChange::Graphics;
    case ROW_VSYNC: s.vsync = !s.vsync; return (int)MenuChange::Graphics;
    case ROW_WIDESCREEN: s.widescreen = !s.widescreen; return (int)MenuChange::Widescreen;
    case ROW_VOLUME: s.volume = std::clamp(s.volume + 10 * dir, 0, 100); return (int)MenuChange::Volume;
    case ROW_TOUCH_OPACITY: s.overlay_opacity = std::clamp(s.overlay_opacity + 0.1f * dir, 0.1f, 1.0f); return (int)MenuChange::TouchControls;
    case ROW_TOUCH_SIZE: s.overlay_scale = std::clamp(s.overlay_scale + 0.1f * dir, 0.7f, 1.4f); return (int)MenuChange::TouchControls;
    case ROW_FULLSCREEN: s.fullscreen = !s.fullscreen; return (int)MenuChange::Fullscreen;
    case ROW_ONLINE_DELAY: { const int d = std::clamp(s.online_delay + dir, 1, 9); if (d == s.online_delay) return -1; s.online_delay = d; return (int)MenuChange::OnlineDelay; }
    case ROW_HUD: s.hud = !s.hud; return (int)MenuChange::Hud;
  }
  return -1;
}

// ---- controls page
const Device* device() { return g_devices.empty() ? nullptr : &g_devices[(size_t)std::clamp(g_device, 0, (int)g_devices.size() - 1)]; }
bool is_keyboard(const Device* d) { return d && d->guid.empty(); }
ControllerConfig pad_config(const std::string& guid) { ControllerConfig c; if (const ControllerConfig* e = controller_config_for(guid)) c = *e; c.guid = guid; return c; }
void refresh_devices() {
  const std::string keep = device() ? device()->guid : std::string("\x01");
  g_devices.clear();
#if !defined(__APPLE__) || TARGET_OS_OSX
  g_devices.push_back({"", "Keyboard"});
#endif
  for (const ControllerInfo& c : window_list_controllers()) if (!c.is_gamecube_adapter) g_devices.push_back({c.guid, c.name});
  g_device = 0;
  for (size_t i = 0; i < g_devices.size(); ++i) if (g_devices[i].guid == keep) g_device = (int)i;
}
std::vector<int> controls_rows() {
  std::vector<int> v{C_DEVICE};
  if (const Device* d = device()) {
    v.push_back(C_REMAP);
    if (is_keyboard(d)) v.push_back(C_MODIFIER);
    else for (int r : {C_STICK_DZ, C_CSTICK_DZ, C_TRIGGER, C_SWAP, C_RUMBLE, C_TEST_RUMBLE}) v.push_back(r);
    v.push_back(C_RESET);
  }
  v.push_back(C_BACK);
  return v;
}
bool value_row(int r) { return r == C_DEVICE || r == C_STICK_DZ || r == C_CSTICK_DZ || r == C_TRIGGER || r == C_SWAP || r == C_RUMBLE || r == C_MODIFIER; }
const char* controls_name(int r) {
  switch (r) {
    case C_DEVICE: return "Controller";
    case C_REMAP: return is_keyboard(device()) ? "Remap keys" : "Remap buttons";
    case C_STICK_DZ: return "Stick deadzone";
    case C_CSTICK_DZ: return "C-stick deadzone";
    case C_TRIGGER: return "Trigger press point";
    case C_SWAP: return "Swap sticks";
    case C_RUMBLE: return "Rumble";
    case C_TEST_RUMBLE: return "Test rumble";
    case C_MODIFIER: return "Stick amount with modifier";
    case C_RESET: return "Reset to defaults";
    case C_BACK: return "Back";
  }
  return "";
}
std::string controls_value(int r) {
  const Device* d = device();
  char b[48];
  const ControllerMap m = d && !is_keyboard(d) ? pad_config(d->guid).map : ControllerMap::defaults();
  switch (r) {
    case C_DEVICE: return d ? ascii(d->name) : std::string("No controller connected");
    case C_REMAP: return "Start";
    case C_STICK_DZ: std::snprintf(b, sizeof b, "%d%%", m.stick_deadzone); return b;
    case C_CSTICK_DZ: std::snprintf(b, sizeof b, "%d%%", m.cstick_deadzone); return b;
    case C_TRIGGER: std::snprintf(b, sizeof b, "%d%%", m.trigger_press); return b;
    case C_SWAP: return m.swap_sticks ? "On" : "Off";
    case C_RUMBLE: return m.rumble ? "On" : "Off";
    case C_MODIFIER: std::snprintf(b, sizeof b, "%d%%", keyboard_map().modifier_percent); return b;
  }
  return "";
}
// Acts on a controls-page row; returns true when a saved setting changed.
bool controls_act(int r, int dir) {
  const Device* d = device();
  switch (r) {
    case C_DEVICE: if (g_devices.size() > 1) g_device = (g_device + dir + (int)g_devices.size()) % (int)g_devices.size(); return false;
    case C_REMAP:
      if (!d) return false;
      g_page = Page::Remap; g_step = 0; g_step_frames = 0; g_step_armed = false; g_capturing.store(true);
      window_take_key_press();   // forget keys pressed before the remap started
      return false;
    case C_BACK: g_page = Page::Main; g_selected = index_of(main_rows(), ROW_CONTROLS); return false;
    case C_TEST_RUMBLE: if (d && !is_keyboard(d)) window_test_rumble(d->guid); return false;
    case C_MODIFIER: { KeyboardMap k = keyboard_map(); k.modifier_percent = std::clamp(k.modifier_percent + 5 * dir, 20, 90); set_keyboard_map(k); return true; }
    case C_RESET:
      if (!d) return false;
      if (is_keyboard(d)) set_keyboard_map(KeyboardMap::defaults());
      else { ControllerConfig c = pad_config(d->guid); c.map = ControllerMap::defaults(); upsert_controller_config(c); }
      return true;
  }
  if (!d || is_keyboard(d)) return false;
  ControllerConfig c = pad_config(d->guid);
  switch (r) {
    case C_STICK_DZ: c.map.stick_deadzone = std::clamp(c.map.stick_deadzone + 2 * dir, 0, 60); break;
    case C_CSTICK_DZ: c.map.cstick_deadzone = std::clamp(c.map.cstick_deadzone + 2 * dir, 0, 60); break;
    case C_TRIGGER: c.map.trigger_press = std::clamp(c.map.trigger_press + 5 * dir, 20, 100); break;
    case C_SWAP: c.map.swap_sticks = !c.map.swap_sticks; break;
    case C_RUMBLE: c.map.rumble = !c.map.rumble; break;
    default: return false;
  }
  upsert_controller_config(c);
  return true;
}

// ---- remap page
int step_count() { return is_keyboard(device()) ? KB_COUNT : GC_CTL_COUNT; }
const char* step_name(int i) { return is_keyboard(device()) ? kKeyboardControlNames[i] : kGcControlNames[i]; }
std::string step_binding(int i) {
  const Device* d = device();
  if (!d) return "";
  return is_keyboard(d) ? key_name(keyboard_map().key[i]) : physical_input_name(pad_config(d->guid).map.binding[i]);
}
void remap_finish() { g_page = Page::Controls; g_capturing.store(false); g_selected = index_of(controls_rows(), C_REMAP); }
void remap_next() { ++g_step; g_step_frames = 0; g_step_armed = false; if (g_step >= step_count()) remap_finish(); }
void remap_assign(int input) {   // the same input on another control is cleared, so one press never does two things
  const Device* d = device();
  if (!d) return;
  if (is_keyboard(d)) {
    KeyboardMap k = keyboard_map();
    for (int j = 0; j < KB_COUNT; ++j) if (j != g_step && k.key[j] == input) k.key[j] = 0;
    k.key[g_step] = input; set_keyboard_map(k);
  } else {
    ControllerConfig c = pad_config(d->guid);
    for (int j = 0; j < GC_CTL_COUNT; ++j) if (j != g_step && c.map.binding[j] == input) c.map.binding[j] = kUnbound;
    c.map.binding[g_step] = input; upsert_controller_config(c);
  }
  g_changed.store(true);
}

std::vector<int> page_rows() { return g_page == Page::Main ? main_rows() : g_page == Page::Controls ? controls_rows() : std::vector<int>{}; }
void set_open(bool open) {
  g_open.store(open);
  g_combo_frames = 0; g_repeat = 0; g_prev_dir = 0;
  if (!open) { g_page = Page::Main; g_capturing.store(false); }
  if (g_selected >= (int)page_rows().size()) g_selected = 0;
}
void open_controls() { refresh_devices(); g_page = Page::Controls; g_selected = 0; }
}  // namespace

void menu_init(const RuntimeSettings& initial, std::function<void(const RuntimeSettings&, MenuChange)> apply) {
  if (const char* open = std::getenv("MELEE_MENU_OPEN")) {   // screenshot aid: start with the menu open
    if (!open[0] || std::atoi(open)) set_open(true);
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  g_settings = initial; g_apply = std::move(apply);
  if (const char* e = std::getenv("MELEE_HUD")) g_settings.hud = *e && *e != '0';
  if (const char* e = std::getenv("MELEE_MENU_OPEN")) g_open_at_frame = std::atoi(e);   // screenshot aid: open at this retrace
}
bool menu_is_open() { return g_open.load(); }
bool menu_capturing() { return g_open.load() && g_capturing.load(); }
bool menu_changed() { return g_changed.load(); }
RuntimeSettings menu_settings() { std::lock_guard<std::mutex> lock(g_mutex); return g_settings; }
void menu_toggle() { g_toggle_request.store(true); }

void menu_frame(PadState pads[4]) {
  int apply_what = -1;
  RuntimeSettings snapshot;
  std::function<void(const RuntimeSettings&, MenuChange)> apply;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_open_at_frame >= 0 && (int)retrace_count() >= g_open_at_frame) {
      g_open_at_frame = -1; set_open(true);
      if (const char* page = std::getenv("MELEE_MENU_PAGE")) {   // screenshot aid: "controls" or "remap"
        open_controls();
        if (!std::strcmp(page, "remap")) controls_act(C_REMAP, 1);
      }
    }
    if (g_toggle_request.exchange(false)) set_open(!g_open.load());
    // The open combo: L + R + Start held on any connected controller for half a second.
    bool combo = false;
    for (int i = 0; i < 4; ++i) if (pads[i].err == 0 && (pads[i].button & (BTN_L | BTN_R | BTN_START)) == (BTN_L | BTN_R | BTN_START)) combo = true;
    g_combo_frames = combo ? g_combo_frames + 1 : 0;
    if (g_combo_frames == 30) set_open(!g_open.load());
    if (!g_open.load()) { g_prev_buttons = 0; return; }

    if (g_page == Page::Remap) {
      const Device* d = device();
      if (!d) remap_finish();
      else {
        int input = kUnbound;
        if (is_keyboard(d)) {
          const int key = window_take_key_press();
          if (key == kEscapeScancode) remap_finish();
          else if (key > 0) input = key;
        } else {
          const int held = window_capture_input(d->guid);
          if (!g_step_armed) { if (held == kUnbound) g_step_armed = true; }   // wait for the previous press to be released
          else input = held;
        }
        if (g_page == Page::Remap) {
          if (input != kUnbound) { remap_assign(input); remap_next(); }
          else if (++g_step_frames >= kSkipFrames) remap_next();
        }
      }
      g_prev_buttons = 0; g_prev_dir = 0;
    } else {
      // Navigation from whichever pad is active (the keyboard maps onto port 1's pad).
      uint16_t buttons = 0; int dir_y = 0, dir_x = 0;
      for (int i = 0; i < 4; ++i) {
        if (pads[i].err != 0) continue;
        buttons |= pads[i].button;
        if (pads[i].stick_y > 64 || (pads[i].button & BTN_UP)) dir_y = -1; else if (pads[i].stick_y < -64 || (pads[i].button & BTN_DOWN)) dir_y = 1;
        if (pads[i].stick_x > 64 || (pads[i].button & BTN_RIGHT)) dir_x = 1; else if (pads[i].stick_x < -64 || (pads[i].button & BTN_LEFT)) dir_x = -1;
      }
      const int dir = dir_y ? dir_y * 2 : dir_x;   // one axis at a time; vertical wins
      bool fire = false;
      if (dir != g_prev_dir) { g_repeat = 0; fire = dir != 0; }
      else if (dir != 0 && ++g_repeat >= 20 && (g_repeat - 20) % 6 == 0) fire = true;
      g_prev_dir = dir;
      const std::vector<int> rows = page_rows();
      if (!rows.empty()) g_selected = std::min(g_selected, (int)rows.size() - 1);
      const uint16_t pressed = buttons & ~g_prev_buttons;
      g_prev_buttons = buttons;
      auto main_change = [&](int r, int step) {
        const int what = main_step(r, step, g_settings);
        if (what >= 0) { g_changed.store(true); apply_what = what; snapshot = g_settings; apply = g_apply; }
      };
      if (fire && dir_y && !rows.empty()) g_selected = (g_selected + dir_y + (int)rows.size()) % (int)rows.size();
      else if (fire && dir_x && !rows.empty()) {
        const int r = rows[g_selected];
        if (g_page == Page::Main) { if (r != ROW_RESUME && r != ROW_CONTROLS) main_change(r, dir_x); }
        else if (value_row(r) && controls_act(r, dir_x)) g_changed.store(true);
      }
      if (!combo && !rows.empty()) {
        const int r = rows[g_selected];
        if (pressed & BTN_A) {
          if (g_page == Page::Main) {
            if (r == ROW_RESUME) set_open(false);
            else if (r == ROW_CONTROLS) open_controls();
            else main_change(r, 1);
          } else if (controls_act(r, 1)) g_changed.store(true);
        } else if (pressed & BTN_B) {
          if (g_page == Page::Controls) { g_page = Page::Main; g_selected = index_of(main_rows(), ROW_CONTROLS); }
          else set_open(false);
        } else if (pressed & BTN_START) set_open(false);
      }
    }
    // The game sees neutral pads while the menu is up (their connection state is kept).
    for (int i = 0; i < 4; ++i) { const int8_t err = pads[i].err; pads[i] = {}; pads[i].err = err; }
  }
  if (apply_what >= 0 && apply) apply(snapshot, (MenuChange)apply_what);
}

bool menu_touch(float px, float py) {
  int apply_what = -1;
  RuntimeSettings snapshot;
  std::function<void(const RuntimeSettings&, MenuChange)> apply;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_open.load()) {
      if (g_touch_visible && px >= g_menu_button[0] && px <= g_menu_button[2] && py >= g_menu_button[1] && py <= g_menu_button[3]) { set_open(true); return true; }
      return false;
    }
    if (g_page == Page::Remap) { remap_finish(); return true; }   // a tap cancels a remap
    const Layout l = g_layout;
    if (px < l.x0 || px > l.x1 || py < l.y0 || py > l.y1) {   // tap outside: back a page, or close
      if (g_page == Page::Controls) { g_page = Page::Main; g_selected = index_of(main_rows(), ROW_CONTROLS); } else set_open(false);
      return true;
    }
    const int row = (int)((py - l.top) / std::max(l.row_h, 1.0f));
    const std::vector<int> rows = page_rows();
    if (row < 0 || row >= (int)rows.size()) return true;
    g_selected = row;
    const int r = rows[row];
    const bool left = px < l.x0 + (l.x1 - l.x0) / 3.0f;   // left third lowers, the rest raises
    if (g_page == Page::Main) {
      if (r == ROW_RESUME) set_open(false);
      else if (r == ROW_CONTROLS) open_controls();
      else {
        const int what = main_step(r, left ? -1 : 1, g_settings);
        if (what >= 0) { g_changed.store(true); apply_what = what; snapshot = g_settings; apply = g_apply; }
      }
    } else if (controls_act(r, left && value_row(r) ? -1 : 1)) g_changed.store(true);
  }
  if (apply_what >= 0 && apply) apply(snapshot, (MenuChange)apply_what);
  return true;
}

void menu_overlay(OverlayFrame& out, int ww, int wh, bool touch_controls_visible) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_touch_visible = touch_controls_visible;
  const RuntimeSettings s = g_settings;
  const float unit = std::min(18.0f * std::max(window_pixels_per_point(), 1.0f), std::min(ww, wh) / 22.0f);   // 18 pt lines on every display, smaller only when the window is tiny
  float safe_t = 0, safe_l = 0, safe_r = 0, safe_b = 0;
  window_safe_insets(safe_t, safe_l, safe_r, safe_b);   // keep clear of the island, the rounded corners and the home indicator
  // Performance HUD: one line, top-left.
  if (s.hud) {
    GcAdapterStatus adapter; const bool have_adapter = gcadapter_status(adapter);
    char line[200];
    char rate[24] = "";
    if (have_adapter && adapter.report_hz > 0) std::snprintf(rate, sizeof rate, "%.0f Hz", adapter.report_hz);   // measured, not bucketed: third-party adapters land between 125 and 1000
    std::snprintf(line, sizeof line, "sim %.1f ms   display %.0f Hz   late %llu%s%s", last_sim_frame_ms(), window_refresh_rate(), (unsigned long long)late_frame_count(),
                  have_adapter ? "   GC adapter " : "", rate);
    std::string hud = line;
    if (const int ping = online_ping_ms(); ping >= 0) { hud += "   ping "; hud += std::to_string(ping); hud += " ms"; }
    if (const char* warn = latency_warning(); warn && *warn) { hud += "   "; hud += warn; }   // Low Power Mode, Bluetooth audio
    const float h = unit * 0.7f, pad = h * 0.4f, w = h * 0.55f * (float)hud.size() + pad * 2;
    const float hx = pad + safe_l, hy = pad + safe_t;
    out.shapes.push_back({hx, hy, hx + w, hy + h + pad * 1.5f, 0.0f, 0.0f, 0.0f, 0.55f, h * 0.35f, 0.0f, 0.0f, 0, 0.0f, 0.0f});
    out.texts.push_back({hx + pad, hy + pad * 0.6f, h, 1, 1, 1, 0.92f, 0, hud, (float)ww - safe_l - safe_r - 4 * pad});
  }
  if (!g_open.load()) {
    if (touch_controls_visible) {   // MENU button, top-right
      const float h = unit * 1.1f, w = h * 2.6f, m = unit * 0.5f;
      g_menu_button[0] = ww - m - safe_r - w; g_menu_button[1] = m + safe_t; g_menu_button[2] = ww - m - safe_r; g_menu_button[3] = m + safe_t + h;
      out.shapes.push_back({g_menu_button[0], g_menu_button[1], g_menu_button[2], g_menu_button[3], 1, 1, 1, 0.18f, h * 0.5f, 2.0f, 0.0f, 0, 0.0f, 0.0f});
      out.texts.push_back({g_menu_button[0] + w * 0.5f, g_menu_button[1] + h * 0.22f, h * 0.56f, 1, 1, 1, 0.9f, 1, "MENU", w * 0.82f});
    }
    return;
  }
  // Dim the game, then the panel.
  out.shapes.push_back({0, 0, (float)ww, (float)wh, 0, 0, 0, 0.55f, 0.0f, 0.0f, 0.0f, 0, 0.0f, 0.0f});
  const std::vector<int> rows = page_rows();
  const int body_rows = g_page == Page::Remap ? 9 : (int)rows.size();
  const float row_h = unit * 1.45f, pad = unit * 0.9f, title_h = unit * 1.3f;
  const float avail_w = (float)ww - safe_l - safe_r, avail_h = (float)wh - safe_t - safe_b;
  const float panel_w = std::min(avail_w - 2 * pad, unit * 24.0f);
  const float panel_h = pad + title_h + unit * 0.5f + row_h * body_rows + unit * 1.6f + pad;
  float x0 = safe_l + (avail_w - panel_w) * 0.5f, y0 = safe_t + std::max(pad, (avail_h - panel_h) * 0.5f);
  // iPhone Duo: an interactive panel never sits in the fold. Shift it whole into the larger
  // adjacent region — the minimum displacement that clears the band — and fall back to centered
  // when the band would push it off-screen (tiny regions get a centered panel over the curve
  // rather than a clipped one).
  float div[4];
  if (window_division_region(div)) {
    const float bx0 = div[0], by0 = div[1], bx1 = div[2], by1 = div[3];
    const float cx1 = x0 + panel_w, cy1 = y0 + panel_h;
    if (cx1 > bx0 && x0 < bx1 && cy1 > by0 && y0 < by1) {
      // Move the panel whole into the larger adjacent region, then clamp on-screen; if neither
      // region fits it, the centered panel stays over the curve rather than being clipped.
      if (by1 - by0 < bx1 - bx0) {   // horizontal band: above or below
        const float above = by0 - safe_t, below = (float)wh - safe_b - by1;
        const float want = below >= above ? by1 : by0 - panel_h;
        if (std::max(above, below) >= panel_h) y0 = std::clamp(want, safe_t, std::max(safe_t, (float)wh - safe_b - panel_h));
      } else {                       // vertical band: left or right
        const float left = bx0 - safe_l, right = (float)ww - safe_r - bx1;
        const float want = right >= left ? bx1 : bx0 - panel_w;
        if (std::max(left, right) >= panel_w) x0 = std::clamp(want, safe_l, std::max(safe_l, (float)ww - safe_r - panel_w));
      }
    }
  }
  const float x1 = x0 + panel_w, y1 = y0 + panel_h;
  out.shapes.push_back({x0, y0, x1, y1, 0.03f, 0.04f, 0.12f, 0.985f, unit * 0.7f, 0.0f, 0.0f, 0, 0.0f, 0.0f});
  out.shapes.push_back({x0, y0, x1, y1, 0.5f, 0.6f, 1.0f, 0.25f, unit * 0.7f, 1.5f, 0.0f, 0, 0.0f, 0.0f});
  // Title bar in Melee's angled yellow.
  const std::string title = g_page == Page::Main ? "DASHDANCE  SETTINGS" : g_page == Page::Controls ? "CONTROLS" : "REMAP  " + upper(device() ? device()->name : "");
  out.shapes.push_back({x0 + pad, y0 + pad, x0 + pad + panel_w * 0.62f, y0 + pad + title_h, 0.97f, 0.79f, 0.28f, 1.0f, unit * 0.15f, 0.0f, 0.0f, 0, 0.0f, 0.0f});
  out.texts.push_back({x0 + pad + unit * 0.5f, y0 + pad + title_h * 0.12f, title_h * 0.75f, 0.10f, 0.08f, 0.02f, 1.0f, 0, title, panel_w * 0.62f - unit * 1.2f});
  const float top = y0 + pad + title_h + unit * 0.5f;
  const float cx = (x0 + x1) * 0.5f;

  if (g_page == Page::Remap) {
    const int n = step_count(), step = std::min(g_step, n - 1);
    char line[96];
    std::snprintf(line, sizeof line, "Step %d of %d", step + 1, n);
    out.texts.push_back({cx, top + unit * 0.2f, unit * 0.8f, 1, 1, 1, 0.6f, 1, line, panel_w - 2 * pad});
    out.texts.push_back({cx, top + unit * 1.7f, unit * 1.05f, 1, 1, 1, 0.9f, 1, is_keyboard(device()) ? "Press the key for" : "Press the button for", panel_w - 2 * pad});
    out.texts.push_back({cx, top + unit * 3.1f, unit * 2.3f, 0.97f, 0.79f, 0.28f, 1.0f, 1, step_name(step), panel_w - 2 * pad});
    out.texts.push_back({cx, top + unit * 6.2f, unit * 0.9f, 1, 1, 1, 0.7f, 1, "Now: " + ascii(step_binding(step)), panel_w - 2 * pad});
    const float bar_w = panel_w - 4 * pad, remaining = 1.0f - std::min(1.0f, g_step_frames / (float)kSkipFrames);
    out.shapes.push_back({cx - bar_w / 2, top + unit * 7.8f, cx + bar_w / 2, top + unit * 8.2f, 1, 1, 1, 0.12f, unit * 0.2f, 0.0f, 0.0f, 0, 0.0f, 0.0f});
    out.shapes.push_back({cx - bar_w / 2, top + unit * 7.8f, cx - bar_w / 2 + bar_w * remaining, top + unit * 8.2f, 0.97f, 0.79f, 0.28f, 0.9f, unit * 0.2f, 0.0f, 0.0f, 0, 0.0f, 0.0f});
    std::snprintf(line, sizeof line, "Keeps the current binding in %d s", (int)std::ceil((kSkipFrames - g_step_frames) / 60.0f));
    out.texts.push_back({cx, top + unit * 8.7f, unit * 0.8f, 1, 1, 1, 0.55f, 1, line, panel_w - 2 * pad});
    out.texts.push_back({cx, y1 - pad - unit * 1.1f, unit * 0.7f, 1, 1, 1, 0.55f, 1,
                         touch_controls_visible ? "Tap anywhere to stop." : is_keyboard(device()) ? "Escape stops the remap." : "Hold L+R+Start to close the menu.", panel_w - 2 * pad});
    g_layout = {x0, y0, x1, y1, row_h, top};
    return;
  }

  for (size_t i = 0; i < rows.size(); ++i) {
    const int r = rows[i];
    const float ry = top + row_h * i;
    const bool sel = (int)i == g_selected;
    if (sel) out.shapes.push_back({x0 + pad * 0.5f, ry, x1 - pad * 0.5f, ry + row_h, 0.97f, 0.79f, 0.28f, 0.18f, unit * 0.35f, 0.0f, 0.0f, 0, 0.0f, 0.0f});
    const float ty = ry + (row_h - unit) * 0.5f;
    const bool main = g_page == Page::Main;
    const std::string value = main ? main_value(r, s) : controls_value(r);
    out.texts.push_back({x0 + pad, ty, unit * 0.95f, 1, 1, 1, sel ? 1.0f : 0.85f, 0, main ? main_name(r) : controls_name(r), value.empty() ? panel_w - 2 * pad : panel_w * 0.56f - pad});
    const bool steppable = main ? (r != ROW_RESUME && r != ROW_CONTROLS) : value_row(r);
    if (!value.empty()) out.texts.push_back({x1 - pad, ty, unit * 0.95f, 0.97f, 0.79f, 0.28f, sel ? 1.0f : 0.8f, 2, sel && steppable ? "<  " + value + "  >" : value, panel_w * 0.42f - pad});
  }
  const char* hint = touch_controls_visible ? "Tap a row's left or right side to change it"
                   : g_page == Page::Main ? "Up/Down select   Left/Right change   B resume"
                                          : "Up/Down select   Left/Right change   B back";
  out.texts.push_back({cx, y1 - pad - unit * 1.1f, unit * 0.7f, 1, 1, 1, 0.55f, 1, hint, panel_w - 2 * pad});
  g_layout = {x0, y0, x1, y1, row_h, top};
}
}  // namespace host
