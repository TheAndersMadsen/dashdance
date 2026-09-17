// Runtime services for recompiled Gekko code: dispatch, MMIO routing, SPRs, PSQ, fres/frsqrte.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "ppc.h"
#include "functions.h"
#include "host.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

namespace ppc {

static std::vector<Fn> g_dispatch;   // indexed by (addr - RAM_BASE) / 4
static uint8_t g_locked_cache[LC_SIZE];
static std::atomic<bool> g_interpreter_allowed{false};
static std::mutex g_fallback_mutex;
static AotDiagnostics g_aot_diagnostics;

void set_interpreter_allowed(bool allowed) { g_interpreter_allowed.store(allowed); }
bool interpreter_allowed() { return g_interpreter_allowed.load(); }
AotDiagnostics aot_diagnostics() {
  std::lock_guard<std::mutex> lock(g_fallback_mutex);
  AotDiagnostics result = g_aot_diagnostics;
  result.strict = !interpreter_allowed();
  interpreter_stats(&result.interpreted_calls, &result.interpreted_instructions);
  return result;
}
void log_aot_diagnostics() {
  const auto d = aot_diagnostics();
  host::log("[aot] strict=%s missing-target attempts=%llu interpreted calls=%llu instructions=%llu",
            d.strict ? "yes" : "no", (unsigned long long)d.missing_attempts,
            (unsigned long long)d.interpreted_calls, (unsigned long long)d.interpreted_instructions);
  for (size_t i = 0; i < d.missing_count; ++i)
    host::log("[aot] missing %08X calls=%llu", d.missing[i].addr,
              (unsigned long long)d.missing[i].calls);
  if (d.unrecorded_attempts)
    host::log("[aot] additional target attempts=%llu (bounded table full; not a unique-target count)",
              (unsigned long long)d.unrecorded_attempts);
  host::log("[aot] interpreter instruction visits=%llu pc-sequence-fnv1a64=%016llX transfers=%llu transfer-sequence-fnv1a64=%016llX",
            (unsigned long long)d.instruction_visits, (unsigned long long)d.pc_sequence_hash,
            (unsigned long long)d.transfer_count, (unsigned long long)d.transfer_sequence_hash);
  for (uint32_t i = 0; i < d.pc_count; ++i) {
    const auto& p = d.pcs[i];
    host::log("[aot] pc=%08X first=%08X last=%08X visits=%llu code-changes=%llu", p.pc,
              p.first_word, p.last_word, (unsigned long long)p.visits, (unsigned long long)p.code_changes);
  }
  if (d.unrecorded_pc_visits)
    host::log("[aot] unsampled PC visits=%llu (bounded samples; not a unique-PC count)",
              (unsigned long long)d.unrecorded_pc_visits);
  for (uint32_t i = 0; i < d.transfer_sample_count; ++i) {
    const auto& t = d.transfers[i];
    host::log("[aot] transfer=%u from=%08X to=%08X", uint32_t(t.kind), t.from, t.to);
  }
  host::log("[aot] module-load events=%llu retained-identities=%u capacity=%zu dropped-events=%llu",
            (unsigned long long)d.loaded_module_records, d.module_count, d.modules.size(),
            (unsigned long long)d.unrecorded_modules);
  for (uint32_t i = 0; i < d.module_count; ++i) {
    const auto& module = d.modules[i];
    host::log("[aot] loaded=%s addr=%08X bytes=%u loads=%llu first-event=%llu last-event=%llu name-truncated=%s host-supplied-sha256=%s",
              module.name, module.addr, module.size, (unsigned long long)module.loads,
              (unsigned long long)module.first_event, (unsigned long long)module.last_event,
              module.name_truncated ? "yes" : "no", module.sha256_valid ? module.sha256 : "unverified");
  }
}
void record_interpreter_entry(Context& c, uint32_t addr) {
  {
    std::lock_guard<std::mutex> lock(g_fallback_mutex);
    auto& d = g_aot_diagnostics;
    ++d.missing_attempts;
    size_t i = 0;
    for (; i < d.missing_count; ++i) {
      if (d.missing[i].addr == addr) { ++d.missing[i].calls; break; }
    }
    if (i == d.missing_count) {
      if (d.missing_count < d.missing.size()) {
        d.missing[d.missing_count++] = {addr, c.lr, 1};
        host::log("[aot] missing target %08X lr=%08X policy=%s", addr, c.lr,
                  interpreter_allowed() ? "diagnostic interpreter" : "strict stop");
      } else if (++d.unrecorded_attempts == 1) {
        host::log("[aot] missing-target table full; further new-target logs suppressed");
      }
    }
  }
  if (!interpreter_allowed()) {
    log_aot_diagnostics();
    fatal(c, "strict AOT: target has no native translation", addr);
  }
}

static void hash_word(uint64_t& hash, uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    hash ^= (value >> shift) & 255;
    hash *= UINT64_C(1099511628211);
  }
}
void record_interpreter_instruction(uint32_t pc, uint32_t word) {
  std::lock_guard<std::mutex> lock(g_fallback_mutex);
  auto& d = g_aot_diagnostics;
  ++d.instruction_visits;
  hash_word(d.pc_sequence_hash, pc);
  hash_word(d.pc_sequence_hash, word);
  for (uint32_t i = 0; i < d.pc_count; ++i) {
    if (d.pcs[i].pc == pc) {
      auto& sample = d.pcs[i];
      ++sample.visits;
      if (sample.last_word != word) { sample.last_word = word; ++sample.code_changes; }
      return;
    }
  }
  if (d.pc_count < d.pcs.size()) d.pcs[d.pc_count++] = {pc, word, word, 1, 0};
  else ++d.unrecorded_pc_visits;
}
void record_interpreter_transfer(uint32_t from, uint32_t to, AotTransferKind kind) {
  std::lock_guard<std::mutex> lock(g_fallback_mutex);
  auto& d = g_aot_diagnostics;
  ++d.transfer_count;
  hash_word(d.transfer_sequence_hash, from);
  hash_word(d.transfer_sequence_hash, to);
  hash_word(d.transfer_sequence_hash, uint32_t(kind));
  if (d.transfer_sample_count < d.transfers.size())
    d.transfers[d.transfer_sample_count++] = {from, to, kind};
}
void record_loaded_module(const char* name, uint32_t addr, uint32_t size, const char* sha256) {
  std::lock_guard<std::mutex> lock(g_fallback_mutex);
  auto& d = g_aot_diagnostics;
  const uint64_t event = ++d.loaded_module_records;
  AotLoadedModule module;
  const char* full_name = name ? name : "";
  module.name_truncated = std::strlen(full_name) >= sizeof(module.name);
  std::snprintf(module.name, sizeof(module.name), "%s", full_name);
  module.addr = addr;
  module.size = size;
  bool valid = sha256 && std::strlen(sha256) == 64;
  for (unsigned i = 0; valid && i < 64; ++i) {
    const char ch = sha256[i];
    valid = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
    if (valid) module.sha256[i] = (ch >= 'A' && ch <= 'F') ? char(ch + ('a' - 'A')) : ch;
  }
  module.sha256_valid = valid;
  if (!valid) module.sha256[0] = '\0';
  module.loads = 1;
  module.first_event = module.last_event = event;
  // Unknown digests and incomplete names cannot establish identical content.
  // Do not let truncation or missing provenance silently collapse changed loads.
  if (valid && module.name[0] && !module.name_truncated) {
    for (uint32_t i = 0; i < d.module_count; ++i) {
      auto& retained = d.modules[i];
      if (retained.sha256_valid && !retained.name_truncated && retained.addr == addr &&
          retained.size == size && std::strcmp(retained.name, module.name) == 0 &&
          std::strcmp(retained.sha256, module.sha256) == 0) {
        ++retained.loads;
        retained.last_event = event;
        return;
      }
    }
  }
  // Search existing identities even after capacity is reached: their later
  // duplicates still count. New/changed identities are explicitly dropped.
  if (d.module_count == d.modules.size()) { ++d.unrecorded_modules; return; }
  d.modules[d.module_count++] = module;
}

