// Controller configuration shared by the dashboards, the in-game menu and the game window: per-controller
// button mappings, port, stick deadzones, trigger press point and rumble (keyed by the controller's stable
// GUID), and the keyboard layout. Persisted in launcher.ini.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace host {
// GameCube controls a physical button can be bound to (index into ControllerMap::binding).
enum GcControl : int { GC_CTL_A, GC_CTL_B, GC_CTL_X, GC_CTL_Y, GC_CTL_Z, GC_CTL_L, GC_CTL_R, GC_CTL_START,
                       GC_CTL_DUP, GC_CTL_DDOWN, GC_CTL_DLEFT, GC_CTL_DRIGHT, GC_CTL_COUNT };
extern const char* const kGcControlNames[GC_CTL_COUNT];
// A physical input: SDL gamepad button ids are 0..N, triggers use kTriggerLeft/kTriggerRight, kUnbound = none.
constexpr int kTriggerLeft = 100, kTriggerRight = 101, kUnbound = -1;
struct ControllerMap {
  int binding[GC_CTL_COUNT];
  bool swap_sticks = false;      // C-stick on the left stick
  int stick_deadzone = 24;       // percent of full deflection ignored around the centre
  int cstick_deadzone = 27;
  int trigger_press = 78;        // percent of analog trigger travel that also registers a digital L/R press
  bool rumble = true;
  static ControllerMap defaults();
  std::string serialize() const;                // "0,2,1,...,dz=24,cdz=27,tp=78" for launcher.ini
  static ControllerMap parse(const std::string& text);
};
struct ControllerConfig {
  std::string guid;              // SDL GUID string
  int port = 0;                  // 1..4 fixed GameCube port, 0 = first free port in connection order
  ControllerMap map = ControllerMap::defaults();
};
// The configuration table (owned by the host; the window layer reads it on every poll).
const std::vector<ControllerConfig>& controller_configs();
void set_controller_configs(std::vector<ControllerConfig> configs);
const ControllerConfig* controller_config_for(const std::string& guid);
void upsert_controller_config(const ControllerConfig& config);
std::string physical_input_name(int input);   // "A / Cross", "Left trigger", ...

// Keyboard: the twelve GameCube controls, eight stick directions and a modifier that shortens the stick
// (walking, tilts, light shield), as SDL scancodes.
enum KeyboardControl : int { KB_STICK_UP = GC_CTL_COUNT, KB_STICK_DOWN, KB_STICK_LEFT, KB_STICK_RIGHT,
                             KB_CSTICK_UP, KB_CSTICK_DOWN, KB_CSTICK_LEFT, KB_CSTICK_RIGHT, KB_MODIFIER, KB_COUNT };
extern const char* const kKeyboardControlNames[KB_COUNT];
struct KeyboardMap {
  int key[KB_COUNT];             // SDL scancodes, 0 = unbound
  int modifier_percent = 50;     // stick deflection while the modifier is held
  static KeyboardMap defaults();
  std::string serialize() const;
  static KeyboardMap parse(const std::string& text);
};
const KeyboardMap& keyboard_map();
void set_keyboard_map(const KeyboardMap& map);
std::string key_name(int scancode);              // "Z", "Left Shift", "Up"
int scancode_from_mac_keycode(int keycode);      // NSEvent.keyCode -> SDL scancode (macOS dashboard), 0 if unknown

// Live controllers, for the dashboards and the in-game menu (requires the gamepad subsystem: window_input_init()).
struct ControllerInfo {
  std::string name, guid; uint32_t instance_id = 0; int assigned_port = 0; bool is_gamecube_adapter = false;
  double report_hz = 0.0;      // measured report rate (0 = not measured yet); for the adapter, the polled rate
  bool wired = false;          // connector rather than Bluetooth
  uint32_t adapter_ports = 0;  // GameCube adapter: bit per port with a controller plugged in
  int adapter_interval_ms = 0; // GameCube adapter: the polling interval the USB host accepted
  bool rate_counted = false;   // report_hz counted from the USB report stream (known without moving a stick)
  bool gamecube_controller = false;   // a GameCube controller behind a PC-mode adapter (Input Integrity Lossless Adapter in XInput mode)
};
void window_input_init();                              // SDL gamepad subsystem without a window
std::vector<ControllerInfo> window_list_controllers();
// Returns the physical input held right now on the given controller (kUnbound if none); callers wait for
// kUnbound first so a held button is not captured twice.
int window_capture_input(const std::string& guid);
// Raw state for the controller editors' live view: sticks -1..1 (y up), triggers 0..1, SDL buttons.
struct ControllerLiveState { float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0; bool button[32] = {}; };
bool window_controller_state(const std::string& guid, ControllerLiveState& out);
void window_test_rumble(const std::string& guid);
// The scancode of the most recent key pressed in the game window since the previous call, -1 if none.
int window_take_key_press();
}  // namespace host
