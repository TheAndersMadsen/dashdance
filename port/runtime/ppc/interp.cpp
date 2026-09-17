// Gekko interpreter for code that only exists in RAM at run time (Slippi's SlippiCSS.dat
// "mnFunction" code, any other dat-loaded or Gecko-generated routine). ppc::call falls back
// to it only when diagnostic interpretation is explicitly enabled. It is not part
// of the strict AOT execution contract. Arithmetic uses the emitter's numeric helpers.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "ppc.h"
#include "host.h"
#include <cstdio>
#include <atomic>

namespace ppc {
Fn lookup(uint32_t addr);

namespace {

inline uint32_t bits(uint32_t w, int start, int count) { return (w >> (32 - start - count)) & ((1u << count) - 1); }
inline uint32_t sext16(uint32_t v) { return (uint32_t)(int32_t)(int16_t)v; }
inline uint32_t sext12(uint32_t v) { return (v & 0x800) ? (v | 0xFFFFF000u) : v; }

struct Interp {
  Context& c;
  uint8_t* m;
  uint32_t entry_lr;
  uint32_t pc;
  bool done = false;

  Interp(Context& ctx, uint8_t* mem, uint32_t start) : c(ctx), m(mem), entry_lr(ctx.lr), pc(start) {}

  double& F0(int n) { return c.f[n].ps0; }
  double& F1(int n) { return c.f[n].ps1; }
  uint64_t& U0(int n) { return c.f[n].u0; }
  uint64_t& U1(int n) { return c.f[n].u1; }

  bool cond(uint32_t bo, uint32_t bi) {
    bool ok = true;
    if (!(bo & 4)) { --c.ctr; ok = (bo & 2) ? c.ctr == 0 : c.ctr != 0; }
    if (!(bo & 16)) { uint32_t bit = crbit(c, bi); ok = ok && ((bo & 8) ? bit != 0 : bit == 0); }
    return ok;
  }

  // Control transfer to `t` after the link register has been set as the instruction requires.
  // Host functions are called and then execution resumes at LR (what their blr would do).
  void transfer(uint32_t t, bool linked) {
    record_interpreter_transfer(pc, t, AotTransferKind::GuestBranch);
    for (;;) {
      if (Fn fn = lookup(t)) {
        record_interpreter_transfer(pc, t, AotTransferKind::CallToAot);
        { CallDepthScope depth(c, t); fn(c, m); }
        record_interpreter_transfer(t, linked ? pc + 4 : c.lr, AotTransferKind::ReturnFromAot);
        if (linked) { pc += 4; return; }         // bl to host code: continue after the call
        t = c.lr;                                 // tail transfer: the host function returned to LR
        if (t == entry_lr) { done = true; return; }
        linked = false;
        continue;
      }
      if (!fast(m, t)) fatal(c, "interpreter jump outside RAM", t);
      pc = t;
      return;
    }
  }

  uint32_t spr_get(uint32_t n) {
    switch (n) {
      case 1: return (c.so << 31) | (c.ov << 30) | (c.ca << 29);
      case 8: return c.lr;
      case 9: return c.ctr;
      case 1008: return c.hid0;
      case 920: return c.hid2;
      case 22: return c.dec;
      case 268: case 284: return (uint32_t)read_tb(c);
      case 269: case 285: return (uint32_t)(read_tb(c) >> 32);
      default:
        if (n >= 912 && n <= 919) return c.gqr[n - 912];
        return spr_read(c, n);
    }
  }
  void spr_put(uint32_t n, uint32_t v) {
    switch (n) {
      case 1: c.so = (v >> 31) & 1; c.ov = (v >> 30) & 1; c.ca = (v >> 29) & 1; return;
      case 8: c.lr = v; return;
      case 9: c.ctr = v; return;
      case 1008: c.hid0 = v; return;
      case 920: c.hid2 = v; return;
      case 22: c.dec = v; return;
      case 284: c.tb = (c.tb & 0xFFFFFFFF00000000ull) | v; return;
      case 285: c.tb = (c.tb & 0xFFFFFFFFull) | ((uint64_t)v << 32); return;
      default:
        if (n >= 912 && n <= 919) { c.gqr[n - 912] = v; return; }
        spr_write(c, n, v);
    }
  }

  void unsupported(uint32_t w) {
    host::log("interpreter: unsupported instruction %08X at %08X", w, pc);
    fatal(c, "interpreter: unsupported instruction", pc);
  }

