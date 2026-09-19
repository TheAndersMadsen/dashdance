// Host services: memory, disc, boot, event delivery, time, MMIO, logging.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "host.h"
#include "memory_range.h"
#include "platform_file.h"
#include "sha1.h"
#include "functions.h"
#include "guest_symbols.h"
#include "gx_core.h"
#include "window.h"
#include "ax_ucode.h"
#include "hle_dvd.h"
#include "exi_slippi.h"
#include "gecko_data.h"
#include <chrono>
#include <mutex>
#if defined(__APPLE__)
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#endif
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <deque>
#include <thread>

namespace guest {
struct NameEntry { uint32_t addr; const char* name; };
extern const NameEntry name_table[];
extern const size_t name_table_count;
}
namespace hle { void audio_tick(bool force); }

namespace host {

Options options;
uint8_t* ram = nullptr;
uint8_t* aram = nullptr;
ppc::Context* cpu = nullptr;

static FILE* g_disc = nullptr;
static FILE* g_state_trace = nullptr;
static bool g_state_trace_ok = true;
void close_state_trace() {
  if (!g_state_trace) return;
  if (std::fflush(g_state_trace) != 0 || std::ferror(g_state_trace)) g_state_trace_ok = false;
  if (std::fclose(g_state_trace) != 0) g_state_trace_ok = false;
  g_state_trace = nullptr;
}
bool state_trace_output_ok() { return g_state_trace_ok; }
static std::string g_dol_sha1;
static bool g_dol_verified = false;
static uint32_t g_fst_offset, g_fst_size, g_fst_max;
static std::deque<Completion> g_completions;
static bool g_pe_finish_pending = false;
static bool g_pe_token_pending = false;
static uint16_t g_pe_token = 0;
static uint32_t g_retraces = 0;
static std::atomic<bool> g_exit{false};
static std::atomic<int> g_exit_code{0};
static std::chrono::steady_clock::time_point g_next_frame;
static uint8_t g_mmio[0x10000];      // 0xCC000000 - 0xCC00FFFF register file (big-endian bytes)
static bool g_in_interrupt = false;
static SceneTrace g_scene_trace;
static std::mutex g_scene_mutex;
static void observe_scene() {
  if (!options.trace_scenes) return;
  uint8_t mode = rd8(SCENE_MODE_ADDRESS), state = rd8(SCENE_STATE_ADDRESS);
  bool changed;
  { std::lock_guard<std::mutex> lock(g_scene_mutex); changed = g_scene_trace.observe(g_retraces, mode, state); }
  if (changed) log("accept: scene retrace=%u mode_byte=0x%02X state_byte=0x%02X combined=0x%04X",
                   g_retraces, mode, state, (unsigned(state) << 8) | mode);
}
SceneTrace scene_trace_snapshot() { std::lock_guard<std::mutex> lock(g_scene_mutex); return g_scene_trace; }

const char* symbol_name(uint32_t addr) {
  // Binary search the sorted function name table for the containing function.
  size_t lo = 0, hi = guest::name_table_count;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (guest::name_table[mid].addr <= addr) lo = mid + 1; else hi = mid;
  }
  if (lo == 0) return "?";
  return guest::name_table[lo - 1].name;
}