// Covers all of RAM: Gecko caves live below .text (bootloader at 0x800028B8) and in the heap
// (the main code table the game loads), and their subroutines are called through pointers.
void init_dispatch() {
  g_dispatch.assign(RAM_SIZE / 4, nullptr);
  for (size_t i = 0; i < guest::fn_table_count; ++i) {
    const auto& e = guest::fn_table[i];
    uint32_t off = e.addr - RAM_BASE;
    if (off < RAM_SIZE) g_dispatch[off / 4] = e.fn;
  }
}

Fn lookup(uint32_t addr) {
  uint32_t off = addr - RAM_BASE;
  if (off >= RAM_SIZE || (addr & 3) || off / 4 >= g_dispatch.size()) return nullptr;
  return g_dispatch[off / 4];
}

void call(Context& c, uint8_t* m, uint32_t addr) {
  Fn fn = lookup(addr);
  CallDepthScope depth(c, addr);
  if (fn) fn(c, m);
  else interpret(c, m, addr);   // code that only exists in RAM (dat-loaded routines)
}

uint64_t g_enter_count = 0;
bool g_trace_funcs = false;
static std::vector<std::pair<uint32_t, uint32_t>> g_traced;   // (addr, remaining prints)
void add_trace_func(uint32_t addr, uint32_t limit) { g_traced.push_back({addr, limit}); g_trace_funcs = true; }
void trace_enter(Context& c, uint32_t pc) {
  for (auto& t : g_traced) {
    if (t.first != pc || !t.second) continue;
    --t.second;
    host::log("[trace] frame %u %s(%08X) r3=%08X r4=%08X r5=%08X lr=%08X (from %s)", host::retrace_count(), host::symbol_name(pc), pc,
              c.r[3], c.r[4], c.r[5], c.lr, host::symbol_name(c.lr));
  }
}

