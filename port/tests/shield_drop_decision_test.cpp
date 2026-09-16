// Shield-drop / spot-dodge decision matrix against the translated guest code.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reproduces the decision that desynced in MAC_FIXES.md entry 2: remote Marth
// shields on a Pokémon Stadium side platform, the stick dives from -0.39 to
// -0.96, real Melee passes through the platform (action 244) at the -0.74
// frame, Dashdance kept GuardOn and spot-dodged (EscapeN, 235) one frame later.
//
// The test drives the ACTUAL translated functions (the same compiled code the
// game runs) with the shield-drop thresholds extracted from the disc's
// PlCo.dat (ftLoadCommonData):
//   x314 = -0.7   spot-dodge stick-y threshold
//   x318 = 4      spot-dodge stick-y tilt window
//   x320 = 4      spot-dodge stick-x tilt window
//   x464 = 0.66   platform-pass stick-y threshold
//   x468 = 6.0    platform-pass tilt window
// and with the Slippi GCT mapped into fake guest RAM at its real address
// (0x8065CC80), so the UCF 0.84 Shield Drop cave (hooked at 0x800998A4 inside
// ftCo_80099894 "enter spot dodge") loads its real constants (0.8, 80.0,
// 0.0001) exactly as it does in a match.
//
// Console semantics being verified (decoded from the 1.02 DOL + the caves):
//   spot dodge from shield = lstick.y <= -0.7 && tilt_y < 4 (a held-down
//                            c-stick also triggers; it is neutral here)
//   UCF shield-drop skip   = cstick.y > -0.7 && tilt_x >= 4 && lstick.y > -0.8
//                            && floor.index != -1 && floor.flags & 0x100
//                            && q(|y|)^2 + q(|x|)^2 > 6400, where
//                            q(v) = trunc(v*80 - 0.0001) + 2
//                            (skip = do not enter the spot dodge, so the IASA
//                            chain reaches the platform-pass check)
//   platform pass (drop)   = shield held && lstick.y <= -0.66 && tilt_y < 6
//                            && on a drop-through platform
//
// Standalone build + run (repo root, after a normal build):
//   tools/mac/shield_drop_test.sh

#include "gecko_data.h"
#include "host.h"
#include "ppc.h"

#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace guest {
void f_8009980C(ppc::Context&, uint8_t*);   // spot-dodge decision (Guard IASA)
void f_80099894(ppc::Context&, uint8_t*);   // enter spot dodge (UCF hook)
void f_8009A080(ppc::Context&, uint8_t*);   // platform-pass decision (Guard IASA)
}  // namespace guest

