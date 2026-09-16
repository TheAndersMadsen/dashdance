// Native Melee port entry point.
// SPDX-License-Identifier: GPL-2.0-or-later
#define NOMINMAX
#include <windows.h>
#include "host.h"
#include "gecko_data.h"
#include "slippi_playback.h"
#include "render_observer.h"
#include "exi_slippi.h"
#include "slippi_online.h"
#include "slippi_net.h"
#include "audio.h"
#include "hle_dvd.h"
#include "numeric.h"
#include "functions.h"
#include "guest_symbols.h"
#include "gx_core.h"
#include "gx_d3d12.h"
#include "pc_settings.h"
#include "threaded_backend.h"
#include "window.h"
#include "updater.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

namespace ppc { void init_dispatch(); }
namespace guest {
struct NameEntry { uint32_t addr; const char* name; };
extern const NameEntry name_table[];
extern const size_t name_table_count;
}

static void usage() {
  std::printf("melee_port --iso <path> [--frames N] [--fast] [--headless] [--scale N|auto] [--window WxH] [--vsync]\n"
              "           [--fps N|monitor|unlocked] [--frame-mode extrapolate|interpolate|authored|off] [--threaded-renderer]\n"
              "           [--fullscreen] [--dlss off|dlaa|quality|balanced|performance|ultra] [--frame-times out.csv] [--volume 0-100] [--audio-dump out.wav]\n"
              "           [--capture out.ppm --capture-frame N] [--trace-calls] [--quiet] [--strict-aot | --allow-interpreter]\n");
}

// Windows hands out ~15.6 ms timer granularity by default, so every pacing sleep (the 60 Hz
// retrace, the presentation deadline, the audio device wait) overshoots by up to a frame. One
// millisecond is what games ask for, and it is what makes 60 Hz land on 60 Hz.
struct TimerResolution {
  bool raised = timeBeginPeriod(1) == TIMERR_NOERROR;
  ~TimerResolution() { if (raised) timeEndPeriod(1); }
};

#ifndef MELEE_PORT_VERSION
#define MELEE_PORT_VERSION "dev"
#endif

