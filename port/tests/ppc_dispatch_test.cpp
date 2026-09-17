// Actual CPU runtime/interpreter with synthetic memory and inert host fixtures.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "ppc.h"
#include "host.h"
#include "functions.h"
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace ppc {
void init_dispatch();
void loop_poll(Context&) {}
void interrupts_enabled(Context&) {}
}
namespace host {
Options options;
void log(const char*, ...) {}
[[noreturn]] void die(const char*, ...) { throw std::runtime_error("expected synthetic guest fault"); }
const char* symbol_name(uint32_t) { return "synthetic"; }
uint32_t retrace_count() { return 1; }
double now_seconds() { return 0; }
uint32_t mmio_read(uint32_t, int) { throw std::runtime_error("unexpected MMIO read"); }
void mmio_write(uint32_t, uint32_t, int) { throw std::runtime_error("unexpected MMIO write"); }
}
namespace guest {
void native_fixture(ppc::Context& c, uint8_t*) { ++c.r[7]; }
// A translated __setjmp caller: it catches a guest longjmp thrown two calls further in, as emit.py's wrapper does.
void longjmp_fixture(ppc::Context& c, uint8_t*) { throw ppc::GuestLongJmp{0, 1}; }
void longjmp_middle_fixture(ppc::Context& c, uint8_t* m) { ppc::call(c, m, 0x80008004); }
void setjmp_fixture(ppc::Context& c, uint8_t* m) {
  try { ppc::call(c, m, 0x80008008); } catch (const ppc::GuestLongJmp&) { c.r[8] = c.call_depth; }
}
const FnEntry fn_table[] = {{0x80008000, native_fixture}, {0x80008004, longjmp_fixture},
                            {0x80008008, longjmp_middle_fixture}, {0x8000800C, setjmp_fixture}};
const size_t fn_table_count = 4;
}
namespace {
unsigned checks = 0, failures = 0;
void eq(const char* name, uint64_t got, uint64_t expected) {
  ++checks;
  if (got != expected) {
    ++failures;
    std::fprintf(stderr, "%s: %016llx != %016llx\n", name,
                 (unsigned long long)got, (unsigned long long)expected);
  }
}
}
int main() {
  ppc::ScopedGuestFpEnvironment fp(0);
  ppc::init_dispatch();
  std::vector<uint8_t> memory(ppc::RAM_SIZE);
  uint8_t* m = memory.data();
  ppc::Context c{};
  constexpr uint32_t entry = 0x80004000, done = 0x80009000;
  // mflr r10; bl native_fixture; mulli r3,r4,-32768; mtlr r10; b +8; trap; blr
  const uint32_t words[] = {0x7d4802a6, 0x48003ffd, 0x1c648000, 0x7d4803a6,
                             0x48000008, 0, 0x4e800020};
  for (size_t i = 0; i < std::size(words); ++i) ppc::st32(c, m, entry + uint32_t(i * 4), words[i]);
  c.lr = done; c.r[4] = 0x7fffffff;
  bool faulted = false;
  try { ppc::call(c, m, entry); } catch (const std::runtime_error&) { faulted = true; }
  eq("strict missing target faults", faulted, true);
  auto d = ppc::aot_diagnostics();
  eq("strict attempted target", d.missing_attempts, 1);
  eq("strict interpreted calls zero", d.interpreted_calls, 0);
  eq("strict instruction visits zero", d.instruction_visits, 0);
  eq("strict leaves guest result untouched", c.r[3], 0);
  eq("strict target recorded", d.missing[0].addr, entry);
  eq("strict caller LR recorded", d.missing[0].first_lr, done);
  ppc::set_interpreter_allowed(true);
  c = {}; c.lr = done; c.r[4] = 0x7fffffff;
  ppc::call(c, m, entry);
  eq("diagnostic guest result", c.r[3], 0x8000);
  eq("nested call entered native target", c.r[7], 1);
  eq("call depth restored", c.call_depth, 0);
  c = {};
  for (int i = 0; i < 25000; ++i) ppc::call(c, m, 0x8000800C);
  eq("longjmp unwinds call depth to the catching function", c.r[8], 1);
  eq("call depth restored after longjmp", c.call_depth, 0);
  d = ppc::aot_diagnostics();
  eq("diagnostic interpreted calls", d.interpreted_calls, 1);
  eq("diagnostic actual steps", d.interpreted_instructions, 6);
  eq("diagnostic instruction visits", d.instruction_visits, 6);
  eq("diagnostic distinct PC samples", d.pc_count, 6);
  eq("diagnostic branch/native/entry/exit transfers", d.transfer_count, 6);
  eq("diagnostic first entry event", uint32_t(d.transfers[0].kind), uint32_t(ppc::AotTransferKind::Entry));
  eq("diagnostic last exit event", uint32_t(d.transfers[5].kind), uint32_t(ppc::AotTransferKind::Exit));
  eq("diagnostic both attempts retained", d.missing[0].calls, 2);
  const uint64_t pc_hash = d.pc_sequence_hash;
  ppc::st32(c, m, entry + 8, 0x1c64ffff);  // mutate mulli immediate, then execute again
  c = {}; c.lr = done; c.r[4] = 2;
  ppc::call(c, m, entry);
  d = ppc::aot_diagnostics();
  eq("diagnostic changed code executed", c.r[3], 0xfffffffe);
  eq("diagnostic PC mutation counted", d.pcs[2].code_changes, 1);
  eq("diagnostic sequence hash changes", d.pc_sequence_hash != pc_hash, true);
  ppc::record_loaded_module("synthetic.dat", entry, 28,
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
  ppc::record_loaded_module("unhashed.dat", entry, 28, "not-a-sha256");
  d = ppc::aot_diagnostics();
  eq("module range recorded", d.modules[0].addr, entry);
  eq("module digest syntax valid", d.modules[0].sha256_valid, true);
  eq("bad digest explicitly unverified", d.modules[1].sha256_valid, false);

  constexpr const char* digest_a = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  constexpr const char* digest_b = "1123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  ppc::record_loaded_module("synthetic.dat", entry, 28, digest_a);
  ppc::record_loaded_module("synthetic.dat", entry, 28,
      "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF");
  d = ppc::aot_diagnostics();
  eq("identical verified reload deduplicates", d.module_count, 2);
  eq("load event count includes duplicates", d.loaded_module_records, 4);
  eq("per-identity load count", d.modules[0].loads, 3);
  eq("first load event remains stable", d.modules[0].first_event, 1);
  eq("last load event includes normalized digest", d.modules[0].last_event, 4);
  ppc::record_loaded_module("synthetic.dat", entry, 28, digest_b);
  ppc::record_loaded_module("synthetic.dat", entry, 32, digest_a);
  ppc::record_loaded_module("synthetic.dat", entry + 64, 28, digest_a);
  ppc::record_loaded_module("different.dat", entry, 28, digest_a);
  ppc::record_loaded_module("unhashed.dat", entry, 28, "not-a-sha256");
  ppc::record_loaded_module("synthetic.dat", entry, 28, digest_a);
  d = ppc::aot_diagnostics();
  eq("changed content/range/name kept separately", d.module_count, 7);
  eq("changed digest remains observable", std::string(d.modules[2].sha256) == digest_b, true);
  eq("changed size remains observable", d.modules[3].size, 32);
  eq("changed address remains observable", d.modules[4].addr, entry + 64);
  eq("changed digest starts new identity", d.modules[2].first_event, 5);
  eq("changed digest does not overwrite old identity", std::string(d.modules[0].sha256) == digest_a, true);
  eq("return to earlier contents increments original", d.modules[0].loads, 4);
  eq("return to earlier contents keeps latest event", d.modules[0].last_event, 10);
  eq("unverified duplicate retains separate event", d.modules[6].first_event, 9);
  const std::string long_name_a(110, 'x');
  const std::string long_name_b = std::string(109, 'x') + 'y';
  ppc::record_loaded_module(long_name_a.c_str(), entry, 28, digest_a);
  ppc::record_loaded_module(long_name_b.c_str(), entry, 28, digest_a);
  d = ppc::aot_diagnostics();
  eq("truncated names never collapse", d.module_count, 9);
  eq("truncated names are explicit", d.modules[7].name_truncated && d.modules[8].name_truncated, true);
  eq("module identity capacity", d.modules.size(), 128);
  const size_t retained_before_fill = d.module_count;
  for (size_t i = retained_before_fill; i < d.modules.size(); ++i) {
    const std::string name = "module-" + std::to_string(i) + ".dat";
    ppc::record_loaded_module(name.c_str(), 0x80010000 + uint32_t(i * 256), 64, digest_a);
  }
  d = ppc::aot_diagnostics();
  eq("module table fills at bounded capacity", d.module_count, d.modules.size());
  const uint64_t events_at_capacity = d.loaded_module_records;
  ppc::record_loaded_module("synthetic.dat", entry, 28,
      "2123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
  ppc::record_loaded_module("synthetic.dat", entry, 28, digest_a);
  ppc::record_loaded_module("overflow.dat", entry, 28, digest_a);
  d = ppc::aot_diagnostics();
  eq("overflow and duplicate events counted", d.loaded_module_records, events_at_capacity + 3);
  eq("overflow drops only unmatched events", d.unrecorded_modules, 2);
  eq("overflow never grows bounded storage", d.module_count, 128);
  eq("full-table duplicate still counts", d.modules[0].loads, 5);
  eq("full-table duplicate gets latest event", d.modules[0].last_event, events_at_capacity + 2);
  eq("changed overflow did not replace old digest", std::string(d.modules[0].sha256) == digest_a, true);
  uint64_t retained_events = 0, previous_first = 0;
  for (uint32_t i = 0; i < d.module_count; ++i) {
    const auto& module = d.modules[i];
    retained_events += module.loads;
    eq("module event range is valid", module.loads > 0 && module.first_event > previous_first &&
       module.first_event <= module.last_event && module.last_event <= d.loaded_module_records &&
       module.loads <= module.last_event - module.first_event + 1, true);
    if (module.loads == 1) eq("single-load event endpoints match", module.first_event, module.last_event);
    previous_first = module.first_event;
  }
  eq("retained plus dropped events reconcile", retained_events + d.unrecorded_modules, d.loaded_module_records);

  // Actual quantized memory helpers, including paired signed values and scale.
  constexpr uint32_t data = 0x80001000;
  ppc::st32(c, m, data, 0x7f800123); ppc::st32(c, m, data + 4, 1);
  ppc::psq_load(c, m, data, 1, 0, 0);
  eq("PSQ load sNaN payload", c.f[1].u0, 0x7ff0002460000000);
  eq("PSQ load subnormal", c.f[1].u1, 0x36a0000000000000);
  c.f[1].ps0 = -0.0;
  ppc::psq_store(c, m, data, 1, 0, 0);
  eq("PSQ store signed zero", ppc::ld32(c, m, data), 0x80000000);
  eq("PSQ store float flush", ppc::ld32(c, m, data + 4), 0);
  c.gqr[1] = 4 | (4 << 16);
  c.f[1].ps0 = 300; c.f[1].ps1 = -7;
  ppc::psq_store(c, m, data, 1, 0, 1);
  eq("PSQ unsigned saturation", ppc::ld16(c, m, data), 0xff00);
  c.gqr[2] = 6 | (6 << 16);
  c.f[1].ps0 = -129; c.f[1].ps1 = 128;
  ppc::psq_store(c, m, data, 1, 0, 2);
  eq("PSQ signed saturation", ppc::ld16(c, m, data), 0x807f);
  ppc::psq_load(c, m, data, 1, 0, 2);
  eq("PSQ signed lane0", ppc::double_to_bits(c.f[1].ps0), ppc::double_to_bits(-128));
  eq("PSQ signed lane1", ppc::double_to_bits(c.f[1].ps1), ppc::double_to_bits(127));
  c.gqr[3] = 5 | (2 << 8) | (5 << 16) | (2u << 24);
  c.f[1].ps0 = 3.5; c.f[1].ps1 = 7.25;
  ppc::psq_store(c, m, data, 1, 0, 3);
  eq("PSQ scaled u16 pair", ppc::ld32(c, m, data), 0x000e001d);
  ppc::psq_load(c, m, data, 1, 0, 3);
  eq("PSQ scaled load", c.f[1].u1, ppc::double_to_bits(7.25));
  c.f[1].u0 = 0x7ff8000000000000;
  faulted = false;
  try { ppc::psq_store(c, m, data, 1, 1, 1); } catch (const std::runtime_error&) { faulted = true; }
  eq("PSQ integer NaN fails explicitly", faulted, true);
  std::printf("PPC dispatch/PSQ: %u checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