// Hang diagnostics run on the simulation thread through hang_check.
void hang_check(Context& c) {
  // MELEE_TEST_FATAL_RETRACE=N: raise a guest fault once retrace N is reached, to exercise the crash diagnostics.
  static const long test_fatal = [] { const char* v = std::getenv("MELEE_TEST_FATAL_RETRACE"); return v ? std::atol(v) : 0L; }();
  if (test_fatal > 0 && host::retrace_count() >= (uint32_t)test_fatal) fatal(c, "test fault (MELEE_TEST_FATAL_RETRACE)", host::retrace_count());
  // No retrace for `hang_watch` seconds while the guest keeps calling functions: report where.
  static uint32_t last_retraces = 0;
  static double stuck_since = 0.0;
  if (!host::options.hang_watch) return;
  uint32_t retraces = host::retrace_count();
  double now = host::now_seconds();
  if (retraces != last_retraces || stuck_since == 0.0) { last_retraces = retraces; stuck_since = now; return; }
  if (now - stuck_since > host::options.hang_watch) fatal(c, "no retrace for too long (guest spin loop?)", retraces);
}

uint32_t g_watch_lo = 0, g_watch_len = 0, g_watch_value = 0;
bool g_watch_value_on = false;
void watch_init() {
  if (const char* v = std::getenv("MELEE_WATCH_VALUE")) {
    g_watch_value = (uint32_t)std::strtoul(v, nullptr, 0); g_watch_value_on = true;
    host::log("watch: stores of the word %08X are logged", g_watch_value);
  }
  const char* a = std::getenv("MELEE_WATCH_ADDR");
  if (!a) return;
  g_watch_lo = (uint32_t)std::strtoul(a, nullptr, 0);
  const char* l = std::getenv("MELEE_WATCH_LEN");
  g_watch_len = l ? (uint32_t)std::strtoul(l, nullptr, 0) : 4u;
  host::log("watch: stores to %08X..%08X are logged", g_watch_lo, g_watch_lo + g_watch_len);
}
void watch_hit(Context& c, uint32_t ea, uint64_t v, int bytes) {
  static int reported = 0;
  if (reported++ >= 200) return;
  host::log("watch: store %d bytes at %08X = %0*llX in %s (%08X) lr=%08X retrace=%u", bytes, ea, bytes * 2, (unsigned long long)v,
            host::symbol_name(c.last_pc), c.last_pc, c.lr, host::retrace_count());
  if (reported <= 3) for (uint32_t i = 52; i < 64; ++i) { uint32_t pc = c.trace[(c.trace_pos + i) & 63]; if (pc) host::log("    %08X %s", pc, host::symbol_name(pc)); }
}

