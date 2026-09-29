// Input Integrity Lossless Adapter in its PC (XInput) mode, read natively on macOS.
//
// In that mode the adapter presents itself as four wired Xbox 360 controllers (VID 045E PID 028E,
// one vendor-class interface per GameCube port, every endpoint at a 1 ms interval). Apple's own
// XboxGamepad driver claims it and publishes one HID gamepad per interface at 1000 reports/s, but
// the GameController framework only ever surfaces the first interface as a controller, and the
// adapter's firmware maps GameCube port N to interface N-1; a controller in any other port is
// invisible to GameController and therefore to SDL. So the four HID devices are read here
// directly (a shared IOHIDManager open next to the system driver, no driver of our own) and
// interface N is GameCube port N+1, the same as the WUP-028 path.
//
// Report (20 bytes, from Microsoft's wired 360 protocol; the mapping is the adapter's default XInput
// profile as decompiled from LosslessAdapterManager2: A/B/X/Y by name, Z on RB, L/R on the analog
// triggers, Start and the d-pad as themselves):
//   [0] 0x00 message type, [1] 0x14 length,
//   [2] d-pad up 01 down 02 left 04 right 08, start 10, back 20, stick presses 40/80,
//   [3] LB 01, RB 02, guide 04 (the adapter's own button), A 10, B 20, X 40, Y 80,
//   [4] left trigger 0..255, [5] right trigger 0..255,
//   [6..13] left X, left Y, right X, right Y as int16 little-endian (up and right positive);
//   the firmware maps the Melee stick range (80 units) onto the full int16 range and emulates
//   Smash's origin logic itself, so idle is zero.
// Apple's driver refuses every output report for a 360-protocol device, so rumble is not possible
// in this mode; the adapter's Switch/Dolphin mode goes through gc_adapter_iokit.cpp and rumbles.
// There is no "controller plugged in" flag in XInput: a port counts as occupied once its interface
// has ever reported a non-idle state, and stays so for the session.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "host.h"
#include "window.h"

#include <TargetConditionals.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

#if TARGET_OS_OSX
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDManager.h>
#include <thread>
#endif

namespace host {
namespace {
enum : uint16_t {
  PAD_LEFT = 0x0001, PAD_RIGHT = 0x0002, PAD_DOWN = 0x0004, PAD_UP = 0x0008, PAD_Z = 0x0010, PAD_R = 0x0020, PAD_L = 0x0040,
  PAD_A = 0x0100, PAD_B = 0x0200, PAD_X = 0x0400, PAD_Y = 0x0800, PAD_START = 0x1000,
};

#if TARGET_OS_OSX
constexpr int kVendorMicrosoft = 0x045E, kProductXbox360 = 0x028E;

struct Port {
  IOHIDDeviceRef device = nullptr;
  uint8_t buffer[64];
  uint8_t report[20] = {};
  bool active = false;      // a non-idle report has been seen: a controller is in this port
  uint64_t reports = 0;
  double window_start = 0.0;
  double hz = 0.0;
};
std::mutex g_mutex;
Port g_ports[4];
int g_open = 0;                    // interfaces open right now
std::atomic<bool> g_started{false};
IOHIDManagerRef g_manager = nullptr;
bool g_logged_open = false;

std::string hid_string(IOHIDDeviceRef device, CFStringRef key) {
  CFTypeRef v = IOHIDDeviceGetProperty(device, key);
  if (!v || CFGetTypeID(v) != CFStringGetTypeID()) return "";
  char buf[256] = {};
  CFStringGetCString((CFStringRef)v, buf, sizeof buf, kCFStringEncodingUTF8);
  return buf;
}
int hid_int(IOHIDDeviceRef device, CFStringRef key) {
  CFTypeRef v = IOHIDDeviceGetProperty(device, key);
  int n = -1;
  if (v && CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)v, kCFNumberIntType, &n);
  return n;
}
// The USB interface number: walk up from the HID device to its IOUSBHostInterface.
int interface_number(IOHIDDeviceRef device) {
  io_service_t service = IOHIDDeviceGetService(device);
  if (!service) return -1;
  io_registry_entry_t entry = service;
  IOObjectRetain(entry);
  for (int depth = 0; depth < 8; ++depth) {
    CFTypeRef v = IORegistryEntryCreateCFProperty(entry, CFSTR("bInterfaceNumber"), kCFAllocatorDefault, 0);
    if (v) {
      int n = -1;
      if (CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)v, kCFNumberIntType, &n);
      CFRelease(v); IOObjectRelease(entry);
      return n;
    }
    io_registry_entry_t parent = 0;
    if (IORegistryEntryGetParentEntry(entry, kIOServicePlane, &parent) != KERN_SUCCESS) break;
    IOObjectRelease(entry); entry = parent;
  }
  IOObjectRelease(entry);
  return -1;
}

