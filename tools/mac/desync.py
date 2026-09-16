#!/usr/bin/env python3
"""Find where a Dashdance replay stops matching real Melee, and narrow down why.

Usage: tools/mac/desync.py REPLAY.slp [--context N] [--no-frames] [--no-dashdance] [--no-ram] [--reuse]

1. Slippi Dolphin re-simulates the whole replay from its recorded inputs (slippi_frames.py, resync off). Every
   pre-frame and post-frame field of Dashdance's recording is diffed against that: the first divergent frame,
   the fields, and the inputs and states leading up to it.
2. Dashdance re-simulates the same replay headless (dashdance_resim.py). If it reproduces its own recording
   there, the bug is deterministic in the translated game or the HLE; if it matches Slippi instead, the bug is
   in something only a live game does (online inputs, rollback, timing).
3. Both emulations snapshot guest RAM entering the divergent frame (slippi_ram.py, the playback build's
   MELEE_RAM_DUMP_FRAMES) and ramdiff.py compares fighters and their animation state. Identical state there
   means the difference comes from code that runs during that frame.
4. Slippi Dolphin's frames around the divergence are rendered as PNGs.

Everything goes to reports/desync/<replay>/: report.md, resim.slp (Slippi), dashdance.slp, ram/, frames/.
"""
import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dashdance_resim  # noqa: E402
import ramdiff  # noqa: E402
import slippi_frames  # noqa: E402
from slp import Replay, diff_records, state_name  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
# Recorded inputs are copied into the regeneration, so these never differ; the rest is simulation.
INPUT_FIELDS = {"joy_x", "joy_y", "cstick_x", "cstick_y", "trigger", "buttons", "phys_buttons", "phys_l", "phys_r",
                "raw_x", "raw_y"}
# Bits that depend on the camera, not the simulation: flags5 0x80 is "is offscreen", which Dashdance's
# widescreen camera legitimately changes. Masked out of the comparison and counted separately.
COSMETIC_MASKS = {"flags5": 0x80}


def fmt(v):
    return f"{v:.4f}" if isinstance(v, float) else str(v)


def sim_diffs(a, b):
    out = []
    for name, x, y in diff_records(a, b):
        mask = COSMETIC_MASKS.get(name, 0)
        if (x & ~mask) != (y & ~mask) if mask else True:
            out.append((name, x, y))
    return out


def cosmetic_count(ours, theirs):
    return sum(1 for k in ours.post if k in theirs.post for name, mask in COSMETIC_MASKS.items()
               if name in ours.post[k] and name in theirs.post[k] and (ours.post[k][name] ^ theirs.post[k][name]) & mask)


def first_divergence(ours, theirs):
    frames = sorted({k[0] for k in ours.post} & {k[0] for k in theirs.post})
    for frame in frames:
        diffs = {}
        for port, follower in ours.players():
            key = (frame, port, follower)
            d = []
            if key in ours.pre and key in theirs.pre:
                d += [("pre." + n, a, b) for n, a, b in diff_records(ours.pre[key], theirs.pre[key]) if n not in INPUT_FIELDS]
            if key in ours.post and key in theirs.post:
                d += [("post." + n, a, b) for n, a, b in sim_diffs(ours.post[key], theirs.post[key])]
            if d:
                diffs[(port, follower)] = d
        if diffs:
            return frame, diffs, len(frames)
    return None, {}, len(frames)


def classify(replay, out, ours, theirs, frame, diffs):
    """Re-simulate in Dashdance and say which side of the online/offline line the bug is on."""
    print("re-simulating in Dashdance ...", flush=True)
    mine = Replay(dashdance_resim.resim(replay, out / "dashdance.slp"))
    lines = ["## Does Dashdance reproduce it offline?", ""]
    for port, follower in diffs:
        k = (frame, port, follower)
        if k not in mine.post:
            lines.append(f"Dashdance's re-simulation has no frame {frame} for port {port + 1}.")
            continue
        matches_recording = not sim_diffs(mine.post[k], ours.post[k])
        matches_slippi = not sim_diffs(mine.post[k], theirs.post[k])
        state = state_name(mine.post[k]["state"])
        if matches_recording:
            lines.append(f"Port {port + 1}: yes. Dashdance's headless re-simulation is in {state} at frame {frame}, like its "
                         "recording, so the bug is deterministic in the translated game or the HLE, not in online play.")
        elif matches_slippi:
            lines.append(f"Port {port + 1}: no. This build's re-simulation matches Slippi Dolphin ({state}). Either the bug is "
                         "already fixed since the recording was made, or it came from something only a live game does: "
                         "online inputs, rollback, or timing.")
        else:
            lines.append(f"Port {port + 1}: it differs from both ({state}); compare dashdance.slp by hand.")
    return lines + [""]


