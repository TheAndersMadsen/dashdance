#!/usr/bin/env python3
"""Compare Dashdance's guest RAM with Slippi Dolphin's at the same frame, field by field.

Usage: tools/mac/ramdiff.py DASHDANCE_RAM.bin DOLPHIN_RAM.bin

Both files come from the start of the same replay frame (dashdance_resim.py --ram-frames, slippi_ram.py).
A raw byte diff is useless: the heaps sit at slightly different addresses and graphics state differs, so this
walks the structures that decide gameplay and compares them with pointers normalized:

- each fighter (player blocks at 80453080 + 0xE90*port, gobj +0xB0/+0xB4, Fighter at gobj+0x2C), named with the
  doldecomp Fighter struct's offset comments;
- each fighter bone's animation (HSD_JObj aobj +0x7C, HSD_AObj frames and FObj interpreter state);
- the static player blocks.

Word pairs that are both RAM pointers count as equal (heap layout). Fields known to be cosmetic (sound voice
ids, draw flags) are only counted. Zero differences means the gameplay state entering the frame is identical,
so a divergence in that frame comes from the code that runs during it (see docs/MAC_FIXES.md for the method).
"""
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RAM_BASE, RAM_SIZE = 0x80000000, 0x01800000
PLAYER_BLOCKS, PLAYER_BLOCK_SIZE = 0x80453080, 0xE90
FIGHTER_SIZE = 0x23EC
# Offsets that differ between two correct runs: AXDriver voice ids (Dashdance's audio HLE hands out other ids),
# draw-pass bits (ftdrawcommon.c, written by rendering), and parts visibility bookkeeping.
COSMETIC = [(0x2140, 0x2164, "AXDriver voice ids"), (0x2223, 0x2224, "draw flags x2223"),
            (0x2227, 0x2229, "draw flags x2227_b7 / x2228_b0"), (0x5AC, 0x5C0, "parts visibility x5AC"),
            (0x20A4, 0x20AC, "drop shadow x20A4")]
# Static player block offsets (per port) that differed in runs whose fighter state matched exactly (2026-09-16,
# Game_20260916T131538 frame 1058). Meaning not yet identified; counted separately, not as differences.
PLAYER_BLOCK_UNEXPLAINED = {0xDEC, 0xE8C}


def fighter_fields():
    """Offset -> field name from the decomp's `/* fp+XXXX */` comments."""
    names = {}
    types = ROOT / "deps/melee/src/melee/ft/types.h"
    if types.exists():
        for m in re.finditer(r"/\*\s*(?:\+[0-9A-Fa-f]+\s+)?fp\+x?([0-9A-Fa-f]+)(?::(\d))?\s*\*/\s*([^;]+);", types.read_text()):
            off = int(m.group(1), 16)
            ident = re.findall(r"[A-Za-z_]\w*", m.group(3).split(":")[0].split("[")[0])
            names.setdefault(off, ident[-1] if ident else "?")
    return names


class Ram:
    def __init__(self, path):
        self.data = Path(path).read_bytes()
        if len(self.data) != RAM_SIZE:
            raise SystemExit(f"{path}: expected {RAM_SIZE} bytes, got {len(self.data)}")

    def u32(self, addr):
        off = addr - RAM_BASE
        return struct.unpack(">I", self.data[off:off + 4])[0] if 0 <= off <= RAM_SIZE - 4 else 0

    def bytes(self, addr, n):
        off = addr - RAM_BASE
        return self.data[off:off + n] if 0 <= off <= RAM_SIZE - n else b""


def is_ptr(v):
    return RAM_BASE <= v < RAM_BASE + RAM_SIZE


def word_diffs(a, b, size):
    """(offset, a word, b word) for words that differ and are not both pointers."""
    out = []
    for off in range(0, size - size % 4, 4):
        wa, wb = struct.unpack(">I", a[off:off + 4])[0], struct.unpack(">I", b[off:off + 4])[0]
        if wa != wb and not (is_ptr(wa) and is_ptr(wb)):
            out.append((off, wa, wb))
    return out


def describe(off, wa, wb, names):
    base = max((o for o in names if o <= off), default=None)
    name = names.get(base, "?") + (f"+{off - base:X}" if base is not None and off != base else "")
    fa, fb = struct.unpack(">f", struct.pack(">I", wa))[0], struct.unpack(">f", struct.pack(">I", wb))[0]
    floats = f"  (as float {fa:.6g} vs {fb:.6g})" if 1e-6 < abs(fa) < 1e7 or 1e-6 < abs(fb) < 1e7 else ""
    return f"+{off:04X} {name:<32} Dashdance {wa:08X}  Dolphin {wb:08X}{floats}"