void longjmp_restore(Context& c, uint8_t* m, uint32_t buf, uint32_t val) {
  // MSL jmp_buf: +0 LR, +4 CR, +8 r1, +12 r2, +20 r13..r31, +96 f14..f31, +240 FPSCR (as a double).
  c.lr = ld32(c, m, buf);
  mtcrf(c, 0xFFu, ld32(c, m, buf + 4));
  c.r[1] = ld32(c, m, buf + 8);
  c.r[2] = ld32(c, m, buf + 12);
  for (uint32_t i = 13, ea = buf + 20; i < 32; ++i, ea += 4) c.r[i] = ld32(c, m, ea);
  for (int i = 14; i < 32; ++i) c.f[i].u0 = ld64(c, m, buf + 96 + 8 * (uint32_t)(i - 14));
  c.f[0].u0 = ld64(c, m, buf + 240);
  c.fpscr = (uint32_t)c.f[0].u0; update_mxcsr(c);
  c.r[3] = val ? val : 1u;
}

namespace {
// Guest call stack from the PowerPC back chain: [sp] is the caller's sp, [caller sp + 4] the LR saved into
// it. Runs of one repeated return address (deep recursion) print once with a count.
void log_guest_backtrace(Context& c, uint8_t* m) {
  host::log("guest call stack (innermost first):");
  uint32_t sp = c.r[1], prev_lr = 0, repeats = 0, shown = 0;
  auto flush = [&] { if (repeats > 1) host::log("    ... %u more times", repeats - 1); };
  for (int depth = 0; depth < 25000 && sp && fast(m, sp) && fast(m, sp + 8); ++depth) {
    uint32_t caller_sp = ld32(c, m, sp);
    if (!caller_sp || caller_sp <= sp || !fast(m, caller_sp + 8)) break;
    uint32_t lr = ld32(c, m, caller_sp + 4);
    if (lr == prev_lr) { ++repeats; sp = caller_sp; continue; }
    flush();
    if (++shown > 64) { host::log("  ... (stack continues)"); repeats = 0; break; }
    host::log("  %08X %s", lr, host::symbol_name(lr));
    prev_lr = lr; repeats = 1; sp = caller_sp;
  }
  flush();
}

#if !defined(MELEE_PORT_OFFLINE) || !MELEE_PORT_OFFLINE
// Guest RAM next to the session log (session-<stamp>.ram, 24 MB), newest three kept, so a crash can be
// inspected after the fact (tools/mac/ramdiff.py, .agents/skills/fix-logs/crash.md).
void write_crash_ram() {
  namespace fs = std::filesystem;
  if (host::options.log_file.empty() || !host::ram) return;
  fs::path path = fs::path(host::options.log_file).replace_extension(".ram");
  std::error_code ec;
  std::vector<fs::path> old;
  for (const auto& e : fs::directory_iterator(path.parent_path(), ec))
    if (e.path().extension() == ".ram") old.push_back(e.path());
  std::sort(old.begin(), old.end());
  for (size_t i = 0; old.size() >= 3 && i + 2 < old.size(); ++i) fs::remove(old[i], ec);
  if (FILE* f = std::fopen(path.string().c_str(), "wb")) {
    std::fwrite(host::ram, 1, RAM_SIZE, f);
    std::fclose(f);
    host::log("guest RAM at the fault written to %s", path.string().c_str());
  }
}
#endif
}  // namespace

