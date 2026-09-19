// Official / Mayflash "GameCube Controller Adapter for Wii U" over libusb. Same protocol as
// Dolphin's GCAdapter and the Windows WinUSB path: one 0x13 byte starts the 37-byte report
// stream on endpoint 0x81 (status + 9 bytes per port), 0x11 + 4 bytes sets rumble.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "host.h"
#include "window.h"

#include <libusb.h>

#include <atomic>
#include <chrono>
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
constexpr uint8_t ENDPOINT_IN = 0x81, ENDPOINT_OUT = 0x02;

libusb_context* g_context = nullptr;
libusb_device_handle* g_handle = nullptr;
bool g_claimed = false;
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

void reader_thread() {
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);   // 1 ms adapter polls must never wait behind background work
  uint8_t start = 0x13;
  int n = 0;
  if (libusb_interrupt_transfer(g_handle, ENDPOINT_OUT, &start, 1, &n, 100) != 0) log("gc adapter: start command failed");
  int failures = 0;
  while (g_running.load()) {
    uint8_t buf[37];
    int got = 0;
    const int result = libusb_interrupt_transfer(g_handle, ENDPOINT_IN, buf, sizeof buf, &got, 100);
    if (result == 0) {
      failures = 0;
      if (got == 37 && buf[0] == 0x21) { std::lock_guard<std::mutex> lk(g_mutex); std::memcpy(g_report, buf, 37); g_have_report = true; }
    } else if (result == LIBUSB_ERROR_TIMEOUT) {
      // nothing this interval
    } else {
      if (++failures > 20 || result == LIBUSB_ERROR_NO_DEVICE) { log("gc adapter: read failed (%s), adapter disconnected", libusb_error_name(result)); break; }
    }
    if (g_rumble_dirty.exchange(false)) {
      uint8_t cmd[5] = {0x11, g_rumble[0], g_rumble[1], g_rumble[2], g_rumble[3]};
      libusb_interrupt_transfer(g_handle, ENDPOINT_OUT, cmd, sizeof cmd, &n, 100);
    }
  }
  g_running.store(false);
}

void close_adapter() {
  g_running.store(false);
  if (g_thread.joinable()) g_thread.join();
  if (g_handle) {
    if (g_claimed) { libusb_release_interface(g_handle, 0); g_claimed = false; }
    libusb_close(g_handle); g_handle = nullptr;
  }
  std::lock_guard<std::mutex> lk(g_mutex);
  g_have_report = false;
  for (auto& o : g_origin) o.set = false;
}

bool open_adapter() {
  if (!g_context && libusb_init(&g_context) != 0) { if (!g_logged_missing) { log("gc adapter: libusb init failed"); g_logged_missing = true; } return false; }
  libusb_device** list = nullptr;
  const ssize_t count = libusb_get_device_list(g_context, &list);
  libusb_device* found = nullptr;
  for (ssize_t i = 0; i < count; ++i) {
    libusb_device_descriptor desc{};
    if (libusb_get_device_descriptor(list[i], &desc) == 0 && desc.idVendor == ADAPTER_VID && desc.idProduct == ADAPTER_PID) { found = list[i]; break; }
  }
  if (!found) {
    libusb_free_device_list(list, 1);
    if (!g_logged_missing) { log("gc adapter: no WUP-028 adapter found (VID 057E PID 0337); keyboard/gamepads stay active"); g_logged_missing = true; }
    return false;
  }
  const int opened = libusb_open(found, &g_handle);
  libusb_free_device_list(list, 1);
  if (opened != 0) {
    if (!g_logged_missing) { log("gc adapter: found but cannot open (%s): another program (Slippi Dolphin?) may hold it", libusb_error_name(opened)); g_logged_missing = true; }
    g_handle = nullptr;
    return false;
  }
  libusb_set_auto_detach_kernel_driver(g_handle, 1);
  const int claimed = libusb_claim_interface(g_handle, 0);
  if (claimed != 0) {
    if (!g_logged_missing) { log("gc adapter: cannot claim interface 0 (%s)", libusb_error_name(claimed)); g_logged_missing = true; }
    libusb_close(g_handle); g_handle = nullptr;
    return false;
  }
  g_claimed = true;
  log("gc adapter: opened WUP-028 over libusb");
  g_logged_missing = false;
  g_running.store(true);
  g_thread = std::thread(reader_thread);
  return true;
}

}  // namespace

// Fills ports that have a controller plugged into the adapter; returns the mask of those ports.
uint32_t gcadapter_poll(PadState out[4]) {
  auto now = std::chrono::steady_clock::now();
  if (!g_handle || !g_running.load()) {
    if (g_handle && !g_running.load()) close_adapter();
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

bool gcadapter_status(GcAdapterStatus&) { return false; }
// PADRecalibrate: drop the captured origins so the next report re-captures them.
void gcadapter_recalibrate(int port) {
  std::lock_guard<std::mutex> lk(g_mutex);
  if (port < 0 || port > 3) { for (auto& o : g_origin) o.set = false; }
  else g_origin[port].set = false;
}
void gcadapter_shutdown() {
  close_adapter();
  if (g_context) { libusb_exit(g_context); g_context = nullptr; }
}

}  // namespace host
