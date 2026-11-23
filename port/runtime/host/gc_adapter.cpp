// Official / Mayflash "GameCube Controller Adapter for Wii U" over WinUSB (the driver Slippi's
// setup installs with Zadig). Same protocol as Dolphin's GCAdapter: one 0x13 byte starts the
// 37-byte report stream on endpoint 0x81 (status + 9 bytes per port), 0x11 + 4 bytes sets rumble.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "host.h"
#include "gc_adapter_reports.h"
#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include <usbiodef.h>
#include <winusb.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace host {
namespace {

enum : uint16_t {
  PAD_LEFT = 0x0001, PAD_RIGHT = 0x0002, PAD_DOWN = 0x0004, PAD_UP = 0x0008, PAD_Z = 0x0010, PAD_R = 0x0020, PAD_L = 0x0040,
  PAD_A = 0x0100, PAD_B = 0x0200, PAD_X = 0x0400, PAD_Y = 0x0800, PAD_START = 0x1000,
};

HANDLE g_file = INVALID_HANDLE_VALUE;
WINUSB_INTERFACE_HANDLE g_usb = nullptr;
std::thread g_thread;
std::atomic<bool> g_running{false};
std::mutex g_mutex;
uint8_t g_report[37] = {};
bool g_have_report = false;
std::chrono::steady_clock::time_point g_next_scan;
bool g_logged_missing = false;
struct Origin { bool set = false; uint8_t sx = 128, sy = 128, cx = 128, cy = 128, tl = 0, tr = 0; } g_origin[4];
gc_adapter_reports::State g_reports;   // input suggestions: game poll counts and the report callback
std::atomic<uint8_t> g_rumble[4]{};
std::atomic<bool> g_rumble_dirty{false};

std::string find_adapter_path() {
  HDEVINFO devs = SetupDiGetClassDevsA(&GUID_DEVINTERFACE_USB_DEVICE, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (devs == INVALID_HANDLE_VALUE) return "";
  std::string found;
  SP_DEVICE_INTERFACE_DATA iface{}; iface.cbSize = sizeof iface;
  for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devs, nullptr, &GUID_DEVINTERFACE_USB_DEVICE, i, &iface); ++i) {
    DWORD needed = 0;
    SetupDiGetDeviceInterfaceDetailA(devs, &iface, nullptr, 0, &needed, nullptr);
    if (!needed) continue;
    std::string buf(needed, '\0');
    auto* detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_A*)buf.data();
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
    if (!SetupDiGetDeviceInterfaceDetailA(devs, &iface, detail, needed, nullptr, nullptr)) continue;
    std::string path(detail->DevicePath);
    std::string lower(path);
    for (auto& c : lower) c = (char)tolower((unsigned char)c);
    if (lower.find("vid_057e&pid_0337") != std::string::npos) { found = path; break; }
  }
  SetupDiDestroyDeviceInfoList(devs);
  return found;
}

void reader_thread() {
  ULONG timeout = 100;
  WinUsb_SetPipePolicy(g_usb, 0x81, PIPE_TRANSFER_TIMEOUT, sizeof timeout, &timeout);
  uint8_t start = 0x13;
  ULONG n = 0;
  if (!WinUsb_WritePipe(g_usb, 0x02, &start, 1, &n, nullptr)) log("gc adapter: start command failed (%lu)", GetLastError());
  int failures = 0;
  while (g_running.load()) {
    uint8_t buf[37];
    ULONG got = 0;
    if (WinUsb_ReadPipe(g_usb, 0x81, buf, sizeof buf, &got, nullptr)) {
      failures = 0;
      if (got == 37 && buf[0] == 0x21) {
        uint64_t counts[4]; uint8_t payload[37];
        bool report = false;
        {
          std::lock_guard<std::mutex> lk(g_mutex);
          std::memcpy(g_report, buf, 37); g_have_report = true;
          report = gc_adapter_reports::capture(g_reports, g_report, counts, payload);
        }
        if (report) gc_adapter_reports::deliver(g_reports, counts, payload);   // outside the lock: the game's reads never wait on it
      }
    } else {
      DWORD err = GetLastError();
      if (err == ERROR_SEM_TIMEOUT || err == WAIT_TIMEOUT) continue;
      if (++failures > 20) { log("gc adapter: read failed (%lu), adapter disconnected", err); break; }
    }
    if (g_rumble_dirty.exchange(false)) {
      uint8_t cmd[5] = {0x11, g_rumble[0], g_rumble[1], g_rumble[2], g_rumble[3]};
      WinUsb_WritePipe(g_usb, 0x02, cmd, sizeof cmd, &n, nullptr);
    }
  }
  g_running.store(false);
}

