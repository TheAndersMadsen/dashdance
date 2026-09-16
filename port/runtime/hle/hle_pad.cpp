// PAD HLE: controller state from the host input layer.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "hle.h"
#include "pad_rumble.h"
#include <cstring>
#include <cstdlib>

static uint32_t s_spec = 5;

HLE(PADInit) { RET(1); }
HLE(PADReset) { RET(1); }
HLE(PADRecalibrate) { RET(1); }
// PADControlMotor(chan, command): 0 stop, 1 rumble, 2 stop hard.
HLE(PADControlMotor) {
  const auto decision = pad::decide_rumble(
      ARG0, ARG1, host::rd8(0x80479D30), host::rd8(0x80479D33), c.r[13],
      host::ram, ppc::RAM_SIZE);
  if (decision.deliver) {
    host::gcadapter_rumble(decision.physical_port, decision.on);
  }
}
HLE(PADControlAllMotors) { for (int i = 0; i < 4; ++i) host::gcadapter_rumble(i, host::rd32(ARG0 + 4 * i) == 1); }
HLE(PADSetSpec) { s_spec = ARG0; }
HLE(PADGetSpec) { RET(s_spec); }
HLE(PADGetType) { if (ARG1) host::wr32(ARG1, 0x08000000); RET(1); }
HLE(PADSync) { RET(1); }
HLE(PADSetAnalogMode) {}
HLE(PADSetSamplingRate) {}

// u32 PADRead(PADStatus* status[4]) -> bitmask of channels with fresh data
HLE(PADRead) {
  host::pump_completions();
#if defined(MELEE_PORT_OFFLINE)
  // MELEE_FORCE_SCENE=0xMMSS: automation hook. When the guest reaches the
  // main-menu scene (0x01), rewrite the state-machine bytes to jump straight
  // to the requested scene — the same trick as a "boot to CSS" Gecko code —
  // so offline acceptance runs do not depend on navigating the Slippi
  // log-in menu. Scene-gated script blocks handle everything after.
  static const uint16_t force_scene = [] {
    if (const char* v = std::getenv("MELEE_FORCE_SCENE")) {
      return static_cast<uint16_t>(std::strtoul(v, nullptr, 16));
    }
    return static_cast<uint16_t>(0);
  }();
  if (force_scene && host::rd8(0x80479D30) == 0x01) {
    host::wr8(0x80479D30, static_cast<uint8_t>(force_scene & 0xFF));
    host::wr8(0x80479D33, static_cast<uint8_t>(force_scene >> 8));
    if (static_cast<bool>(std::getenv("MELEE_PAD_TRACE"))) {
      host::log("[padtrace] forced scene to 0x%04X", force_scene);
    }
  }
#endif
  static int reported = 0;
  if (host::options.trace_calls && reported++ < 10) host::log("[pad] PADRead(%08X)", ARG0);
#if defined(MELEE_PORT_OFFLINE) || defined(TARGET_PC)
  static unsigned pad_trace_count = 0;
  static const bool pad_trace = std::getenv("MELEE_PAD_TRACE") != nullptr;
  if (pad_trace && ++pad_trace_count % 120 == 0) {
    host::PadState probe[4];
    host::input_poll(probe);
    uint64_t cmap_button = host::rd32(0x80479C30);
    cmap_button |= (uint64_t)host::rd32(0x80479C34) << 32;
    uint64_t cmap_trigger = host::rd32(0x80479C38);
    cmap_trigger |= (uint64_t)host::rd32(0x80479C3C) << 32;
    host::log("[padtrace] call=%u scene=%02x/%02x p1: err=%d btn=%04x sx=%d sy=%d cmap btn=%04llx trig=%04llx",
              pad_trace_count, host::rd8(0x80479D30), host::rd8(0x80479D33),
              probe[0].err, probe[0].button, probe[0].stick_x, probe[0].stick_y,
              (unsigned long long)(cmap_button & 0xFFFF), (unsigned long long)(cmap_trigger & 0xFFFF));
  }
#endif
  host::PadState pads[4];
  host::input_poll(pads);
  uint32_t base = ARG0, mask = 0;
  for (int i = 0; i < 4; ++i) {
    uint32_t p = base + i * 12;
    host::wr16(p + 0, pads[i].button);
    host::wr8(p + 2, (uint8_t)pads[i].stick_x);
    host::wr8(p + 3, (uint8_t)pads[i].stick_y);
    host::wr8(p + 4, (uint8_t)pads[i].sub_x);
    host::wr8(p + 5, (uint8_t)pads[i].sub_y);
    host::wr8(p + 6, pads[i].trig_l);
    host::wr8(p + 7, pads[i].trig_r);
    host::wr8(p + 8, pads[i].analog_a);
    host::wr8(p + 9, pads[i].analog_b);
    host::wr8(p + 10, (uint8_t)pads[i].err);
    host::wr8(p + 11, 0);
    if (pads[i].err == 0) mask |= 0x80000000u >> i;
  }
  RET(mask);
}
