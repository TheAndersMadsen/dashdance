// Script behavior matches the valid-script path in the original Win32 window.cpp:
// last eligible entry in file order wins; absolute entries stop at @match; @loop
// repeats relative entries; port 1 and any explicitly scripted ports are connected.
// @scene MODE STATE blocks key their frame numbers on the guest state machine
// instead of the wall clock, so menu navigation survives boot-length variance.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "input_script.h"
#include <algorithm>
#include <charconv>
#include <sstream>
#include <string_view>

namespace host {
namespace {
template <typename T> bool number(std::string_view text, T& out) {
  if (text.empty()) return false;
  auto result = std::from_chars(text.data(), text.data() + text.size(), out, 10);
  return result.ec == std::errc() && result.ptr == text.data() + text.size();
}
bool hex_byte(std::string_view text, uint8_t& out) {
  size_t pos = text.rfind("0x", 0) == 0 ? 2 : 0;
  if (pos >= text.size()) return false;
  int value = 0;
  auto result = std::from_chars(text.data() + pos, text.data() + text.size(), value, 16);
  if (result.ec != std::errc() || result.ptr != text.data() + text.size()) return false;
  if (value < 0 || value > 0xFF) return false;
  out = static_cast<uint8_t>(value);
  return true;
}
}  // namespace

bool InputScript::load(std::istream& stream, std::string* error) {
  InputScript parsed;
  bool relative = false;
  bool scene_gated = false;
  std::string line;
  size_t line_number = 0;
  auto fail = [&](const std::string& reason) {
    if (error) *error = "line " + std::to_string(line_number) + ": " + reason;
    return false;
  };
  while (std::getline(stream, line)) {
    ++line_number;
    if (line.size() > 4096) return fail("script line is too long");
    if (auto comment = line.find('#'); comment != std::string::npos) line.erase(comment);
    std::istringstream fields(line);
    std::string first;
    if (!(fields >> first)) continue;
    if (first == "@match") {
      std::string extra;
      if (fields >> extra) return fail("@match takes no value");
      relative = true;      // @match entries are match-relative and ungated
      scene_gated = false;  // scene gates drive menus only
      continue;
    }
    if (first == "@loop") {
      std::string value, extra;
      if (!(fields >> value) || !number(value, parsed.loop_) || (fields >> extra)) return fail("invalid @loop frame count");
      continue;
    }
    if (first == "@scene") {
      if (relative) return fail("@scene must precede @match");
      std::string mode_text, state_text, extra;
      if (!(fields >> mode_text >> state_text) || (fields >> extra)) return fail("@scene takes MODE and STATE");
      uint8_t mode, state;
      if (!hex_byte(mode_text, mode) || !hex_byte(state_text, state)) return fail("invalid @scene MODE STATE bytes");
      for (const auto& gate : parsed.gates_)
        if (gate.mode == mode && gate.state == state) return fail("duplicate @scene gate");
      parsed.gates_.push_back({mode, state});
      scene_gated = true;
      continue;
    }
    Entry entry;
    entry.relative = relative;
    if (scene_gated) entry.scene_group = static_cast<uint8_t>(parsed.gates_.size() - 1);
    if (!number(first, entry.frame)) return fail("invalid frame number");
    std::string token;
    while (fields >> token) {
      size_t start = 0;
      do {
        size_t end = token.find('+', start);
        std::string_view part(token.data() + start, (end == std::string::npos ? token.size() : end) - start);
        if (part == "A") entry.pad.buttons |= 0x0100;
        else if (part == "B") entry.pad.buttons |= 0x0200;
        else if (part == "X") entry.pad.buttons |= 0x0400;
        else if (part == "Y") entry.pad.buttons |= 0x0800;
        else if (part == "Z") entry.pad.buttons |= 0x0010;
        else if (part == "L") entry.pad.buttons |= 0x0040;
        else if (part == "R") entry.pad.buttons |= 0x0020;
        else if (part == "START") entry.pad.buttons |= 0x1000;
        else if (part == "DU") entry.pad.buttons |= 0x0008;
        else if (part == "DD") entry.pad.buttons |= 0x0004;
        else if (part == "DL") entry.pad.buttons |= 0x0001;
        else if (part == "DR") entry.pad.buttons |= 0x0002;
        else if (part.substr(0, 2) == "p=") {
          unsigned port;
          if (!number(part.substr(2), port) || port < 1 || port > 4) return fail("controller port must be 1..4");
          entry.port = port - 1;
        } else if (part.substr(0, 3) == "sx=" || part.substr(0, 3) == "sy=" ||
                   part.substr(0, 3) == "cx=" || part.substr(0, 3) == "cy=") {
          int value;
          if (!number(part.substr(3), value) || value < -128 || value > 127) return fail("stick value must be -128..127");
          if (part.substr(0, 2) == "sx") entry.pad.sx = int8_t(value);
          else if (part.substr(0, 2) == "sy") entry.pad.sy = int8_t(value);
          else if (part.substr(0, 2) == "cx") entry.pad.cx = int8_t(value);
          else entry.pad.cy = int8_t(value);
        } else return fail("unknown input token: " + std::string(part));
        if (end == std::string::npos) break;
        start = end + 1;
      } while (true);
    }
    parsed.ports_ |= 1u << entry.port;
    parsed.entries_.push_back(entry);
  }
  if (stream.bad()) return fail("cannot read script");
  if (parsed.entries_.empty()) return fail("script has no input entries");
  *this = std::move(parsed);
  return true;
}

std::array<ScriptPad, 4> InputScript::sample(uint32_t retrace, uint32_t match_start) const {
  return sample(retrace, match_start, kUngated, kUngated);
}

std::array<ScriptPad, 4> InputScript::sample(uint32_t retrace, uint32_t match_start,
                                             uint8_t scene_mode, uint8_t scene_state) const {
  std::array<ScriptPad, 4> result{};
  bool in_match = match_start && retrace >= match_start;
  uint32_t relative = in_match ? retrace - match_start : 0;
  if (in_match && loop_) relative %= loop_;

  const SceneGate* active = nullptr;
  uint32_t gate_relative = 0;
  for (const auto& gate : gates_) {
    if (gate.mode != scene_mode || gate.state != scene_state) continue;
    if (!gate.origin_set) {
      gate.origin = retrace;
      gate.origin_set = true;
    }
    gate_relative = retrace - gate.origin;
    active = &gate;
    break;
  }

  for (unsigned port = 0; port < result.size(); ++port) {
    if (!(ports_ & (1u << port))) continue;
    for (const Entry& entry : entries_) {
      if (entry.port != port) continue;
      bool applies;
      if (entry.relative) {
        applies = in_match && entry.frame <= relative;
      } else if (entry.scene_group == kUngated) {
        // While a scene gate drives the pads, legacy time-keyed entries are
        // suspended: the two timing models must not fight over the controllers.
        applies = !in_match && !active && entry.frame <= retrace;
      } else {
        const SceneGate& gate = gates_[entry.scene_group];
        applies = !in_match && active == &gate && entry.frame <= gate_relative;
      }
      if (applies) result[port] = entry.pad;
    }
    // Last so the entry assignment cannot clear it: a scripted port is
    // connected from boot, matching the Win32 sampler.
    result[port].connected = true;
  }
  return result;
}
}  // namespace host
