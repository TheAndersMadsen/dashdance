// Report-rate measurement for GameController-framework pads (Bluetooth, USB-C, MFi) on Apple
// platforms. iOS and macOS do not expose a controller's HID polling interval, but every report that
// changes a value fires the framework's change handler; while a stick moves, the spacing of those
// events is the report interval. On macOS the same controllers are also visible as HID devices, and
// a USB one delivers a report every polling interval whether or not anything changed, so its rate
// is counted directly from the report stream and known the moment it is plugged in (this is how the
// Input Integrity Lossless Adapter in PC/XInput mode shows 1000 Hz through Apple's own Xbox driver).
// The dashboard shows the result so players can see what their controller and connection actually deliver.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>
#include <vector>
namespace host {
struct ControllerReport {
  std::string name;      // the framework's vendor name (the HID product string; SDL may use a guessed generic name instead)
  bool wired = false;    // attached through the connector rather than Bluetooth
  double hz = 0.0;       // measured report rate, 0 until enough events have been seen
  unsigned samples = 0;
  bool counted = false;  // hz counted from the USB HID report stream (macOS), not inferred from value changes
};
void controller_rate_init();                       // start observing connections (call once, main thread)
std::vector<ControllerReport> controller_reports();
}  // namespace host
