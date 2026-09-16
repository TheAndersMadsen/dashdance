// Official / Mayflash "GameCube Controller Adapter for Wii U" (WUP-028) over IOKit on macOS.
// Same protocol as Dolphin's GCAdapter: one 0x13 byte starts the 37-byte report stream on the
// interrupt IN endpoint (status + 9 bytes per port); 0x11 + 4 bytes sets rumble.
//
// The adapter's endpoint descriptor asks for an 8 ms polling interval (125 Hz), which adds up to
// 8 ms of input latency. Linux players fix that with a kernel module (gcadapter-oc-kmod) and Mac
// players used to need a kernel extension (GCAdapterDriver) plus a Recovery-mode change. Here the
// interface is opened directly through IOUSBLib and the pipe policy asks the host controller for a
// 1 ms interval (1000 Hz) in-process: no driver, no SIP change. The achieved rate is measured from
// the report stream and shown in the dashboard, because some ports and hubs refuse the request.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "host.h"
#include "window.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/usb/USBSpec.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <pthread/qos.h>
#include <thread>

namespace host {
namespace {

enum : uint16_t {
  PAD_LEFT = 0x0001, PAD_RIGHT = 0x0002, PAD_DOWN = 0x0004, PAD_UP = 0x0008, PAD_Z = 0x0010, PAD_R = 0x0020, PAD_L = 0x0040,
  PAD_A = 0x0100, PAD_B = 0x0200, PAD_X = 0x0400, PAD_Y = 0x0800, PAD_START = 0x1000,
};
constexpr uint16_t ADAPTER_VID = 0x057E, ADAPTER_PID = 0x0337;

IOUSBDeviceInterface942** g_device = nullptr;
IOUSBInterfaceInterface942** g_interface = nullptr;
bool g_device_open = false, g_interface_open = false;
uint8_t g_pipe_in = 0, g_pipe_out = 0;
std::thread g_thread;
std::atomic<bool> g_running{false};
std::mutex g_mutex;
uint8_t g_report[37] = {};
bool g_have_report = false;
std::chrono::steady_clock::time_point g_next_scan;
bool g_logged_missing = false;
struct Origin { bool set = false; uint8_t sx = 128, sy = 128, cx = 128, cy = 128, tl = 0, tr = 0; } g_origin[4];
std::atomic<uint8_t> g_rumble[4]{};
std::atomic<bool> g_rumble_dirty{false};
std::atomic<int> g_interval_ms{0};        // polling interval the host accepted (from the pipe properties)
std::atomic<double> g_report_hz{0.0};     // measured reports per second
std::atomic<uint32_t> g_ports{0};         // ports with a controller plugged in (bit per port)

int requested_interval_ms() {
  const char* e = std::getenv("MELEE_ADAPTER_INTERVAL_MS");   // 1 (default, 1000 Hz), 2, 4, 8 (the adapter's own 125 Hz)
  int v = e ? std::atoi(e) : 1;
  return v < 1 ? 1 : v > 8 ? 8 : v;
}

void reader_thread() {
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);   // 1 ms polls must never wait behind background work
  uint8_t start = 0x13;
  if ((*g_interface)->WritePipe(g_interface, g_pipe_out, &start, 1) != kIOReturnSuccess) log("gc adapter: start command failed");
  int failures = 0;
  uint64_t reports = 0;
  auto window_start = std::chrono::steady_clock::now();
  bool rate_logged = false;
  bool timeouts_supported = true;   // some hosts reject ReadPipeTO on interrupt pipes (kIOReturnBadArgument); fall back to blocking reads
  while (g_running.load()) {
    uint8_t buf[37];
    UInt32 size = sizeof buf;
    IOReturn r = timeouts_supported ? (*g_interface)->ReadPipeTO(g_interface, g_pipe_in, buf, &size, 100, 200) : (*g_interface)->ReadPipe(g_interface, g_pipe_in, buf, &size);
    if (r == kIOReturnBadArgument && timeouts_supported) {
      timeouts_supported = false;
      log("gc adapter: timed reads rejected, using blocking reads");
      size = sizeof buf;
      r = (*g_interface)->ReadPipe(g_interface, g_pipe_in, buf, &size);
    }
    if (r == kIOReturnSuccess) {
      failures = 0;
      if (size == 37 && buf[0] == 0x21) {
        std::lock_guard<std::mutex> lk(g_mutex);
        std::memcpy(g_report, buf, 37); g_have_report = true;
        uint32_t ports = 0;
        for (int p = 0; p < 4; ++p) if (buf[1 + p * 9] & 0x30) ports |= 1u << p;
        g_ports.store(ports);
      }
      ++reports;
      const auto now = std::chrono::steady_clock::now();
      const double elapsed = std::chrono::duration<double>(now - window_start).count();
      if (elapsed >= 1.0) {
        g_report_hz.store(reports / elapsed);
        if (!rate_logged) { log("gc adapter: %.0f reports/s measured (host interval %d ms; asked for %d ms)", reports / elapsed, g_interval_ms.load(), requested_interval_ms()); rate_logged = true; }
        reports = 0; window_start = now;
      }
    } else if (r == kIOUSBTransactionTimeout) {
      // nothing this interval
    } else if (r == kIOUSBPipeStalled) {
      (*g_interface)->ClearPipeStallBothEnds(g_interface, g_pipe_in);
    } else {
      if (++failures > 20 || r == kIOReturnNotResponding || r == kIOReturnNoDevice || r == kIOReturnNotAttached) { log("gc adapter: read failed (%08x), adapter disconnected", r); break; }
    }
    if (g_rumble_dirty.exchange(false)) {
      uint8_t cmd[5] = {0x11, g_rumble[0], g_rumble[1], g_rumble[2], g_rumble[3]};
      (*g_interface)->WritePipe(g_interface, g_pipe_out, cmd, sizeof cmd);
    }
  }
  g_running.store(false);
}

void close_adapter() {
  g_running.store(false);
  if (g_interface && g_interface_open && g_pipe_in) (*g_interface)->AbortPipe(g_interface, g_pipe_in);   // wakes a blocking read
  if (g_thread.joinable()) g_thread.join();
  if (g_interface) {
    if (g_interface_open) { (*g_interface)->USBInterfaceClose(g_interface); g_interface_open = false; }
    (*g_interface)->Release(g_interface); g_interface = nullptr;
  }
  if (g_device) {
    if (g_device_open) { (*g_device)->USBDeviceClose(g_device); g_device_open = false; }
    (*g_device)->Release(g_device); g_device = nullptr;
  }
  std::lock_guard<std::mutex> lk(g_mutex);
  g_have_report = false; g_ports.store(0); g_report_hz.store(0.0); g_interval_ms.store(0);
  for (auto& o : g_origin) o.set = false;
}

template <class T> T** query(IOCFPlugInInterface** plug, CFUUIDRef id) {
  T** out = nullptr;
  (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(id), (LPVOID*)&out);
  return out;
}

bool open_adapter() {
  CFMutableDictionaryRef matching = IOServiceMatching(kIOUSBHostDeviceClassName);
  if (!matching) return false;
  const int vid = ADAPTER_VID, pid = ADAPTER_PID;
  CFNumberRef vidn = CFNumberCreate(nullptr, kCFNumberIntType, &vid), pidn = CFNumberCreate(nullptr, kCFNumberIntType, &pid);
  CFDictionarySetValue(matching, CFSTR(kUSBVendorID), vidn); CFDictionarySetValue(matching, CFSTR(kUSBProductID), pidn);
  CFRelease(vidn); CFRelease(pidn);
  io_iterator_t iterator = 0;
  if (IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator) != KERN_SUCCESS) return false;   // consumes `matching`
  io_service_t service = IOIteratorNext(iterator);
  IOObjectRelease(iterator);
  if (!service) {
    if (!g_logged_missing) { log("gc adapter: no WUP-028 adapter found (VID 057E PID 0337); keyboard/gamepads stay active"); g_logged_missing = true; }
    return false;
  }
  IOCFPlugInInterface** plug = nullptr; SInt32 score = 0;
  const kern_return_t kr = IOCreatePlugInInterfaceForService(service, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
  IOObjectRelease(service);
  if (kr != KERN_SUCCESS || !plug) { if (!g_logged_missing) { log("gc adapter: found but no device plug-in (%08x)", kr); g_logged_missing = true; } return false; }
  g_device = query<IOUSBDeviceInterface942>(plug, kIOUSBDeviceInterfaceID942);
  (*plug)->Release(plug);
  if (!g_device) return false;
  g_device_open = (*g_device)->USBDeviceOpenSeize(g_device) == kIOReturnSuccess;   // optional: the interface seize is what matters
  IOUSBFindInterfaceRequest req{kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare};
  io_iterator_t interfaces = 0;
  if ((*g_device)->CreateInterfaceIterator(g_device, &req, &interfaces) != kIOReturnSuccess) { close_adapter(); return false; }
  io_service_t interface = IOIteratorNext(interfaces);
  IOObjectRelease(interfaces);
  if (!interface) { close_adapter(); return false; }
  IOCFPlugInInterface** iplug = nullptr;
  const kern_return_t ikr = IOCreatePlugInInterfaceForService(interface, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &iplug, &score);
  IOObjectRelease(interface);
  if (ikr != KERN_SUCCESS || !iplug) { close_adapter(); return false; }
  g_interface = query<IOUSBInterfaceInterface942>(iplug, kIOUSBInterfaceInterfaceID942);
  (*iplug)->Release(iplug);
  if (!g_interface) { close_adapter(); return false; }
  const IOReturn opened = (*g_interface)->USBInterfaceOpenSeize(g_interface);
  if (opened != kIOReturnSuccess) {
    if (!g_logged_missing) { log("gc adapter: found but cannot open its interface (%08x): another program (Slippi Dolphin?) may hold it", opened); g_logged_missing = true; }
    close_adapter(); return false;
  }
  g_interface_open = true;
  UInt8 endpoints = 0;
  (*g_interface)->GetNumEndpoints(g_interface, &endpoints);
  UInt16 in_packet = 37;
  for (UInt8 pipe = 1; pipe <= endpoints; ++pipe) {
    UInt8 direction = 0, number = 0, type = 0, interval = 0; UInt16 packet = 0;
    if ((*g_interface)->GetPipeProperties(g_interface, pipe, &direction, &number, &type, &packet, &interval) != kIOReturnSuccess) continue;
    if (type != kUSBInterrupt) continue;
    if (direction == kUSBIn) { g_pipe_in = pipe; in_packet = packet; g_interval_ms.store(interval); }
    else if (direction == kUSBOut) g_pipe_out = pipe;
  }
  if (!g_pipe_in || !g_pipe_out) { log("gc adapter: interrupt endpoints not found"); close_adapter(); return false; }
  // Ask for a shorter polling interval than the descriptor's 8 ms. Fall back through 2 and 4 ms if the
  // host controller refuses 1 ms; the descriptor value stays if nothing is accepted.
  const int asked = requested_interval_ms();
  for (int ms = asked; ms < 8; ms *= 2) {
    if ((*g_interface)->SetPipePolicy(g_interface, g_pipe_in, in_packet, (UInt8)ms) == kIOReturnSuccess) break;
  }
  UInt8 direction = 0, number = 0, type = 0, interval = 0; UInt16 packet = 0;
  if ((*g_interface)->GetPipeProperties(g_interface, g_pipe_in, &direction, &number, &type, &packet, &interval) == kIOReturnSuccess) g_interval_ms.store(interval);
  log("gc adapter: opened WUP-028 over IOKit; polling interval %d ms (%s)", g_interval_ms.load(), g_interval_ms.load() <= asked ? "1000 Hz class" : "the host kept the adapter's default; try another USB port or hub");
  g_logged_missing = false;
  g_running.store(true);
  g_thread = std::thread(reader_thread);
  return true;
}

}  // namespace

