// Guest CPU state and memory/float helpers for statically recompiled Gekko code.
// Portable return-value profiles are documented in numeric.h / numeric.cpp.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include "numeric.h"

namespace ppc {

constexpr uint32_t RAM_BASE = 0x80000000u;
constexpr uint32_t RAM_SIZE = 0x01800000u;
constexpr uint32_t LC_BASE = 0xE0000000u;
constexpr uint32_t LC_SIZE = 0x4000u;

union FPR {
  struct { double ps0, ps1; };
  struct { uint64_t u0, u1; };
};

struct Context {
  uint32_t r[32];
  FPR f[32];
  uint8_t cr[8];       // per field: LT=8 GT=4 EQ=2 SO=1
  uint32_t lr, ctr;
  uint32_t ca, so, ov; // XER pieces
  uint32_t fpscr;
  uint32_t gqr[8];
  uint32_t msr;
  uint32_t hid0, hid2, dec;
  uint64_t tb;
  uint32_t spr[1024];  // anything else, by number
  uint32_t call_depth;
  uint32_t backedges;  // loop back-edge counter for event polling (see backedge)
  uint32_t last_pc;    // diagnostics only (function entry address)
  uint32_t entry;      // mid-function entry address requested by a dispatch thunk (0 = normal entry)
  uint32_t trace_pos;
  uint32_t trace[64];  // ring of recently entered functions (diagnostics)
};
// Loop back-edge poll: every 1024 iterations of any guest loop, let virtual time advance and
// deliver due host events (alarms, AI DMA frames, completions) if interrupts are enabled.
void loop_poll(Context& c);
inline void backedge(Context& c) { if ((++c.backedges & 0x3FFu) == 0) loop_poll(c); }
// Hang watchdog: every 2^20 function entries the host checks whether simulation time still
// advances (diagnostics for guest spin loops; see host::hang_check).
extern uint64_t g_enter_count;
extern bool g_trace_funcs;          // --trace-func: log entries of selected guest functions
void hang_check(Context& c);
void trace_enter(Context& c, uint32_t pc);

void add_trace_func(uint32_t addr, uint32_t limit);
inline void enter(Context& c, uint32_t pc) {
  c.last_pc = pc; c.trace[c.trace_pos++ & 63] = pc;
  if ((++g_enter_count & 0xFFFFFu) == 0) hang_check(c);
  if (g_trace_funcs) trace_enter(c, pc);
}
// MSR writes: when the guest re-enables external interrupts (EE), deliver pending host events,
// exactly where a real interrupt would have been taken.
void interrupts_enabled(Context& c);
inline void mtmsr(Context& c, uint32_t v) {
  bool enable = !(c.msr & 0x8000u) && (v & 0x8000u);
  c.msr = v;
  if (enable) interrupts_enabled(c);
}
inline bool interrupts_on(const Context& c) { return (c.msr & 0x8000u) != 0; }

using Fn = void (*)(Context&, uint8_t*);

// ---- runtime services implemented in ppc_runtime.cpp ----
void call(Context& c, uint8_t* m, uint32_t addr);         // indirect call by guest address
void interpret(Context& c, uint8_t* m, uint32_t addr);    // run RAM-resident code until it returns (interp.cpp)
void interpreter_stats(uint64_t* calls, uint64_t* insns);
void set_interpreter_allowed(bool allowed);  // false by default: missing AOT targets fail closed
bool interpreter_allowed();
void record_interpreter_entry(Context& c, uint32_t addr);
void log_aot_diagnostics();  // bounded target list plus aggregate counters
struct AotMissingTarget { uint32_t addr = 0, first_lr = 0; uint64_t calls = 0; };
struct AotPcSample {
  uint32_t pc = 0, first_word = 0, last_word = 0;
  uint64_t visits = 0, code_changes = 0;
};
enum class AotTransferKind : uint32_t { Entry, GuestBranch, CallToAot, ReturnFromAot, Exit };
struct AotTransfer { uint32_t from = 0, to = 0; AotTransferKind kind = AotTransferKind::Entry; };
struct AotLoadedModule {
  char name[96]{};
  uint32_t addr = 0, size = 0;
  char sha256[65]{};  // supplied by host over the actual loaded byte range
  bool sha256_valid = false;
  bool name_truncated = false;
  uint64_t loads = 0, first_event = 0, last_event = 0;  // event IDs start at one
};
struct AotDiagnostics {
  bool strict = true;
  uint64_t missing_attempts = 0, interpreted_calls = 0, interpreted_instructions = 0;
  uint64_t unrecorded_attempts = 0;
  uint32_t missing_count = 0;
  std::array<AotMissingTarget, 32> missing{};
  // Visits include a decoded-but-faulting instruction. Successful interpreter
  // steps are counted separately above. FNV-1a hashes are sequence diagnostics,
  // NOT cryptographic identities or a complete coverage proof.
  uint64_t instruction_visits = 0, pc_sequence_hash = UINT64_C(14695981039346656037);
  uint64_t unrecorded_pc_visits = 0;
  uint32_t pc_count = 0;
  std::array<AotPcSample, 128> pcs{};
  uint64_t transfer_count = 0, transfer_sequence_hash = UINT64_C(14695981039346656037);
  uint32_t transfer_sample_count = 0;
  std::array<AotTransfer, 128> transfers{};
  // Event count, not identity count. Verified identical name/range/digest loads
  // share a record; changed contents or placement retain a separate identity.
  // sum(modules[i].loads) + unrecorded_modules == loaded_module_records.
  uint64_t loaded_module_records = 0, unrecorded_modules = 0;
  uint32_t module_count = 0;
  std::array<AotLoadedModule, 128> modules{};
};
AotDiagnostics aot_diagnostics();  // locked, non-logging snapshot for manifests
void record_interpreter_instruction(uint32_t pc, uint32_t word);
void record_interpreter_transfer(uint32_t from, uint32_t to, AotTransferKind kind);
void record_loaded_module(const char* name, uint32_t addr, uint32_t size, const char* sha256);
[[noreturn]] void fatal(Context& c, const char* what, uint32_t a);
// One level of host guest-call nesting. A scope, not ++/-- around the call, because guest longjmp and
// OSLoadContext unwind as C++ exceptions: a manual decrement was skipped for every frame they passed, and
// a stage's setjmp/longjmp animation lookup leaked hundreds of levels per use until a match hit the limit.
struct CallDepthScope {
  Context& c;
  CallDepthScope(Context& ctx, uint32_t addr) : c(ctx) {
    if (c.call_depth >= 20000) fatal(c, "guest call depth exceeded", addr);
    ++c.call_depth;
  }
  ~CallDepthScope() { --c.call_depth; }
  CallDepthScope(const CallDepthScope&) = delete;
  CallDepthScope& operator=(const CallDepthScope&) = delete;
};
// __longjmp: thrown by the HLE, caught by the translated function that called __setjmp on
// `buf` (its body is wrapped in a retry loop; see Emitter). The catch restores the registers
// the MSL longjmp would and resumes at the setjmp return address saved in the buffer.
struct GuestLongJmp { uint32_t buf; uint32_t val; };
void longjmp_restore(Context& c, uint8_t* m, uint32_t buf, uint32_t val);
uint32_t mmio_read(Context& c, uint32_t ea, int bytes);
void mmio_write(Context& c, uint32_t ea, uint32_t value, int bytes);
uint64_t mmio_read64(Context& c, uint32_t ea);
void mmio_write64(Context& c, uint32_t ea, uint64_t value);
uint32_t spr_read(Context& c, uint32_t n);
void spr_write(Context& c, uint32_t n, uint32_t v);
void syscall(Context& c, uint8_t* m);
void update_fp_environment(Context& c);
inline void update_mxcsr(Context& c) { update_fp_environment(c); }  // original host API
uint8_t* locked_cache();

// Timebase reads advance time slightly so guest delay loops (OSGetTime polling) terminate.
inline uint64_t read_tb(Context& c) { c.tb += 32; return c.tb; }

// ---- memory ----
// Host pointer for a guest address in RAM, or nullptr when it needs the slow path.
inline uint8_t* fast(uint8_t* m, uint32_t ea) {
  uint32_t off = ea & 0x3FFFFFFFu;
  return off < RAM_SIZE ? m + off : nullptr;
}
inline uint8_t* slowptr(uint32_t ea) {
  if ((ea & 0xFFFFC000u) == LC_BASE) return locked_cache() + (ea & (LC_SIZE - 1));
  return nullptr;
}
inline uint32_t ld8(Context& c, uint8_t* m, uint32_t ea) {
  if (uint8_t* p = fast(m, ea)) return *p;
  if (uint8_t* p = slowptr(ea)) return *p;
  return mmio_read(c, ea, 1);
}
inline uint32_t ld16(Context& c, uint8_t* m, uint32_t ea) {
  if (uint8_t* p = fast(m, ea)) { uint16_t v; std::memcpy(&v, p, 2); return bswap16(v); }
  if (uint8_t* p = slowptr(ea)) { uint16_t v; std::memcpy(&v, p, 2); return bswap16(v); }
  return mmio_read(c, ea, 2);
}
inline uint32_t ld32(Context& c, uint8_t* m, uint32_t ea) {
  if (uint8_t* p = fast(m, ea)) { uint32_t v; std::memcpy(&v, p, 4); return bswap32(v); }
  if (uint8_t* p = slowptr(ea)) { uint32_t v; std::memcpy(&v, p, 4); return bswap32(v); }
  return mmio_read(c, ea, 4);
}
inline uint64_t ld64(Context& c, uint8_t* m, uint32_t ea) {
  if (uint8_t* p = fast(m, ea)) { uint64_t v; std::memcpy(&v, p, 8); return bswap64(v); }
  if (uint8_t* p = slowptr(ea)) { uint64_t v; std::memcpy(&v, p, 8); return bswap64(v); }
  return mmio_read64(c, ea);
}
// Store watchpoint (MELEE_WATCH_ADDR / MELEE_WATCH_LEN): one compare on the store path.
extern uint32_t g_watch_lo, g_watch_len, g_watch_value;
extern bool g_watch_value_on;
void watch_hit(Context& c, uint32_t ea, uint64_t v, int bytes);
void watch_init();   // reads MELEE_WATCH_ADDR / MELEE_WATCH_LEN / MELEE_WATCH_VALUE
inline void watch(Context& c, uint32_t ea, uint64_t v, int bytes) {
  if (__builtin_expect(ea + (uint32_t)bytes - g_watch_lo < g_watch_len + (uint32_t)bytes, 0)) watch_hit(c, ea, v, bytes);
  if (__builtin_expect(g_watch_value_on && bytes >= 4 && ((uint32_t)v == g_watch_value || (bytes == 8 && (uint32_t)(v >> 32) == g_watch_value)), 0)) watch_hit(c, ea, v, bytes);
}
inline void st8(Context& c, uint8_t* m, uint32_t ea, uint32_t v) {
  watch(c, ea, v, 1);
  if (uint8_t* p = fast(m, ea)) { *p = (uint8_t)v; return; }
  if (uint8_t* p = slowptr(ea)) { *p = (uint8_t)v; return; }
  mmio_write(c, ea, v & 0xFF, 1);
}
inline void st16(Context& c, uint8_t* m, uint32_t ea, uint32_t v) {
  watch(c, ea, v, 2);
  uint16_t s = bswap16((uint16_t)v);
  if (uint8_t* p = fast(m, ea)) { std::memcpy(p, &s, 2); return; }
  if (uint8_t* p = slowptr(ea)) { std::memcpy(p, &s, 2); return; }
  mmio_write(c, ea, v & 0xFFFF, 2);
}
inline void st32(Context& c, uint8_t* m, uint32_t ea, uint32_t v) {
  watch(c, ea, v, 4);
  uint32_t s = bswap32(v);
  if (uint8_t* p = fast(m, ea)) { std::memcpy(p, &s, 4); return; }
  if (uint8_t* p = slowptr(ea)) { std::memcpy(p, &s, 4); return; }
  mmio_write(c, ea, v, 4);
}
inline void st64(Context& c, uint8_t* m, uint32_t ea, uint64_t v) {
  watch(c, ea, v, 8);
  uint64_t s = bswap64(v);
  if (uint8_t* p = fast(m, ea)) { std::memcpy(p, &s, 8); return; }
  if (uint8_t* p = slowptr(ea)) { std::memcpy(p, &s, 8); return; }
  mmio_write64(c, ea, v);
}
// Byte-reversed forms read the guest bytes in host (little-endian) order.
inline uint32_t ld32r(Context& c, uint8_t* m, uint32_t ea) { return bswap32(ld32(c, m, ea)); }
inline uint32_t ld16r(Context& c, uint8_t* m, uint32_t ea) { return bswap16((uint16_t)ld16(c, m, ea)); }
inline void st32r(Context& c, uint8_t* m, uint32_t ea, uint32_t v) { st32(c, m, ea, bswap32(v)); }
inline void st16r(Context& c, uint8_t* m, uint32_t ea, uint32_t v) { st16(c, m, ea, bswap16((uint16_t)v)); }
void dcbz(Context& c, uint8_t* m, uint32_t ea);
void lswi(Context& c, uint8_t* m, uint32_t ea, uint32_t rd, uint32_t nb);
void stswi(Context& c, uint8_t* m, uint32_t ea, uint32_t rs, uint32_t nb);
void psq_load(Context& c, uint8_t* m, uint32_t ea, uint32_t rd, uint32_t w, uint32_t i);
void psq_store(Context& c, uint8_t* m, uint32_t ea, uint32_t rs, uint32_t w, uint32_t i);

// ---- condition register ----
inline void cr_set_s(Context& c, int field, int32_t a, int32_t b) {
  c.cr[field] = (uint8_t)((a < b ? 8 : a > b ? 4 : 2) | c.so);
}
inline void cr_set_u(Context& c, int field, uint32_t a, uint32_t b) {
  c.cr[field] = (uint8_t)((a < b ? 8 : a > b ? 4 : 2) | c.so);
}
inline void cr0(Context& c, uint32_t v) { cr_set_s(c, 0, (int32_t)v, 0); }
inline uint32_t mfcr(const Context& c) {
  uint32_t v = 0;
  for (int i = 0; i < 8; ++i) v |= (uint32_t)(c.cr[i] & 15) << (28 - 4 * i);
  return v;
}
inline void mtcrf(Context& c, uint32_t crm, uint32_t v) {
  for (int i = 0; i < 8; ++i)
    if (crm & (0x80 >> i)) c.cr[i] = (uint8_t)((v >> (28 - 4 * i)) & 15);
}
inline uint32_t crbit(const Context& c, int bit) { return (c.cr[bit >> 2] >> (3 - (bit & 3))) & 1; }
inline void crbit_set(Context& c, int bit, uint32_t v) {
  uint8_t mask = (uint8_t)(8 >> (bit & 3));
  c.cr[bit >> 2] = (uint8_t)(v ? (c.cr[bit >> 2] | mask) : (c.cr[bit >> 2] & ~mask));
}
inline uint32_t mask(int mb, int me) {
  uint32_t begin = 0xFFFFFFFFu >> mb, end = 0x7FFFFFFFu >> me, m = begin ^ end;
  return me < mb ? ~m : m;
}
inline uint32_t carry(uint32_t a, uint32_t b) { return b > ~a; }
inline uint32_t divw(int32_t a, int32_t b) {
  if (b == 0 || ((uint32_t)a == 0x80000000u && b == -1)) return (a < 0 && b == 0) ? 0xFFFFFFFFu : 0;
  return (uint32_t)(a / b);
}
inline uint32_t divwu(uint32_t a, uint32_t b) { return b ? a / b : 0; }
inline uint32_t sraw(Context& c, uint32_t rs, uint32_t rb) {
  if (rb & 0x20) { c.ca = (rs & 0x80000000u) ? 1 : 0; return c.ca ? 0xFFFFFFFFu : 0; }
  int amount = rb & 31;
  if (!amount) { c.ca = 0; return rs; }
  c.ca = (rs & 0x80000000u) && (rs & ((1u << amount) - 1u));
  return arithmetic_shift_right(rs, uint32_t(amount));
}
inline uint32_t srawi(Context& c, uint32_t rs, int amount) {
  amount &= 31;
  if (!amount) { c.ca = 0; return rs; }
  c.ca = (rs & 0x80000000u) && (rs & ((1u << amount) - 1u));
  return arithmetic_shift_right(rs, uint32_t(amount));
}

// ---- floating point ----
inline void fcmp(Context& c, int field, double a, double b) {
  c.cr[field] = (uint8_t)((a != a || b != b) ? 1 : a < b ? 8 : a > b ? 4 : 2);
}

}  // namespace ppc
