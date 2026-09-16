// Deterministic controller script state, independent of the window or CPU backend.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <istream>
#include <string>
#include <vector>

namespace host {
struct ScriptPad {
  uint16_t buttons = 0;
  int8_t sx = 0, sy = 0, cx = 0, cy = 0;
  bool connected = false;
};
class InputScript {
public:
  bool load(std::istream& stream, std::string* error = nullptr);
  // Legacy form: time-keyed entries and @match/@loop only; scene-gated blocks
  // never match. Scene-aware callers pass the guest state-machine bytes
  // (0x80479D30 mode, 0x80479D33 state id).
  std::array<ScriptPad, 4> sample(uint32_t retrace, uint32_t match_start = 0) const;
  std::array<ScriptPad, 4> sample(uint32_t retrace, uint32_t match_start,
                                  uint8_t scene_mode, uint8_t scene_state) const;
  bool empty() const { return entries_.empty(); }
private:
  static constexpr uint8_t kUngated = 0xFF;
  struct Entry {
    uint32_t frame = 0;
    ScriptPad pad;
    unsigned port = 0;
    bool relative = false;
    uint8_t scene_group = kUngated;  // kUngated: time-keyed (@scene blocks set this)
  };
  // A @scene MODE STATE block. origin/origin_set latch the first retrace the
  // gate matched so its frame numbers count from scene entry, surviving
  // boot-length variance.
  struct SceneGate {
    uint8_t mode = 0, state = 0;
    mutable uint32_t origin = 0;
    mutable bool origin_set = false;
  };
  std::vector<Entry> entries_;
  std::vector<SceneGate> gates_;
  uint32_t ports_ = 1;
  uint32_t loop_ = 0;
};
}  // namespace host