def compare_ram(replay, out, frame):
    """RAM entering the divergent frame, in both emulations, compared field by field."""
    ram = out / "ram"
    ram.mkdir(exist_ok=True)
    print(f"snapshotting RAM entering frame {frame} ...", flush=True)
    dashdance_resim.resim(replay, ram / "dashdance.slp", ram_frames=[frame])
    (ram / "dashdance.slp").unlink(missing_ok=True)
    if subprocess.run([sys.executable, str(Path(__file__).with_name("slippi_ram.py")), str(replay), str(ram / "dolphin"),
                       str(frame)]).returncode:
        return ["RAM snapshot from Slippi Dolphin failed; see the output above.", ""]
    count, detail = ramdiff.compare(ramdiff.Ram(ram / f"ram_{frame}.bin"), ramdiff.Ram(ram / f"dolphin/ram_{frame}.bin"))
    lines = [f"## Guest RAM entering frame {frame}", "",
             f"ram/ram_{frame}.bin (Dashdance) vs ram/dolphin/ram_{frame}.bin (Slippi Dolphin): {count} differences outside "
             "the known-cosmetic fields." + (" The state entering the frame matches, so the divergence comes from code that "
             "runs during it: trace that frame's fighter logic (MELEE_WATCH_ADDR, --trace-calls) against the decomp."
             if count == 0 else " The first differing field is where to start."), "", "```", *detail, "```", ""]
    return lines


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("replay", type=Path)
    ap.add_argument("--context", type=int, default=8, help="frames of history to show and render before the divergence")
    ap.add_argument("--no-frames", action="store_true", help="skip rendering Slippi's frames")
    ap.add_argument("--reuse", action="store_true", help="reuse an existing resim.slp instead of re-simulating")
    ap.add_argument("--no-dashdance", action="store_true", help="skip Dashdance's own re-simulation and the RAM comparison")
    ap.add_argument("--no-ram", action="store_true", help="skip the RAM snapshots and comparison")
    args = ap.parse_args()

    replay = args.replay.expanduser().resolve()
    ours = Replay(replay)
    out = ROOT / "reports/desync" / replay.stem
    out.mkdir(parents=True, exist_ok=True)
    if not (args.reuse and (out / "resim.slp").exists()):
        print(f"re-simulating {replay.name} ({ours.last_frame} frames) in Slippi Dolphin ...", flush=True)
        slippi_frames.run(replay, ours.last_frame, ours.last_frame, out, frames=False)
    theirs = Replay(out / "resim.slp")
    if theirs.last_frame is None or theirs.last_frame < ours.last_frame:
        print(f"warning: the re-simulation stops at frame {theirs.last_frame}, the recording at {ours.last_frame}")
    frame, diffs, compared = first_divergence(ours, theirs)

    lines = [f"# Desync report: {replay.name}", "",
             f"Dashdance's recording vs Slippi Dolphin re-simulating its inputs (resync off), {compared} frames compared.",
             f"Ignored: {cosmetic_count(ours, theirs)} player-frames where only the camera-dependent offscreen bit differs.", ""]
    if frame is None:
        lines += ["No divergence: every compared pre-frame and post-frame field matches."]
    else:
        lines += [f"**First divergent frame: {frame}** (online frame {frame + 124}).", ""]
        for (port, follower), d in diffs.items():
            who = f"port {port + 1}" + (" (follower)" if follower else "")
            lines += [f"## {who}", "", "| field | Dashdance | Slippi Dolphin |", "|---|---|---|"]
            for name, a, b in d:
                if name.endswith(".state"):
                    a, b = state_name(a), state_name(b)
                lines.append(f"| {name} | {fmt(a)} | {fmt(b)} |")
            lines += ["", f"History for {who} (Dashdance's recording; inputs are what both simulations read):", "",
                      "| frame | state (ours) | state (Slippi) | x | y | joy x | joy y | c x | c y | trigger | buttons |",
                      "|---|---|---|---|---|---|---|---|---|---|---|"]
            for f in range(frame - args.context, frame + 4):
                k = (f, port, follower)
                if k not in ours.pre or k not in ours.post:
                    continue
                pre, post = ours.pre[k], ours.post[k]
                slippi_state = state_name(theirs.post[k]["state"]) if k in theirs.post else "-"
                lines.append(f"| {f} | {state_name(post['state'])} | {slippi_state} | {post['x']:.2f} | {post['y']:.2f} | "
                             f"{pre['joy_x']:.4f} | {pre['joy_y']:.4f} | {pre['cstick_x']:.4f} | {pre['cstick_y']:.4f} | "
                             f"{pre['trigger']:.3f} | {pre['buttons']:08X} |")
            lines.append("")
        if not args.no_dashdance:
            classification = classify(replay, out, ours, theirs, frame, diffs)
            lines += classification
            if not args.no_ram and any(": yes." in line for line in classification):
                lines += compare_ram(replay, out, frame)
        if not args.no_frames:
            lo, hi = frame - args.context, frame + 6
            written, _, _ = slippi_frames.run(replay, lo, hi, out / "frames")
            (out / "frames/resim.slp").unlink(missing_ok=True)  # partial; the full one is out/resim.slp
            lines += [f"Slippi Dolphin's frames {lo}..{hi} ({written} images) are in frames/frame_<n>.png."]
    (out / "report.md").write_text("\n".join(lines) + "\n")
    print("\n".join(lines))
    print(f"\nreport: {out / 'report.md'}")
    return 0 if frame is None else 1


if __name__ == "__main__":
    sys.exit(main())
