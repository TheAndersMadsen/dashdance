// Offline native AOT bring-up gate. The full frontend will own window, Metal,
// physical input, and device audio; guest scheduling and GX decoding run here.
// SPDX-License-Identifier: GPL-2.0-or-later
#if !defined(MELEE_PORT_OFFLINE) || !MELEE_PORT_OFFLINE
#error "The headless bring-up executable must be compiled with MELEE_PORT_OFFLINE=1"
#endif
#include "audio.h"
#include "audio_headless.h"
#include "exi_slippi.h"
#include "gx_core.h"
#include "headless_options.h"
#include "headless_manifest.h"
#include "hle_dvd.h"
#include "host.h"
#include "numeric.h"
#include "slippi_online.h"
#include "slippi_playback.h"
#include "window.h"
#include <cstdio>
#include <exception>

#ifndef MELEE_PORT_VERSION
#define MELEE_PORT_VERSION "dev"
#endif

namespace ppc { void init_dispatch(); }

int main(int argc, char** argv) {
  host::HeadlessOptions launch;
  std::string error;
  switch (host::parse_headless_options(argc, argv, launch, error)) {
    case host::HeadlessParse::Help: std::fputs(host::headless_usage(), stdout); return 0;
    case host::HeadlessParse::Version: std::printf("%s (offline headless AOT gate)\n", MELEE_PORT_VERSION); return 0;
    case host::HeadlessParse::Error:
      std::fprintf(stderr, "%s\n%s", error.c_str(), host::headless_usage()); return 2;
    case host::HeadlessParse::Ready: break;
  }
  if (launch.validate_only) {
    const auto& o = launch.runtime;
    std::printf("configuration valid; no outputs created and no guest executed\n"
                "offline=true strict_aot=%s frames=%u time_base=%llu\n"
                "iso=%s\nsys_dir=%s\nprofile_dir=%s\ncard_dir=%s\ncache_dir=%s\n"
                "log_file=%s\nreplay_dir=%s\nstate_trace=%s\naudio_dump=%s\n",
                launch.allow_interpreter ? "false (diagnostic interpreter allowed)" : "true", o.frames,
                static_cast<unsigned long long>(o.time_base), o.iso.c_str(), o.sys_dir.c_str(),
                o.profile_dir.c_str(), o.card_dir.c_str(), o.cache_dir.c_str(), o.log_file.c_str(),
                o.replay_dir.c_str(), o.state_trace.c_str(), o.audio_dump.c_str());
    if (launch.expect_scene) std::printf("expect_scene=0x%04X (state byte high, mode byte low)\n", launch.expected_scene);
    if (!launch.gx_capture.empty()) std::printf("gx_capture=%s sequence=%llu\n", launch.gx_capture.c_str(), static_cast<unsigned long long>(launch.gx_capture_sequence));
    return 0;
  }
  if (!launch.gx_capture.empty()) { std::fputs("GX capture backend is not yet wired in this executable\n", stderr); return 2; }
  // Script parsing also precedes any persistent output. No profile/card discovery
  // or service initialization has occurred at this point.
  if (!launch.script.empty() && !host::input_load_script(launch.script.c_str())) {
    std::fprintf(stderr, "cannot load input script\n"); return 2;
  }
  if (!host::prepare_headless_outputs(launch, error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 2; }
  host::options = launch.runtime;
  if (!launch.replay.empty()) slippi::playback::set_replay(launch.replay);  // needs a --playback translation
  slippi::online::config().offline = true;
  slippi::online::config().user_dir = host::options.profile_dir;
  ppc::set_interpreter_allowed(launch.allow_interpreter);
  host::HeadlessManifest manifest;
  if (!manifest.begin(launch, argc, argv, error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 2; }
  host::enable_headless_fatal_mode(true);
  bool quiesced = false;
  auto quiesce = [&] {
    if (quiesced) return;
    // Slippi owns preload and jukebox producers. Both may read the disc or log;
    // join them before DVD/audio/log finalization. This runs on the main thread
    // only, after a fatal boundary has unwound every guest-held lock.
    slippi::shutdown();
    hle::dvd_shutdown();
    host::gcadapter_shutdown();
    host::audio_close();
    host::close_state_trace();
    quiesced = true;
  };
  host::set_fatal_observer([&manifest, &quiesce](const std::string& reason) {
    quiesce();
    host::close_log_file();
    std::string failure;
    if (!manifest.finish(3, "fatal", reason, false, true, failure)) std::fprintf(stderr, "%s\n", failure.c_str());
  });
  auto finalize = [&](int code, const std::string& cause, const std::string& detail, bool orderly) {
    quiesce();
    host::close_log_file();
    if (!manifest.finish(code, cause, detail, orderly, true, error)) { std::fprintf(stderr, "%s\n", error.c_str()); if (code != 3) code = 1; }
    host::set_fatal_observer({});
    host::enable_headless_fatal_mode(false);
    return code;
  };
  host::log("Melee Unlocked %s: offline headless AOT gate", MELEE_PORT_VERSION);
  host::log("capabilities: guest CPU/HLE/GX decoding active; window, GPU presentation, physical input, audio device and online services unavailable");
  host::log("input: %s", !launch.replay.empty() ? "replay playback" : launch.script.empty() ? "neutral controller on port 1" : "deterministic controller script");
  host::log("execution: strict_aot=%s, frames=%u, time_base=%llu, fp_profile=%s",
            launch.allow_interpreter ? "false (diagnostic interpreter allowed)" : "true", host::options.frames,
            static_cast<unsigned long long>(host::options.time_base), ppc::fp_profile_name(ppc::fp_profile()));
  host::log("paths: profile=%s card=%s cache=%s", host::options.profile_dir.c_str(), host::options.card_dir.c_str(), host::options.cache_dir.c_str());
  host::log("build input manifest: %s sha256=%s", MELEE_PORT_INPUT_MANIFEST_PATH, MELEE_PORT_INPUT_MANIFEST_SHA256);
  int code = 0;
  bool fatal = false;
  std::string cause = "frame_bound", detail;
  {
    host::ScopedFatalBoundary fatal_boundary;
    ppc::ScopedGuestFpEnvironment fp_environment(0);
    try {
      if (!host::disc_open(host::options.iso)) {
        host::log("cannot open disc image"); code = 1; cause = "disc_open_failed";
      } else {
        gx::init(nullptr);
        if (!host::audio_open(0, host::options.audio_dump.c_str(), false)) {
          host::log("cannot open headless AI DMA output"); code = 1; cause = "audio_open_failed";
        } else {
          ppc::init_dispatch();
          host::boot_setup();
          host::log("boot: entering __start at 8000522C");
          ppc::call(*host::cpu, host::ram, 0x8000522Cu);
          host::log("guest returned from __start before the requested frame bound");
          code = 4; cause = "guest_returned_early";
        }
      }
    } catch (const host::HostFatal& e) {
      code = 3; fatal = true; cause = "fatal"; detail = e.what();
    } catch (const ExitRequested& stop) {
      code = stop.code;
      if (code) cause = "requested_exit";
    } catch (const LoadContextUnwind&) {
      host::log("unexpected OSLoadContext at top level");
      code = 3;
      cause = "unexpected_load_context";
    } catch (const std::exception& e) {
      host::log("host exception: %s", e.what());
      code = 3; fatal = true;
      cause = "host_exception"; detail = e.what();
    } catch (...) {
      code = 3; fatal = true; cause = "host_exception"; detail = "unknown host exception";
    }
  }
  quiesce();
  // A failure may arrive during the final worker join, after the last guest wait
  // point. It must turn a completed frame bound into a failed run.
  std::string background_error;
  if (host::take_background_failure(background_error)) {
    if (detail.empty()) detail = "background task failed: " + background_error;
    else detail += "; background task failed: " + background_error;
    host::log("fatal background failure: %s", background_error.c_str());
    code = 3; fatal = true; cause = "fatal";
  }
  if (!host::headless_audio_output_ok() || !host::state_trace_output_ok()) {
    host::log("headless AI DMA or state trace output failed"); if (code != 3) { code = 1; cause = "output_failed"; }
  }
  ppc::log_aot_diagnostics();
  uint64_t interpreted_calls = 0, interpreted_instructions = 0;
  ppc::interpreter_stats(&interpreted_calls, &interpreted_instructions);
  host::log("interpreter: %llu calls, %llu instructions", static_cast<unsigned long long>(interpreted_calls),
            static_cast<unsigned long long>(interpreted_instructions));
  host::log("audio: %llu AI DMA stereo frames consumed; no device playback or jukebox mix",
            static_cast<unsigned long long>(host::audio_pushed_frames()));
  host::log("slippi: %llu EXI commands, %llu replays written, GCT at %08X",
            static_cast<unsigned long long>(slippi::commands_seen()), static_cast<unsigned long long>(slippi::replays_written()),
            slippi::gct_load_address());
  if (!code && host::retrace_count() != host::options.frames) {
    host::log("requested frame bound was not reached");
    code = 4;
    cause = "frame_bound_not_reached";
  }
  if (!code && launch.expect_scene && !host::scene_trace_snapshot().saw(launch.expected_scene)) {
    host::log("required scene 0x%04X was not observed (state byte high, mode byte low)", launch.expected_scene);
    code = 5; cause = "expected_scene_not_observed";
  }
  host::log("headless result: exit=%d retraces=%u requested=%u", code, host::retrace_count(), host::options.frames);
  if (fatal && cause == "fatal") {
    host::notify_fatal_observer(detail);
    host::set_fatal_observer({});
    host::enable_headless_fatal_mode(false);
    return 3;
  }
  return finalize(code, cause, detail, !fatal);
}