// Fills ports that have a controller plugged into the adapter; returns the mask of those ports.
uint32_t gcadapter_poll(PadState out[4]) {
  auto now = std::chrono::steady_clock::now();
  if (!g_interface || !g_running.load()) {
    if (g_interface && !g_running.load()) close_adapter();
    if (now < g_next_scan) return 0;
    g_next_scan = now + std::chrono::seconds(2);
    if (!open_adapter()) return 0;
  }
  uint8_t rep[37];
  {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_have_report) return 0;
    std::memcpy(rep, g_report, 37);
  }
  uint32_t mask = 0;
  for (int port = 0; port < 4; ++port) {
    const uint8_t* c = rep + 1 + port * 9;
    uint8_t status = c[0] & 0x30;
    if (!status) { g_origin[port].set = false; continue; }
    Origin& o = g_origin[port];
    if (!o.set) { o.set = true; o.sx = c[3]; o.sy = c[4]; o.cx = c[5]; o.cy = c[6]; o.tl = c[7]; o.tr = c[8]; }
    PadState& p = out[port];
    std::memset(&p, 0, sizeof p);
    p.err = 0;
    uint16_t b = 0;
    if (c[1] & 0x01) b |= PAD_A; if (c[1] & 0x02) b |= PAD_B; if (c[1] & 0x04) b |= PAD_X; if (c[1] & 0x08) b |= PAD_Y;
    if (c[1] & 0x10) b |= PAD_LEFT; if (c[1] & 0x20) b |= PAD_RIGHT; if (c[1] & 0x40) b |= PAD_DOWN; if (c[1] & 0x80) b |= PAD_UP;
    if (c[2] & 0x01) b |= PAD_START; if (c[2] & 0x02) b |= PAD_Z; if (c[2] & 0x04) b |= PAD_R; if (c[2] & 0x08) b |= PAD_L;
    p.button = b;
    auto axis = [](uint8_t v, uint8_t origin) { int a = (int)v - (int)origin; return (int8_t)(a > 127 ? 127 : a < -128 ? -128 : a); };
    p.stick_x = axis(c[3], o.sx); p.stick_y = axis(c[4], o.sy);
    p.sub_x = axis(c[5], o.cx); p.sub_y = axis(c[6], o.cy);
    p.trig_l = (uint8_t)(c[7] > o.tl ? c[7] - o.tl : 0);
    p.trig_r = (uint8_t)(c[8] > o.tr ? c[8] - o.tr : 0);
    mask |= 1u << port;
  }
  return mask;
}

void gcadapter_rumble(int port, bool on) {
  if (port < 0 || port > 3) return;
  window_gamepad_rumble(port, on);   // ports fed by SDL gamepads (never adapter ports) rumble through SDL
  uint8_t v = on ? 1 : 0;
  if (g_rumble[port] != v) { g_rumble[port] = v; g_rumble_dirty = true; }
}

bool gcadapter_status(GcAdapterStatus& out) {
  if (!g_interface || !g_running.load()) return false;
  out.ports = g_ports.load(); out.interval_ms = g_interval_ms.load(); out.report_hz = g_report_hz.load();
  return true;
}

void gcadapter_shutdown() { close_adapter(); }

}  // namespace host