void close_adapter() {
  g_running.store(false);
  if (g_thread.joinable()) g_thread.join();
  if (g_usb) { WinUsb_Free(g_usb); g_usb = nullptr; }
  if (g_file != INVALID_HANDLE_VALUE) { CloseHandle(g_file); g_file = INVALID_HANDLE_VALUE; }
  std::lock_guard<std::mutex> lk(g_mutex);
  g_have_report = false;
  for (auto& o : g_origin) o.set = false;
  g_reports.game_ports.store(0);
}

bool open_adapter() {
  std::string path = find_adapter_path();
  if (path.empty()) {
    if (!g_logged_missing) { log("gc adapter: no WUP-028 adapter found (VID 057E PID 0337 with the WinUSB driver); keyboard/XInput stay active"); g_logged_missing = true; }
    return false;
  }
  g_file = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
  if (g_file == INVALID_HANDLE_VALUE) {
    if (!g_logged_missing) { log("gc adapter: found but cannot open (%lu): another program (Dolphin?) may hold it, or the driver is not WinUSB", GetLastError()); g_logged_missing = true; }
    return false;
  }
  if (!WinUsb_Initialize(g_file, &g_usb)) {
    if (!g_logged_missing) { log("gc adapter: WinUsb_Initialize failed (%lu): install the WinUSB driver with Zadig as for Slippi", GetLastError()); g_logged_missing = true; }
    CloseHandle(g_file); g_file = INVALID_HANDLE_VALUE;
    return false;
  }
  log("gc adapter: opened %s", path.c_str());
  g_logged_missing = false;
  g_running.store(true);
  g_thread = std::thread(reader_thread);
  return true;
}

}  // namespace

// Fills ports that have a controller plugged into the adapter; returns the mask of those ports.
uint32_t gcadapter_poll(PadState out[4]) {
  auto now = std::chrono::steady_clock::now();
  if (!g_usb || !g_running.load()) {
    if (g_usb && !g_running.load()) close_adapter();
    if (now < g_next_scan) return 0;
    g_next_scan = now + std::chrono::seconds(2);
    if (!open_adapter()) return 0;
  }
  uint8_t rep[37];
  Origin origins[4];
  {
    // The report, the game's poll count and the origins change together, under the lock the reader stores reports
    // under: a report stamped with poll count N was stored after poll N read its own.
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_have_report) return 0;
    std::memcpy(rep, g_report, 37);
    gc_adapter_reports::count_game_poll(g_reports);
    for (int port = 0; port < 4; ++port) {
      const uint8_t* c = rep + 1 + port * 9;
      Origin& o = g_origin[port];
      if (!(c[0] & 0x30)) o.set = false;
      else if (!o.set) { o.set = true; o.sx = c[3]; o.sy = c[4]; o.cx = c[5]; o.cy = c[6]; o.tl = c[7]; o.tr = c[8]; }
      origins[port] = o;
    }
  }
  uint32_t mask = 0;
  for (int port = 0; port < 4; ++port) {
    const uint8_t* c = rep + 1 + port * 9;
    uint8_t status = c[0] & 0x30;
    if (!status) continue;
    const Origin& o = origins[port];
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
  if (g_game_pad_poll) g_reports.game_ports.store(mask);
  return mask;
}

void gcadapter_rumble(int port, bool on) {
  if (port < 0 || port > 3) return;
  uint8_t v = on ? 1 : 0;
  if (g_rumble[port] != v) { g_rumble[port] = v; g_rumble_dirty = true; }
}

bool gcadapter_status(GcAdapterStatus&) { return false; }
void gcadapter_shutdown() { close_adapter(); }

void gcadapter_set_report_callback(GcAdapterReportCallback callback) { g_reports.callback.store(callback, std::memory_order_release); }
uint64_t gcadapter_game_poll_count(int port) {
  if (port < 0 || port > 3) return 0;
  std::lock_guard<std::mutex> lk(g_mutex);
  return g_reports.game_poll_counts[port];
}
bool gcadapter_feeds_game_port(int port) { return port >= 0 && port < 4 && (g_reports.game_ports.load() & (1u << port)); }
bool gcadapter_game_origin(int port, uint8_t origin[6]) {
  if (port < 0 || port > 3) return false;
  std::lock_guard<std::mutex> lk(g_mutex);
  const Origin& o = g_origin[port];
  if (!o.set) return false;
  origin[0] = o.sx; origin[1] = o.sy; origin[2] = o.cx; origin[3] = o.cy; origin[4] = o.tl; origin[5] = o.tr;
  return true;
}
void gcadapter_set_game_input_replaced(bool replaced) { g_reports.replaced.store(replaced, std::memory_order_relaxed); }

}  // namespace host
