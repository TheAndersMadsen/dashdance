// SPDX-License-Identifier: GPL-2.0-or-later
#include "headless_options.h"
#include <array>
#include <charconv>
#include <filesystem>
#include <limits>
#include <set>
#include <string_view>

namespace host {
namespace {
namespace fs = std::filesystem;
bool unsigned_number(std::string_view value, uint64_t& result) {
  int base = 10;
  if (value.substr(0, 2) == "0x" || value.substr(0, 2) == "0X") { base = 16; value.remove_prefix(2); }
  if (value.empty()) return false;
  auto parsed = std::from_chars(value.data(), value.data() + value.size(), result, base);
  return parsed.ec == std::errc() && parsed.ptr == value.data() + value.size();
}
bool absent(const fs::path& path, std::string& error) {
  std::error_code ec;
  auto status = fs::symlink_status(path, ec);
  if (ec && ec != std::errc::no_such_file_or_directory) { error = "cannot inspect output path: " + path.string(); return false; }
  if (status.type() != fs::file_type::not_found) { error = "output path already exists: " + path.string(); return false; }
  return true;
}
bool fresh_directory(std::string& value, const char* flag, std::string& error) {
  fs::path path(value);
  if (value.empty() || !path.is_absolute()) { error = std::string(flag) + " requires an absolute fresh directory"; return false; }
  path = path.lexically_normal();
  if (path.filename().empty() || path.filename() == "." || path.filename() == "..") {
    error = std::string(flag) + " requires a named fresh directory"; return false;
  }
  std::error_code ec;
  fs::path parent = fs::canonical(path.parent_path(), ec);
  if (ec || !fs::is_directory(parent, ec) || ec) { error = std::string(flag) + " parent directory must already exist"; return false; }
  path = parent / path.filename();
  if (!absent(path, error)) return false;
  value = path.string();
  return true;
}
bool existing_input(std::string& value, const char* flag, bool directory, std::string& error) {
  if (value.empty() || !fs::path(value).is_absolute()) { error = std::string(flag) + " requires an absolute path"; return false; }
  std::error_code ec;
  fs::path path = fs::canonical(value, ec);
  if (ec || (directory ? !fs::is_directory(path, ec) : !fs::is_regular_file(path, ec)) || ec) {
    error = std::string(flag) + (directory ? " must name an existing directory" : " must name an existing regular file"); return false;
  }
  value = path.string();
  return true;
}
bool output_file(std::string& value, const fs::path& cache, std::string& error) {
  if (value.empty()) return true;
  fs::path path(value);
  if (!path.is_absolute() || path.filename().empty() || path.filename() == "." || path.filename() == "..") {
    error = "output files require absolute paths inside --cache-dir"; return false;
  }
  // cache itself is intentionally absent. Resolve its existing parent so /tmp
  // and /private/tmp name the same isolation boundary on macOS.
  std::error_code ec;
  auto parent = fs::weakly_canonical(path.parent_path(), ec);
  if (ec || parent != cache || path.filename() == "replays" || path.filename() == "launch.json" || path.filename() == "result.json" ||
      path.filename() == "launch.json.tmp" || path.filename() == "result.json.tmp") {
    error = "output files must be direct children of --cache-dir (excluding reserved replay/manifest paths)"; return false;
  }
  path = cache / path.filename();
  if (!absent(path, error)) return false;
  value = path.string();
  return true;
}
}  // namespace

const char* headless_usage() {
  return "melee_port_headless --offline --iso ABS_FILE --sys-dir ABS_DIR\n"
         "  --frames 1..36000 --time-base UINT64\n"
         "  --profile-dir ABS_FRESH_DIR --card-dir ABS_FRESH_DIR --cache-dir ABS_FRESH_DIR\n"
         "  [--fast] [--script ABS_FILE | --replay ABS_FILE] [--strict-aot | --allow-interpreter]\n"
         "  [--state-trace ABS_CACHE_FILE] [--audio-dump ABS_CACHE_FILE] [--log-file ABS_CACHE_FILE]\n"
         "  [--trace-calls] [--hang-watch 1..60] [--validate-only]\n"
         "  [--expect-scene 0xSSMM] [--gx-capture ABS_CACHE_FILE --gx-capture-sequence N]\n"
         "The headless gate is offline. Output directories must be absent and have existing parents.\n"
         "--validate-only checks arguments and paths without creating output or executing the guest.\n";
}

HeadlessParse parse_headless_options(int argc, const char* const* argv, HeadlessOptions& out, std::string& error) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") return HeadlessParse::Help;
  if (argc == 2 && std::string_view(argv[1]) == "--version") return HeadlessParse::Version;
  HeadlessOptions parsed;
  parsed.runtime.hang_watch = 10;
  parsed.runtime.trace_scenes = true;
  // No inherited relative path is usable in this executable.
  parsed.runtime.sys_dir.clear(); parsed.runtime.card_dir.clear(); parsed.runtime.replay_dir.clear();
  std::set<std::string> seen;
  auto fail = [&](const std::string& reason) { error = reason; return HeadlessParse::Error; };
  for (int i = 1; i < argc; ++i) {
    std::string flag(argv[i]);
    if (!seen.insert(flag).second) return fail("duplicate option: " + flag);
    if (flag == "--offline") parsed.runtime.offline = true;
    else if (flag == "--fast") parsed.runtime.fast = true;
    else if (flag == "--trace-calls") parsed.runtime.trace_calls = true;
    else if (flag == "--validate-only") parsed.validate_only = true;
    else if (flag == "--strict-aot") parsed.allow_interpreter = false;
    else if (flag == "--allow-interpreter") parsed.allow_interpreter = true;
    else {
      if (i + 1 >= argc) return fail("missing value for " + flag);
      std::string value(argv[++i]);
      if (flag == "--iso") parsed.runtime.iso = value;
      else if (flag == "--sys-dir") parsed.runtime.sys_dir = value;
      else if (flag == "--profile-dir") parsed.runtime.profile_dir = value;
      else if (flag == "--card-dir") parsed.runtime.card_dir = value;
      else if (flag == "--cache-dir") parsed.runtime.cache_dir = value;
      else if (flag == "--script") parsed.script = value;
      else if (flag == "--replay") parsed.replay = value;
      else if (flag == "--state-trace") parsed.runtime.state_trace = value;
      else if (flag == "--audio-dump") parsed.runtime.audio_dump = value;
      else if (flag == "--log-file") parsed.runtime.log_file = value;
      else if (flag == "--gx-capture") parsed.gx_capture = value;
      else if (flag == "--frames" || flag == "--time-base" || flag == "--hang-watch" || flag == "--expect-scene" || flag == "--gx-capture-sequence") {
        uint64_t number = 0;
        if (!unsigned_number(value, number)) return fail("invalid numeric value for " + flag);
        if (flag == "--frames") {
          if (!number || number > 36000) return fail("--frames must be 1..36000");
          parsed.runtime.frames = uint32_t(number);
        } else if (flag == "--time-base") {
          parsed.runtime.time_base = number;
          parsed.runtime.time_base_set = true;
        } else if (flag == "--expect-scene") {
          if (number > 0xffff) return fail("--expect-scene must be a 16-bit combined state/mode value (0xSSMM)");
          parsed.expect_scene = true; parsed.expected_scene = uint16_t(number);
        } else if (flag == "--gx-capture-sequence") {
          if (!number || number > 36000) return fail("--gx-capture-sequence must be 1..36000");
          parsed.gx_capture_sequence = number;
        } else {
          if (!number || number > 60) return fail("--hang-watch must be 1..60 seconds");
          parsed.runtime.hang_watch = double(number);
        }
      } else return fail("unknown or unavailable headless option: " + flag);
    }
  }
  if (!parsed.runtime.offline) return fail("--offline is required; online services are unavailable in this executable");
  if (!parsed.runtime.frames) return fail("an explicit bounded --frames value is required");
  if (!parsed.runtime.time_base_set) return fail("an explicit --time-base is required");
  if (seen.count("--strict-aot") && seen.count("--allow-interpreter")) return fail("choose one interpreter policy");
  if (seen.count("--script") && seen.count("--replay")) return fail("choose an input script or a replay, not both");
  if (seen.count("--gx-capture-sequence") && parsed.gx_capture.empty()) return fail("--gx-capture-sequence requires --gx-capture");
  if (!parsed.gx_capture.empty() && parsed.gx_capture_sequence > parsed.runtime.frames) return fail("capture sequence cannot exceed --frames");
  if (!existing_input(parsed.runtime.iso, "--iso", false, error) ||
      !existing_input(parsed.runtime.sys_dir, "--sys-dir", true, error) ||
      (!parsed.script.empty() && !existing_input(parsed.script, "--script", false, error)) ||
      (!parsed.replay.empty() && !existing_input(parsed.replay, "--replay", false, error))) return HeadlessParse::Error;
  if (!fresh_directory(parsed.runtime.profile_dir, "--profile-dir", error) ||
      !fresh_directory(parsed.runtime.card_dir, "--card-dir", error) ||
      !fresh_directory(parsed.runtime.cache_dir, "--cache-dir", error)) return HeadlessParse::Error;
  std::set<std::string> dirs{parsed.runtime.profile_dir, parsed.runtime.card_dir, parsed.runtime.cache_dir};
  if (dirs.size() != 3) return fail("profile, card, and cache directories must be distinct");
  fs::path cache(parsed.runtime.cache_dir);
  if (parsed.runtime.log_file.empty()) parsed.runtime.log_file = (cache / "melee_port.log").string();
  if (!output_file(parsed.runtime.log_file, cache, error) || !output_file(parsed.runtime.state_trace, cache, error) ||
      !output_file(parsed.runtime.audio_dump, cache, error) || !output_file(parsed.gx_capture, cache, error)) return HeadlessParse::Error;
  std::set<std::string> files;
  for (const auto* value : {&parsed.runtime.log_file, &parsed.runtime.state_trace, &parsed.runtime.audio_dump, &parsed.gx_capture})
    if (!value->empty() && !files.insert(*value).second) return fail("log, state trace, and audio outputs must be distinct");
  parsed.runtime.replay_dir = (cache / "replays").string();
  out = std::move(parsed);
  return HeadlessParse::Ready;
}

bool prepare_headless_outputs(const HeadlessOptions& options, std::string& error) {
  const auto& runtime = options.runtime;
  for (const auto* path : {&runtime.profile_dir, &runtime.card_dir, &runtime.cache_dir}) {
    std::error_code ec;
    if (!fs::create_directory(*path, ec) || ec) { error = "cannot reserve fresh output directory: " + *path; return false; }
    fs::permissions(*path, fs::perms::owner_all, fs::perm_options::replace, ec);
    if (ec) { error = "cannot restrict output directory permissions: " + *path; return false; }
  }
  std::error_code ec;
  if (!fs::create_directory(runtime.replay_dir, ec) || ec) { error = "cannot create replay output directory"; return false; }
  fs::permissions(runtime.replay_dir, fs::perms::owner_all, fs::perm_options::replace, ec);
  if (ec) { error = "cannot restrict replay directory permissions"; return false; }
  return true;
}
}  // namespace host
