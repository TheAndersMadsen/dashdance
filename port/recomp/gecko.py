"""Gecko code support for the recompiler.

Slippi Dolphin boots Melee with two code tables: `bootloader.gct` (installed by Dolphin at
0x800028B8 and applied once by codehandler.bin) and the main table generated from the enabled
codes of GALE01r2.ini, which the game itself fetches over the Slippi EXI device (commands D3/D4)
and applies with its own in-game handler (types 04, 06, 08, C0, C2). The recompiled code cannot
execute instructions the guest writes into RAM, so this module expresses every code as
instruction-level patches the recompiler applies ahead of time:
  * 32/16/8-bit and string writes are applied to the DOL image;
  * C2 hooks replace the hooked instruction with the cave's instructions (spliced in place);
  * hooks and C0 caves outside any function become synthetic functions.
The guest still runs the real handler at boot, so RAM ends up exactly as under Dolphin.
"""
import hashlib
import re
import struct

CODEHANDLER_BASE = 0x80001800
BOOTLOADER_BASE = 0x800028B8   # codehandler.bin length (4288) - 8, per Gecko::InstallCodeHandler


# Codes compiled in for player options. Their table entries follow the required codes so the
# addresses of required caves remain stable for every selection.
RUNTIME_OPTIONAL = {
    "Optional: Widescreen 16:9": "widescreen",
    "Optional: Flash Red on Failed L-Cancel": "flash_failed_lcancel",
}


class GeckoCode:
    def __init__(self, name):
        self.name = name
        self.codes = []      # (address_word, data_word)
        self.enabled = False
        self.optional = RUNTIME_OPTIONAL.get(name)   # flag name when switchable at run time


def load_ini(path):
    """Parses [Gecko] and [Gecko_Enabled] like Dolphin's GeckoCodeConfig::LoadCodes."""
    codes, enabled, section, current = [], set(), None, None
    with open(path, encoding="utf-8", errors="replace") as source:
        for raw in source:
            line = raw.strip()
            if not line:
                continue
            if line.startswith("["):
                section = line
                current = None
                continue
            if section == "[Gecko_Enabled]":
                if line.startswith("$"):
                    enabled.add(line[1:].strip())
                continue
            if section != "[Gecko]":
                continue
            if line.startswith("$"):
                name = line[1:]
                bracket = name.find("[")
                if bracket >= 0:
                    name = name[:bracket]
                current = GeckoCode(name.strip())
                codes.append(current)
                continue
            if line.startswith("*") or current is None:
                continue
            m = re.match(r"^([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]{8})", line)
            if m:
                current.codes.append((int(m.group(1), 16), int(m.group(2), 16)))
    for code in codes:
        code.enabled = code.name in enabled or code.optional is not None
    return codes


def generate_gct(codes, include_optional=True):
    """Byte-for-byte Gecko::GenerateGct for enabled codes, with optional codes last.
    `include_optional` selects all, none, or a set of option flags. Returns (table, optional offset)."""
    out = bytearray(struct.pack(">II", 0x00D0C0DE, 0x00D0C0DE))
    for code in codes:
        if code.enabled and code.optional is None:
            for a, d in code.codes:
                out += struct.pack(">II", a, d)
    offset = len(out)
    for code in codes:
        if code.enabled and code.optional is not None and (
            include_optional is True or
            include_optional is not False and code.optional in include_optional
        ):
            for a, d in code.codes:
                out += struct.pack(">II", a, d)
    out += struct.pack(">II", 0xFF000000, 0)
    return bytes(out), offset


class Hook:
    __slots__ = ("hook", "cave_addr", "words", "optional", "absorbed_by")

    def __init__(self, hook, cave_addr, words):
        self.hook, self.cave_addr, self.words = hook, cave_addr, words
        self.optional = None   # flag name when the hook belongs to a run-time optional code
        self.absorbed_by = None  # HLE name after a pinned payload is validated


def canonical_hook_body(hook):
    """Relocation-independent C2 body used for equality and identity checks.

    The Gecko handler replaces the final word with a branch back to hook+4, so
    that word differs when byte-identical codes occupy different table slots.
    """
    return tuple(hook.words[:-1])


