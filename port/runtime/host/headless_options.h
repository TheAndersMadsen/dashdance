// Offline launch validation. Parsing/validation never creates files or directories.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "host.h"

namespace host {
struct HeadlessOptions {
  Options runtime;
  std::string script;
  std::string replay;             // playback build: re-simulate this .slp from its recorded inputs
  bool allow_interpreter = false;
  bool validate_only = false;
  bool expect_scene = false;
  uint16_t expected_scene = 0;    // explicit 0xSSMM: state byte high, mode byte low
  std::string gx_capture;
  uint64_t gx_capture_sequence = 1;
};
enum class HeadlessParse { Ready, Help, Version, Error };
HeadlessParse parse_headless_options(int argc, const char* const* argv, HeadlessOptions& out, std::string& error);
// Reserves all three previously validated fresh directories. No existing directory
// is accepted, including a directory created since validation. Leaves any newly
// created directories in place on failure so the launch can be inspected.
bool prepare_headless_outputs(const HeadlessOptions& options, std::string& error);
const char* headless_usage();
}  // namespace host
