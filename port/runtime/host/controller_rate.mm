// See controller_rate.h.
// SPDX-License-Identifier: GPL-2.0-or-later
#import <GameController/GameController.h>
#include <TargetConditionals.h>
#include "controller_rate.h"
#include "host.h"
#include <algorithm>
#include <mutex>
#include <unordered_map>
#if TARGET_OS_OSX
#include <IOKit/hid/IOHIDManager.h>
#include <thread>
#endif

namespace host {
namespace {
struct Track {
  std::string name; bool wired = false;
  double last = 0.0;
  std::vector<double> intervals;   // seconds between consecutive change events, newest last (bounded)
  unsigned samples = 0;
};
std::mutex g_mutex;
std::unordered_map<const void*, Track> g_tracks;   // keyed by the GCController object
id g_connect = nil, g_disconnect = nil;

void record(GCController* controller) {
  const double now = now_seconds();
  std::lock_guard<std::mutex> lock(g_mutex);
  Track& t = g_tracks[(__bridge const void*)controller];
  if (t.last > 0.0) {
    const double dt = now - t.last;
    if (dt > 0.0002 && dt < 0.05) {   // a report interval, not an idle gap
      if (t.intervals.size() >= 256) t.intervals.erase(t.intervals.begin());
      t.intervals.push_back(dt); ++t.samples;
    }
  }
  t.last = now;
}

void attach(GCController* controller) {
  if (!controller) return;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    Track& t = g_tracks[(__bridge const void*)controller];
    t.name = controller.vendorName ? controller.vendorName.UTF8String : "Controller";
    t.wired = controller.attachedToDevice;
  }
  log("controller: %s connected (%s)", controller.vendorName.UTF8String ?: "controller", controller.attachedToDevice ? "wired" : "wireless");
  // The connect notification is posted from inside the framework's own event transaction for the new
  // device, and the framework mutates the device's input state on its own queues while SDL reads it on
  // the main thread. Installing a change handler inside that notification, or moving the handlers to a
  // private queue, trips the framework's "Recursive or concurrent mutation detected" assertion and kills
  // the process (Dashdance died that way whenever a controller was plugged in after launch). The
  // handlers therefore stay on the main queue and are installed on its next turn.
  __weak GCController* weak = controller;
  dispatch_async(dispatch_get_main_queue(), ^{
    GCController* c = weak;
    if (!c) return;
    if (c.extendedGamepad) c.extendedGamepad.valueChangedHandler = ^(GCExtendedGamepad*, GCControllerElement*) { if (GCController* live = weak) record(live); };
    else if (c.microGamepad) c.microGamepad.valueChangedHandler = ^(GCMicroGamepad*, GCControllerElement*) { if (GCController* live = weak) record(live); };
  });
}
void detach(GCController* controller) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_tracks.erase((__bridge const void*)controller);
}

#if TARGET_OS_OSX
// USB HID gamepads and joysticks, counted straight from their input reports. The framework's
// `attachedToDevice` is false for USB controllers on the Mac (it means the Lightning/USB-C
// connector of an iPhone or iPad), so a device seen here is also what marks a pad as wired.
struct HidDevice {
  std::string product;
  uint8_t buffer[256];
  uint64_t reports = 0;
  double window_start = 0.0;
  double hz = 0.0;
  bool logged = false;
};
std::mutex g_hid_mutex;
std::unordered_map<IOHIDDeviceRef, HidDevice*> g_hid_devices;
IOHIDManagerRef g_hid_manager = nullptr;
std::thread g_hid_thread;

std::string hid_string(IOHIDDeviceRef device, CFStringRef key) {
  CFTypeRef v = IOHIDDeviceGetProperty(device, key);
  if (!v || CFGetTypeID(v) != CFStringGetTypeID()) return "";
  char buf[256] = {};
  CFStringGetCString((CFStringRef)v, buf, sizeof buf, kCFStringEncodingUTF8);
  return buf;
}

void hid_report(void* context, IOReturn result, void*, IOHIDReportType, uint32_t, uint8_t*, CFIndex) {
  if (result != kIOReturnSuccess) return;
  HidDevice* d = (HidDevice*)context;
  const double now = now_seconds();
  ++d->reports;
  if (d->window_start <= 0.0) { d->window_start = now; d->reports = 0; return; }
  const double elapsed = now - d->window_start;
  if (elapsed < 1.0) return;
  const double hz = d->reports / elapsed;
  d->reports = 0; d->window_start = now;
  {
    std::lock_guard<std::mutex> lock(g_hid_mutex);
    d->hz = hz;
  }
  if (!d->logged) {   // once per product: a multi-port adapter is one line, not one per interface
    d->logged = true;
    static std::vector<std::string> logged_products;
    if (std::find(logged_products.begin(), logged_products.end(), d->product) == logged_products.end()) {
      logged_products.push_back(d->product);
      log("controller: %s over USB delivers %.0f reports/s (%.1f ms)", d->product.c_str(), hz, 1000.0 / hz);
    }
  }
}

