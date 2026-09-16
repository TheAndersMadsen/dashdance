#!/usr/bin/env python3
"""Render a frame range of a .slp replay with Slippi's own playback Dolphin, one PNG per game frame.

Ground truth for desyncs: Slippi Dolphin re-simulates the game from the recorded inputs, so its frames show
what real Melee does with those inputs, not what Dashdance recorded.

Usage: tools/mac/slippi_frames.py REPLAY.slp START END [--out DIR] [--resync] [--scale N]

Writes DIR/frame_<n>.png for every frame in START..END that the replay has, plus DIR/resim.slp: the replay
as Slippi Dolphin regenerated it, whose post-frame data (action state, position) is Slippi's simulation.

How it works, and why it is odd:
- The Mac playback Dolphin is built without libav, and Ishiiruka only honours [Movie] DumpFrames when libav is
  present (Renderer::IsFrameDumping). This script copies the app into ~/Library/Caches/Dashdance and patches
  IsFrameDumping to return true, which re-enables the PNG frame dump path that is still compiled in.
- Frame dumping crashes MoltenVK, so the copy runs with the OpenGL backend.
- Dolphin runs with a throwaway copy of the playback user dir; the real one is never written.
- Nothing is dumped while playback fast-forwards to START, so the dump is a few "Waiting for game" frames and
  then exactly one image per "[CURRENT_FRAME] n" line Dolphin prints. The script checks that count.
- Slippi playback normally snaps each character's position back to the recorded one when they differ
  ("shouldResync"). That hides a desync, so it is off by default; --resync turns it back on.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
import uuid
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
PLAYBACK_APP = Path.home() / "Library/Application Support/Slippi Launcher/playback/Slippi Dolphin.app"
PLAYBACK_USER = Path.home() / "Library/Application Support/com.project-slippi.dolphin/playback/User"
CACHE = Path.home() / "Library/Caches/Dashdance/slippi-frames"
PATCH_SYMBOL = "__ZN8Renderer14IsFrameDumpingEv"
PROLOGUE = bytes.fromhex("554889e5")  # push rbp; mov rbp, rsp
RETURN_TRUE = bytes.fromhex("b001c3")  # mov al, 1; ret


def die(msg):
    print(f"slippi_frames: {msg}", file=sys.stderr)
    sys.exit(1)


def patched_app():
    """A copy of the playback app with frame dumping re-enabled, rebuilt when the Launcher updates Dolphin."""
    src_bin = PLAYBACK_APP / "Contents/MacOS/Slippi Dolphin"
    if not src_bin.exists():
        die(f"Slippi playback Dolphin not found at {PLAYBACK_APP} (open Slippi Launcher once to install it)")
    digest = hashlib.sha256(src_bin.read_bytes()).hexdigest()[:16]
    app = CACHE / f"app-{digest}" / "Slippi Dolphin.app"
    binary = app / "Contents/MacOS/Slippi Dolphin"
    if binary.exists():
        return binary
    shutil.rmtree(app.parent, ignore_errors=True)
    app.parent.mkdir(parents=True)
    subprocess.run(["ditto", str(PLAYBACK_APP), str(app)], check=True)
    syms = subprocess.run(["nm", str(binary)], capture_output=True, text=True, check=True).stdout
    m = re.search(rf"^([0-9a-f]+) T {re.escape(PATCH_SYMBOL)}$", syms, re.M)
    if not m:
        die(f"{PATCH_SYMBOL} not found; this Slippi Dolphin build is not one the patch knows")
    lc = subprocess.run(["otool", "-l", str(binary)], capture_output=True, text=True, check=True).stdout
    seg = re.search(r"segname __TEXT\n\s+vmaddr 0x([0-9a-f]+)\n.*?\n\s+fileoff (\d+)", lc)
    offset = int(m.group(1), 16) - int(seg.group(1), 16) + int(seg.group(2))
    data = bytearray(binary.read_bytes())
    if data[offset:offset + 4] != PROLOGUE:
        die(f"unexpected bytes at {PATCH_SYMBOL}; refusing to patch")
    data[offset:offset + len(RETURN_TRUE)] = RETURN_TRUE
    binary.write_bytes(data)
    subprocess.run(["codesign", "--force", "--deep", "-s", "-", str(app)], check=True, capture_output=True)
    return binary


def set_ini(path, values):
    """Set section/key pairs in a Dolphin ini, adding sections and keys that are missing."""
    lines = path.read_text().splitlines() if path.exists() else []
    for (section, key), value in values.items():
        header = f"[{section}]"
        if header not in lines:
            lines += [header]
        start = lines.index(header) + 1
        end = next((i for i in range(start, len(lines)) if lines[i].startswith("[")), len(lines))
        for i in range(start, end):
            if lines[i].split("=")[0].strip() == key:
                lines[i] = f"{key} = {value}"
                break
        else:
            lines.insert(end, f"{key} = {value}")
    path.write_text("\n".join(lines) + "\n")


def is_waiting_screen(png):
    """Pre-roll frames are black or the "Waiting for game" text; gameplay always has a lit stage and HUD."""
    hist = Image.open(png).convert("L").histogram()
    return sum(hist[40:]) / sum(hist) < 0.05


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("replay", type=Path)
    ap.add_argument("start", type=int)
    ap.add_argument("end", type=int)
    ap.add_argument("--out", type=Path, help="default: reports/slippi-frames/<replay>-<start>-<end>")
    ap.add_argument("--resync", action="store_true", help="let playback snap positions to the recorded ones")
    ap.add_argument("--scale", type=int, default=4, help="Dolphin EFBScale: 2 = native 640x528, 4 = 2x (default)")
    ap.add_argument("--iso", type=Path, help="default: the path in .disc-path")
    ap.add_argument("--timeout", type=int, default=600, help="seconds before giving up")
    ap.add_argument("--keep-work", action="store_true", help="keep the Dolphin user dir and raw dumps")
    args = ap.parse_args()

    replay = args.replay.expanduser().resolve()
    if not replay.exists():
        die(f"no replay at {replay}")
    if args.end < args.start:
        die("END is before START")
    iso = args.iso or Path((ROOT / ".disc-path").read_text().strip())
    out = args.out or ROOT / "reports/slippi-frames" / f"{replay.stem}-{args.start}-{args.end}"
    out.mkdir(parents=True, exist_ok=True)
    for old in out.glob("frame_*.png"):
        old.unlink()

    binary = patched_app()
    work = CACHE / "runs" / time.strftime("%Y%m%d-%H%M%S")
    user = work / "User"
    if PLAYBACK_USER.exists():
        shutil.copytree(PLAYBACK_USER, user, ignore=shutil.ignore_patterns("Logs", "Cache", "ScreenShots", "StateSaves", "Dump"))
    (user / "Config").mkdir(parents=True, exist_ok=True)
    frames_dir = user / "Dump/Frames"
    frames_dir.mkdir(parents=True)  # Dolphin does not create it and silently dumps nothing without it
    regen = work / "regen"
    regen.mkdir()
    set_ini(user / "Config/Dolphin.ini", {
        ("Movie", "DumpFrames"): "True",
        ("Movie", "DumpFramesSilent"): "True",
        ("Core", "EmulationSpeed"): "0.00000000",
        ("Core", "SlippiSaveReplays"): "True",
        ("Core", "SlippiRegenerateReplays"): "True",
        ("Core", "SlippiReplayDir"): str(regen),
        ("Core", "SlippiReplayRegenerateDir"): str(regen),
        ("Core", "SlippiPlaybackDisplayFrameIndex"): "False",
        ("Core", "SlippiEnableSpectator"): "False",
        ("Core", "SlippiJukeboxEnabled"): "False",
        ("DSP", "Volume"): "0",
        ("Interface", "ConfirmStop"): "False",
        ("Interface", "UsePanicHandlers"): "False",
        ("Interface", "PauseOnFocusLost"): "False",
    })
    set_ini(user / "Config/GFX.ini", {
        ("Settings", "DumpFramesAsImages"): "True",
        ("Settings", "InternalResolutionFrameDumps"): "True",
        ("Settings", "EFBScale"): str(args.scale),
    })
    comm = work / "comm.json"
    comm.write_text(json.dumps({
        "mode": "normal", "replay": str(replay), "commandId": uuid.uuid4().hex,
        "startFrame": args.start, "endFrame": args.end, "shouldResync": args.resync,
    }))

    log = open(work / "dolphin.out", "w")
    proc = subprocess.Popen(
        [str(binary), "-u", str(user), "-i", str(comm), "-e", str(iso), "-b", "--cout", "--hide-seekbar", "-v", "OGL"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
    os.set_blocking(proc.stdout.fileno(), False)
    played, game_end, pending = [], None, b""
    deadline, last_frame_at = time.time() + args.timeout, None
    try:
        while True:
            chunk = proc.stdout.read(65536) or b""
            if chunk:
                log.write(chunk.decode(errors="replace"))
                pending += chunk
                *lines, pending = pending.split(b"\n")
                for line in lines:
                    if m := re.match(rb"\[CURRENT_FRAME\] (-?\d+)", line):
                        played.append(int(m.group(1)))
                        last_frame_at = time.time()
                    elif m := re.match(rb"\[GAME_END_FRAME\] (-?\d+)", line):
                        game_end = int(m.group(1))
            if proc.poll() is not None:
                break
            # Playback stops a frame past END (or past the replay's last frame) and idles on "Waiting for game".
            if last_frame_at and time.time() - last_frame_at > 3:
                break
            if time.time() > deadline:
                die(f"timed out; Dolphin output is in {work / 'dolphin.out'}")
            time.sleep(0.05)
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(15)
            except subprocess.TimeoutExpired:
                proc.kill()
        log.close()

    if not played:
        die(f"Dolphin never played a frame; output is in {work / 'dolphin.out'}")
    images = sorted(frames_dir.glob("framedump_*.png"), key=lambda p: int(p.stem.split("_")[1]))
    first = next((i for i, p in enumerate(images) if not is_waiting_screen(p)), None)
    if first is None or len(images) - first < len(played):
        die(f"{len(played)} frames played but only {len(images) - (first or 0)} gameplay images dumped in {frames_dir}")
    if played != list(range(played[0], played[0] + len(played))):
        die("Dolphin skipped or repeated a frame number; the image-to-frame mapping would be wrong")
    last = min(args.end, game_end if game_end is not None else args.end)
    written = 0
    for frame, image in zip(played, images[first:]):
        if args.start <= frame <= last:
            shutil.move(str(image), out / f"frame_{frame}.png")
            written += 1
    regenerated = sorted(regen.glob("*.slp"))
    if regenerated:
        shutil.move(str(regenerated[-1]), out / "resim.slp")
    if not args.keep_work:
        shutil.rmtree(work, ignore_errors=True)
    print(f"{written} frames ({args.start}..{last}) in {out}" + ("" if args.resync else ", resync off"))
    if last < args.end:
        print(f"the replay ends at frame {game_end}")


if __name__ == "__main__":
    main()