def cosmetic(off, wa=0, wb=0):
    """The label when every differing byte of the word at off is in a cosmetic range."""
    labels = set()
    for i in range(4):
        if (wa >> (24 - 8 * i)) & 0xFF != (wb >> (24 - 8 * i)) & 0xFF:
            label = next((label for lo, hi, label in COSMETIC if lo <= off + i < hi), None)
            if label is None:
                return None
            labels.add(label)
    return ", ".join(sorted(labels)) or None


def compare(ours, theirs):
    names = fighter_fields()
    lines, real = [], 0
    for port in range(4):
        block = PLAYER_BLOCKS + port * PLAYER_BLOCK_SIZE
        for slot, who in ((0xB0, f"port {port + 1}"), (0xB4, f"port {port + 1} follower")):
            ga, gb = ours.u32(block + slot), theirs.u32(block + slot)
            if not ga and not gb:
                continue
            if bool(ga) != bool(gb):
                lines.append(f"{who}: exists in only one run"); real += 1
                continue
            fa, fb = ours.u32(ga + 0x2C), theirs.u32(gb + 0x2C)
            diffs = word_diffs(ours.bytes(fa, FIGHTER_SIZE), theirs.bytes(fb, FIGHTER_SIZE), FIGHTER_SIZE)
            gameplay = [d for d in diffs if not cosmetic(*d)]
            skipped = sorted({cosmetic(*d) for d in diffs if cosmetic(*d)})
            lines.append(f"{who}: Fighter {fa:08X} vs {fb:08X}, {len(gameplay)} field differences"
                         + (f" (ignored: {', '.join(skipped)})" if skipped else ""))
            lines += ["  " + describe(*d, names) for d in gameplay]
            real += len(gameplay)
            # Bone animation state: FighterBone array at fp+0x5E8, 0x10 per bone.
            pa, pb = ours.u32(fa + 0x5E8), theirs.u32(fb + 0x5E8)
            anim = 0
            for bone in range(128):
                ja, jb = ours.u32(pa + bone * 0x10), theirs.u32(pb + bone * 0x10)
                if not (is_ptr(ja) and is_ptr(jb)):
                    break
                aa, ab = ours.u32(ja + 0x7C), theirs.u32(jb + 0x7C)
                if bool(aa) != bool(ab):
                    lines.append(f"  bone {bone}: animation object in only one run"); anim += 1
                    continue
                if not aa:
                    continue
                if ours.bytes(aa, 20) != theirs.bytes(ab, 20):
                    va, vb = struct.unpack(">Iffff", ours.bytes(aa, 20)), struct.unpack(">Iffff", theirs.bytes(ab, 20))
                    lines.append(f"  bone {bone} AObj (flags, frame, rewind, end, rate): Dashdance {va} Dolphin {vb}"); anim += 1
                oa, ob, k = ours.u32(aa + 0x14), theirs.u32(ab + 0x14), 0
                while is_ptr(oa) and is_ptr(ob) and k < 32:
                    cursor_a, cursor_b = ours.u32(oa + 4) - ours.u32(oa + 8), theirs.u32(ob + 4) - theirs.u32(ob + 8)
                    if ours.bytes(oa + 0xC, 0x24) != theirs.bytes(ob + 0xC, 0x24) or cursor_a != cursor_b:
                        lines.append(f"  bone {bone} FObj {k}: Dashdance {ours.bytes(oa + 0xC, 0x24).hex()} cursor {cursor_a}"
                                     f" Dolphin {theirs.bytes(ob + 0xC, 0x24).hex()} cursor {cursor_b}"); anim += 1
                    oa, ob, k = ours.u32(oa), theirs.u32(ob), k + 1
            lines.append(f"  bone animation: {anim} differences")
            real += anim
    block_diffs = word_diffs(ours.bytes(PLAYER_BLOCKS, 4 * PLAYER_BLOCK_SIZE), theirs.bytes(PLAYER_BLOCKS, 4 * PLAYER_BLOCK_SIZE),
                             4 * PLAYER_BLOCK_SIZE)
    known = [d for d in block_diffs if d[0] % PLAYER_BLOCK_SIZE in PLAYER_BLOCK_UNEXPLAINED]
    block_diffs = [d for d in block_diffs if d not in known]
    lines.append(f"static player blocks: {len(block_diffs)} differences"
                 + (f" (ignored {len(known)} at offsets seen differing in matching runs)" if known else ""))
    lines += [f"  {PLAYER_BLOCKS + off:08X} (port {off // PLAYER_BLOCK_SIZE + 1} +{off % PLAYER_BLOCK_SIZE:03X}) "
              f"Dashdance {wa:08X} Dolphin {wb:08X}" for off, wa, wb in block_diffs]
    real += len(block_diffs)
    return real, lines


def main():
    if len(sys.argv) != 3:
        print(__doc__.split("\n\n")[1], file=sys.stderr)
        return 2
    real, lines = compare(Ram(sys.argv[1]), Ram(sys.argv[2]))
    print("\n".join(lines))
    print(f"\n{real} differences outside the known-cosmetic fields")
    return 0 if real == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
