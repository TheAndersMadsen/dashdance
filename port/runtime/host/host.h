// Host services used by the recompiled guest and the HLE layer.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "ppc.h"
#include "scene_trace.h"
#include "fatal_boundary.h"

namespace host {

struct Options {
  std::string iso;
  std::string state_trace;        // optional per-retrace CPU/RAM/ARAM verification CSV
  std::string log_file;           // console log copy (default melee_port.log in the working directory)
  uint32_t frames = 0;           // stop after N retraces (0 = run until exit)
  bool fast = false;             // no real-time pacing
  bool trace_calls = false;      // log HLE calls
  bool quiet = false;
  uint64_t time_base = 0;        // preset timebase (0 = derive from wall clock like Dolphin)
  bool time_base_set = false;    // explicit preset, including zero, for deterministic validation
  int volume = 0;                // audio output volume percent (0 = muted, the development default)
  double hang_watch = 0.0;       // seconds without a retrace before the guest is declared hung (0 = off)
  std::string sys_dir = "port/slippi_sys";   // Slippi Sys folder (code tables, GameFiles served over the EXI device)
  std::string replay_dir = "replays";        // where .slp recordings are written
  std::string card_dir = "User/GC/CardA";    // memory card slot A as a folder of .gci files
  std::string audio_dump;        // optional WAV file receiving everything the AI DMA plays
  bool offline = false;          // explicitly disables online services in portable launchers
  std::string profile_dir;       // explicit profile location; portable CLI never discovers one
  std::string cache_dir;         // explicit isolated output/cache location for portable launchers
  bool trace_scenes = false;     // raw state-machine transitions for native acceptance evidence
};

extern Options options;
extern uint8_t* ram;             // 24 MB guest RAM
extern uint8_t* aram;            // 16 MB audio RAM (host side)
extern ppc::Context* cpu;

// ---- logging ----
void log(const char* fmt, ...);
void close_log_file();  // finalizes the explicit log before artifact identity is recorded
void log_guest_text(const char* data, size_t len);  // OSReport output
using FatalObserver = std::function<void(const std::string&)>;
void set_fatal_observer(FatalObserver observer);   // diagnostic artifact hook; never controls the exit
void notify_fatal_observer(const std::string& reason);  // invoke only after simulation unwinding/quiescence
[[noreturn]] void die(const char* fmt, ...);
const char* symbol_name(uint32_t addr);

// ---- guest memory (host side, big-endian) ----
uint32_t rd32(uint32_t addr);
uint16_t rd16(uint32_t addr);
uint8_t rd8(uint32_t addr);
void wr32(uint32_t addr, uint32_t v);
void wr16(uint32_t addr, uint16_t v);
void wr8(uint32_t addr, uint8_t v);
uint8_t* ptr(uint32_t addr, uint32_t bytes = 1); // checks the complete RAM span
std::string cstr(uint32_t addr, size_t max = 256);

// ---- disc ----
struct DiscFile { uint32_t offset, size; };
bool disc_open(const std::string& path);
bool disc_read(uint32_t offset, void* dst, uint32_t size);
uint32_t disc_fst_offset();
uint32_t disc_fst_size();
uint32_t disc_fst_max_size();
bool disc_find_file(const std::string& name, uint32_t* offset, uint32_t* size);
const std::string& disc_dol_sha1();
bool disc_dol_verified();

// ---- boot ----
void boot_setup();               // low memory, FST placement, DOL load, registers

// ---- events (interrupt delivery at guest wait points) ----
using Completion = std::function<void()>;
void post_completion(Completion fn);   // callback delivered at the next wait point
void set_pe_finish_pending();
void set_pe_token_pending(uint16_t token);
void wait_event();                     // one OSSleepThread step
void pump_completions();               // deliver queued callbacks now (from HLE entry points)
void retrace();                        // one VI retrace: time, alarms, VI interrupt
void deliver_interrupt(uint32_t number);
bool exit_requested();
void request_exit(int code);
int exit_code();
uint32_t retrace_count();
SceneTrace scene_trace_snapshot();
void close_state_trace();
bool state_trace_output_ok();

// ---- simulation-thread cost accounting (per retrace; logged when a frame exceeds 20 ms) ----
enum SimCost { SIM_DVD, SIM_AX, SIM_JUKEBOX, SIM_EXI, SIM_SNAPSHOT, SIM_QUEUE, SIM_OBSERVE, SIM_RENDER, SIM_TEXTURE, SIM_PUMP, SIM_GPUWAIT, SIM_DRAWABLE, SIM_SAVESTATE, SIM_COST_COUNT };
void sim_cost_add(int slot, double seconds);
double last_sim_frame_ms();            // work time of the most recent simulation frame (sleep excluded)
uint64_t late_frame_count();           // simulation frames that took longer than one 60 Hz period

// ---- time ----
constexpr uint64_t TB_HZ = 40500000ull;   // bus clock / 4
constexpr uint64_t TB_PER_FRAME = TB_HZ / 60;
void advance_time(uint64_t ticks);
// Retrace pacing multiplier (Slippi Online nudges it by up to 1% to keep peers in step).
void set_emulation_speed(double speed);
double emulation_speed();
// Host steady-clock seconds of the current simulation frame's retrace (the scheduled deadline when
// paced, wall time when --fast). Sub-frame presentation measures its phase from this.
double frame_time();
// Display phase lock. The renderer reports, per presented frame, how long the XFB copy took
// to reach the panel; the retrace grid then drifts (at most 0.25 ms per frame, the 60 Hz
// average is untouched) so frames are finished just before a refresh instead of at a random
// phase, which removes most of the wait-for-vblank latency. Reporting the display period
// lets the lock recognise a wrap (a frame that missed its refresh by a hair).
void present_feedback(double latency_ms, double display_period_ms);
double phase_lock_total_ms();
// Asks the kernel for real-time scheduling of the calling (simulation) thread: a 60 Hz period with
// a few milliseconds of guaranteed computation, so background work on the machine cannot push a
// frame past its deadline. Apple platforms only; a no-op elsewhere.
void simulation_thread_realtime();
// The same policy for any 60 Hz thread (the renderer): `computation_ms` is its typical work per frame.
void thread_realtime(const char* name, double computation_ms);
// Power and thermal state (Apple platforms; no-ops elsewhere): keeps the machine from throttling
// timers or sleeping the display while a game runs, and logs thermal-state changes.
void power_play_begin();
// A system notification when the app is not in front (macOS app bundles only; a no-op elsewhere).
void notify_local(const std::string& title, const std::string& body);
void power_play_end();
const char* thermal_state_name();
// A short note for the performance HUD when the device adds latency the app cannot remove (Low Power Mode, Bluetooth
// audio); empty when nothing applies.
const char* latency_warning();
// Round-trip time to the first remote player in an online match, from Slippi's pad acks; -1 when not connected.
void set_online_ping_ms(int ms);
int online_ping_ms();
// iPhone, iPad, Vision Pro: logs the audio buffer and route the system actually granted (call after opening the device).
void audio_session_report();
// What in the player's setup costs latency right now, in plain words: display refresh, full screen (Mac), Low Power Mode,
// network type, controller and its report rate, audio route, online delay and heat. Shown on the dashboards and logged
// when a match starts. Apple platforms; empty elsewhere.
struct ReadinessItem { bool ok; std::string text; };
std::vector<ReadinessItem> competitive_readiness(int display_hz, bool fullscreen, int online_delay);
double now_seconds();
struct SimCostScope { int slot; double t0; explicit SimCostScope(int s) : slot(s), t0(now_seconds()) {} ~SimCostScope() { sim_cost_add(slot, now_seconds() - t0); } };

// ---- GX FIFO sink ----
void gx_write(uint32_t value, int bytes);  // write-gather pipe data
void gx_frame_present(uint32_t xfb_addr);
void gx_stats(uint64_t* commands, uint64_t* draws, uint64_t* vertices, uint32_t* efb_copies);
extern uint64_t g_disc_reads, g_disc_bytes;
extern bool g_has_window;

// ---- MMIO (0xCC000000 range) ----
uint32_t mmio_read(uint32_t addr, int bytes);
void mmio_write(uint32_t addr, uint32_t value, int bytes);

// ---- input ----
struct PadState { uint16_t button; int8_t stick_x, stick_y, sub_x, sub_y; uint8_t trig_l, trig_r, analog_a, analog_b; int8_t err; };
void input_poll(PadState out[4]);
// GameCube controller adapter (WUP-028 over WinUSB): fills plugged ports, returns their mask.
uint32_t gcadapter_poll(PadState out[4]);
struct GcAdapterStatus { uint32_t ports = 0; int interval_ms = 0; double report_hz = 0.0; };   // ports: bit per plugged controller
bool gcadapter_status(GcAdapterStatus& out);   // false when no adapter is open
void gcadapter_rumble(int port, bool on);
void gcadapter_shutdown();

// Guest call helpers for HLE code.
void call_guest(uint32_t addr, uint32_t r3 = 0, uint32_t r4 = 0, uint32_t r5 = 0, uint32_t r6 = 0);

}  // namespace host

// Thrown by hle::OSLoadContext to unwind a guest interrupt/exception handler.
struct LoadContextUnwind { uint32_t context; };

// Unwind a normal game stop so renderer threads can drain and join.
struct ExitRequested { int code; };
