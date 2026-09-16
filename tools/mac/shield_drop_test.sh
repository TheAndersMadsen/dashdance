#!/bin/sh
# Build and run the shield-drop decision test: drives the ACTUAL translated
# guest functions (spot-dodge decision, UCF Shield Drop hook, platform-pass
# check) with the documented stick values from the Pokémon Stadium desync, and
# verifies the drop / no-drop outcomes match real Melee (see MAC_FIXES.md #2).
#
# Usage: tools/mac/shield_drop_test.sh [build-dir]     (default build/mac)
set -e
cd "$(dirname "$0")/../.."
BUILD=${1:-build/mac}
[ -f "$BUILD/port/CMakeFiles/guest.dir" -o -d "$BUILD/port/CMakeFiles/guest.dir" ] || {
  echo "shield_drop_test: $BUILD has no translated guest objects; run the normal build first" >&2
  exit 1
}
OUT=$BUILD/shield_drop_decision_test
OBJS=$OUT.objs
find "$BUILD/port/CMakeFiles/guest.dir" -name '*.o' > "$OBJS"
for o in numeric ppc_runtime interp; do
  find "$BUILD/port/CMakeFiles/runtime_mac.dir/runtime/ppc" -name "$o.cpp.o" >> "$OBJS"
done
FLAGS="-O1 -std=c++20 -fno-fast-math -ffp-model=strict -ffp-contract=off"
INC="-I port/runtime/ppc -I build/mac/generated/guest -I port/runtime/hle -I port/runtime/host"
clang++ $FLAGS $INC -c port/tests/shield_drop_test_stubs.cpp -o "$OUT.stubs.o"
clang++ $FLAGS $INC -c port/tests/shield_drop_decision_test.cpp -o "$OUT.test.o"
echo "$OUT.stubs.o" >> "$OBJS"
echo "$OUT.test.o" >> "$OBJS"
clang++ @"$OBJS" -o "$OUT"
"$OUT"
