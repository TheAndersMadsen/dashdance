#!/usr/bin/env python3
"""Golden test: Dashdance's C++ Slippi game-file link resolver against the Rust original.

Slippi 3.7.0 ships game files (GameSetup_gui.dat, SlippiCSS_gui.dat) whose Nintendo assets are
`slp:` links into other files on the player's disc. This resolves each file with both
implementations against your own ISO and checks the outputs are byte-identical:

  * Rust: `slippi-hsd resolve` from slippi-rust-extensions (the hsd crate), built here with cargo.
  * C++:  tools/gamefile_links/resolve_cli.cpp, which calls resolve_links() from
          port/runtime/hle/slippi_gamefile_links.cpp, the code the game-file loader runs.

    python3 tools/gamefile_links/golden.py \
        --rust-ext ~/Documents/projects/slippi-ref/rust-ext \
        --gamefiles ~/Documents/projects/slippi-ref/ishiiruka/Data/Sys/GameFiles/GALE01 \
        [--iso melee.iso] [--work build/gamefile-links-golden] [GameSetup_gui.dat SlippiCSS_gui.dat ...]

Each file is resolved against the disc as it is and against perturbed copies of the disc files
(re-laid out, edited, root symbols renamed, the .usd copies or a file removed), which drive every
fallback: hash search, the other region's copy, modified assets and failures. The outputs, the
warnings and the error messages must all match.

The ISO defaults to MELEE_ISO, then the checkout's .disc-path. Everything written (disc files,
resolved outputs) stays under --work, which is inside the gitignored build/ by default: the
outputs contain Nintendo assets and must never be committed. Cargo is needed only for this check,
never for a Dashdance build. Exit status 0 when every file matches.
"""
import argparse
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from extract_dol import open_image  # noqa: E402  (CISO-aware image reader)

DEFAULT_FILES = ["GameSetup_gui.dat", "SlippiCSS_gui.dat"]


def find_iso(explicit):
    for candidate in (explicit, os.environ.get("MELEE_ISO")):
        if candidate and Path(candidate).expanduser().is_file():
            return Path(candidate).expanduser()
    for base in (ROOT, *ROOT.parents):
        marker = base / ".disc-path"
        if marker.is_file():
            path = Path(marker.read_text().strip()).expanduser()
            if path.is_file():
                return path
    raise SystemExit("cannot find the Melee ISO: pass --iso, set MELEE_ISO, or write its path to .disc-path")


def disc_files(iso):
    """Root-level FST entries: name -> (offset, size)."""
    with open_image(iso) as f:
        header = f.read(0x440)
        if header[:6] != b"GALE01":
            raise SystemExit(f"{iso}: not Melee NTSC (game id {header[:6]!r})")
        fst_off, fst_size = struct.unpack(">II", header[0x424:0x42C])
        f.seek(fst_off)
        fst = f.read(fst_size)
    count = struct.unpack_from(">I", fst, 8)[0]
    strings = fst[count * 12:]
    files, i = {}, 1
    while i < count:
        word, off, size = struct.unpack_from(">III", fst, i * 12)
        name_off = word & 0xFFFFFF
        name = strings[name_off:strings.index(b"\0", name_off)].decode("latin-1")
        if word >> 24:          # directory: skip its contents, links name root-level files
            i = size
            continue
        files.setdefault(name, (off, size))
        i += 1
    return files


def twin(name):
    stem, dot, ext = name.rpartition(".")
    return {"usd": f"{stem}.dat", "dat": f"{stem}.usd"}.get(ext) if dot else None


def be(b, at):
    return struct.unpack_from(">I", b, at)[0]


def tables(b):
    data_size, relocs, roots, refs = (be(b, 4 + 4 * i) for i in range(4))
    reloc_off = 0x20 + data_size
    roots_off = reloc_off + relocs * 4
    refs_off = roots_off + roots * 8
    return data_size, relocs, roots, refs, reloc_off, roots_off, refs_off, refs_off + refs * 8