[[noreturn]] void fatal(Context& c, const char* what, uint32_t a) {
  host::log("recent function entries (oldest first):");
  for (uint32_t i = 0; i < 64; ++i) {
    uint32_t pc = c.trace[(c.trace_pos + i) & 63];
    if (pc) host::log("  %08X %s", pc, host::symbol_name(pc));
  }
  host::log("r3=%08X r4=%08X r5=%08X r6=%08X r12=%08X r31=%08X", c.r[3], c.r[4], c.r[5], c.r[6], c.r[12], c.r[31]);
  for (int i = 0; i < 32; i += 8)
    host::log("r%d-r%d: %08X %08X %08X %08X %08X %08X %08X %08X", i, i + 7, c.r[i], c.r[i + 1], c.r[i + 2], c.r[i + 3],
              c.r[i + 4], c.r[i + 5], c.r[i + 6], c.r[i + 7]);
  host::log("lr=%08X ctr=%08X last function %08X", c.lr, c.ctr, c.last_pc);
  if (host::ram) log_guest_backtrace(c, host::ram);
#if !defined(MELEE_PORT_OFFLINE) || !MELEE_PORT_OFFLINE
  write_crash_ram();
#endif
  host::die("guest fault: %s (%08X) in %s (%08X); lr=%08X r1=%08X", what, a,
            host::symbol_name(c.last_pc), c.last_pc, c.lr, c.r[1]);
}

uint8_t* locked_cache() { return g_locked_cache; }

uint32_t mmio_read(Context& c, uint32_t ea, int bytes) { return host::mmio_read(ea, bytes); }
void mmio_write(Context& c, uint32_t ea, uint32_t value, int bytes) { host::mmio_write(ea, value, bytes); }
uint64_t mmio_read64(Context& c, uint32_t ea) {
  return ((uint64_t)host::mmio_read(ea, 4) << 32) | host::mmio_read(ea + 4, 4);
}
void mmio_write64(Context& c, uint32_t ea, uint64_t value) {
  host::mmio_write(ea, (uint32_t)(value >> 32), 4);
  host::mmio_write(ea + 4, (uint32_t)value, 4);
}

uint32_t spr_read(Context& c, uint32_t n) {
  switch (n) {
    case 1017: return c.spr[n] & ~1u;  // L2CR: invalidate-in-progress bit always clear
    case 921: return 0;                // WPAR: write-gather pipe never busy
    case 272: case 273: case 274: case 275: return c.spr[n];  // SPRG0-3
    default: return c.spr[n & 1023];
  }
}

void spr_write(Context& c, uint32_t n, uint32_t v) { c.spr[n & 1023] = v; }

void syscall(Context& c, uint8_t* m) {
  // Melee only uses sc for cache maintenance from OS code; nothing to do.
}

void update_fp_environment(Context& c) {
  set_fp_environment(c.fpscr, fp_profile());
}

void dcbz(Context& c, uint8_t* m, uint32_t ea) {
  ea &= ~31u;
  if (uint8_t* p = fast(m, ea)) { std::memset(p, 0, 32); return; }
  if (uint8_t* p = slowptr(ea)) { std::memset(p, 0, 32); return; }
  fatal(c, "dcbz outside RAM", ea);
}

void lswi(Context& c, uint8_t* m, uint32_t ea, uint32_t rd, uint32_t nb) {
  uint32_t r = rd;
  while (nb > 0) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      v <<= 8;
      if (nb > 0) { v |= ld8(c, m, ea++); --nb; }
    }
    c.r[r] = v;
    r = (r + 1) & 31;
  }
}

void stswi(Context& c, uint8_t* m, uint32_t ea, uint32_t rs, uint32_t nb) {
  uint32_t r = rs;
  int shift = 24;
  while (nb > 0) {
    st8(c, m, ea++, (c.r[r] >> shift) & 0xFF);
    --nb;
    shift -= 8;
    if (shift < 0) { shift = 24; r = (r + 1) & 31; }
  }
}

