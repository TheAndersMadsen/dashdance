"""Minimal .slp reader for desync work: every pre-frame and post-frame field, keyed by (frame, port, follower).

Offsets follow the Slippi replay spec (project-slippi/slippi-wiki SPEC.md), relative to the byte after the
command byte. Fields a replay's version does not carry are simply absent.
"""
import struct
from pathlib import Path

# (name, offset, struct format)
PRE_FIELDS = [
    ("seed", 0x06, ">I"), ("state", 0x0A, ">H"), ("x", 0x0C, ">f"), ("y", 0x10, ">f"), ("facing", 0x14, ">f"),
    ("joy_x", 0x18, ">f"), ("joy_y", 0x1C, ">f"), ("cstick_x", 0x20, ">f"), ("cstick_y", 0x24, ">f"),
    ("trigger", 0x28, ">f"), ("buttons", 0x2C, ">I"), ("phys_buttons", 0x30, ">H"), ("phys_l", 0x32, ">f"),
    ("phys_r", 0x36, ">f"), ("raw_x", 0x3A, ">b"), ("percent", 0x3B, ">f"), ("raw_y", 0x3F, ">b"),
]
POST_FIELDS = [
    ("char", 0x06, ">B"), ("state", 0x07, ">H"), ("x", 0x09, ">f"), ("y", 0x0D, ">f"), ("facing", 0x11, ">f"),
    ("percent", 0x15, ">f"), ("shield", 0x19, ">f"), ("last_attack", 0x1D, ">B"), ("combo", 0x1E, ">B"),
    ("last_hit_by", 0x1F, ">B"), ("stocks", 0x20, ">B"), ("state_frame", 0x21, ">f"), ("flags1", 0x25, ">B"),
    ("flags2", 0x26, ">B"), ("flags3", 0x27, ">B"), ("flags4", 0x28, ">B"), ("flags5", 0x29, ">B"),
    ("misc_as", 0x2A, ">f"), ("airborne", 0x2E, ">B"), ("ground", 0x2F, ">H"), ("jumps", 0x31, ">B"),
    ("l_cancel", 0x32, ">B"), ("hurtbox", 0x33, ">B"), ("air_vx", 0x34, ">f"), ("vy", 0x38, ">f"),
    ("attack_vx", 0x3C, ">f"), ("attack_vy", 0x40, ">f"), ("ground_vx", 0x44, ">f"), ("hitlag", 0x48, ">f"),
    ("anim", 0x4C, ">I"), ("hit_by_instance", 0x50, ">H"), ("instance", 0x52, ">H"),
]

# Common action state names (ftCo_MS_*), read from the decomp's enum when it is checked out.
def _load_state_names():
    import re
    names = {}
    forward = Path(__file__).resolve().parents[2] / "deps/melee/src/melee/ft/kinds/ftCommon/forward.h"
    if not forward.exists():
        return names
    text = forward.read_text()
    first = text.find("ftCo_MS_")
    body = text[text.rfind("{", 0, first) + 1:text.find("}", first)]
    value = -1
    for line in body.splitlines():
        line = re.sub(r"//.*|/\*.*?\*/", "", line).strip().rstrip(",")
        if not line.startswith("ftCo_MS_"):
            continue
        name, _, explicit = line.partition("=")
        value = int(explicit.strip(), 0) if explicit.strip() else value + 1
        names[value] = name.strip()[len("ftCo_MS_"):]
    return names


STATE_NAMES = _load_state_names()


def state_name(s):
    return f"{s} {STATE_NAMES[s]}" if s in STATE_NAMES else str(s)


class Replay:
    def __init__(self, path):
        self.path = Path(path)
        data = self.path.read_bytes()
        i = data.find(b"raw[$U#l")
        if i < 0:
            raise ValueError(f"{path}: no raw block")
        n = struct.unpack(">I", data[i + 8:i + 12])[0]
        raw = data[i + 12:i + 12 + n] if n else data[i + 12:]  # n is 0 while a game is still being written
        if raw[0] != 0x35:
            raise ValueError(f"{path}: raw block does not start with the payload sizes event")
        sizes = {raw[2 + 3 * k]: struct.unpack(">H", raw[3 + 3 * k:5 + 3 * k])[0] for k in range((raw[1] - 1) // 3)}
        self.pre, self.post = {}, {}
        self.game_end = None
        pos = 1 + raw[1]
        while pos < len(raw) and raw[pos] in sizes:
            cmd, size = raw[pos], sizes[raw[pos]]
            body = raw[pos + 1:pos + 1 + size]
            if cmd in (0x37, 0x38) and len(body) >= 6:
                frame, port, follower = struct.unpack(">iBB", body[:6])
                fields = PRE_FIELDS if cmd == 0x37 else POST_FIELDS
                rec = {}
                for name, off, fmt in fields:
                    width = struct.calcsize(fmt)
                    if off + width <= len(body):
                        rec[name] = struct.unpack(fmt, body[off:off + width])[0]
                (self.pre if cmd == 0x37 else self.post)[(frame, port, follower)] = rec
            elif cmd == 0x39:
                self.game_end = body[0]
            pos += 1 + size
        self.last_frame = max((k[0] for k in self.post), default=None)

    def players(self):
        return sorted({(k[1], k[2]) for k in self.post})


def diff_records(a, b):
    """Fields present in both records whose values differ, as (name, a, b)."""
    return [(k, a[k], b[k]) for k in a if k in b and a[k] != b[k]]
