// Frame-by-frame oracle for the pinned vs_match script.  LegacyScript is the
// original Win32 window.cpp parser/sampler kept independent from InputScript.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "input_script.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
struct LegacyEntry {
  uint32_t frame;
  uint16_t buttons;
  int8_t sx, sy, cx, cy;
  int port;
  bool relative = false;
};

class LegacyScript {
public:
  bool load(const char* path) {
    FILE* file = std::fopen(path, "r");
    if (!file) return false;
    char line[256];
    while (std::fgets(line, sizeof line, file)) {
      LegacyEntry entry{};
      char* cursor = line;
      if (*cursor == '#' || *cursor == '\n' || *cursor == '\r') continue;
      if (!std::strncmp(cursor, "@match", 6)) { relative_section_ = true; continue; }
      if (!std::strncmp(cursor, "@loop", 5)) {
        loop_ = static_cast<uint32_t>(std::strtoul(cursor + 5, nullptr, 10));
        continue;
      }
      entry.relative = relative_section_;
      entry.frame = static_cast<uint32_t>(std::strtoul(cursor, &cursor, 10));
      while (*cursor) {
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        if (!*cursor || *cursor == '\n' || *cursor == '\r' || *cursor == '#') break;
        char token[32];
        int length = 0;
        while (*cursor && *cursor != ' ' && *cursor != '+' && *cursor != '\n' &&
               *cursor != '\r' && length < 31) {
          token[length++] = *cursor++;
        }
        token[length] = 0;
        if (*cursor == '+') ++cursor;
        if (!std::strcmp(token, "A")) entry.buttons |= 0x0100;
        else if (!std::strcmp(token, "B")) entry.buttons |= 0x0200;
        else if (!std::strcmp(token, "X")) entry.buttons |= 0x0400;
        else if (!std::strcmp(token, "Y")) entry.buttons |= 0x0800;
        else if (!std::strcmp(token, "Z")) entry.buttons |= 0x0010;
        else if (!std::strcmp(token, "L")) entry.buttons |= 0x0040;
        else if (!std::strcmp(token, "R")) entry.buttons |= 0x0020;
        else if (!std::strcmp(token, "START")) entry.buttons |= 0x1000;
        else if (!std::strcmp(token, "DU")) entry.buttons |= 0x0008;
        else if (!std::strcmp(token, "DD")) entry.buttons |= 0x0004;
        else if (!std::strcmp(token, "DL")) entry.buttons |= 0x0001;
        else if (!std::strcmp(token, "DR")) entry.buttons |= 0x0002;
        else if (!std::strncmp(token, "sx=", 3)) entry.sx = static_cast<int8_t>(std::atoi(token + 3));
        else if (!std::strncmp(token, "sy=", 3)) entry.sy = static_cast<int8_t>(std::atoi(token + 3));
        else if (!std::strncmp(token, "cx=", 3)) entry.cx = static_cast<int8_t>(std::atoi(token + 3));
        else if (!std::strncmp(token, "cy=", 3)) entry.cy = static_cast<int8_t>(std::atoi(token + 3));
        else if (!std::strncmp(token, "p=", 2)) {
          entry.port = std::atoi(token + 2) - 1;
          if (entry.port < 0 || entry.port > 3) entry.port = 0;
          ports_ |= 1u << entry.port;
        }
      }
      entries_.push_back(entry);
    }
    std::fclose(file);
    return !entries_.empty();
  }

  std::array<host::ScriptPad, 4> sample(uint32_t retrace, uint32_t match_start = 0) const {
    std::array<host::ScriptPad, 4> result{};
    bool in_match = match_start && retrace >= match_start;
    uint32_t relative = in_match ? retrace - match_start : 0;
    if (in_match && loop_) relative %= loop_;
    for (int port = 0; port < 4; ++port) {
      if (port && !(ports_ & (1u << port))) continue;
      result[port].connected = true;
      const LegacyEntry* current = nullptr;
      for (const LegacyEntry& entry : entries_) {
        if (entry.port != port) continue;
        if (entry.relative) {
          if (in_match && entry.frame <= relative) current = &entry;
        } else if (!in_match && entry.frame <= retrace) {
          current = &entry;
        }
      }
      if (current) {
        result[port].buttons = current->buttons;
        result[port].sx = current->sx;
        result[port].sy = current->sy;
        result[port].cx = current->cx;
        result[port].cy = current->cy;
      }
    }
    return result;
  }

private:
  std::vector<LegacyEntry> entries_;
  uint32_t ports_ = 1;
  bool relative_section_ = false;
  uint32_t loop_ = 0;
};

[[noreturn]] void fail(const char* message, uint32_t retrace = 0, int port = 0) {
  std::fprintf(stderr, "%s (retrace %u, port %d)\n", message, retrace, port + 1);
  std::exit(1);
}

void require(bool condition, const char* message) {
  if (!condition) fail(message);
}