  void step() {
    uint32_t w = ld32(c, m, pc);
    record_interpreter_instruction(pc, w);
    uint32_t op = w >> 26;
    uint32_t rd = bits(w, 6, 5), ra = bits(w, 11, 5), rb = bits(w, 16, 5), rs = rd;
    uint32_t simm = sext16(w & 0xFFFF), uimm = w & 0xFFFF;
    uint32_t rc = w & 1;
    uint32_t* R = c.r;
    uint32_t A = ra ? R[ra] : 0u;
    switch (op) {
      // ---------------- D-form integer ----------------
      case 3: break;  // twi
      case 7: R[rd] = R[ra] * simm; break;
      case 8: { uint32_t a = R[ra]; R[rd] = simm - a; c.ca = (a == 0) || carry(0u - a, simm); break; }
      case 10: cr_set_u(c, bits(w, 6, 3), R[ra], uimm); break;
      case 11: cr_set_s(c, bits(w, 6, 3), (int32_t)R[ra], (int32_t)simm); break;
      case 12: { uint32_t a = R[ra]; R[rd] = a + simm; c.ca = carry(a, simm); break; }
      case 13: { uint32_t a = R[ra]; R[rd] = a + simm; c.ca = carry(a, simm); cr0(c, R[rd]); break; }
      case 14: R[rd] = A + simm; break;
      case 15: R[rd] = A + (simm << 16); break;
      case 24: R[ra] = R[rs] | uimm; break;
      case 25: R[ra] = R[rs] | (uimm << 16); break;
      case 26: R[ra] = R[rs] ^ uimm; break;
      case 27: R[ra] = R[rs] ^ (uimm << 16); break;
      case 28: R[ra] = R[rs] & uimm; cr0(c, R[ra]); break;
      case 29: R[ra] = R[rs] & (uimm << 16); cr0(c, R[ra]); break;
      // ---------------- D-form loads/stores ----------------
      case 32: R[rd] = ld32(c, m, A + simm); break;
      case 33: { uint32_t ea = R[ra] + simm; R[rd] = ld32(c, m, ea); R[ra] = ea; break; }
      case 34: R[rd] = ld8(c, m, A + simm); break;
      case 35: { uint32_t ea = R[ra] + simm; R[rd] = ld8(c, m, ea); R[ra] = ea; break; }
      case 36: st32(c, m, A + simm, R[rs]); break;
      case 37: { uint32_t ea = R[ra] + simm; st32(c, m, ea, R[rs]); R[ra] = ea; break; }
      case 38: st8(c, m, A + simm, R[rs]); break;
      case 39: { uint32_t ea = R[ra] + simm; st8(c, m, ea, R[rs]); R[ra] = ea; break; }
      case 40: R[rd] = ld16(c, m, A + simm); break;
      case 41: { uint32_t ea = R[ra] + simm; R[rd] = ld16(c, m, ea); R[ra] = ea; break; }
      case 42: R[rd] = (uint32_t)(int32_t)(int16_t)ld16(c, m, A + simm); break;
      case 43: { uint32_t ea = R[ra] + simm; R[rd] = (uint32_t)(int32_t)(int16_t)ld16(c, m, ea); R[ra] = ea; break; }
      case 44: st16(c, m, A + simm, R[rs]); break;
      case 45: { uint32_t ea = R[ra] + simm; st16(c, m, ea, R[rs]); R[ra] = ea; break; }
      case 46: { uint32_t ea = A + simm; for (uint32_t i = rd; i < 32; ++i, ea += 4) R[i] = ld32(c, m, ea); break; }
      case 47: { uint32_t ea = A + simm; for (uint32_t i = rs; i < 32; ++i, ea += 4) st32(c, m, ea, R[i]); break; }
      case 48: F0(rd) = F1(rd) = float_bits_to_double(ld32(c, m, A + simm)); break;
      case 49: { uint32_t ea = R[ra] + simm; F0(rd) = F1(rd) = float_bits_to_double(ld32(c, m, ea)); R[ra] = ea; break; }
      case 50: U0(rd) = ld64(c, m, A + simm); break;
      case 51: { uint32_t ea = R[ra] + simm; U0(rd) = ld64(c, m, ea); R[ra] = ea; break; }
      case 52: st32(c, m, A + simm, double_to_float_bits(F0(rs))); break;
      case 53: { uint32_t ea = R[ra] + simm; st32(c, m, ea, double_to_float_bits(F0(rs))); R[ra] = ea; break; }
      case 54: st64(c, m, A + simm, U0(rs)); break;
      case 55: { uint32_t ea = R[ra] + simm; st64(c, m, ea, U0(rs)); R[ra] = ea; break; }
      // ---------------- paired-single quantized (D-form) ----------------
      case 56: psq_load(c, m, A + sext12(w & 0xFFF), rd, bits(w, 16, 1), bits(w, 17, 3)); break;
      case 57: { uint32_t ea = R[ra] + sext12(w & 0xFFF); psq_load(c, m, ea, rd, bits(w, 16, 1), bits(w, 17, 3)); R[ra] = ea; break; }
      case 60: psq_store(c, m, A + sext12(w & 0xFFF), rs, bits(w, 16, 1), bits(w, 17, 3)); break;
      case 61: { uint32_t ea = R[ra] + sext12(w & 0xFFF); psq_store(c, m, ea, rs, bits(w, 16, 1), bits(w, 17, 3)); R[ra] = ea; break; }
      // ---------------- rotates ----------------
      case 20: { uint32_t mk = mask(bits(w, 21, 5), bits(w, 26, 5)); R[ra] = (R[ra] & ~mk) | (rotl32(R[rs], rb) & mk); if (rc) cr0(c, R[ra]); break; }
      case 21: { uint32_t mk = mask(bits(w, 21, 5), bits(w, 26, 5)); R[ra] = rotl32(R[rs], rb) & mk; if (rc) cr0(c, R[ra]); break; }
      case 23: { uint32_t mk = mask(bits(w, 21, 5), bits(w, 26, 5)); R[ra] = rotl32(R[rs], R[rb] & 31) & mk; if (rc) cr0(c, R[ra]); break; }
      // ---------------- branches ----------------
      case 16: {
        uint32_t bo = rd, bi = ra;
        uint32_t bd = w & 0xFFFC; if (bd & 0x8000) bd |= 0xFFFF0000u;
        uint32_t t = (w & 2) ? bd : pc + bd;
        bool take = cond(bo, bi);
        if (!take) break;
        if (w & 1) c.lr = pc + 4;
        if (t <= pc) backedge(c);
        transfer(t, (w & 1) != 0);
        return;
      }
      case 18: {
        uint32_t li = w & 0x03FFFFFC; if (li & 0x02000000) li |= 0xFC000000u;
        uint32_t t = (w & 2) ? li : pc + li;
        if (w & 1) c.lr = pc + 4;
        if (t <= pc) backedge(c);
        transfer(t, (w & 1) != 0);
        return;
      }
      case 17: syscall(c, m); break;
      case 19: {
        uint32_t xo = bits(w, 21, 10);
        uint32_t bo = rd, bi = ra;
        if (xo == 16) {                                     // bclr
          if (!cond(bo, bi)) break;
          uint32_t t = c.lr;
          if (w & 1) c.lr = pc + 4;
          if (t == entry_lr) { done = true; return; }       // return to the host caller (blrl: with the new LR)
          if (!(w & 1) && lookup(t)) fatal(c, "interpreter: blr into a function entry", t);
          transfer(t, (w & 1) != 0);
          return;
        }
        if (xo == 528) {                                    // bcctr
          if (!cond(bo | 4, bi)) break;                     // no CTR decrement form
          uint32_t t = c.ctr;
          if (w & 1) c.lr = pc + 4;
          transfer(t, (w & 1) != 0);
          return;
        }
        if (xo == 0) { c.cr[bits(w, 6, 3)] = c.cr[bits(w, 11, 3)]; break; }   // mcrf
        if (xo == 50) { done = true; return; }              // rfi
        if (xo == 150) break;                               // isync
        uint32_t a = crbit(c, ra), b = crbit(c, rb), v;
        switch (xo) {
          case 257: v = a & b; break;           // crand
          case 449: v = a | b; break;           // cror
          case 193: v = a ^ b; break;           // crxor
          case 225: v = !(a & b); break;        // crnand
          case 33: v = !(a | b); break;         // crnor
          case 289: v = !(a ^ b); break;        // creqv
          case 129: v = a & !b; break;          // crandc
          case 417: v = a | !b; break;          // crorc
          default: unsupported(w); return;
        }
        crbit_set(c, rd, v);
        break;
      }
      // ---------------- X-form integer ----------------
      case 31: {
        uint32_t xo = bits(w, 21, 10), xo9 = xo & 0x1FF;
        uint32_t B = R[rb];
        bool arith = true;
        switch (xo9) {
          case 8: { uint32_t a = R[ra]; R[rd] = B - a; c.ca = (a == 0) || carry(B, 0u - a); break; }
          case 10: { uint32_t a = R[ra]; R[rd] = a + B; c.ca = carry(a, B); break; }
          case 11: R[rd] = (uint32_t)(((uint64_t)R[ra] * (uint64_t)B) >> 32); break;
          case 40: R[rd] = B - R[ra]; break;
          case 75: R[rd] = mulhw(R[ra], B); break;
          case 104: R[rd] = 0u - R[ra]; break;
          case 136: { uint32_t a = ~R[ra], k = c.ca; R[rd] = a + B + k; c.ca = carry(a, B) || carry(a + B, k); break; }
          case 138: { uint32_t a = R[ra], k = c.ca; R[rd] = a + B + k; c.ca = carry(a, B) || (k && carry(a + B, k)); break; }
          case 200: { uint32_t a = ~R[ra], k = c.ca; R[rd] = a + k; c.ca = carry(a, k); break; }
          case 202: { uint32_t a = R[ra], k = c.ca; R[rd] = a + k; c.ca = carry(a, k); break; }
          case 232: { uint32_t a = ~R[ra], k = c.ca; R[rd] = a + k - 1u; c.ca = carry(a, k - 1u); break; }
          case 234: { uint32_t a = R[ra], k = c.ca; R[rd] = a + k - 1u; c.ca = carry(a, k - 1u); break; }
          case 235: R[rd] = R[ra] * B; break;
          case 266: R[rd] = R[ra] + B; break;
          case 459: R[rd] = divwu(R[ra], B); break;
          case 491: R[rd] = divw((int32_t)R[ra], (int32_t)B); break;
          default: arith = false; break;
        }
        if (arith && (xo == xo9 || xo == xo9 + 512)) { if (rc) cr0(c, R[rd]); break; }
        uint32_t ea = A + B;
        switch (xo) {
          case 0: cr_set_s(c, bits(w, 6, 3), (int32_t)R[ra], (int32_t)B); break;
          case 4: break;                                                     // tw
          case 19: R[rd] = mfcr(c); break;
          case 20: R[rd] = ld32(c, m, ea); break;                            // lwarx
          case 23: R[rd] = ld32(c, m, ea); break;
          case 24: R[ra] = (B & 0x20) ? 0u : (R[rs] << (B & 31)); if (rc) cr0(c, R[ra]); break;
          case 26: R[ra] = cntlzw(R[rs]); if (rc) cr0(c, R[ra]); break;
          case 28: R[ra] = R[rs] & B; if (rc) cr0(c, R[ra]); break;
          case 32: cr_set_u(c, bits(w, 6, 3), R[ra], B); break;
          case 54: case 86: case 246: case 278: case 470: case 982: case 306: case 566: case 598: case 854: break;
          case 55: { uint32_t e = R[ra] + B; R[rd] = ld32(c, m, e); R[ra] = e; break; }
          case 60: R[ra] = R[rs] & ~B; if (rc) cr0(c, R[ra]); break;
          case 83: R[rd] = c.msr; break;
          case 87: R[rd] = ld8(c, m, ea); break;
          case 119: { uint32_t e = R[ra] + B; R[rd] = ld8(c, m, e); R[ra] = e; break; }
          case 124: R[ra] = ~(R[rs] | B); if (rc) cr0(c, R[ra]); break;
          case 144: mtcrf(c, bits(w, 12, 8), R[rs]); break;
          case 146: mtmsr(c, R[rs]); break;
          case 150: st32(c, m, ea, R[rs]); c.cr[0] = (uint8_t)(2 | c.so); break;   // stwcx
          case 151: st32(c, m, ea, R[rs]); break;
          case 183: { uint32_t e = R[ra] + B; st32(c, m, e, R[rs]); R[ra] = e; break; }
          case 210: case 242: break;                                         // mtsr, mtsrin
          case 215: st8(c, m, ea, R[rs]); break;
          case 247: { uint32_t e = R[ra] + B; st8(c, m, e, R[rs]); R[ra] = e; break; }
          case 279: R[rd] = ld16(c, m, ea); break;
          case 284: R[ra] = ~(R[rs] ^ B); if (rc) cr0(c, R[ra]); break;
          case 311: { uint32_t e = R[ra] + B; R[rd] = ld16(c, m, e); R[ra] = e; break; }
          case 316: R[ra] = R[rs] ^ B; if (rc) cr0(c, R[ra]); break;
          case 339: R[rd] = spr_get(bits(w, 16, 5) << 5 | bits(w, 11, 5)); break;
          case 343: R[rd] = (uint32_t)(int32_t)(int16_t)ld16(c, m, ea); break;
          case 371: R[rd] = ((bits(w, 16, 5) << 5 | bits(w, 11, 5)) == 268) ? (uint32_t)read_tb(c) : (uint32_t)(read_tb(c) >> 32); break;
          case 375: { uint32_t e = R[ra] + B; R[rd] = (uint32_t)(int32_t)(int16_t)ld16(c, m, e); R[ra] = e; break; }
          case 407: st16(c, m, ea, R[rs]); break;
          case 412: R[ra] = R[rs] | ~B; if (rc) cr0(c, R[ra]); break;
          case 439: { uint32_t e = R[ra] + B; st16(c, m, e, R[rs]); R[ra] = e; break; }
          case 444: R[ra] = R[rs] | B; if (rc) cr0(c, R[ra]); break;
          case 467: spr_put(bits(w, 16, 5) << 5 | bits(w, 11, 5), R[rs]); break;
          case 476: R[ra] = ~(R[rs] & B); if (rc) cr0(c, R[ra]); break;
          case 512: c.cr[bits(w, 6, 3)] = (uint8_t)((c.so << 3) | (c.ov << 2) | (c.ca << 1)); c.so = c.ov = c.ca = 0; break;
          case 534: R[rd] = ld32r(c, m, ea); break;
          case 535: F0(rd) = F1(rd) = float_bits_to_double(ld32(c, m, ea)); break;
          case 536: R[ra] = (B & 0x20) ? 0u : (R[rs] >> (B & 31)); if (rc) cr0(c, R[ra]); break;
          case 567: { uint32_t e = R[ra] + B; F0(rd) = F1(rd) = float_bits_to_double(ld32(c, m, e)); R[ra] = e; break; }
          case 595: case 659: R[rd] = 0; break;                              // mfsr, mfsrin
          case 597: lswi(c, m, A, rd, rb ? rb : 32); break;
          case 599: U0(rd) = ld64(c, m, ea); break;
          case 631: { uint32_t e = R[ra] + B; U0(rd) = ld64(c, m, e); R[ra] = e; break; }
          case 662: st32r(c, m, ea, R[rs]); break;
          case 663: st32(c, m, ea, double_to_float_bits(F0(rs))); break;
          case 695: { uint32_t e = R[ra] + B; st32(c, m, e, double_to_float_bits(F0(rs))); R[ra] = e; break; }
          case 725: stswi(c, m, A, rs, rb ? rb : 32); break;
          case 727: st64(c, m, ea, U0(rs)); break;
          case 759: { uint32_t e = R[ra] + B; st64(c, m, e, U0(rs)); R[ra] = e; break; }
          case 790: R[rd] = ld16r(c, m, ea); break;
          case 792: R[ra] = sraw(c, R[rs], B); if (rc) cr0(c, R[ra]); break;
          case 824: R[ra] = srawi(c, R[rs], rb); if (rc) cr0(c, R[ra]); break;
          case 918: st16r(c, m, ea, R[rs]); break;
          case 922: R[ra] = (uint32_t)(int32_t)(int16_t)R[rs]; if (rc) cr0(c, R[ra]); break;
          case 954: R[ra] = (uint32_t)(int32_t)(int8_t)R[rs]; if (rc) cr0(c, R[ra]); break;
          case 983: st32(c, m, ea, (uint32_t)U0(rs)); break;
          case 1014: dcbz(c, m, ea); break;
          default: unsupported(w); return;
        }
        break;
      }
      // ---------------- floating point ----------------
      case 59: {
        uint32_t xo = bits(w, 26, 5), fa = ra, fb = rb, fc = bits(w, 21, 5);
        double a = F0(fa), b = F0(fb), k = F0(fc), r;
        switch (xo) {
          case 18: r = fs(fdiv(a, b)); break;
          case 20: r = fs(fsub(a, b)); break;
          case 21: r = fs(fadd(a, b)); break;
          case 24: r = fres(b); break;
          case 25: r = fs(fmul(a, f25(k))); break;
          case 28: r = fmsubs(a, f25(k), b); break;
          case 29: r = fmadds(a, f25(k), b); break;
          case 30: r = fnmsubs(a, f25(k), b); break;
          case 31: r = fnmadds(a, f25(k), b); break;
          default: unsupported(w); return;
        }
        F0(rd) = F1(rd) = r;
        break;
      }
      case 63: {
        uint32_t xo5 = bits(w, 26, 5), xo = bits(w, 21, 10), fa = ra, fb = rb, fc = bits(w, 21, 5);
        double a = F0(fa), b = F0(fb), k = F0(fc);
        switch (xo5) {
          case 18: F0(rd) = fdiv(a, b); return_ok: pc += 4; return;
          case 20: F0(rd) = fsub(a, b); goto return_ok;
          case 21: F0(rd) = fadd(a, b); goto return_ok;
          case 23: F0(rd) = (a >= -0.0) ? k : b; goto return_ok;
          case 25: F0(rd) = fmul(a, k); goto return_ok;
          case 26: F0(rd) = frsqrte(b); goto return_ok;
          case 28: F0(rd) = fmsub(a, k, b); goto return_ok;
          case 29: F0(rd) = fmadd(a, k, b); goto return_ok;
          case 30: F0(rd) = fnmsub(a, k, b); goto return_ok;
          case 31: F0(rd) = fnmadd(a, k, b); goto return_ok;
          default: break;
        }
        switch (xo) {
          case 0: case 32: fcmp(c, bits(w, 6, 3), a, b); break;
          case 12: F0(rd) = F1(rd) = fs(b); break;
          case 14: U0(rd) = fctiw(b, false); break;
          case 15: U0(rd) = fctiw(b, true); break;
          case 38: c.fpscr |= 0x80000000u >> rd; update_fp_environment(c); break;
          case 40: U0(rd) = U0(fb) ^ 0x8000000000000000ull; break;
          case 64: c.cr[bits(w, 6, 3)] = (uint8_t)((c.fpscr >> (28 - 4 * bits(w, 11, 3))) & 15); break;
          case 70: c.fpscr &= ~(0x80000000u >> rd); update_fp_environment(c); break;
          case 72: U0(rd) = U0(fb); break;
          case 134: { uint32_t sh = 28 - 4 * bits(w, 6, 3); c.fpscr = (c.fpscr & ~(0xFu << sh)) | (bits(w, 16, 4) << sh); update_fp_environment(c); break; }
          case 136: U0(rd) = U0(fb) | 0x8000000000000000ull; break;
          case 264: U0(rd) = U0(fb) & 0x7FFFFFFFFFFFFFFFull; break;
          case 583: U0(rd) = 0xFFF8000000000000ull | c.fpscr; break;
          case 711: {
            uint32_t fm = bits(w, 7, 8), mk = 0;
            for (int i = 0; i < 8; ++i) if (fm & (0x80 >> i)) mk |= 0xFu << (28 - 4 * i);
            c.fpscr = (c.fpscr & ~mk) | ((uint32_t)U0(fb) & mk); update_fp_environment(c); break;
          }
          default: unsupported(w); return;
        }
        break;
      }
      // ---------------- paired single ----------------
      case 4: {
        uint32_t xo6 = bits(w, 25, 6), xo5 = bits(w, 26, 5), xo = bits(w, 21, 10), fa = ra, fb = rb, fc = bits(w, 21, 5);
        if (xo6 == 6 || xo6 == 7 || xo6 == 38 || xo6 == 39) {
          uint32_t ea = (xo6 & 32) ? R[ra] + R[rb] : A + R[rb];
          if (xo6 & 1) psq_store(c, m, ea, rs, bits(w, 21, 1), bits(w, 22, 3));
          else psq_load(c, m, ea, rd, bits(w, 21, 1), bits(w, 22, 3));
          if (xo6 & 32) R[ra] = ea;
          break;
        }
        double a0 = F0(fa), a1 = F1(fa), b0 = F0(fb), b1 = F1(fb), k0 = F0(fc), k1 = F1(fc), x, y;
        bool ps = true;
        switch (xo5) {
          case 10: x = fadd(a0, b1); y = k1; break;
          case 11: x = k0; y = fadd(a0, b1); break;
          case 12: { double k = f25(k0); x = fmul(a0, k); y = fmul(a1, k); break; }
          case 13: { double k = f25(k1); x = fmul(a0, k); y = fmul(a1, k); break; }
          case 14: { double k = f25(k0); x = fmadds(a0, k, b0); y = fmadds(a1, k, b1); break; }
          case 15: { double k = f25(k1); x = fmadds(a0, k, b0); y = fmadds(a1, k, b1); break; }
          case 18: x = fdiv(a0, b0); y = fdiv(a1, b1); break;
          case 20: x = fsub(a0, b0); y = fsub(a1, b1); break;
          case 21: x = fadd(a0, b0); y = fadd(a1, b1); break;
          case 23: F0(rd) = (a0 >= -0.0) ? k0 : b0; F1(rd) = (a1 >= -0.0) ? k1 : b1; ps = false; goto ps_done;
          case 24: x = fres(b0); y = fres(b1); break;
          case 25: x = fmul(a0, f25(k0)); y = fmul(a1, f25(k1)); break;
          case 26: x = frsqrte(b0); y = frsqrte(b1); break;
          case 28: x = fmsubs(a0, f25(k0), b0); y = fmsubs(a1, f25(k1), b1); break;
          case 29: x = fmadds(a0, f25(k0), b0); y = fmadds(a1, f25(k1), b1); break;
          case 30: x = fnmsubs(a0, f25(k0), b0); y = fnmsubs(a1, f25(k1), b1); break;
          case 31: x = fnmadds(a0, f25(k0), b0); y = fnmadds(a1, f25(k1), b1); break;
          default: ps = false; break;
        }
        if (ps) { F0(rd) = fs(x); F1(rd) = fs(y); break; }
        switch (xo) {
          case 0: case 32: fcmp(c, bits(w, 6, 3), a0, b0); break;
          case 64: case 96: fcmp(c, bits(w, 6, 3), a1, b1); break;
          case 40: { uint64_t p = U0(fb) ^ 0x8000000000000000ull, q = U1(fb) ^ 0x8000000000000000ull; U0(rd) = p; U1(rd) = q; break; }
          case 72: { uint64_t p = U0(fb), q = U1(fb); U0(rd) = p; U1(rd) = q; break; }
          case 136: { uint64_t p = U0(fb) | 0x8000000000000000ull, q = U1(fb) | 0x8000000000000000ull; U0(rd) = p; U1(rd) = q; break; }
          case 264: { uint64_t p = U0(fb) & 0x7FFFFFFFFFFFFFFFull, q = U1(fb) & 0x7FFFFFFFFFFFFFFFull; U0(rd) = p; U1(rd) = q; break; }
          case 528: { uint64_t p = U0(fa), q = U0(fb); U0(rd) = p; U1(rd) = q; break; }
          case 560: { uint64_t p = U0(fa), q = U1(fb); U0(rd) = p; U1(rd) = q; break; }
          case 592: { uint64_t p = U1(fa), q = U0(fb); U0(rd) = p; U1(rd) = q; break; }
          case 624: { uint64_t p = U1(fa), q = U1(fb); U0(rd) = p; U1(rd) = q; break; }
          case 1014: dcbz(c, m, A + R[rb]); break;
          default: unsupported(w); return;
        }
      ps_done:
        break;
      }
      default: unsupported(w); return;
    }
    pc += 4;
  }
};

std::atomic<uint64_t> g_interpreted_calls{0}, g_interpreted_insns{0};

}  // namespace

void interpret(Context& c, uint8_t* m, uint32_t addr) {
  record_interpreter_entry(c, addr);
  if (!fast(m, addr) || (addr & 3)) fatal(c, "call to unmapped guest address", addr);
  g_interpreted_calls.fetch_add(1, std::memory_order_relaxed);
  enter(c, addr);
  Interp in(c, m, addr);
  record_interpreter_transfer(c.lr, addr, AotTransferKind::Entry);
  while (!in.done) { in.step(); g_interpreted_insns.fetch_add(1, std::memory_order_relaxed); }
  record_interpreter_transfer(in.pc, c.lr, AotTransferKind::Exit);
}

void interpreter_stats(uint64_t* calls, uint64_t* insns) {
  *calls = g_interpreted_calls.load(std::memory_order_relaxed);
  *insns = g_interpreted_insns.load(std::memory_order_relaxed);
}

}  // namespace ppc