bool idle(const uint8_t* r) {
  if (r[2] || (r[3] & ~0x04) || r[4] || r[5]) return false;   // the guide bit is the adapter's own button, not a controller
  for (int i = 6; i < 14; i += 2) {
    const int16_t v = (int16_t)(r[i] | (r[i + 1] << 8));
    if (v > 4096 || v < -4096) return false;
  }
  return true;
}

void on_report(void* context, IOReturn result, void*, IOHIDReportType, uint32_t, uint8_t* report, CFIndex length) {
  if (result != kIOReturnSuccess || length < 14 || report[0] != 0x00) return;
  Port& p = *(Port*)context;
  const double now = now_seconds();
  std::lock_guard<std::mutex> lock(g_mutex);
  std::memcpy(p.report, report, length < 20 ? length : 20);
  if (!p.active && !idle(report)) {
    p.active = true;
    log("lossless adapter: controller detected on port %d", (int)(&p - g_ports) + 1);
  }
  ++p.reports;
  if (p.window_start <= 0.0) { p.window_start = now; p.reports = 0; return; }
  const double elapsed = now - p.window_start;
  if (elapsed >= 1.0) { p.hz = p.reports / elapsed; p.reports = 0; p.window_start = now; }
}

void on_matched(void*, IOReturn, void*, IOHIDDeviceRef device) {
  if (hid_int(device, CFSTR(kIOHIDVendorIDKey)) != kVendorMicrosoft || hid_int(device, CFSTR(kIOHIDProductIDKey)) != kProductXbox360) return;
  if (hid_string(device, CFSTR(kIOHIDProductKey)).find("Lossless") == std::string::npos) return;   // a real wired 360 pad stays with SDL
  const int n = interface_number(device);
  if (n < 0 || n > 3) return;
  if (IOHIDDeviceOpen(device, kIOHIDOptionsTypeNone) != kIOReturnSuccess) { log("lossless adapter: cannot open interface %d", n); return; }
  Port& p = g_ports[n];
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    p.device = device; p.active = false; p.reports = 0; p.window_start = 0.0; p.hz = 0.0;
    std::memset(p.report, 0, sizeof p.report);
    ++g_open;
  }
  IOHIDDeviceRegisterInputReportCallback(device, p.buffer, sizeof p.buffer, on_report, &p);
  if (!g_logged_open) { g_logged_open = true; log("lossless adapter: XInput mode, read natively through Apple's Xbox driver; interface N is GameCube port N+1, 1 ms polling, no rumble in this mode"); }
}
void on_removed(void*, IOReturn, void*, IOHIDDeviceRef device) {
  Port* found = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (Port& p : g_ports) if (p.device == device) { found = &p; p.device = nullptr; p.active = false; p.hz = 0.0; --g_open; }
    if (g_open == 0) g_logged_open = false;
  }
  if (!found) return;
  IOHIDDeviceRegisterInputReportCallback(device, found->buffer, sizeof found->buffer, nullptr, nullptr);
  IOHIDDeviceClose(device, kIOHIDOptionsTypeNone);
  log("lossless adapter: interface closed (adapter unplugged or mode changed)");
}

