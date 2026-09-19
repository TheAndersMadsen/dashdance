// Offline arm64 Cocoa/Metal/Aurora frontend for the translated game runtime.
// SPDX-License-Identifier: GPL-2.0-or-later
#if !defined(MELEE_PORT_OFFLINE) || !MELEE_PORT_OFFLINE
#error "The first Metal executable is an explicitly offline target"
#endif

#include "audio.h"
#include "audio_headless.h"
#include "exi_slippi.h"
#include "gx_aurora_bridge.h"
#include "gx_core.h"
#include "headless_options.h"
#include "hle_dvd.h"
#include "host.h"
#include "metal_frontend.h"
#include "metal_window.h"
#include "numeric.h"
#include "slippi_online.h"
#include "window.h"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>

#ifndef MELEE_PORT_VERSION
#define MELEE_PORT_VERSION "dev"
#endif

namespace ppc { void init_dispatch(); }

namespace {
const char* metal_usage() {
  return "melee_port_metal --offline --iso ABS_FILE --sys-dir ABS_DIR\n"
         "  --frames 1..36000 --time-base UINT64\n"
         "  --profile-dir ABS_FRESH_DIR --card-dir ABS_FRESH_DIR --cache-dir ABS_FRESH_DIR\n"
         "  [--script ABS_FILE] [--strict-aot | --allow-interpreter]\n"
         "  [--state-trace ABS_CACHE_FILE] [--audio-dump ABS_CACHE_FILE] [--log-file ABS_CACHE_FILE]\n"
         "  [--gx-capture ABS_CACHE_FILE [--gx-capture-sequence N]] [--expect-scene 0xSSMM]\n"
         "  [--trace-calls] [--hang-watch 1..60] [--validate-only]\n"
         "This first Metal target is offline. It uses deterministic input and no audio device.\n"
         "All output directories must be explicit, absent, and have existing parents.\n";
}
} // namespace