// ---- paired-single quantized loads/stores (Interpreter_LoadStorePaired semantics) ----
static const float dequantize_table[] = {
  1.0f / (1u << 0), 1.0f / (1u << 1), 1.0f / (1u << 2), 1.0f / (1u << 3), 1.0f / (1u << 4), 1.0f / (1u << 5),
  1.0f / (1u << 6), 1.0f / (1u << 7), 1.0f / (1u << 8), 1.0f / (1u << 9), 1.0f / (1u << 10), 1.0f / (1u << 11),
  1.0f / (1u << 12), 1.0f / (1u << 13), 1.0f / (1u << 14), 1.0f / (1u << 15), 1.0f / (1u << 16), 1.0f / (1u << 17),
  1.0f / (1u << 18), 1.0f / (1u << 19), 1.0f / (1u << 20), 1.0f / (1u << 21), 1.0f / (1u << 22), 1.0f / (1u << 23),
  1.0f / (1u << 24), 1.0f / (1u << 25), 1.0f / (1u << 26), 1.0f / (1u << 27), 1.0f / (1u << 28), 1.0f / (1u << 29),
  1.0f / (1u << 30), 1.0f / (1u << 31),
  (float)(1ull << 32), (float)(1u << 31), (float)(1u << 30), (float)(1u << 29), (float)(1u << 28), (float)(1u << 27),
  (float)(1u << 26), (float)(1u << 25), (float)(1u << 24), (float)(1u << 23), (float)(1u << 22), (float)(1u << 21),
  (float)(1u << 20), (float)(1u << 19), (float)(1u << 18), (float)(1u << 17), (float)(1u << 16), (float)(1u << 15),
  (float)(1u << 14), (float)(1u << 13), (float)(1u << 12), (float)(1u << 11), (float)(1u << 10), (float)(1u << 9),
  (float)(1u << 8), (float)(1u << 7), (float)(1u << 6), (float)(1u << 5), (float)(1u << 4), (float)(1u << 3),
  (float)(1u << 2), (float)(1u << 1),
};
static const float quantize_table[] = {
  (float)(1u << 0), (float)(1u << 1), (float)(1u << 2), (float)(1u << 3), (float)(1u << 4), (float)(1u << 5),
  (float)(1u << 6), (float)(1u << 7), (float)(1u << 8), (float)(1u << 9), (float)(1u << 10), (float)(1u << 11),
  (float)(1u << 12), (float)(1u << 13), (float)(1u << 14), (float)(1u << 15), (float)(1u << 16), (float)(1u << 17),
  (float)(1u << 18), (float)(1u << 19), (float)(1u << 20), (float)(1u << 21), (float)(1u << 22), (float)(1u << 23),
  (float)(1u << 24), (float)(1u << 25), (float)(1u << 26), (float)(1u << 27), (float)(1u << 28), (float)(1u << 29),
  (float)(1u << 30), (float)(1u << 31),
  1.0f / (float)(1ull << 32), 1.0f / (1u << 31), 1.0f / (1u << 30), 1.0f / (1u << 29), 1.0f / (1u << 28), 1.0f / (1u << 27),
  1.0f / (1u << 26), 1.0f / (1u << 25), 1.0f / (1u << 24), 1.0f / (1u << 23), 1.0f / (1u << 22), 1.0f / (1u << 21),
  1.0f / (1u << 20), 1.0f / (1u << 19), 1.0f / (1u << 18), 1.0f / (1u << 17), 1.0f / (1u << 16), 1.0f / (1u << 15),
  1.0f / (1u << 14), 1.0f / (1u << 13), 1.0f / (1u << 12), 1.0f / (1u << 11), 1.0f / (1u << 10), 1.0f / (1u << 9),
  1.0f / (1u << 8), 1.0f / (1u << 7), 1.0f / (1u << 6), 1.0f / (1u << 5), 1.0f / (1u << 4), 1.0f / (1u << 3),
  1.0f / (1u << 2), 1.0f / (1u << 1),
};

template <typename T>
static T scale_clamp(Context& c, double ps, uint32_t st_scale) {
  float conv = (float)ps * quantize_table[st_scale];
  // Pinned interpreter's NaN-to-integer cast is not a defined C++ oracle.
  // Stop explicitly until a hardware-verified rule is selected, rather than
  // invoking UB or silently choosing different ARM/x86 saturation values.
  if (std::isnan(conv)) { fatal(c, "psq_st integer NaN conversion is unverified", st_scale); return T(0); }
  float lo = (float)std::numeric_limits<T>::min(), hi = (float)std::numeric_limits<T>::max();
  if (conv < lo) conv = lo;
  if (conv > hi) conv = hi;
  return (T)conv;
}

