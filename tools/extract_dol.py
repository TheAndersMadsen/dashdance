"""Extracts main.dol from a GameCube ISO/GCM or CISO image (the DOL offset is stored at
0x420 in the disc header).

    python tools/extract_dol.py <game.iso> <out/main.dol>
"""
import struct
import sys
from pathlib import Path


def open_image(image_path):
    """Returns a file object positioned at logical offset 0, transparently mapping CISO.

    Both known CISO header layouts are accepted: block size at offset 4 with the map at 8
    (GCRebuilder), or version 1 at 4 with the block size at 8 and the map at 0xC (Dolphin).
    Stored blocks are packed after the 0x8000 header; absent map entries read as zeros.
    """

    class Image:
        def __init__(self, f, block_size, phys, data_offset):
            self.f, self.block_size, self.phys, self.data_offset = f, block_size, phys, data_offset
            self.pos = 0

        def seek(self, offset, whence=0):
            assert whence == 0
            self.pos = offset
            return self.pos

        def __enter__(self):
            return self

        def __exit__(self, *exc):
            self.f.close()
            return False

        def read(self, size):
            out = bytearray()
            pos = self.pos
            while len(out) < size:
                index, within = divmod(pos, self.block_size)
                chunk = min(size - len(out), self.block_size - within)
                phys = self.phys[index] if index < len(self.phys) else -1
                if phys < 0:
                    out += bytes(chunk)
                else:
                    self.f.seek(self.data_offset + phys * self.block_size + within)
                    data = self.f.read(chunk)
                    out += data + bytes(chunk - len(data))
                pos += chunk
            self.pos = pos
            return bytes(out)

    f = open(image_path, "rb")
    header = f.read(0x8000)
    if header[:4] != b"CISO":
        f.seek(0)
        return f
    a = struct.unpack_from("<I", header, 4)[0]
    b = struct.unpack_from("<I", header, 8)[0]
    if a >= 0x200 and a & (a - 1) == 0:
        block_size, map_bytes = a, header[8:]
    elif a == 1 and b >= 0x200 and b & (b - 1) == 0:
        block_size, map_bytes = b, header[0xC:]
    else:
        raise SystemExit(f"{image_path}: unsupported CISO header layout")
    blocks = 0
    for i, entry in enumerate(map_bytes):
        if entry > 1:
            break
        if entry == 1:
            blocks = i + 1
    if not blocks:
        raise SystemExit(f"{image_path}: empty CISO map")
    phys, next_block = [], 0
    for entry in map_bytes[:blocks]:
        if entry == 1:
            phys.append(next_block)
            next_block += 1
        else:
            phys.append(-1)
    print(f"{image_path}: CISO, {blocks} x {block_size >> 10} KB blocks ({next_block} stored)")
    return Image(f, block_size, phys, 0x8000)


def extract(image_path, out_path):
    with open_image(image_path) as f:
        header = f.read(0x440)
        if header[:6] != b"GALE01":
            raise SystemExit(f"{image_path}: not Melee NTSC (game id {header[:6]!r}); this port needs GALE01 v1.02")
        if header[7] != 2:
            raise SystemExit(f"{image_path}: disc revision {header[7]}, need v1.02 (revision 2)")
        dol_offset = struct.unpack(">I", header[0x420:0x424])[0]
        f.seek(dol_offset)
        dol_header = f.read(0x100)
        # DOL: 7 text + 11 data sections; file size = max(offset + size) over the sections.
        offsets = struct.unpack(">18I", dol_header[0:72])
        sizes = struct.unpack(">18I", dol_header[0x90:0x90 + 72])
        size = max(o + s for o, s in zip(offsets, sizes))
        f.seek(dol_offset)
        data = f.read(size)
    Path(out_path).parent.mkdir(parents=True, exist_ok=True)
    Path(out_path).write_bytes(data)
    print(f"main.dol: {size} bytes from disc offset 0x{dol_offset:X} -> {out_path}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    extract(sys.argv[1], sys.argv[2])