def shift(b, by=0x40):
    """The same archive with its data section moved `by` bytes later: every offset changes."""
    if b is None:
        return None
    data_size, relocs, roots, refs, reloc_off, roots_off, refs_off, _ = tables(b)
    data = bytearray(b[0x20:0x20 + data_size])
    tail = bytearray(b[reloc_off:])
    for i in range(relocs):
        field = be(tail, i * 4)
        struct.pack_into(">I", data, field, be(data, field) + by)
        struct.pack_into(">I", tail, i * 4, field + by)
    for i in range(roots):
        at = roots_off - reloc_off + i * 8
        struct.pack_into(">I", tail, at, be(tail, at) + by)
    for i in range(refs):
        at = refs_off - reloc_off + i * 8
        link = be(tail, at)
        struct.pack_into(">I", tail, at, link + by)
        while be(data, link) != 0xFFFFFFFF:
            nxt = be(data, link)
            struct.pack_into(">I", data, link, nxt + by)
            link = nxt
    header = bytearray(b[:0x20])
    struct.pack_into(">II", header, 0, len(b) + by, data_size + by)
    return bytes(header + bytes(by) + data + tail)


def rename_roots(b):
    """Every root symbol renamed (first letter's case flipped), so no path anchor resolves."""
    _, _, roots, _, _, roots_off, _, strings_off = tables(b)
    out = bytearray(b)
    for i in range(roots):
        at = strings_off + be(b, roots_off + i * 8 + 4)
        out[at] ^= 0x20
    return bytes(out)


def edit(b):
    """A byte changed every 4 KB of the data section, never inside a pointer field."""
    data_size, relocs, _, _, reloc_off, _, _, _ = tables(b)
    fields = set()
    for i in range(relocs):
        f = be(b, reloc_off + i * 4)
        fields.update(range(f, f + 4))
    out = bytearray(b)
    for at in range(0x101, data_size, 0x1000):
        if at not in fields:
            out[0x20 + at] ^= 0x5A
    return bytes(out)


def report(proc):
    """The lines both CLIs print with the same wording: warnings, link statuses, the error."""
    lines = [l for l in (proc.stdout + proc.stderr).splitlines() if l.startswith(("warning:", "links:", "error:"))]
    # An unreadable disc file: Rust's CLI reports the OS error, the loader's front door (like
    # Dolphin's) reports "not found on the disc".
    return [re.sub(r"No such file or directory \(os error 2\)", "not found on the disc", l) for l in lines]


def compare(rust, cxx, src, disc, outdir, scenario):
    name = src.name
    r_out, c_out = outdir / f"{scenario}-{name}.rust", outdir / f"{scenario}-{name}.cpp"
    for path in (r_out, c_out):
        path.unlink(missing_ok=True)
    r = subprocess.run([rust, "resolve", src, "--disc", disc, "-o", r_out], capture_output=True, text=True)
    c = subprocess.run([cxx, src, "--disc", disc, "-o", c_out], capture_output=True, text=True)
    rr, cr = report(r), report(c)
    label = f"[{scenario}] {name}"
    if rr != cr:
        print(f"DIFFER {label}: the reports differ")
        for line in sorted(set(rr) ^ set(cr))[:10]:
            print(f"  {'rust' if line in rr else 'c++ '}: {line}")
        return False
    summary = next((l for l in rr if l.startswith(("links:", "error:"))), "?")
    warnings = sum(l.startswith("warning:") for l in rr)
    if r.returncode or c.returncode:
        same = r.returncode != 0 and c.returncode != 0 and not r_out.exists() and not c_out.exists()
        print(f"{'MATCH' if same else 'DIFFER'} {label}: both fail ({summary})" if same else
              f"DIFFER {label}: rust exit {r.returncode}, c++ exit {c.returncode}")
        return same
    rb, cb = r_out.read_bytes(), c_out.read_bytes()
    same = rb == cb
    print(f"{'MATCH' if same else 'DIFFER'} {label}: {src.stat().st_size} -> {len(rb)} bytes (c++ {len(cb)}), "
          f"sha1 {hashlib.sha1(rb).hexdigest()[:12]}; {summary}; {warnings} warning(s), identical")
    if not same:
        at = next((i for i, (x, y) in enumerate(zip(rb, cb)) if x != y), min(len(rb), len(cb)))
        print(f"  first difference at byte 0x{at:x}")
    return same