// GQR layout: st_type bits 0-2, st_scale bits 8-13, ld_type bits 16-18, ld_scale bits 24-29.
void psq_load(Context& c, uint8_t* m, uint32_t ea, uint32_t rd, uint32_t w, uint32_t i) {
  uint32_t gqr = c.gqr[i];
  uint32_t type = (gqr >> 16) & 7, scale = (gqr >> 24) & 63;
  float ps0, ps1;
  switch (type) {
    case 0:  // Preserve load-conversion payloads and subnormals independently of host FZ.
      c.f[rd].ps0 = float_bits_to_double(ld32(c, m, ea));
      c.f[rd].ps1 = w ? 1.0 : float_bits_to_double(ld32(c, m, ea + 4));
      return;
    case 4:  // u8
      if (w) { ps0 = (float)(uint8_t)ld8(c, m, ea) * dequantize_table[scale]; ps1 = 1.0f; }
      else { uint32_t v = ld16(c, m, ea); ps0 = (float)(uint8_t)(v >> 8) * dequantize_table[scale]; ps1 = (float)(uint8_t)v * dequantize_table[scale]; }
      break;
    case 5:  // u16
      if (w) { ps0 = (float)(uint16_t)ld16(c, m, ea) * dequantize_table[scale]; ps1 = 1.0f; }
      else { uint32_t v = ld32(c, m, ea); ps0 = (float)(uint16_t)(v >> 16) * dequantize_table[scale]; ps1 = (float)(uint16_t)v * dequantize_table[scale]; }
      break;
    case 6:  // s8
      if (w) { ps0 = (float)(int8_t)ld8(c, m, ea) * dequantize_table[scale]; ps1 = 1.0f; }
      else { uint32_t v = ld16(c, m, ea); ps0 = (float)(int8_t)(v >> 8) * dequantize_table[scale]; ps1 = (float)(int8_t)v * dequantize_table[scale]; }
      break;
    case 7:  // s16
      if (w) { ps0 = (float)(int16_t)ld16(c, m, ea) * dequantize_table[scale]; ps1 = 1.0f; }
      else { uint32_t v = ld32(c, m, ea); ps0 = (float)(int16_t)(v >> 16) * dequantize_table[scale]; ps1 = (float)(int16_t)v * dequantize_table[scale]; }
      break;
    default:
      fatal(c, "psq_l invalid GQR type", gqr);
  }
  c.f[rd].ps0 = ps0;
  c.f[rd].ps1 = ps1;
}

void psq_store(Context& c, uint8_t* m, uint32_t ea, uint32_t rs, uint32_t w, uint32_t i) {
  uint32_t gqr = c.gqr[i];
  uint32_t type = gqr & 7, scale = (gqr >> 8) & 63;
  double ps0 = c.f[rs].ps0, ps1 = c.f[rs].ps1;
  switch (type) {
    case 0: {
      uint32_t a = double_to_float_bits_ftz(ps0);
      if (w) st32(c, m, ea, a);
      else { st32(c, m, ea, a); st32(c, m, ea + 4, double_to_float_bits_ftz(ps1)); }
      break;
    }
    case 4: {
      uint8_t a = (uint8_t)scale_clamp<uint8_t>(c, ps0, scale);
      if (w) st8(c, m, ea, a);
      else st16(c, m, ea, ((uint32_t)a << 8) | (uint8_t)scale_clamp<uint8_t>(c, ps1, scale));
      break;
    }
    case 5: {
      uint16_t a = (uint16_t)scale_clamp<uint16_t>(c, ps0, scale);
      if (w) st16(c, m, ea, a);
      else st32(c, m, ea, ((uint32_t)a << 16) | (uint16_t)scale_clamp<uint16_t>(c, ps1, scale));
      break;
    }
    case 6: {
      uint8_t a = (uint8_t)scale_clamp<int8_t>(c, ps0, scale);
      if (w) st8(c, m, ea, a);
      else st16(c, m, ea, ((uint32_t)a << 8) | (uint8_t)scale_clamp<int8_t>(c, ps1, scale));
      break;
    }
    case 7: {
      uint16_t a = (uint16_t)scale_clamp<int16_t>(c, ps0, scale);
      if (w) st16(c, m, ea, a);
      else st32(c, m, ea, ((uint32_t)a << 16) | (uint16_t)scale_clamp<int16_t>(c, ps1, scale));
      break;
    }
    default:
      fatal(c, "psq_st invalid GQR type", gqr);
  }
}

}  // namespace ppc