void reader_thread() {
  pthread_setname_np("lossless adapter");
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  g_manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
  if (!g_manager) return;
  const int vid = kVendorMicrosoft, pid = kProductXbox360;
  CFNumberRef v = CFNumberCreate(nullptr, kCFNumberIntType, &vid), p = CFNumberCreate(nullptr, kCFNumberIntType, &pid);
  const void* keys[] = {CFSTR(kIOHIDVendorIDKey), CFSTR(kIOHIDProductIDKey)};
  const void* values[] = {v, p};
  CFDictionaryRef match = CFDictionaryCreate(nullptr, keys, values, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CFRelease(v); CFRelease(p);
  IOHIDManagerSetDeviceMatching(g_manager, match);
  CFRelease(match);
  IOHIDManagerRegisterDeviceMatchingCallback(g_manager, on_matched, nullptr);
  IOHIDManagerRegisterDeviceRemovalCallback(g_manager, on_removed, nullptr);
  IOHIDManagerScheduleWithRunLoop(g_manager, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
  IOHIDManagerOpen(g_manager, kIOHIDOptionsTypeNone);
  CFRunLoopRun();
}

void start() {
  if (g_started.exchange(true)) return;
  std::thread(reader_thread).detach();
}

int8_t stick(const uint8_t* r, int offset) {
  const int16_t v = (int16_t)(r[offset] | (r[offset + 1] << 8));
  const int gc = (v * 80 + (v >= 0 ? 16383 : -16383)) / 32767;   // the firmware maps 80 GameCube units onto the full int16 range
  return (int8_t)(gc > 127 ? 127 : gc < -128 ? -128 : gc);
}
#endif
}  // namespace

// Fills the ports with a controller; returns their mask. 0 when the adapter is absent or in another mode.
uint32_t lossless_poll(PadState out[4]) {
#if TARGET_OS_OSX
  start();
  uint32_t mask = 0;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_open) return 0;
  for (int port = 0; port < 4; ++port) {
    const Port& p = g_ports[port];
    if (!p.device || !p.active) continue;
    const uint8_t* r = p.report;
    PadState& o = out[port];
    std::memset(&o, 0, sizeof o);
    o.err = 0;
    uint16_t b = 0;
    if (r[2] & 0x01) b |= PAD_UP; if (r[2] & 0x02) b |= PAD_DOWN; if (r[2] & 0x04) b |= PAD_LEFT; if (r[2] & 0x08) b |= PAD_RIGHT;
    if (r[2] & 0x10) b |= PAD_START;
    if (r[3] & 0x02) b |= PAD_Z;
    if (r[3] & 0x10) b |= PAD_A; if (r[3] & 0x20) b |= PAD_B; if (r[3] & 0x40) b |= PAD_X; if (r[3] & 0x80) b |= PAD_Y;
    o.trig_l = r[4]; o.trig_r = r[5];
    if (r[4] == 255) b |= PAD_L;   // the firmware reaches 255 just before the physical click; Melee's digital press
    if (r[5] == 255) b |= PAD_R;
    o.button = b;
    o.stick_x = stick(r, 6); o.stick_y = stick(r, 8);
    o.sub_x = stick(r, 10); o.sub_y = stick(r, 12);
    mask |= 1u << port;
  }
  return mask;
#else
  (void)out;
  return 0;
#endif
}

// Adapter present in XInput mode: ports with a controller, the 1 ms interval and the measured rate.
bool lossless_status(GcAdapterStatus& out) {
#if TARGET_OS_OSX
  start();
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_open) return false;
  out.ports = 0; out.interval_ms = 1; out.report_hz = 0.0;
  for (int port = 0; port < 4; ++port) {
    if (g_ports[port].active) out.ports |= 1u << port;
    if (g_ports[port].hz > out.report_hz) out.report_hz = g_ports[port].hz;
  }
  return true;
#else
  (void)out;
  return false;
#endif
}
}  // namespace host