def run(cmd, **kw):
    print("$", " ".join(str(c) for c in cmd), flush=True)
    return subprocess.run(cmd, **kw)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("files", nargs="*", default=DEFAULT_FILES)
    p.add_argument("--rust-ext", default="~/Documents/projects/slippi-ref/rust-ext")
    p.add_argument("--gamefiles", default="~/Documents/projects/slippi-ref/ishiiruka/Data/Sys/GameFiles/GALE01")
    p.add_argument("--iso")
    p.add_argument("--work", default=str(ROOT / "build" / "gamefile-links-golden"))
    args = p.parse_args()

    rust_ext = Path(args.rust_ext).expanduser()
    gamefiles = Path(args.gamefiles).expanduser()
    work = Path(args.work).expanduser()
    iso = find_iso(args.iso)
    (work / "disc").mkdir(parents=True, exist_ok=True)
    (work / "out").mkdir(parents=True, exist_ok=True)

    # Both resolvers.
    if not shutil.which("cargo"):
        raise SystemExit("cargo is missing: brew install rustup && rustup-init -y (only needed for this check)")
    env = dict(os.environ, CARGO_TARGET_DIR=str(work / "cargo-target"))
    if run(["cargo", "build", "--release", "-q", "-p", "slippi-hsd", "--bin", "slippi-hsd"], cwd=rust_ext, env=env).returncode:
        raise SystemExit("cargo build of slippi-hsd failed")
    rust = work / "cargo-target" / "release" / "slippi-hsd"
    cxx = work / "resolve_cli"
    cc_env = {k: v for k, v in os.environ.items() if k != "DEVELOPER_DIR"}
    if run([os.environ.get("CXX", "clang++"), "-std=c++17", "-O2", "-Wall", "-I", ROOT / "port/runtime/hle",
            ROOT / "tools/gamefile_links/resolve_cli.cpp", ROOT / "port/runtime/hle/slippi_gamefile_links.cpp",
            "-o", cxx], env=cc_env).returncode:
        raise SystemExit("compiling resolve_cli failed")

    # The disc files the links name, and their other-region twins, straight from the ISO.
    table = disc_files(iso)
    wanted = set()
    for name in args.files:
        for m in re.finditer(rb"slp:([^:\0]+):", (gamefiles / name).read_bytes()):
            linked = m.group(1).decode("latin-1")
            wanted.update(n for n in (linked, twin(linked)) if n and n in table)
    with open_image(iso) as f:
        for name in sorted(wanted):
            off, size = table[name]
            f.seek(off)
            (work / "disc" / name).write_bytes(f.read(size))
    print(f"{len(wanted)} disc file(s) from {iso.name}: {', '.join(sorted(wanted))}")

    # Scenarios: the disc as it is, then perturbed copies that drive the resolver's fallbacks.
    # Both implementations get the same bytes; only the resolution is under test.
    originals = {n: (work / "disc" / n).read_bytes() for n in sorted(wanted)}
    scenarios = {
        "disc": lambda n, b: b,
        "relaid": lambda n, b: shift(b),                            # every offset moves: paths still find assets
        "edited": lambda n, b: edit(b),                             # bytes changed: the disc's version, with warnings
        "no-usd": lambda n, b: None if n.endswith(".usd") else b,   # the other region's copy stands in
        "renamed": lambda n, b: rename_roots(b),                    # paths fail: found by hash at the recorded offset
        "renamed-relaid": lambda n, b: shift(rename_roots(b)),      # paths and offsets fail: whole-file hash search
        "renamed-no-usd": lambda n, b: None if n.endswith(".usd") else shift(rename_roots(b)),
        "missing": lambda n, b: None if n.startswith("MnSlChr") else b,   # nothing to find: both must fail alike
    }
    ok = True
    for scenario, perturb in scenarios.items():
        disc = work / f"disc-{scenario}"
        shutil.rmtree(disc, ignore_errors=True)
        disc.mkdir(parents=True)
        for n, b in originals.items():
            out = perturb(n, b)
            if out is not None:
                (disc / n).write_bytes(out)
        for name in args.files:
            ok &= compare(rust, cxx, gamefiles / name, disc, work / "out", scenario)
    print("all outputs byte-identical" if ok else "MISMATCH")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