void hid_matched(void*, IOReturn, void*, IOHIDDeviceRef device) {
  if (hid_string(device, CFSTR(kIOHIDTransportKey)) != "USB") return;   // Bluetooth pads only report on change; the framework path measures those
  const std::string product = hid_string(device, CFSTR(kIOHIDProductKey));
  if (product.empty()) return;
  if (product.find("Lossless") != std::string::npos) return;   // read and counted natively by lossless_xinput.cpp
  if (IOHIDDeviceOpen(device, kIOHIDOptionsTypeNone) != kIOReturnSuccess) return;   // shared open: the system driver and SDL keep the device
  HidDevice* d = new HidDevice; d->product = product;
  {
    std::lock_guard<std::mutex> lock(g_hid_mutex);
    g_hid_devices[device] = d;
  }
  IOHIDDeviceRegisterInputReportCallback(device, d->buffer, sizeof d->buffer, hid_report, d);
}
void hid_removed(void*, IOReturn, void*, IOHIDDeviceRef device) {
  HidDevice* d = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_hid_mutex);
    auto it = g_hid_devices.find(device);
    if (it == g_hid_devices.end()) return;
    d = it->second; g_hid_devices.erase(it);
  }
  IOHIDDeviceRegisterInputReportCallback(device, d->buffer, sizeof d->buffer, nullptr, nullptr);
  IOHIDDeviceClose(device, kIOHIDOptionsTypeNone);
  delete d;
}

CFDictionaryRef hid_usage_match(int page, int usage) {
  CFNumberRef p = CFNumberCreate(nullptr, kCFNumberIntType, &page), u = CFNumberCreate(nullptr, kCFNumberIntType, &usage);
  const void* keys[] = {CFSTR(kIOHIDDeviceUsagePageKey), CFSTR(kIOHIDDeviceUsageKey)};
  const void* values[] = {p, u};
  CFDictionaryRef dict = CFDictionaryCreate(nullptr, keys, values, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CFRelease(p); CFRelease(u);
  return dict;
}

void hid_thread() {
  pthread_setname_np("controller report counter");
  g_hid_manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
  if (!g_hid_manager) return;
  CFDictionaryRef gamepad = hid_usage_match(kHIDPage_GenericDesktop, kHIDUsage_GD_GamePad), joystick = hid_usage_match(kHIDPage_GenericDesktop, kHIDUsage_GD_Joystick);
  const void* matches[] = {gamepad, joystick};
  CFArrayRef array = CFArrayCreate(nullptr, matches, 2, &kCFTypeArrayCallBacks);
  IOHIDManagerSetDeviceMatchingMultiple(g_hid_manager, array);
  CFRelease(array); CFRelease(gamepad); CFRelease(joystick);
  IOHIDManagerRegisterDeviceMatchingCallback(g_hid_manager, hid_matched, nullptr);
  IOHIDManagerRegisterDeviceRemovalCallback(g_hid_manager, hid_removed, nullptr);
  IOHIDManagerScheduleWithRunLoop(g_hid_manager, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
  IOHIDManagerOpen(g_hid_manager, kIOHIDOptionsTypeNone);   // devices are opened one by one in hid_matched
  CFRunLoopRun();
}

// The counted rate for a controller with this product name (its best interface: a multi-port
// adapter exposes one HID device per port, and the empty ones stay quiet). 0 if none.
double hid_rate_for(const std::string& name, bool* present) {
  std::lock_guard<std::mutex> lock(g_hid_mutex);
  double hz = 0.0;
  for (auto& [device, d] : g_hid_devices)
    if (d->product == name) { *present = true; hz = std::max(hz, d->hz); }
  return hz;
}
#endif
}  // namespace

void controller_rate_init() {
  @autoreleasepool {
    for (GCController* c in GCController.controllers) attach(c);
    g_connect = [NSNotificationCenter.defaultCenter addObserverForName:GCControllerDidConnectNotification object:nil queue:nil usingBlock:^(NSNotification* n) { attach((GCController*)n.object); }];
    g_disconnect = [NSNotificationCenter.defaultCenter addObserverForName:GCControllerDidDisconnectNotification object:nil queue:nil usingBlock:^(NSNotification* n) { detach((GCController*)n.object); }];
  }
#if TARGET_OS_OSX
  static bool counting = false;
  if (!counting) { counting = true; g_hid_thread = std::thread(hid_thread); g_hid_thread.detach(); }
#endif
}

std::vector<ControllerReport> controller_reports() {
  std::vector<ControllerReport> out;
  std::lock_guard<std::mutex> lock(g_mutex);
  for (auto& [key, t] : g_tracks) {
    ControllerReport r; r.name = t.name; r.wired = t.wired; r.samples = t.samples;
    if (t.intervals.size() >= 16) {
      // Reports arrive at a fixed interval while a stick moves; the 10th percentile ignores the idle gaps in between.
      std::vector<double> v = t.intervals; std::sort(v.begin(), v.end());
      r.hz = 1.0 / v[v.size() / 10];
    }
#if TARGET_OS_OSX
    bool usb = false;
    const double counted = hid_rate_for(t.name, &usb);
    if (usb) r.wired = true;
    if (counted > 0.0) { r.hz = counted; r.counted = true; }
#endif
    out.push_back(r);
  }
  return out;
}
}  // namespace host