int main(int argc, char** argv) {
  host::HeadlessOptions launch;
  std::string error;
  switch (host::parse_headless_options(argc, argv, launch, error)) {
    case host::HeadlessParse::Help: std::fputs(metal_usage(), stdout); return 0;
    case host::HeadlessParse::Version:
      std::printf("%s (offline arm64 Cocoa/Metal/Aurora gate)\n", MELEE_PORT_VERSION); return 0;
    case host::HeadlessParse::Error:
      std::fprintf(stderr, "%s\n%s", error.c_str(), metal_usage()); return 2;
    case host::HeadlessParse::Ready: break;
  }
  if (launch.validate_only) {
    const auto& options = launch.runtime;
    std::printf("Metal configuration valid; no outputs created, GPU initialized, or guest executed\n"
                "offline=true strict_aot=%s frames=%u time_base=%llu\n"
                "iso=%s\nsys_dir=%s\nprofile_dir=%s\ncard_dir=%s\ncache_dir=%s\n",
                launch.allow_interpreter ? "false (diagnostic interpreter allowed)" : "true",
                options.frames, static_cast<unsigned long long>(options.time_base),
                options.iso.c_str(), options.sys_dir.c_str(), options.profile_dir.c_str(),
                options.card_dir.c_str(), options.cache_dir.c_str());
    return 0;
  }
  if (!launch.script.empty() && !host::input_load_script(launch.script.c_str())) {
    std::fprintf(stderr, "cannot load input script\n");
    return 2;
  }
  if (!host::prepare_headless_outputs(launch, error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 2;
  }

  host::options = launch.runtime;
  slippi::online::config().offline = true;
  slippi::online::config().user_dir = host::options.profile_dir;
  ppc::set_interpreter_allowed(launch.allow_interpreter);
  host::log("Melee Unlocked %s: offline Cocoa/Metal/Aurora gate", MELEE_PORT_VERSION);
  host::log("capabilities: translated CPU/HLE/GX decoding and native Metal presentation active; online services, physical input and audio device unavailable");
  host::log("execution: strict_aot=%s, frames=%u, explicit isolated paths=true",
            launch.allow_interpreter ? "false (diagnostic interpreter allowed)" : "true",
            host::options.frames);
  if (!host::disc_open(host::options.iso)) {
    host::log("cannot open disc image");
    return 1;
  }

  std::unique_ptr<host::MetalFrontend> frontend;
  bool attached = false;
  bool audio_opened = false;
  int code = 0;
  try {
    host::AuroraMetalConfig metal;
    metal.user_path = host::options.profile_dir;
    metal.cache_path = host::options.cache_dir;
    metal.resources_path = host::options.sys_dir;
    frontend = host::create_aurora_metal_frontend(metal);
    host::metal_window_attach(*frontend);
    attached = true;
    host::g_has_window = true;
    gx::init(std::getenv("MELEE_DIAG_NO_GX") ? nullptr : frontend.get());   // isolation aid
    if (!host::audio_open(0, host::options.audio_dump.c_str(), false))
      throw std::runtime_error("cannot open isolated null AI DMA output");
    audio_opened = true;

    ppc::init_dispatch();
    {
      ppc::ScopedGuestFpEnvironment fp_environment(0);
      host::boot_setup();
      host::log("boot: entering __start at 8000522C");
      try {
        ppc::call(*host::cpu, host::ram, 0x8000522Cu);
        host::log("guest returned from __start before the requested frame bound");
        code = 4;
      } catch (const ExitRequested& stop) {
        code = stop.code;
      } catch (const LoadContextUnwind&) {
        host::log("unexpected OSLoadContext at top level");
        code = 3;
      }
    }
  } catch (const gx::aurora_bridge::CoverageError& failure) {
    host::log("unsupported Metal render state: %s", failure.what());
    code = 5;
  } catch (const std::exception& failure) {
    host::log("Metal frontend exception: %s", failure.what());
    code = 3;
  }

  gx::init(nullptr);
  host::g_has_window = false;
  if (attached) host::metal_window_detach(*frontend);
  if (audio_opened) host::audio_close();
  hle::dvd_shutdown();
  host::gcadapter_shutdown();
  slippi::shutdown();

  if (frontend) {
    const auto stats = frontend->stats();
    host::log("metal: attempts=%llu presented=%llu draws=%llu xfb=%llu events=%llu resizes=%llu",
              static_cast<unsigned long long>(stats.submit_attempts),
              static_cast<unsigned long long>(stats.presented_frames),
              static_cast<unsigned long long>(stats.decoded_draws),
              static_cast<unsigned long long>(stats.presented_xfb),
              static_cast<unsigned long long>(stats.native_events),
              static_cast<unsigned long long>(stats.resize_events));
    host::log("metal unsupported: render=%llu order=%llu interpolation=%llu drain=%llu lifecycle=%llu thread=%llu",
              static_cast<unsigned long long>(stats.unsupported.render_state),
              static_cast<unsigned long long>(stats.unsupported.frame_order),
              static_cast<unsigned long long>(stats.unsupported.interpolation),
              static_cast<unsigned long long>(stats.unsupported.drain_without_present),
              static_cast<unsigned long long>(stats.unsupported.after_shutdown),
              static_cast<unsigned long long>(stats.unsupported.wrong_thread));
    if (!code && !stats.has_real_output()) {
      host::log("Metal gate failed: no successful frame had both decoded draws and a presented XFB");
      code = 5;
    }
    frontend->shutdown();
  }

  if (!host::headless_audio_output_ok()) {
    host::log("isolated AI DMA output failed");
    code = 1;
  }
  if (!code && launch.expect_scene && !host::scene_trace_snapshot().saw(launch.expected_scene)) {
    host::log("required scene 0x%04X was not observed (state byte high, mode byte low)", launch.expected_scene);
    code = 5;
  }
  ppc::log_aot_diagnostics();
  uint64_t calls = 0, instructions = 0;
  ppc::interpreter_stats(&calls, &instructions);
  host::log("interpreter: %llu calls, %llu instructions",
            static_cast<unsigned long long>(calls),
            static_cast<unsigned long long>(instructions));
  host::log("Metal result: exit=%d retraces=%u requested=%u", code,
            host::retrace_count(), host::options.frames);
  return code;
}
