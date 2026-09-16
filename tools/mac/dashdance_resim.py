#!/usr/bin/env python3
"""Re-simulate a .slp replay in Dashdance itself, headless, and write Dashdance's own regeneration of it.

Usage: tools/mac/dashdance_resim.py REPLAY.slp [--out FILE.slp] [--ram-frames F1,F2] [--allow-interpreter]

This is the Dashdance half of a desync investigation (tools/mac/desync.py runs both halves). It uses the
replay playback path (port/runtime/hle/slippi_playback.cpp): the game is translated against the Slippi
Playback code set, reads each frame's recorded inputs from the replay, and records what it simulates.

Two builds, both under build/ and reused while their inputs are unchanged:
- build/mac-playback: playback code set only. A short run of it makes the game install the replay's own code
  list, and logs where; the list is written to gecko_list.bin.
- build/mac-playback-replay: the same plus that code list translated ahead of time (--extra-gct), so the
  replay's codes (UCF and friends) run as translated code exactly like they do in a live game. Rebuilt
  incrementally on every run; a different code list recompiles only what changed.
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from slp import Replay  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
BASE_BUILD = ROOT / "build/mac-playback"
REPLAY_BUILD = ROOT / "build/mac-playback-replay"
SYS_DIR = ROOT / "port/slippi_sys_playback"
BOOT_FRAMES = 1500  # retraces from boot to the playback scene, with slack


def log(msg):
    print(f"dashdance_resim: {msg}", flush=True)


def die(msg):
    print(f"dashdance_resim: {msg}", file=sys.stderr)
    sys.exit(1)


def build(build_dir, extra=None):
    """Translate and compile the headless playback executable into build_dir (incremental)."""
    cmd = [sys.executable, ROOT / "tools/bootstrap_port.py", "--decomp-root", ROOT / "deps/melee", "--dol",
           ROOT / "deps/disc/main.dol", "--build-dir", build_dir, "--playback", "--gct-base", "0x8065CC80",
           "--stage", "configure", "--macos-arch", "arm64"]
    if extra:
        cmd += ["--extra-gct", extra[0], "--extra-gct-base", f"0x{extra[1]:08X}"]
    logfile = Path(str(build_dir) + ".log")
    with open(logfile, "w") as f:
        for step in (cmd, ["cmake", "--build", build_dir, "--target", "melee_port_headless", "--parallel", "14"]):
            if subprocess.run([str(c) for c in step], cwd=ROOT, stdout=f, stderr=subprocess.STDOUT).returncode:
                die(f"build failed; see {logfile}")
    exe = Path(build_dir) / "port/melee_port_headless"
    if not exe.exists():
        die(f"no executable at {exe}; see {logfile}")
    return exe


def run(exe, replay, frames, allow_interpreter, env=None):
    """Run a headless playback; returns (cache dir, log text). The caller removes the cache dir."""
    iso = Path((ROOT / ".disc-path").read_text().strip())
    parent = Path(tempfile.mkdtemp(prefix="dashdance-resim-"))
    cmd = [exe, "--offline", "--fast", "--iso", iso, "--sys-dir", SYS_DIR, "--replay", replay,
           "--frames", str(min(frames, 36000)), "--time-base", "0",
           "--profile-dir", parent / "profile", "--card-dir", parent / "card", "--cache-dir", parent / "cache",
           "--allow-interpreter" if allow_interpreter else "--strict-aot"]
    started = time.time()
    subprocess.run([str(c) for c in cmd], cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   env={**os.environ, **(env or {})})
    text = (parent / "cache/melee_port.log").read_text(errors="replace") if (parent / "cache/melee_port.log").exists() else ""
    log(f"ran {Path(exe).parent.parent.name} for {time.time() - started:.0f} s")
    return parent, text


def resim(replay, out=None, ram_frames=None, allow_interpreter=False):
    """Re-simulate REPLAY in Dashdance; returns the path of the regenerated .slp (and the log next to it).
    ram_frames: frames whose guest RAM to write next to it as ram_<frame>.bin."""
    replay = Path(replay).expanduser().resolve()
    out = Path(out) if out else ROOT / "reports/desync" / replay.stem / "dashdance.slp"
    out.parent.mkdir(parents=True, exist_ok=True)
    last = Replay(replay).last_frame

    # Stage 1: learn the replay's code list and where the game installs it.
    exe = build(BASE_BUILD)
    parent, text = run(exe, replay, 900, allow_interpreter=True)
    m = re.search(r"playback: game placed the replay code list at ([0-9A-F]{8})", text)
    listing = parent / "cache/replays/gecko_list.bin"
    if not m or not listing.exists():
        shutil.rmtree(parent, ignore_errors=True)
        die("the playback build never installed the replay's code list; is this a Slippi replay?")
    addr = int(m.group(1), 16)
    key = hashlib.sha256(listing.read_bytes() + addr.to_bytes(4, "big")).hexdigest()[:16]
    cached_list = ROOT / "build" / f"gecko_list-{key}.bin"
    shutil.copyfile(listing, cached_list)
    shutil.rmtree(parent, ignore_errors=True)

    # Stage 2: translate that list too, then play the whole replay with strict translated code.
    # Always rebuild (incrementally): the build verifies its sources, so any runtime edit needs regeneration.
    log(f"building {REPLAY_BUILD.name} with the replay's code list (at {addr:08X})")
    exe = build(REPLAY_BUILD, (cached_list, addr))
    env = None
    if ram_frames:
        env = {"MELEE_RAM_DUMP_FRAMES": ",".join(str(f) for f in ram_frames), "MELEE_RAM_DUMP_DIR": str(out.parent)}
    parent, text = run(exe, replay, last + 123 + BOOT_FRAMES, allow_interpreter, env)
    recorded = sorted((parent / "cache/replays").glob("Game_*.slp"))
    (out.parent / "dashdance.log").write_text(text)
    if not recorded:
        shutil.rmtree(parent, ignore_errors=True)
        die(f"no recording written; the log is in {out.parent / 'dashdance.log'}")
    shutil.move(str(recorded[-1]), out)
    shutil.rmtree(parent, ignore_errors=True)
    interp = re.search(r"interpreter: (\d+) calls, (\d+) instructions", text)
    ours = Replay(out)
    log(f"wrote {out} (frames up to {ours.last_frame} of {last}"
        + (f"; {interp.group(1)} interpreted calls" if interp and interp.group(1) != "0" else "") + ")")
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("replay", type=Path)
    ap.add_argument("--out", type=Path, help="default: reports/desync/<replay>/dashdance.slp")
    ap.add_argument("--ram-frames", help="comma-separated frames whose guest RAM to write next to --out (ram_<frame>.bin)")
    ap.add_argument("--allow-interpreter", action="store_true",
                    help="let code with no translation run in the interpreter instead of failing")
    args = ap.parse_args()
    frames = [int(f) for f in args.ram_frames.split(",")] if args.ram_frames else None
    resim(args.replay, args.out, frames, args.allow_interpreter)

if __name__ == "__main__":
    main()