namespace {

constexpr uint32_t kFighter = 0x81200000u;   // fake Fighter
constexpr uint32_t kGobj = 0x81300000u;      // fake Fighter_GObj
constexpr uint32_t kCommon = 0x81000000u;    // fake ftCommonData
constexpr uint32_t kPCommonPtr = 0x804D6554u;
constexpr uint32_t kStack = 0x816F0000u;
constexpr uint32_t kToc = 0x81107340u;       // so -0x7340(r2) hits 0x81100000
constexpr uint32_t kGctBase = 0x8065CC80u;   // the GCT address the build pins
constexpr uint32_t kMotionId = kFighter + 0x10;
constexpr uint32_t kMagicLr = 0x7C7D5A00u;   // fake saved return of GuardOn_IASA

unsigned checks = 0, failures = 0;

void expect(const char* what, bool got, bool want) {
  ++checks;
  if (got != want) {
    ++failures;
    std::printf("  FAIL %s: got %d want %d\n", what, got, want);
  }
}

// Guest RAM: MAP_SHARED so a forked child's verdict writes stay visible.
uint8_t* guest_ram() {
  static uint8_t* p = [] {
    void* p = mmap(nullptr, ppc::RAM_SIZE + 64, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    std::memset(p, 0, ppc::RAM_SIZE + 64);
    return static_cast<uint8_t*>(p);
  }();
  return p;
}
std::vector<uint8_t>& pristine_ram() {
  static std::vector<uint8_t> p;
  return p;
}

struct Rig {
  ppc::Context c{};

  Rig() {
    uint8_t* ram = guest_ram();
    std::memcpy(&ram[kGctBase - ppc::RAM_BASE], gecko::slippi_gct, gecko::slippi_gct_size);
    // ftCommonData thresholds (values extracted from the disc's PlCo.dat).
    put_f(kCommon + 0x314, -0.7f);
    put_u(kCommon + 0x318, 4);
    put_u(kCommon + 0x320, 4);
    put_f(kCommon + 0x464, 0.66f);
    put_f(kCommon + 0x468, 6.0f);
    put_u(kPCommonPtr, kCommon);
    // The integer-to-double constant ftCo_8009A080 loads via -0x7340(r2).
    put_d(0x81100000, 0x4330000000000000ull);
    // Fighter: Marth (FTKIND_MARS = 12), shield held, standing on a
    // drop-through platform floor, c-stick neutral.
    put_u(kFighter + 4, 12);            // kind
    put_u(kFighter + 0x65C, 0x80000000u); // held_buttons: shield (HSD_PAD_LR = 1<<31)
    put_u(kFighter + 0x83C, 0);         // coll_data.floor.index 0 (floor table below)
    put_u(kFighter + 0x840, 0x100);     // coll_data.floor.flags: drop-through
    put_u(kGobj + 0x2C, kFighter);      // gobj->user_data
    // Fake the stage floor table mpColl_IsOnPlatform consults:
    //   [0x804D64B4] -> { ..., int count @ +12 }
    //   [0x804D64BC] -> per-index array of floor entries; entry+14 = flags u16
    //   with bit 0x100 = drop-through platform.
    put_u(0x804D64B4, 0x81400000);
    put_u(0x81400000 + 12, 1);          // floor count (must exceed the index)
    put_u(0x804D64BC, 0x81401000);
    put_u(0x81401000, 0x81402000);      // floors[0] -> entry
    put_u(0x81402000 + 12, 0x01000100); // +12 pad, +14 flags: 0x100 drop-through
    // Extended-cave flag table: [0x80075460] = 0 keeps the hook inert (the
    // unranked default; flag 1 behaves identically, >1 force-drops).
    // Fake saved LR slot the UCF early return reads (GuardOn_IASA's).
    put_u(kStack + 0x1C, kMagicLr);
    pristine_ram().assign(guest_ram(), guest_ram() + ppc::RAM_SIZE + 64);
    reset_ctx();
  }

  void reset_ctx() {
    c = ppc::Context{};
    c.r[13] = kPCommonPtr + 0x514C;  // the guest small-data base the game uses
    c.r[2] = kToc;
    c.r[1] = kStack;
    c.r[3] = kGobj;
  }

  void set_stick(float x, float y) {
    put_f(kFighter + 0x620, x);       // input.lstick[0].x
    put_f(kFighter + 0x624, y);       // input.lstick[0].y
    put_f(kFighter + 0x628, 0.0f);    // input.lstick[1]
    put_f(kFighter + 0x62C, 0.0f);
    put_f(kFighter + 0x638, 0.0f);    // input.cstick[0].x
    put_f(kFighter + 0x63C, 0.0f);    // input.cstick[0].y
  }
  void set_tilt(uint8_t tilt_x, uint8_t tilt_y) {
    guest_ram()[kFighter + 0x670 - ppc::RAM_BASE] = tilt_x;
    guest_ram()[kFighter + 0x671 - ppc::RAM_BASE] = tilt_y;
  }
  void put_u(uint32_t guest, uint32_t v) {
    v = __builtin_bswap32(v);
    std::memcpy(&guest_ram()[guest - ppc::RAM_BASE], &v, 4);
  }
  void put_f(uint32_t guest, float v) {
    uint32_t b;
    std::memcpy(&b, &v, 4);
    put_u(guest, b);
  }
  void put_d(uint32_t guest, uint64_t v) {
    v = __builtin_bswap64(v);
    std::memcpy(&guest_ram()[guest - ppc::RAM_BASE], &v, 8);
  }
  uint32_t get_u(uint32_t guest) {
    uint32_t v;
    std::memcpy(&v, &guest_ram()[guest - ppc::RAM_BASE], 4);
    return __builtin_bswap32(v);
  }
};

// Calls fn in a forked child (a state-change dive the fake fighter cannot
// fully survive is contained) and reports the child's r3. A pristine RAM image
// is restored first, then setup() applies the case.
struct Run {
  bool ok;
  uint32_t r3;
};

Run run_isolated(Rig& rig, const std::function<void(Rig&)>& setup,
                 void (*fn)(ppc::Context&, uint8_t*)) {
  std::memcpy(guest_ram(), pristine_ram().data(), pristine_ram().size());
  setup(rig);
  auto* shared = static_cast<uint32_t*>(
      mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
  shared[0] = 0xDEADBEEF;
  pid_t pid = fork();
  if (pid == 0) {
    alarm(10);
    fn(rig.c, guest_ram());
    shared[0] = 0xC0DE0000u | (rig.c.r[3] & 1u);
    _exit(0);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  Run r{false, 0};
  if (shared[0] != 0xDEADBEEF) {
    r.ok = true;
    r.r3 = shared[0] & 1u;
  }
  munmap(shared, 4096);
  return r;
}

}  // namespace

int main() {
  ppc::ScopedGuestFpEnvironment fp(0);
  Rig rig;

  // The console ground truth (Slippi resim, MAC_FIXES entry 2): GuardOn in
  // both sims at 3789, real Melee enters Pass at 3791, Dashdance spot-dodged
  // at 3792. Stick dive from the log, as the game sees it (byte / 127):
  const float y[4] = {-50.0f / 127, -76.0f / 127, -94.0f / 127, -122.0f / 127};
  const char* yn[4] = {"-0.39", "-0.60", "-0.74", "-0.96"};
  const float diag = -84.0f / 127;  // |x| ~ 0.66, the dash-dance diagonal

  std::printf("spot-dodge decision as GuardOn_IASA sees it (tilt_y=2, tilt_x=4, x=%.3f)\n", diag);
  for (int i = 0; i < 4; ++i) {
    Run r = run_isolated(rig, [&](Rig& rr) { rr.set_stick(diag, y[i]); rr.set_tilt(4, 2); },
                         guest::f_8009980C);
    // r3 = 1 means the spot dodge enters (EscapeN comes out); r3 = 0 means the
    // IASA chain continues to the platform-pass check - either the stick is
    // above the -0.7 spot-dodge threshold or the UCF hook suppressed the enter.
    std::printf("  y=%s: %s\n", yn[i], r.r3 ? "SPOT DODGE (EscapeN)" : "no spot dodge");
    char label[64];
    std::snprintf(label, sizeof label, "dodge y=%s", yn[i]);
    expect(label, r.r3 != 0, i == 3);
  }

  std::printf("platform-pass check alone (shield held, on drop-through floor)\n");
  for (int i = 0; i < 4; ++i) {
    for (uint8_t tilt : {uint8_t(2), uint8_t(4), uint8_t(5), uint8_t(6)}) {
      Run r = run_isolated(rig, [&](Rig& rr) { rr.set_stick(diag, y[i]); rr.set_tilt(4, tilt); },
                           guest::f_8009A080);
      std::printf("  y=%s tilt_y=%u: %s\n", yn[i], tilt,
                  r.r3 ? "PASS (drop)" : "stay GuardOn");
      char label[80];
      std::snprintf(label, sizeof label, "pass y=%s tilt=%u", yn[i], tilt);
      bool want = i >= 2 && tilt < 6;  // y must be past -0.66, tilt under 6
      expect(label, r.r3 != 0, want);
    }
  }

  std::printf("vanilla spot-dodge window (vertical stick: UCF magnitude fails)\n");
  {
    auto dodge = [&](float yy) {
      return run_isolated(rig, [&](Rig& rr) { rr.set_stick(0.0f, yy); rr.set_tilt(4, 2); },
                          guest::f_8009980C);
    };
    expect("y=-0.39: above spot-dodge threshold", dodge(y[0]).r3 == 0, true);
    expect("y=-0.60: above spot-dodge threshold", dodge(y[1]).r3 == 0, true);
    expect("y=-0.74 tilt_y=2: spot dodge fires", dodge(y[2]).r3 == 1, true);
    expect("y=-0.96 tilt_y=2: spot dodge fires", dodge(y[3]).r3 == 1, true);
  }
  std::printf("UCF tilt_x gate (tilt_x < 4 disables the skip)\n");
  {
    Run r = run_isolated(rig, [&](Rig& rr) { rr.set_stick(diag, y[2]); rr.set_tilt(2, 2); },
                         guest::f_8009980C);
    expect("y=-0.74 diagonal tilt_x=2: spot dodge fires", r.r3 == 1, true);
  }

  std::printf("%u checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