bool equal(const host::ScriptPad& left, const host::ScriptPad& right) {
  return left.connected == right.connected && left.buttons == right.buttons &&
         left.sx == right.sx && left.sy == right.sy && left.cx == right.cx &&
         left.cy == right.cy;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) fail("usage: input_script_equivalence_test /path/to/vs_match.txt");
  std::ifstream source(argv[1]);
  require(static_cast<bool>(source), "cannot open production script");
  const std::string source_text((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
  source.clear();
  source.seekg(0);

  // @scene blocks are a Dashdance extension the Win32 oracle cannot parse and
  // cannot verify (their entries are scene-gated, not time-keyed). Strip the
  // blocks from the comparison text so both parsers see the same legacy
  // subset; the gated engine itself is checked separately below.
  std::string legacy_text;
  bool in_scene_block = false;
  {
    std::istringstream reader(source_text);
    std::string line;
    while (std::getline(reader, line)) {
      auto comment = line.find('#');
      std::string code = line.substr(0, comment == std::string::npos ? line.size() : comment);
      std::istringstream fields(code);
      std::string first;
      if (!(fields >> first)) continue;   // blank: keep for readability
      if (first == "@scene") { in_scene_block = true; continue; }
      if (first == "@match" || first == "@loop") { in_scene_block = false; }
      if (in_scene_block) continue;
      legacy_text += line;
      legacy_text += '\n';
    }
  }
  const std::string filtered_path = std::string(argv[1]) + ".ungated";
  {
    std::ofstream out(filtered_path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(out), "cannot write filtered script");
    out << legacy_text;
  }

  host::InputScript production;
  std::string error;
  std::istringstream production_stream(legacy_text);
  require(production.load(production_stream, &error), error.c_str());
  LegacyScript legacy;
  require(legacy.load(filtered_path.c_str()), "legacy parser cannot open production script");

  for (uint32_t match_start : {0u, 1500u}) {
    for (uint32_t retrace = 0; retrace <= 3000; ++retrace) {
      const auto actual = production.sample(retrace, match_start);
      const auto expected = legacy.sample(retrace, match_start);
      for (int port = 0; port < 4; ++port) {
        if (!equal(actual[port], expected[port])) {
          std::fprintf(stderr, "MISMATCH retrace=%u match_start=%u port=%d actual(conn=%d btn=%04x sx=%d sy=%d) expected(conn=%d btn=%04x sx=%d sy=%d)\n",
                       retrace, match_start, port,
                       actual[port].connected ? 1 : 0, actual[port].buttons, actual[port].sx, actual[port].sy,
                       expected[port].connected ? 1 : 0, expected[port].buttons, expected[port].sx, expected[port].sy);
          fail("portable script trace differs from Win32 trace", retrace, port);
        }
      }
    }
  }

  auto pads = production.sample(0);
  require(pads[0].connected && pads[1].connected && !pads[2].connected,
          "a port mentioned later in the file is connected from boot, matching Win32");

  // The production script uses @scene blocks (a Dashdance extension the Win32
  // oracle cannot express); those are stripped from the comparison above.
  // Verify the gated engine directly instead: entries apply only while their
  // scene matches, with frames counted from the first matching retrace. The
  // script's boot-scene block presses A at relative frames 440, 540, and 640.
  if (source_text.find("@scene") == std::string::npos) return 0;   // legacy scripts: nothing gated to check
  host::InputScript gated;
  std::string gate_error;
  std::istringstream gate_stream(source_text);
  require(gated.load(gate_stream, &gate_error), gate_error.c_str());
  const auto before = gated.sample(300, 0, 0x28, 0x00);   // origin arms at 300
  require(before[0].buttons == 0, "gate entry must not apply before its frame");
  const auto during = gated.sample(750, 0, 0x28, 0x00);   // relative 450: A held
  require(during[0].buttons == 0x0100, "gate entry applies while the scene matches");
  const auto gap = gated.sample(800, 0, 0x28, 0x00);      // relative 500: neutral gap
  require(gap[0].buttons == 0, "gate neutral line releases the button");
  // A different scene must not activate the block.
  const auto other = gated.sample(750, 0, 0x01, 0x00);
  require(other[0].buttons == 0, "gate entries must not leak into other scenes");

  if (source_text.find("@match") != std::string::npos) {
  pads = production.sample(1530, 1500);
  require(pads[0].sx == 80 && pads[1].sx == -80, "relative frame 30 opposing movement");
  pads = production.sample(1570, 1500);
  require(pads[0].buttons == 0x0100 && pads[1].buttons == 0x0100,
          "relative frame 70 two-port A press");
  pads = production.sample(1605, 1500);
  require(pads[0].buttons == 0x0400 && pads[1].buttons == 0x0800,
          "relative frame 105 distinct jump buttons");
  pads = production.sample(1740, 1500);
  require(pads[0].buttons == 0 && pads[0].sx == 0 && pads[1].buttons == 0 && pads[1].sx == 0,
          "relative loop wraps to frame zero");
  }
  return 0;
}