def hook_fingerprint(hook):
    body = canonical_hook_body(hook)
    payload = struct.pack(">%dI" % len(body), *body) if body else b""
    return hashlib.sha256(payload).hexdigest()


def canonicalize_hooks(hooks):
    """Return later-winner hooks and (address, identical-body) duplicates."""
    winners = {}
    duplicates = []
    for hook in hooks:
        previous = winners.get(hook.hook)
        if previous is not None:
            duplicates.append((
                hook.hook,
                canonical_hook_body(previous) == canonical_hook_body(hook)
                and previous.optional == hook.optional,
            ))
        winners[hook.hook] = hook
    return list(winners.values()), duplicates


class Cave:
    """A C0 (execute once) cave: a function at cave_addr ending in blr."""
    __slots__ = ("cave_addr", "words")

    def __init__(self, cave_addr, words):
        self.cave_addr, self.words = cave_addr, words


class GctPatches:
    def __init__(self):
        self.writes = []        # (addr, bytes)
        self.hooks = []         # Hook
        self.c0 = []            # Cave
        self.unsupported = []   # (line index, address word, data word)


def parse_gct(data, base_addr):
    """Decodes a GCT located at `base_addr` in guest RAM into patches. The cave address of a
    C2/C0 code is where its instructions live inside the table (the in-game handler branches
    there), so it depends on the table's address."""
    words = struct.unpack(">%dI" % (len(data) // 4), data)
    p = GctPatches()
    i = 0
    while i + 1 < len(words):
        a, b = words[i], words[i + 1]
        t = a >> 24
        addr = 0x80000000 | (a & 0x01FFFFFF)
        if a == 0x00D0C0DE and b == 0x00D0C0DE:
            i += 2
            continue
        if t in (0xF0, 0xFF, 0xE0):
            break
        if t == 0x00:
            count = (b >> 16) + 1
            p.writes.append((addr, bytes([b & 0xFF]) * count))
            i += 2
        elif t == 0x02:
            count = (b >> 16) + 1
            p.writes.append((addr, struct.pack(">H", b & 0xFFFF) * count))
            i += 2
        elif t == 0x04:
            p.writes.append((addr, struct.pack(">I", b)))
            i += 2
        elif t == 0x06:
            n = b
            lines = (n + 7) // 8
            blob = data[(i + 2) * 4:(i + 2) * 4 + n]
            p.writes.append((addr, blob))
            i += 2 + lines * 2
        elif t == 0x08:
            # 08XXXXXX YYYYYYYY TNNNZZZZ VVVVVVVV: T = size (0 byte, 1 half, 2 word), NNN = count - 1,
            # ZZZZ = address step, VVVVVVVV = value step.
            c, d = words[i + 2], words[i + 3]
            size = c >> 28
            count = ((c >> 16) & 0xFFF) + 1
            step = c & 0xFFFF
            vstep = d
            value = b
            blob_addr = addr
            for k in range(count):
                v = (value + k * vstep) & 0xFFFFFFFF
                if size == 0:
                    p.writes.append((blob_addr, bytes([v & 0xFF])))
                elif size == 1:
                    p.writes.append((blob_addr, struct.pack(">H", v & 0xFFFF)))
                else:
                    p.writes.append((blob_addr, struct.pack(">I", v)))
                blob_addr += step
            i += 4
        elif t == 0xC2:
            n = b
            cave_addr = base_addr + (i + 2) * 4
            cave = list(words[i + 2:i + 2 + n * 2])
            # The handler overwrites the last word of the cave with `b hook+4`.
            last_addr = cave_addr + (n * 2 - 1) * 4
            cave[-1] = 0x48000000 | (((addr + 4) - last_addr) & 0x03FFFFFC)
            p.hooks.append(Hook(addr, cave_addr, cave))
            i += 2 + n * 2
        elif t == 0xC0:
            n = b
            cave_addr = base_addr + (i + 2) * 4
            p.c0.append(Cave(cave_addr, list(words[i + 2:i + 2 + n * 2])))
            i += 2 + n * 2
        else:
            p.unsupported.append((i, a, b))
            i += 2
    return p
