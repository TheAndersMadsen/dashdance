#!/usr/bin/env python3
"""Build DashdancePlayback, the replay player that tools/package_macos_app.sh puts inside Dashdance.app.

Usage: tools/mac/build_player.py [REPLAY.slp]

The player is the Metal app translated against the Slippi Playback code set plus a replay's own code list
(--extra-gct), so the codes a replay carries (UCF and friends) run as translated code and playback is
frame-exact. Every replay Dashdance records carries the same list apart from data words, so one list covers
them all; it is learned from REPLAY, default the newest replay in Dashdance's replay folder, with the same
short headless run tools/mac/dashdance_resim.py uses. Replays from other Slippi versions carry other lists:
the player logs a "code list mismatch" for those and may desync.

Output: build/mac-player/port/melee_port_mac. Incremental; exits 0 without building when there is no replay yet.
"""
import hashlib
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dashdance_resim  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build/mac-player"
REPLAYS = Path.home() / "Library/Application Support/Dashdance/Replays"


def log(msg):
    print(f"build_player: {msg}", flush=True)


def code_list(replay):
    """Learn the replay's served code list and install address from a short headless playback."""
    exe = dashdance_resim.build(dashdance_resim.BASE_BUILD)
    parent, text = dashdance_resim.run(exe, replay, 900, allow_interpreter=True)
    try:
        m = re.search(r"playback: game placed the replay code list at ([0-9A-F]{8})", text)
        listing = parent / "cache/replays/gecko_list.bin"
        if not m or not listing.exists():
            sys.exit(f"build_player: {replay.name} never installed a code list; is it a Slippi replay?")
        addr = int(m.group(1), 16)
        key = hashlib.sha256(listing.read_bytes() + addr.to_bytes(4, "big")).hexdigest()[:16]
        cached = ROOT / "build" / f"gecko_list-{key}.bin"
        shutil.copyfile(listing, cached)
        return cached, addr
    finally:
        shutil.rmtree(parent, ignore_errors=True)


def main():
    if len(sys.argv) > 1:
        replay = Path(sys.argv[1]).expanduser().resolve()
    else:
        found = sorted(REPLAYS.glob("Game_*.slp"), key=lambda p: p.stat().st_mtime)
        if not found:
            log(f"no replays in {REPLAYS} yet; play one game, then rebuild to add the replay player")
            return
        replay = found[-1]
    listing, addr = code_list(replay)
    log(f"code list from {replay.name}: {listing.name} at {addr:08X}")
    decomp, dol = ROOT / "deps/melee", ROOT / "deps/disc/main.dol"
    steps = [
        [sys.executable, ROOT / "tools/bootstrap_port.py", "--decomp-root", decomp, "--dol", dol, "--build-dir", BUILD,
         "--playback", "--extra-gct", listing, "--extra-gct-base", f"0x{addr:08X}", "--gct-base", "0x8065CC80",
         "--macos-arch", "arm64", "--stage", "generate"],
        ["cmake", "-S", ROOT, "-B", BUILD, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
         f"-DMELEE_DECOMP_ROOT={decomp}", f"-DMELEE_DOL_PATH={dol}", f"-DMELEE_PORT_GENERATED_DIR={BUILD}/generated/guest",
         "-DMELEE_BUILD_PORT_TESTS=OFF", "-DMELEE_BUILD_PORT_HEADLESS=OFF", "-DMELEE_BUILD_PORT_METAL=ON",
         "-DCMAKE_OSX_ARCHITECTURES=arm64"],
        ["cmake", "--build", BUILD, "--target", "melee_port_mac"],
    ]
    logfile = Path(str(BUILD) + ".log")
    with open(logfile, "w") as f:
        for step in steps:
            if subprocess.run([str(c) for c in step], cwd=ROOT, stdout=f, stderr=subprocess.STDOUT).returncode:
                sys.exit(f"build_player: build failed; see {logfile}")
    log(f"built {BUILD / 'port/melee_port_mac'}")


if __name__ == "__main__":
    main()
