#!/usr/bin/env python3
"""Inspect a guest RAM snapshot around an address: what the object is, and what points at it.

Usage: tools/mac/crashram.py RAM.bin ADDR [--words N]

RAM.bin is a 24 MB guest RAM snapshot: session-<stamp>.ram written next to the session log when Dashdance hits
a guest fault, or ram_<frame>.bin from the desync tools. ADDR is usually a register from the FATAL block.

Prints the words at ADDR, and every RAM word that equals ADDR with its symbol, or its owning fighter when the
word sits inside a Fighter struct. When ADDR looks like an HSD_JObj (its first word is a JObj class pointer),
it also decodes the JObj and walks its parent chain, reporting a cycle.
"""
import argparse
import bisect
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RAM_BASE, RAM_SIZE = 0x80000000, 0x01800000
JOBJ_CLASSES = {0x803C08F8, 0x80406708}  # HSD_JObj class info pointers seen in fighter bones (NTSC 1.02)
PLAYER_BLOCKS, PLAYER_BLOCK_SIZE, FIGHTER_SIZE = 0x80453080, 0xE90, 0x23EC


def load_symbols():
    syms = []
    path = ROOT / "deps/melee/config/GALE01/symbols.txt"
    if path.exists():
        for line in path.read_text().splitlines():
            try:
                name, rest = line.split("=", 1)
                addr = int(rest.split(";")[0].split(":")[1], 16)
                syms.append((addr, name.strip()))
            except (ValueError, IndexError):
                pass
    syms.sort()
    return syms


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("ram", type=Path)
    ap.add_argument("addr", type=lambda v: int(v, 16))
    ap.add_argument("--words", type=int, default=16)
    args = ap.parse_args()
    data = args.ram.read_bytes()
    if len(data) != RAM_SIZE:
        sys.exit(f"{args.ram}: expected {RAM_SIZE} bytes")

    def u32(a):
        o = a - RAM_BASE
        return struct.unpack(">I", data[o:o + 4])[0] if 0 <= o <= RAM_SIZE - 4 else None

    syms = load_symbols()

    def where(a):
        for port in range(4):
            for slot, who in ((0xB0, ""), (0xB4, " follower")):
                gobj = u32(PLAYER_BLOCKS + port * PLAYER_BLOCK_SIZE + slot)
                fp = u32(gobj + 0x2C) if gobj and RAM_BASE <= gobj < RAM_BASE + RAM_SIZE else None
                if fp and fp <= a < fp + FIGHTER_SIZE:
                    return f"port {port + 1}{who} Fighter+{a - fp:X}"
        i = bisect.bisect_right(syms, (a, "￿")) - 1
        return f"{syms[i][1]}+{a - syms[i][0]:X}" if i >= 0 and a - syms[i][0] < 0x100000 else "heap"

    a = args.addr
    print(f"{a:08X} ({where(a)}):")
    for k in range(args.words):
        w = u32(a + 4 * k)
        if w is not None:
            print(f"  +{4 * k:02X} {w:08X}")

    if u32(a) in JOBJ_CLASSES:
        print("HSD_JObj: next %08X parent %08X child %08X flags %08X aobj %08X" % (
            u32(a + 8), u32(a + 0xC), u32(a + 0x10), u32(a + 0x14), u32(a + 0x7C)))
        seen, cur = [], a
        while cur and cur not in seen and len(seen) < 64:
            seen.append(cur)
            cur = u32(cur + 0xC)
        chain = " -> ".join(f"{x:08X}" for x in seen)
        print(f"parent chain: {chain}" + (f" -> {cur:08X} (CYCLE)" if cur in seen else ""))

    needle = struct.pack(">I", a)
    refs, pos = [], data.find(needle)
    while pos >= 0 and len(refs) < 40:
        if pos % 4 == 0:
            refs.append(RAM_BASE + pos)
        pos = data.find(needle, pos + 1)
    print(f"words equal to {a:08X}: {len(refs)}" + (" (first 40)" if len(refs) == 40 else ""))
    for r in refs:
        print(f"  {r:08X} {where(r)}")


if __name__ == "__main__":
    main()