// Records the disc this run used, next to the launcher's own settings. However the game was
// started (a batch file, a shortcut, the launcher, a development command line), the launcher can
// then offer that disc instead of leaving Play greyed out with an empty box.
static void remember_iso(const std::string& iso) {
  char full[MAX_PATH];
  if (!GetFullPathNameA(iso.c_str(), MAX_PATH, full, nullptr)) return;
  char* local = nullptr; size_t n = 0;
  if (_dupenv_s(&local, &n, "LOCALAPPDATA") != 0 || !local) return;
  std::string dir = std::string(local) + "\\MeleeUnlocked";
  free(local);
  CreateDirectoryA(dir.c_str(), nullptr);
  FILE* f = std::fopen((dir + "\\launcher.ini").c_str(), "w");
  if (!f) return;
  std::fprintf(f, "iso=%s\n", full);
  std::fclose(f);
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--version") { std::printf("%s\n", MELEE_PORT_VERSION); return 0; }
  TimerResolution timer_resolution;
  host::Options& o = host::options;
  bool headless = false, hidden = false, threaded = false, fps_requested = false;
  bool allow_interpreter = true;  // preserve the existing Windows dynamic-module path until its AOT coverage is proven
  gx::D3D12Options gfx;
  bool automated = false, explicit_frame_mode = false;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--hidden" || arg == "--headless") automated = true;
    if (arg == "--settings-path" && i+1 < argc) gfx.settings_path = argv[++i];
    if (arg == "--frame-mode") explicit_frame_mode = true;
  }
  gfx.pc_settings = !automated;
  if (!automated) {
    gx::load_pc_settings(gfx, o.volume);
    threaded = true;
    if (!explicit_frame_mode) gfx.subframe = gx::SubFrameMode::Authored;
  }
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> const char* { if (i + 1 >= argc) { usage(); std::exit(2); } return argv[++i]; };
    if (a == "--iso") o.iso = next();
    else if (a == "--state-trace") o.state_trace = next();
    else if (a == "--frames") o.frames = (uint32_t)std::strtoul(next(), nullptr, 0);
    else if (a == "--fast") o.fast = true;
    else if (a == "--strict-aot") allow_interpreter = false;
    else if (a == "--allow-interpreter") allow_interpreter = true;
    else if (a == "--headless") headless = true;
    else if (a == "--hidden") hidden = true;
    else if (a == "--threaded-renderer") threaded = true;
    else if (a == "--fps") {   // display rate: N or "unlocked"; enables the render thread
      std::string v = next(); gfx.fps_cap = v == "unlocked" ? 0 : v == "monitor" ? -1 : std::atoi(v.c_str());
      if (v != "unlocked" && v != "monitor" && (gfx.fps_cap < 1 || v.find_first_not_of("0123456789") != std::string::npos)) { usage(); return 2; }
      threaded = true;
      fps_requested = true;
    }
    else if (a == "--frame-mode") {
      std::string v = next();
      if (v == "extrapolate") gfx.subframe = gx::SubFrameMode::Extrapolate;
      else if (v == "interpolate") gfx.subframe = gx::SubFrameMode::Interpolate;
      else if (v == "authored") gfx.subframe = gx::SubFrameMode::Authored;
      else if (v == "authored-interpolate") gfx.subframe = gx::SubFrameMode::AuthoredInterpolate;
      else if (v == "off") gfx.subframe = gx::SubFrameMode::Off;
      else { usage(); return 2; }
      threaded = true;
    }
    else if (a == "--scale") { std::string v = next(); gfx.efb_scale = v == "auto" ? 0 : std::atoi(v.c_str()); if (v != "auto" && gfx.efb_scale < 1) { usage(); return 2; } }
    else if (a == "--window") { if (std::sscanf(next(), "%dx%d", &gfx.window_w, &gfx.window_h) != 2 || gfx.window_w < 320 || gfx.window_h < 240) { usage(); return 2; } }
    else if (a == "--settings-path") gfx.settings_path = next();
    else if (a == "--pc-settings-open") { gfx.pc_settings = true; gfx.settings_open = true; }
    else if (a == "--fullscreen") gfx.fullscreen = true;
    else if (a == "--dlss") { std::string v = next(); gfx.dlss_mode = v == "off" ? 0 : v == "dlaa" ? 1 : v == "quality" ? 2 : v == "balanced" ? 3 : v == "performance" ? 4 : v == "ultra" ? 5 : -1;
      if (gfx.dlss_mode < 0) { std::fprintf(stderr, "--dlss off|dlaa|quality|balanced|performance|ultra\n"); return 2; } }
    else if (a == "--dlss-jitter-sign") gfx.dlss_jitter_sign = (float)std::atof(next());
    else if (a == "--frame-times") gfx.frame_times = next();
    else if (a == "--vsync") gfx.vsync = true;
    else if (a == "--capture") gfx.capture_path = next();
    else if (a == "--capture-frame") gfx.capture_frame = (uint32_t)std::strtoul(next(), nullptr, 0);
    else if (a == "--capture-every") gfx.capture_every = (uint32_t)std::strtoul(next(), nullptr, 0);
    else if (a == "--capture-burst") gfx.capture_burst = (uint32_t)std::strtoul(next(), nullptr, 0);
    else if (a == "--capture-sim-frame") gfx.capture_sim_frame = std::strtoull(next(), nullptr, 0);
    else if (a == "--script") { if (!host::input_load_script(next())) { std::fprintf(stderr, "cannot load input script\n"); return 1; } }
    else if (a == "--dump") gfx.dump_path = next();
    else if (a == "--shader-cache") gfx.shader_cache = next();
    else if (a == "--trace-func") { const char* spec = next(); uint32_t addr = (uint32_t)std::strtoul(spec, nullptr, 16); uint32_t limit = 40;
      if (const char* colon = std::strchr(spec, ':')) limit = (uint32_t)std::strtoul(colon + 1, nullptr, 10);
      if (!addr) { std::string name(spec, std::strchr(spec, ':') ? std::strchr(spec, ':') - spec : std::strlen(spec));
        for (size_t i = 0; i < guest::name_table_count; ++i) if (name == guest::name_table[i].name) { addr = guest::name_table[i].addr; break; } }
      if (!addr) { std::fprintf(stderr, "unknown function %s\n", spec); return 2; }
      ppc::add_trace_func(addr, limit); }
    else if (a == "--sys-dir") o.sys_dir = next();
    else if (a == "--replay-dir") o.replay_dir = next();
    else if (a == "--card-dir") o.card_dir = next();
    else if (a == "--log-file") o.log_file = next();
    else if (a == "--replay") slippi::playback::set_replay(next());   // playback build: play this .slp
    else if (a == "--user-dir") slippi::online::config().user_dir = next();
    else if (a == "--online-delay") slippi::online::config().delay = std::atoi(next());
    else if (a == "--chat") { std::string v = next(); slippi::online::config().chat = v == "off" ? 2 : v == "direct" ? 1 : 0; }
    else if (a == "--netplay-port") slippi::Matchmaking::forced_port = (uint16_t)std::atoi(next());
    else if (a == "--local-peer") {
      // idx:local_port:remote_ip:remote_port; two instances peer directly without the matchmaking server.
      std::string v = next(); auto& lp = slippi::Matchmaking::local_peer;
      size_t a1 = v.find(':'), a2 = v.find(':', a1 + 1), a3 = v.find(':', a2 + 1);
      if (a1 == std::string::npos || a2 == std::string::npos || a3 == std::string::npos) { std::fprintf(stderr, "--local-peer idx:port:ip:port"); return 2; }
      lp.enabled = true; lp.local_index = std::atoi(v.substr(0, a1).c_str()); lp.local_port = (uint16_t)std::atoi(v.substr(a1 + 1, a2 - a1 - 1).c_str());
      lp.remote_ip = v.substr(a2 + 1, a3 - a2 - 1); lp.remote_port = (uint16_t)std::atoi(v.substr(a3 + 1).c_str()); }
    else if (a == "--dump-frame") gfx.dump_frame = (uint32_t)std::strtoul(next(), nullptr, 0);
    else if (a == "--trace-calls") o.trace_calls = true;
    else if (a == "--quiet") o.quiet = true;
    else if (a == "--time-base") { o.time_base = std::strtoull(next(), nullptr, 0); o.time_base_set = true; }
    else if (a == "--volume") o.volume = std::atoi(next());
    else if (a == "--widescreen") gfx.widescreen = true;
    else if (a == "--sharpness") gfx.sharpness = std::clamp((float)std::atof(next()), 0.0f, 1.0f);
    else if (a == "--ssaa") gfx.ssaa = std::atoi(next()) >= 2 ? 2 : 1;
    else if (a == "--anisotropy") gfx.anisotropy = std::clamp(std::atoi(next()), 1, 16);
    else if (a == "--hang-watch") o.hang_watch = std::atof(next());
    else if (a == "--audio-dump") o.audio_dump = next();
    else { usage(); return 2; }
  }
  gecko::option_widescreen = gfx.widescreen;   // before the game loads the code table
  if (fps_requested && gfx.subframe == gx::SubFrameMode::Off) {
    std::fprintf(stderr, "--fps requires explicit experimental --frame-mode interpolate, extrapolate or authored\n");
    return 2;
  }
  if (o.iso.empty()) { usage(); return 2; }
  if (!host::disc_open(o.iso)) { std::fprintf(stderr, "cannot open ISO %s\n", o.iso.c_str()); return 1; }
  remember_iso(o.iso);   // so the launcher can offer this disc without being told again

  std::unique_ptr<gx::Backend> backend;
  if (!headless && threaded) {
    backend = gx::create_threaded_backend(gfx, !hidden);
  } else if (!headless) {
    void* hwnd = host::window_create(gfx.window_w, gfx.window_h, L"Melee Unlocked (development)", !hidden);
    if (gfx.fullscreen) host::window_set_fullscreen(true);
    backend.reset(gx::create_d3d12_backend(hwnd, gfx.window_w, gfx.window_h, gfx));
    host::window_set_resize_callback([renderer = backend.get()](int w, int h) { gx::d3d12_resize(renderer, w, h); });
    host::g_has_window = true;
  }
  gx::set_authored_capture(gfx.subframe == gx::SubFrameMode::Authored || gfx.subframe == gx::SubFrameMode::AuthoredInterpolate);
  gx::init(backend.get());
  host::audio_open(o.volume, o.audio_dump.c_str(), !headless);

  ppc::init_dispatch();
  ppc::set_interpreter_allowed(allow_interpreter);
  host::log("aot: %s", allow_interpreter ? "legacy Windows diagnostic interpreter enabled; use --strict-aot to reject missing translations" : "strict, missing translations stop execution");
  ppc::ScopedGuestFpEnvironment fp_environment(0);
  host::boot_setup();
  host::log("boot: entering __start at %08X", 0x8000522Cu);
  int code = 0;
  try {
    ppc::call(*host::cpu, host::ram, 0x8000522Cu);
    host::log("guest returned from __start after %u retraces", host::retrace_count());
  } catch (const ExitRequested& stop) {
    backend.reset();
    code = stop.code;
  } catch (const LoadContextUnwind&) {
    host::log("OSLoadContext reached top level");
  }
  { uint64_t silent_ms = 0, underruns = host::audio_underruns(&silent_ms);
    double rate_low = 1.0, rate_high = 1.0; host::audio_rate_range(&rate_low, &rate_high);
    host::log("audio: %llu frames played, %llu blocks dropped, %llu gaps (%llu ms held), clock tracking %+.3f%% to %+.3f%%",
              (unsigned long long)host::audio_pushed_frames(), (unsigned long long)host::audio_dropped_blocks(),
              (unsigned long long)underruns, (unsigned long long)silent_ms, (rate_low - 1.0) * 100.0, (rate_high - 1.0) * 100.0); }
  host::audio_close();
  hle::dvd_shutdown();
  host::updater::shutdown();   // the settings panel may have started an update check; join it before exit
  host::gcadapter_shutdown();
  slippi::shutdown();
  { uint64_t calls = 0, insns = 0; ppc::interpreter_stats(&calls, &insns);
    if (calls) host::log("interpreter: %llu calls into RAM-resident code, %llu instructions", (unsigned long long)calls, (unsigned long long)insns); }
  host::log("slippi: %llu EXI commands, %llu replays written, GCT at %08X", (unsigned long long)slippi::commands_seen(),
            (unsigned long long)slippi::replays_written(), slippi::gct_load_address());
  return code;
}