// ---------------- memory ----------------
uint8_t* ptr(uint32_t addr, uint32_t bytes) {
  uint32_t off = addr & 0x3FFFFFFFu;
  if (!valid_range(off, bytes, ppc::RAM_SIZE)) die("host access outside RAM: %08X+%X", addr, bytes);
  return ram + off;
}
uint32_t rd32(uint32_t a) { const uint8_t* p = ptr(a, 4); return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint16_t rd16(uint32_t a) { const uint8_t* p = ptr(a, 2); return uint16_t((uint16_t(p[0]) << 8) | p[1]); }
uint8_t rd8(uint32_t a) { return *ptr(a); }
void wr32(uint32_t a, uint32_t v) { uint8_t* p = ptr(a, 4); p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
void wr16(uint32_t a, uint16_t v) { uint8_t* p = ptr(a, 2); p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
void wr8(uint32_t a, uint8_t v) { *ptr(a) = v; }
std::string cstr(uint32_t addr, size_t max) {
  std::string s;
  for (size_t i = 0; i < max; ++i) { char ch = (char)rd8(addr + (uint32_t)i); if (!ch) break; s += ch; }
  return s;
}

// ---------------- disc ----------------
// CISO containers (compact GameCube images) are read in place: a one-byte map per logical
// 2 MB-class block says whether the block is stored (data blocks are packed after the
// 0x8000 header) or zero-filled. Both known header layouts are accepted: block size at
// offset 4 with the map at 8 (GCRebuilder), or version 1 at 4 with the block size at 8
// and the map at 0xC (Dolphin). Raw ISO/GCM needs no map and reads straight through.
static struct {
  bool active = false;
  uint32_t block_size = 0;
  uint64_t data_offset = 0x8000;
  std::vector<int64_t> phys;   // logical block -> physical block index in the file, or -1 = zero fill
} g_ciso;

static bool ciso_open() {
  uint8_t header[0x8000];
  if (!seek_file(g_disc, 0) || std::fread(header, 1, sizeof header, g_disc) != sizeof header) return false;
  if (std::memcmp(header, "CISO", 4) != 0) return false;
  auto le32 = [&](size_t o) { return (uint32_t)header[o] | ((uint32_t)header[o + 1] << 8) | ((uint32_t)header[o + 2] << 16) | ((uint32_t)header[o + 3] << 24); };
  const uint32_t a = le32(4), b = le32(8);
  uint32_t block_size = 0;
  const uint8_t* map = nullptr;
  size_t map_bytes = 0;
  if (a >= 0x200 && (a & (a - 1)) == 0) { block_size = a; map = header + 8; map_bytes = sizeof header - 8; }
  else if (a == 1 && b >= 0x200 && (b & (b - 1)) == 0) { block_size = b; map = header + 0xC; map_bytes = sizeof header - 0xC; }
  else { log("warning: unrecognized CISO header layout; treating the image as raw"); return false; }
  uint32_t blocks = 0;
  for (size_t i = 0; i < map_bytes && map[i] <= 1; ++i)
    if (map[i] == 1) blocks = (uint32_t)i + 1;
  if (!blocks) { log("warning: empty CISO map; treating the image as raw"); return false; }
  g_ciso.active = true;
  g_ciso.block_size = block_size;
  g_ciso.phys.resize(blocks);
  int64_t next = 0;
  for (uint32_t i = 0; i < blocks; ++i) g_ciso.phys[i] = map[i] == 1 ? next++ : -1;
  log("disc: CISO image, %u x %u KB blocks (%u stored), data at %llu", blocks, block_size >> 10, (unsigned)next, (unsigned long long)g_ciso.data_offset);
  return true;
}

bool disc_open(const std::string& path) {
  g_disc = std::fopen(path.c_str(), "rb");
  if (!g_disc) return false;
  std::setvbuf(g_disc, nullptr, _IOFBF, 1u << 20);   // disc reads are large and sequential: a 1 MB stdio buffer, plus kernel read-ahead
#if defined(__APPLE__)
  fcntl(fileno(g_disc), F_RDAHEAD, 1);
#endif
  ciso_open();
  uint8_t hdr[0x440];
  if (!disc_read(0, hdr, sizeof hdr)) return false;
  auto be = [&](int o) { return ((uint32_t)hdr[o] << 24) | ((uint32_t)hdr[o + 1] << 16) | ((uint32_t)hdr[o + 2] << 8) | hdr[o + 3]; };
  g_fst_offset = be(0x424);
  g_fst_size = be(0x428);
  g_fst_max = be(0x42C);
  if (std::memcmp(hdr, "GALE01", 6) != 0) log("warning: disc id is not GALE01");
  return true;
}
uint64_t g_disc_reads = 0, g_disc_bytes = 0;
static std::mutex g_disc_mutex;   // the DVD worker and the simulation thread share the file
bool disc_read(uint32_t offset, void* dst, uint32_t size) {
  std::lock_guard<std::mutex> lk(g_disc_mutex);
  if (!g_disc) return false;
  ++g_disc_reads;
  g_disc_bytes += size;
  if (!g_ciso.active) {
    if (!seek_file(g_disc, offset)) return false;
    return std::fread(dst, 1, size, g_disc) == size;
  }
  const uint64_t data_offset = g_ciso.data_offset;
  const uint64_t block = g_ciso.block_size;
  auto* out = (uint8_t*)dst;
  uint64_t pos = offset, remaining = size;
  while (remaining) {
    const uint32_t index = (uint32_t)(pos / block);
    const size_t within = (size_t)(pos % block);
    const size_t chunk = std::min<uint64_t>(remaining, block - within);
    const int64_t phys = index < g_ciso.phys.size() ? g_ciso.phys[index] : -1;
    if (phys < 0) {
      std::memset(out, 0, chunk);
    } else if (!seek_file(g_disc, data_offset + (uint64_t)phys * block + within) ||
               std::fread(out, 1, chunk, g_disc) != chunk) {
      return false;
    }
    out += chunk;
    pos += chunk;
    remaining -= chunk;
  }
  return true;
}
uint32_t disc_fst_offset() { return g_fst_offset; }
uint32_t disc_fst_size() { return g_fst_size; }

// Looks a file up by name in the disc's FST (root and nested directories; exact match first,
// then case-insensitive). Used to serve ISO files to host-side loaders (Slippi game files).
bool disc_find_file(const std::string& name, uint32_t* offset, uint32_t* size) {
  static std::vector<uint8_t> fst;
  if (fst.empty()) {
    if (!g_fst_size) return false;
    fst.resize(g_fst_size);
    if (!disc_read(g_fst_offset, fst.data(), g_fst_size)) { fst.clear(); return false; }
  }
  auto be32 = [&](size_t o) { return o + 4 <= fst.size() ? ((uint32_t)fst[o] << 24) | ((uint32_t)fst[o + 1] << 16) | ((uint32_t)fst[o + 2] << 8) | fst[o + 3] : 0u; };
  uint32_t entries = be32(8);
  size_t strings = (size_t)entries * 12;
  if (strings > fst.size()) return false;
  for (int pass = 0; pass < 2; ++pass) {
    for (uint32_t i = 1; i < entries; ++i) {
      uint32_t a = be32(i * 12);
      if (a >> 24) continue;   // directory
      size_t so = strings + (a & 0xFFFFFF);
      if (so >= fst.size()) continue;
      const char* n = (const char*)&fst[so];
      size_t maxlen = fst.size() - so;
      if (fst_name_equal(n, maxlen, name, pass != 0)) {
        if (offset) *offset = be32(i * 12 + 4);
        if (size) *size = be32(i * 12 + 8);
        return true;
      }
    }
  }
  return false;
}
uint32_t disc_fst_max_size() { return g_fst_max; }
const std::string& disc_dol_sha1() { return g_dol_sha1; }
bool disc_dol_verified() { return g_dol_verified; }

// ---------------- boot ----------------
static void load_dol_from_disc() {
  uint8_t hdr[0x20];
  if (!disc_read(0x420, hdr, 4)) die("cannot read disc DOL offset");
  uint32_t dol_offset = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) | ((uint32_t)hdr[2] << 8) | hdr[3];
  constexpr uint32_t dol_size = 0x4385E0u;
  std::vector<uint8_t> image(dol_size);
  if (!disc_read(dol_offset, image.data(), dol_size)) die("cannot read full Melee DOL");
  const auto digest = sha1(image.data(), image.size());
  g_dol_sha1.clear();
  for (uint8_t byte : digest) { g_dol_sha1 += "0123456789abcdef"[byte >> 4]; g_dol_sha1 += "0123456789abcdef"[byte & 15]; }
  const uint8_t expected[20] = {0x08,0xe0,0xbf,0x20,0x13,0x4d,0xfc,0xb2,0x60,0x69,0x96,0x71,0x00,0x45,0x27,0xb2,0xd6,0xbb,0x1a,0x45};
  if (std::memcmp(digest.data(), expected, digest.size()))
    die("ISO DOL does not match vanilla Melee NTSC 1.02; recompiled code cannot run this image");
  g_dol_verified = true;
  uint8_t dh[0x100];
  if (!disc_read(dol_offset, dh, sizeof dh)) die("cannot read DOL header");
  auto be = [&](int o) { return ((uint32_t)dh[o] << 24) | ((uint32_t)dh[o + 1] << 16) | ((uint32_t)dh[o + 2] << 8) | dh[o + 3]; };
  for (int i = 0; i < 18; ++i) {
    uint32_t off = be(i * 4), addr = be(0x48 + i * 4), size = be(0x90 + i * 4);
    if (!size) continue;
    if (!disc_read(dol_offset + off, ptr(addr, size), size)) die("cannot read DOL section %d", i);
  }
  // The DOL header's bss range overlaps the loaded .sdata section; the guest's own
  // __init_data zeroes .bss/.sbss precisely and RAM starts zeroed, so do not memset here.
  log("boot: DOL loaded from disc offset %08X, bss %08X+%X, entry %08X", dol_offset, be(0xD8), be(0xDC), be(0xE0));
}

// Reproduces Slippi Dolphin's boot-time Gecko installation in guest RAM: codehandler.bin at
// 0x80001800, bootloader.gct at 0x800028B8, then the effect of running the handler once (its
// 32-bit writes and the C2 hook branches into the caves inside the table). The recompiled code
// already contains these patches; this keeps RAM identical to what the game expects to read.
static void install_gecko_boot() {
  if (!gecko::codehandler_bin_size) { log("boot: translated without Slippi code tables"); return; }
  std::memcpy(ptr(0x80001800u, (uint32_t)gecko::codehandler_bin_size), gecko::codehandler_bin, gecko::codehandler_bin_size);
  wr32(0x80001D6Cu, 0x4E800020u);   // USB Gecko I/O replaced by blr, as Slippi does
  wr32(0x80001800u, 0xD01F1BADu);   // handler magic
  std::memcpy(ptr(0x800028B8u, (uint32_t)gecko::bootloader_gct_size), gecko::bootloader_gct, gecko::bootloader_gct_size);
  wr8(0x80001807u, 1);              // codes on
  for (size_t i = 0; i < gecko::boot_writes_count; ++i) {
    const gecko::Write& w = gecko::boot_writes[i];
    std::memcpy(ptr(w.addr, w.size), w.data, w.size);
  }
  for (size_t i = 0; i < gecko::boot_hooks_count; ++i) {
    const gecko::HookInstall& h = gecko::boot_hooks[i];
    wr32(h.hook, 0x48000000u | ((h.cave_addr - h.hook) & 0x03FFFFFCu));
    uint32_t last = h.cave_addr + (h.words - 1) * 4;
    wr32(last, 0x48000000u | (((h.hook + 4) - last) & 0x03FFFFFCu));
  }
  slippi::init();
  log("boot: Slippi code tables installed (%zu boot writes, %zu boot hooks, main GCT %zu bytes served over EXI)",
      gecko::boot_writes_count, gecko::boot_hooks_count, gecko::slippi_gct_size);
}

void boot_setup() {
  if (!options.state_trace.empty()) {
    g_state_trace = std::fopen(options.state_trace.c_str(), "w");
    if (!g_state_trace) die("cannot open state trace");
    std::fprintf(g_state_trace, "retrace,cpu,ram,aram,events\n");
  }
  ram = (uint8_t*)std::calloc(ppc::RAM_SIZE + 64, 1);
  aram = (uint8_t*)std::calloc(0x01000000, 1);
  std::memset(ram, 0, ppc::RAM_SIZE + 64); std::memset(aram, 0, 0x01000000);   // prefault every page now, not on first touch mid-match
  ax::set_memory({rd16, rd32, wr16, wr32, aram, 0x01000000});
  ax::reset();
  cpu = new ppc::Context();
  std::memset(cpu, 0, sizeof *cpu);
  if (!ram || !aram) die("out of memory");
  std::memset(g_mmio, 0, sizeof g_mmio);

  load_dol_from_disc();

  // Low memory, mirroring Dolphin's Boot_BS2Emu.cpp (GC path) plus what the apploader leaves.
  disc_read(0, ptr(0x80000000, 0x20), 0x20);              // disc id
  wr32(0x80000020, 0x0D15EA5E);                      // booted from bootrom
  wr32(0x80000028, ppc::RAM_SIZE);                   // physical memory size
  wr32(0x8000002C, 0x10000006);                      // console type (Dolphin reports devkit)
  wr32(0x80000030, 0);                               // arena lo (0 = use linker default)
  wr32(0x800000CC, 0);                               // NTSC
  wr32(0x800000D0, 0x01000000);                      // ARAM size
  wr32(0x800000F0, ppc::RAM_SIZE);                   // simulated memory size
  wr32(0x800000F8, 0x09A7EC80);                      // bus clock
  wr32(0x800000FC, 0x1CF7C580);                      // cpu clock
  wr32(0x80000300, 0x4C000064);                      // rfi stubs
  wr32(0x80000800, 0x4C000064);
  wr32(0x80000C00, 0x4C000064);
  uint64_t tb = options.time_base;
  if (!tb && !options.time_base_set) {
    // Dolphin presets the timebase from the RTC (seconds since GC epoch 2000-01-01) * 40.5 MHz.
    auto now = std::chrono::system_clock::now().time_since_epoch();
    uint64_t secs = (uint64_t)std::chrono::duration_cast<std::chrono::seconds>(now).count();
    const uint64_t GC_EPOCH = 946684800ull;
    tb = (secs - GC_EPOCH) * TB_HZ;
  }
  // Like Dolphin: the timebase register starts near zero; 0x800030D8 holds the epoch adjust
  // that __OSGetSystemTime adds to mftb.
  wr32(0x800030D8, (uint32_t)(tb >> 32));
  wr32(0x800030DC, (uint32_t)tb);
  cpu->tb = 0;

  // Apploader: FST at the top of RAM, arena hi below it.
  if (!valid_range(0, g_fst_max, ppc::RAM_SIZE) || g_fst_size > g_fst_max) die("invalid FST size");
  uint32_t fst_addr = (0x81800000u - g_fst_max) & ~31u;
  if (!disc_read(g_fst_offset, ptr(fst_addr, g_fst_size), g_fst_size)) die("cannot read FST");
  wr32(0x80000038, fst_addr);
  wr32(0x8000003C, g_fst_max);
  wr32(0x80000034, fst_addr);                        // arena hi
  log("boot: FST %u bytes at %08X (max %X), arena hi %08X", g_fst_size, fst_addr, g_fst_max, fst_addr);
  install_gecko_boot();

  cpu->msr = 0x00002030u | 0x8000u;                  // FP | DR | IR | EE
  cpu->fpscr = 0;
  ppc::update_mxcsr(*cpu);
  g_next_frame = std::chrono::steady_clock::now();
  observe_scene();
}

// ---------------- guest calls from host ----------------
void call_guest(uint32_t addr, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6) {
  ppc::Context& c = *cpu;
  uint32_t saved_lr = c.lr;
  c.r[3] = r3; c.r[4] = r4; c.r[5] = r5; c.r[6] = r6;
  c.lr = 0;
  ppc::call(c, ram, addr);
  c.lr = saved_lr;
}

// ---------------- events ----------------
void post_completion(Completion fn) { g_completions.push_back(std::move(fn)); }
void set_pe_finish_pending() { g_pe_finish_pending = true; }
void set_pe_token_pending(uint16_t token) { g_pe_token = token; g_pe_token_pending = true; }
bool exit_requested() { return g_exit; }
void request_exit(int code) { g_exit_code.store(code); g_exit.store(true); }
int exit_code() { return g_exit_code.load(); }
uint32_t retrace_count() { return g_retraces; }
// The VI retrace is periodic in virtual time, like the hardware interrupt: `g_next_retrace_tb`
// is the timebase value of the next retrace. A sleeping guest (wait_event) jumps time straight
// to that boundary; a guest that busy-waits with interrupts enabled advances time in small steps
// at HLE entry points and loop polls and takes the retrace when it crosses the boundary (Slippi's
// lag-reduction code waits for pad data that the retrace path produces).
static uint64_t g_next_retrace_tb = TB_PER_FRAME;
static bool g_in_retrace = false;
void advance_time(uint64_t ticks) { cpu->tb += ticks; }
static void advance_frame() {
  if (cpu->tb < g_next_retrace_tb) cpu->tb = g_next_retrace_tb;   // idle: jump to the boundary
  g_next_retrace_tb += TB_PER_FRAME;
}

void deliver_interrupt(uint32_t number) {
  // __OSInterruptHandlerTable lives at 0x80003040 (OS_INTERRUPTTABLE_ADDR).
  uint32_t handler = rd32(0x80003040u + number * 4);
  if (!handler) return;
  uint32_t context = rd32(0x800000D4u);  // OS current context (virtual address)
  ppc::Context& c = *cpu;
  ppc::Context saved = c;                // handlers clobber registers; restore like an rfi would
  bool was = g_in_interrupt;
  g_in_interrupt = true;
  try {
    call_guest(handler, number, context);
  } catch (const LoadContextUnwind&) {
  }
  g_in_interrupt = was;
  uint64_t tb = c.tb;
  c = saved;
  c.tb = tb;
  ppc::update_mxcsr(c);
}

static void fire_due_alarms(bool force);
static bool deliver_completions(bool force);

static void validate_alarm_queue(const char* where);
void pump_completions() {
  std::string background_error;
  if (take_background_failure(background_error)) die("background task failed: %s", background_error.c_str());
  // Called from HLE entry points the guest polls. Virtual time flows a little so periodic
  // alarms (pad sampling) fire even in loops that never sleep. Nothing is delivered while the
  // guest has interrupts disabled; ppc::mtmsr flushes when they come back on.
  advance_time(2048);
  hle::dvd_poll();
  validate_alarm_queue("hle entry");
  if (!ppc::interrupts_on(*cpu)) return;
  fire_due_alarms(false);
  hle::audio_tick(false);
  deliver_completions(false);
  if (cpu->tb >= g_next_retrace_tb && !g_in_retrace) retrace();   // periodic VI interrupt during busy waits
}

// Diagnostic: the OSAlarm queue must only ever link alarms whose handlers are code. A corrupt
// link is reported at the first HLE entry after it appears so the call trace points at the writer.
static void validate_alarm_queue(const char* where) {
  static bool reported = false;
  if (reported) return;
  uint32_t a = rd32(gs::AlarmQueue), prev = 0;
  for (int guard = 0; a && guard < 64; ++guard) {
    bool bad = a < 0x80003000u || a >= 0x81800000u;
    uint32_t handler = bad ? 0 : rd32(a);
    // Handlers live in the DOL's text or in the Slippi code table caves; nothing else is code.
    bool code = (handler >= 0x80003100u && handler < 0x803B7240u) || (handler >= 0x8065C000u && handler < 0x8071B000u);
    if (!bad) bad = !code || (handler & 3) || rd32(a + 16) != prev;
    if (bad) {
      reported = true;
      log("ALARM QUEUE CORRUPT (%s): entry %08X handler %08X prev %08X (expected %08X) next %08X head %08X tail %08X retrace %u",
          where, a, handler, bad && a >= 0x80003000u && a < 0x81800000u ? rd32(a + 16) : 0, prev, a >= 0x80003000u && a < 0x81800000u ? rd32(a + 20) : 0,
          rd32(gs::AlarmQueue), rd32(gs::AlarmQueue + 4), g_retraces);
      ppc::fatal(*cpu, "alarm queue corrupt", a);
      return;
    }
    prev = a; a = rd32(a + 20);
  }
}

static void fire_due_alarms(bool force) {
  // OSAlarm queue head lives in the SDK's static AlarmQueue; fire through the installed
  // decrementer exception handler (OSExceptionTable[8] at 0x80003000 + 8*4) so the guest's
  // own callback logic runs. The handler expects an exception frame; the asm wrapper just
  // saves GPRs into the context and tail-calls DecrementerExceptionCallback, which processes
  // one alarm and re-arms periodic ones, so loop while the head is due.
  static bool firing = false;
  if (firing) return;
  if (!force && !ppc::interrupts_on(*cpu)) return;
  firing = true;
  for (int guard = 0; guard < 16; ++guard) {
    validate_alarm_queue(guard ? "after previous alarm handler" : "entry");
    uint32_t head = rd32(gs::AlarmQueue);
    if (!head) break;
    uint64_t fire = ((uint64_t)rd32(head + 8) << 32) | rd32(head + 12);
    // Alarm times are OS system time: timebase + the adjust at 0x800030D8.
    uint64_t adjust = ((uint64_t)rd32(0x800030D8u) << 32) | rd32(0x800030DCu);
    if ((int64_t)fire > (int64_t)(cpu->tb + adjust)) break;
    uint32_t handler = rd32(0x80003000u + 8 * 4);
    if (!handler) break;
    uint32_t context = rd32(0x800000D4u);
    static int reported = 0;
    if (options.trace_calls && reported++ < 40)
      log("[alarm] head=%08X fire=%llu tb=%llu handler=%08X cb=%08X period=%llu", head, fire, cpu->tb,
          handler, rd32(head), ((uint64_t)rd32(head + 24) << 32) | rd32(head + 28));
    ppc::Context& c = *cpu;
    ppc::Context saved = c;
    try {
      call_guest(handler, 8, context);
    } catch (const LoadContextUnwind&) {
    }
    uint64_t tb = c.tb;
    c = saved;
    c.tb = tb;
    ppc::update_mxcsr(c);
    static char where[64];
    std::snprintf(where, sizeof where, "after alarm %08X handler %08X", head, rd32(head));
    validate_alarm_queue(where);
  }
  firing = false;
}

bool g_has_window = false;

// Field-wise CPU hash excludes C++ padding and diagnostic counters/trace history.
static void trace_state() {
  if (!g_state_trace) return;
  uint64_t h = 0;
  auto add = [&](const auto& v) { h = (h ^ gx::hash_bytes(&v, sizeof v)) * 0x100000001b3ull; };
  const auto& c = *cpu;
  add(c.r); add(c.f); add(c.cr); add(c.lr); add(c.ctr);
  add(c.ca); add(c.so); add(c.ov); add(c.fpscr); add(c.gqr);
  add(c.msr); add(c.hid0); add(c.hid2); add(c.dec); add(c.tb); add(c.spr);
  uint64_t events = (uint64_t)g_completions.size() << 32 |
      (uint64_t)g_pe_token << 8 | (g_pe_finish_pending ? 1 : 0) | (g_pe_token_pending ? 2 : 0);
  std::fprintf(g_state_trace, "%u,%016llX,%016llX,%016llX,%016llX\n", g_retraces,
      h, gx::hash_bytes(ram, ppc::RAM_SIZE), gx::hash_bytes(aram, 0x01000000), events);
  std::fflush(g_state_trace);
}

static double g_frame_time = 0.0;
static double g_emulation_speed = 1.0;
void set_emulation_speed(double speed) { g_emulation_speed = speed < 0.5 ? 0.5 : speed > 2.0 ? 2.0 : speed; }
double emulation_speed() { return g_emulation_speed; }
double now_seconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
double frame_time() { return g_frame_time; }

// Simulation-thread cost accounting: HLE entry points add their time to a slot; at the next
// retrace the frame's work time (sleep excluded) is logged when it exceeds 20 ms, with the
// slots that explain it, so a hitch is attributed instead of guessed.
static double g_sim_costs[SIM_COST_COUNT];
static double g_sim_costs_window[SIM_COST_COUNT];   // accumulated over the 60-frame log interval
static double g_sim_ms_window = 0, g_sim_ms_worst = 0;
static const char* const g_sim_cost_names[SIM_COST_COUNT] = {"disc", "ax", "jukebox", "exi", "texsnap", "queue", "observe", "render", "texture", "pump", "gpuwait", "drawable", "savestate"};
static double g_sim_frame_start = 0.0, g_last_sim_ms = 0.0;
static std::atomic<std::thread::id> g_sim_thread;    // set by the first retrace; other threads (the renderer) report separately
static double g_render_costs_window[SIM_COST_COUNT];
static std::mutex g_render_costs_mutex;
void sim_cost_add(int slot, double seconds) {
  if (slot < 0 || slot >= SIM_COST_COUNT) return;
  const std::thread::id sim = g_sim_thread.load(std::memory_order_relaxed);
  if (sim == std::thread::id() || std::this_thread::get_id() == sim) { g_sim_costs[slot] += seconds; g_sim_costs_window[slot] += seconds; return; }
  std::lock_guard<std::mutex> lock(g_render_costs_mutex);
  g_render_costs_window[slot] += seconds;
}
// "sim: 3.1 ms/frame (worst 12.4) | observe 0.9 texsnap 0.4" for the periodic frame log.
static std::string sim_cost_line(uint32_t frames) {
  char buf[512];
  size_t n = (size_t)std::snprintf(buf, sizeof buf, "sim: %.1f ms/frame (worst %.1f)", g_sim_ms_window / std::max(1u, frames), g_sim_ms_worst);
  bool first = true;
  for (int i = 0; i < SIM_COST_COUNT; ++i) {
    double ms = g_sim_costs_window[i] * 1000.0 / std::max(1u, frames);
    if (ms < 0.05) continue;
    n += (size_t)std::snprintf(buf + n, sizeof buf - n, "%s %s %.2f", first ? " |" : "", g_sim_cost_names[i], ms);
    first = false;
  }
  std::memset(g_sim_costs_window, 0, sizeof g_sim_costs_window);
  g_sim_ms_window = 0; g_sim_ms_worst = 0;
  {
    std::lock_guard<std::mutex> lock(g_render_costs_mutex);
    first = true;
    for (int i = 0; i < SIM_COST_COUNT; ++i) {
      double ms = g_render_costs_window[i] * 1000.0 / std::max(1u, frames);
      if (ms < 0.05) continue;
      n += (size_t)std::snprintf(buf + n, sizeof buf - n, "%s %s %.2f", first ? " | render thread:" : "", g_sim_cost_names[i], ms);
      first = false;
    }
    std::memset(g_render_costs_window, 0, sizeof g_render_costs_window);
  }
  return buf;
}
static uint64_t g_late_frames = 0;
uint64_t late_frame_count() { return g_late_frames; }
double last_sim_frame_ms() { return g_last_sim_ms; }

void thread_realtime(const char* name, double computation_ms) {
#if defined(__APPLE__)
  if (const char* e = std::getenv("MELEE_REALTIME")) if (*e == '0') return;
  mach_timebase_info_data_t tb; mach_timebase_info(&tb);
  const double ns_per_tick = (double)tb.numer / (double)tb.denom;
  auto ticks = [&](double ms) { return (uint32_t)(ms * 1e6 / ns_per_tick); };
  thread_time_constraint_policy_data_t policy;
  policy.period = ticks(16.667);                          // one simulation frame
  policy.computation = ticks(computation_ms);             // typical work per frame
  policy.constraint = ticks(12.0);                        // must be done well inside the period
  policy.preemptible = TRUE;
  kern_return_t kr = thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&policy, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
  log("%s thread: real-time scheduling %s (period 16.7 ms, computation %.0f ms, constraint 12 ms)", name, kr == KERN_SUCCESS ? "on" : "unavailable", computation_ms);
#else
  (void)name; (void)computation_ms;
#endif
}
void simulation_thread_realtime() { thread_realtime("simulation", 5.0); }
#if !defined(__APPLE__)
void notify_local(const std::string&, const std::string&) {}
void power_play_begin() {}
void power_play_end() {}
const char* thermal_state_name() { return "unknown"; }
const char* latency_warning() { return ""; }
void audio_session_report() {}
std::vector<ReadinessItem> competitive_readiness(int, bool, int) { return {}; }
#endif

// ---- display phase lock (see host.h). Latency as a function of submission phase is a sawtooth:
// it falls as the grid moves later, then jumps by a display period once a frame misses its
// refresh. The lock walks later while the latest latency sits above the recent floor plus a
// margin, and steps back a little when it sees a jump. Nothing here changes the 60 Hz average.
static std::mutex g_phase_mutex;
static double g_phase_latency = -1, g_phase_period = 0;    // newest report
static double g_phase_floor = 1e9;                         // lowest latency seen in the current window
static int g_phase_window = 0, g_phase_hold = 0;
static double g_phase_total_ms = 0;                        // cumulative shift, for the log
void present_feedback(double latency_ms, double display_period_ms) {
  std::lock_guard<std::mutex> lock(g_phase_mutex);
  g_phase_latency = latency_ms; g_phase_period = display_period_ms;
}
static double phase_lock_shift_ms() {
  double latency, period;
  { std::lock_guard<std::mutex> lock(g_phase_mutex); latency = g_phase_latency; period = g_phase_period; g_phase_latency = -1; }
  if (latency < 0 || period <= 0 || g_emulation_speed != 1.0) return 0;
  if (g_phase_hold > 0) { --g_phase_hold; return 0; }
  const double margin = 1.5;
  if (latency < g_phase_floor) g_phase_floor = latency;
  if (++g_phase_window >= 240) { g_phase_window = 0; g_phase_floor = latency; }   // let the floor rise again if the pipeline changes
  double shift = 0;
  if (latency > g_phase_floor + period * 0.6) {           // wrapped: a frame missed its refresh, back off and hold
    shift = -std::min(3.0, period * 0.25); g_phase_hold = 30;
    g_phase_floor = 1e9; g_phase_window = 0;
  } else if (latency > g_phase_floor + margin) {          // still slack before the refresh: creep later
    shift = std::min(0.25, (latency - g_phase_floor - margin) * 0.2);
  }
  g_phase_total_ms += shift;
  return shift;
}
double phase_lock_total_ms() { return g_phase_total_ms; }

void retrace() {
  struct Guard { Guard() { g_in_retrace = true; } ~Guard() { g_in_retrace = false; } } guard;
  if (g_sim_thread.load(std::memory_order_relaxed) == std::thread::id()) g_sim_thread.store(std::this_thread::get_id());
  ++g_retraces;
  {
    double now = now_seconds();
    if (g_sim_frame_start > 0.0) {
      g_last_sim_ms = (now - g_sim_frame_start) * 1000.0;
      g_sim_ms_window += g_last_sim_ms;
      if (g_last_sim_ms > g_sim_ms_worst) g_sim_ms_worst = g_last_sim_ms;
      if (g_last_sim_ms > 16.7) {
        if (g_retraces > 300) ++g_late_frames;   // boot warm-up (shader and texture first use) is not gameplay
        char detail[256] = ""; size_t n = 0;
        for (int i = 0; i < SIM_COST_COUNT; ++i) if (g_sim_costs[i] * 1000.0 >= 0.5) n += (size_t)std::snprintf(detail + n, sizeof detail - n, " %s %.1f", g_sim_cost_names[i], g_sim_costs[i] * 1000.0);
        log("sim frame %u took %.1f ms (ms:%s%s)", g_retraces, g_last_sim_ms, detail, n ? "" : " guest code");
      }
    }
    std::memset(g_sim_costs, 0, sizeof g_sim_costs);
  }
  slippi::poll_options();
  advance_frame();
  if (!options.fast) {
    g_next_frame += std::chrono::microseconds((long long)(16667.0 / g_emulation_speed + phase_lock_shift_ms() * 1000.0));
    auto now = std::chrono::steady_clock::now();
    if (g_next_frame > now) std::this_thread::sleep_until(g_next_frame);
    else if (now - g_next_frame > std::chrono::milliseconds(34)) g_next_frame = now;   // after a stall, resume at 60 Hz instead of sprinting to catch up (audio would crackle)
    g_frame_time = std::chrono::duration<double>(g_next_frame.time_since_epoch()).count();
  } else {
    g_frame_time = now_seconds();
  }
  // Late input sampling: pump the window and controller events *after* the frame sleep, right before
  // the VI interrupt that makes the game read its pads. Pumping before the sleep left every keyboard
  // and Bluetooth-pad press up to a frame's worth of slack (about 12 ms here) stale by the time the
  // game saw it. The GameCube adapter has its own 1 ms reader thread and is unaffected.
  if (g_has_window) { SimCostScope pump(SIM_PUMP); window_pump(); }
  g_sim_frame_start = now_seconds();
  fire_due_alarms(true);
  hle::audio_tick(true);
  // VI: mark display-interrupt 0 as pending (bit 15 of DI0 status, VI reg index 0x18).
  uint16_t di0 = ((uint16_t)g_mmio[0x2030] << 8) | g_mmio[0x2031];
  di0 |= 0x8000;
  g_mmio[0x2030] = (uint8_t)(di0 >> 8); g_mmio[0x2031] = (uint8_t)di0;
  deliver_interrupt(24);  // __OS_INTERRUPT_PI_VI
  observe_scene();
  trace_state();
  if (g_retraces % 60 == 0 || (options.frames && g_retraces >= options.frames)) {
    uint64_t commands, draws, vertices; uint32_t copies;
    gx_stats(&commands, &draws, &vertices, &copies);
    // call depth: host guest-call nesting at the retrace; it should stay flat, and growth means a leak.
    log("[frame %u] gx: %llu cmds %llu draws %llu verts %u efb-copies | disc: %llu reads %.1f MB | call depth %u | %s",
        g_retraces, commands, draws, vertices, copies, g_disc_reads, g_disc_bytes / 1048576.0, cpu->call_depth, sim_cost_line(60).c_str());
  }
  if (options.frames && g_retraces >= options.frames) request_exit(0);
  if (g_exit) {
    log("exit requested after %u retraces", g_retraces);
    std::fflush(stdout);
    throw ExitRequested{g_exit_code.load()};
  }
}

// Nesting: a callback that sleeps (OSSleepThread inside a DVD/ARQ chain) is a wait point where
// hardware would run further completions, so forced delivery may nest; polled entry points
// (force = false) never nest so callback order stays as posted.
static int g_pump_depth = 0;
static bool deliver_completions(bool force) {
  if (g_completions.empty()) return false;
  if (!force && (g_pump_depth > 0 || !ppc::interrupts_on(*cpu))) return false;
  if (g_pump_depth >= 16) return false;
  ++g_pump_depth;
  size_t n = g_completions.size();
  ppc::Context saved = *cpu;
  for (size_t i = 0; i < n && !g_completions.empty(); ++i) {
    Completion fn = std::move(g_completions.front());
    g_completions.pop_front();
    fn();
  }
  uint64_t tb = cpu->tb;
  *cpu = saved;
  cpu->tb = tb;
  ppc::update_mxcsr(*cpu);
  --g_pump_depth;
  return true;
}

void wait_event() {
  std::string background_error;
  if (take_background_failure(background_error)) die("background task failed: %s", background_error.c_str());
  if (g_pe_finish_pending) {
    g_pe_finish_pending = false;
    // PE_ISR (0xCC00100A): finish interrupt status bit 3.
    g_mmio[0x100B] |= 0x08;
    deliver_interrupt(19);  // __OS_INTERRUPT_PI_PE_FINISH
    return;
  }
  if (g_pe_token_pending) {
    g_pe_token_pending = false;
    g_mmio[0x100B] |= 0x04;
    g_mmio[0x100E] = (uint8_t)(g_pe_token >> 8); g_mmio[0x100F] = (uint8_t)g_pe_token;
    deliver_interrupt(18);  // __OS_INTERRUPT_PI_PE_TOKEN
    return;
  }
  // The sleeping thread yields: interrupts are effectively enabled during the switch, so pending
  // completions run now (nested if this sleep happens inside another callback). Otherwise time moves on.
  hle::dvd_poll();
  if (deliver_completions(true)) return;
  retrace();
}

}  // namespace host

namespace ppc {
void loop_poll(Context& c) {
  (void)c;
  host::pump_completions();   // advances time; fires alarms / audio frames / completions when EE is set
}
void interrupts_enabled(Context& c) {
  // Called from mtmsr when EE goes 0 -> 1: flush events that arrived while masked.
  if (host::g_pump_depth == 0) host::deliver_completions(true);   // never nest from inside a callback here
}
}  // namespace ppc

namespace host {

// ---------------- MMIO ----------------
static uint32_t mmio_get(uint32_t off, int bytes) {
  uint32_t v = 0;
  for (int i = 0; i < bytes; ++i) v = (v << 8) | g_mmio[(off + i) & 0xFFFF];
  return v;
}
static void mmio_put(uint32_t off, uint32_t value, int bytes) {
  for (int i = bytes - 1; i >= 0; --i) { g_mmio[(off + i) & 0xFFFF] = (uint8_t)value; value >>= 8; }
}

uint32_t mmio_read(uint32_t addr, int bytes) {
  if ((addr & 0xFFFF0000u) == 0xCC000000u) {
    uint32_t off = addr & 0xFFFF;
    switch (off & 0xFFFE) {
      case 0x2002: return 0;                 // VI: vertical position (VIGetCurrentLine)
      case 0x2000: return 0;
      case 0x0000: return 0;                 // CP status: fifo idle, not overflowed
      case 0x0004: return 0;                 // CP control
      case 0x0034: case 0x0036: return mmio_get(off, bytes);   // CP fifo rw distance (we keep 0)
      case 0x3000: return 0;                 // PI INTSR
      case 0x5004: return 0;                 // DSP mailbox from DSP: nothing pending
      case 0x5000: return 0;                 // DSP mailbox to DSP: not busy
      case 0x500A: return mmio_get(off, bytes) & ~0x0001u;  // DSP CSR: DSP not "reset in progress"
      default: return mmio_get(off, bytes);
    }
  }
  if ((addr & 0xF8000000u) == 0xC8000000u) return 0;  // EFB peek
  static int reported = 0;
  if (reported++ < 20) {
    log("mmio read %08X (%d) from %s lr=%08X retrace=%u", addr, bytes, symbol_name(cpu->last_pc), cpu->lr, retrace_count());
    if (reported <= 2) {
      log("  recent entries:");
      for (uint32_t i = 48; i < 64; ++i) { uint32_t pc = cpu->trace[(cpu->trace_pos + i) & 63]; if (pc) log("    %08X %s", pc, symbol_name(pc)); }
      for (int r = 0; r < 32; r += 8)
        log("  r%02d %08X %08X %08X %08X %08X %08X %08X %08X", r, cpu->r[r], cpu->r[r + 1], cpu->r[r + 2], cpu->r[r + 3], cpu->r[r + 4], cpu->r[r + 5], cpu->r[r + 6], cpu->r[r + 7]);
      // Guest memory around pointer-looking registers: the corrupted object is usually one of them.
      for (int r : {3, 4, 5, 6, 27, 28, 29, 30, 31}) {
        uint32_t p = cpu->r[r];
        if ((p & 0xFE000000u) != 0x80000000u || (p & 0x01FFFFFFu) + 96 > ppc::RAM_SIZE) continue;
        char line[200]; int n = std::snprintf(line, sizeof line, "  [r%d=%08X]", r, p);
        for (int i = 0; i < 20; ++i) n += std::snprintf(line + n, sizeof line - n, " %08X", rd32(p + 4u * (uint32_t)i));
        log("%s", line);
      }
    }
  }
  return 0;
}

void mmio_write(uint32_t addr, uint32_t value, int bytes) {
  if ((addr & 0xFFFFC000u) == 0xCC008000u) { gx_write(value, bytes); return; }
  if ((addr & 0xFFFF0000u) == 0xCC000000u) {
    uint32_t off = addr & 0xFFFF;
    mmio_put(off, value, bytes);
    if (off == 0x3000 || off == 0x3004) return;
    return;
  }
  if ((addr & 0xF8000000u) == 0xC8000000u) return;  // EFB poke
  static int reported = 0;
  if (reported++ < 20) log("mmio write %08X = %08X (%d) from %s", addr, value, bytes, symbol_name(cpu->last_pc));
}

// ---------------- GX glue ----------------
void gx_write(uint32_t value, int bytes) { gx::write_fifo(value, bytes); }
void gx_frame_present(uint32_t) {}
void gx_stats(uint64_t* commands, uint64_t* draws, uint64_t* vertices, uint32_t* efb_copies) {
  gx::stats(commands, draws, vertices, efb_copies);
}

namespace { std::atomic<int> g_online_ping_ms{-1}; }
void set_online_ping_ms(int ms) { g_online_ping_ms.store(ms, std::memory_order_relaxed); }
int online_ping_ms() { return g_online_ping_ms.load(std::memory_order_relaxed); }
}  // namespace host
