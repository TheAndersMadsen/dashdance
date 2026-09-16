#!/usr/bin/env python3
"""Snapshot Melee's RAM inside Slippi Dolphin at chosen frames of a replay.

Usage: tools/mac/slippi_ram.py REPLAY.slp OUT_DIR FRAME [FRAME ...]

Writes OUT_DIR/ram_<frame>.bin (24 MB of guest RAM) taken when the game asks the Slippi device for that frame's
inputs (CEXISlippi::prepareFrameData), before the frame is simulated. Dashdance's playback build writes the same
snapshot at the same point (MELEE_RAM_DUMP_FRAMES, see slippi_playback.cpp), so tools/mac/ramdiff.py can compare
them field by field.

How: the patched playback Dolphin from slippi_frames.py runs under lldb with a breakpoint on prepareFrameData,
and each hit reads the frame number from the payload and copies Memory::m_pRAM. Fastmem is turned off because
its deliberate access faults would stop the debugger on every guest memory access. Resync is off, like the rest
of the desync tools. Needs Xcode's lldb; the script re-runs itself under Xcode's Python to get the lldb module.
"""
import json
import os
import shutil
import struct
import subprocess
import sys
import time
import uuid
from pathlib import Path

HERE = Path(__file__).resolve().parent


def main():
    if len(sys.argv) < 4:
        print(__doc__.split("\n\n")[1], file=sys.stderr)
        return 2
    try:
        import lldb
    except ImportError:
        if os.environ.get("SLIPPI_RAM_REEXEC"):
            print("slippi_ram: cannot import lldb even under Xcode's Python", file=sys.stderr)
            return 1
        pythonpath = subprocess.run(["lldb", "-P"], capture_output=True, text=True, check=True).stdout.strip()
        env = {**os.environ, "PYTHONPATH": pythonpath, "SLIPPI_RAM_REEXEC": "1"}
        return subprocess.run(["/usr/bin/python3", __file__, *sys.argv[1:]], env=env).returncode

    sys.path.insert(0, str(HERE))
    import slippi_frames as sf

    replay = Path(sys.argv[1]).expanduser().resolve()
    out = Path(sys.argv[2]).expanduser().resolve()
    wanted = {int(f) for f in sys.argv[3:]}
    out.mkdir(parents=True, exist_ok=True)
    work = sf.CACHE / "runs" / f"ram-{time.strftime('%Y%m%d-%H%M%S')}-{uuid.uuid4().hex[:6]}"
    user = work / "User"
    shutil.copytree(sf.PLAYBACK_USER, user, ignore=shutil.ignore_patterns("Logs", "Cache", "ScreenShots", "StateSaves", "Dump"))
    sf.set_ini(user / "Config/Dolphin.ini", {
        ("Movie", "DumpFrames"): "False", ("Core", "EmulationSpeed"): "0.00000000", ("Core", "Fastmem"): "False",
        ("Core", "SlippiEnableSpectator"): "False", ("Core", "SlippiJukeboxEnabled"): "False",
        ("Core", "SlippiSaveReplays"): "False", ("DSP", "Volume"): "0", ("Interface", "ConfirmStop"): "False",
    })
    comm = work / "comm.json"
    last = max(wanted) + 2
    comm.write_text(json.dumps({"mode": "normal", "replay": str(replay), "commandId": uuid.uuid4().hex,
                                "startFrame": last, "endFrame": last, "shouldResync": False}))
    iso = (sf.ROOT / ".disc-path").read_text().strip()

    debugger = lldb.SBDebugger.Create()
    debugger.SetAsync(False)
    target = debugger.CreateTarget(str(sf.patched_app()))
    target.BreakpointCreateByName("CEXISlippi::prepareFrameData")
    info = lldb.SBLaunchInfo(["-u", str(user), "-i", str(comm), "-e", iso, "-b", "--hide-seekbar", "-v", "OGL"])
    info.SetEnvironmentEntries([f"{k}={v}" for k, v in os.environ.items()], True)  # Dolphin needs HOME
    error = lldb.SBError()
    process = target.Launch(info, error)
    if not error.Success():
        print(f"slippi_ram: launch failed: {error}", file=sys.stderr)
        return 1
    deadline = time.time() + 600
    try:
        while wanted and process.GetState() == lldb.eStateStopped and time.time() < deadline:
            thread = process.GetSelectedThread()
            if thread.GetStopReason() == lldb.eStopReasonBreakpoint:
                payload = thread.GetFrameAtIndex(0).FindRegister("rsi").GetValueAsUnsigned()
                frame = struct.unpack(">i", process.ReadMemory(payload, 4, error))[0]
                if frame in wanted:
                    ram_ptr = target.FindSymbols("Memory::m_pRAM")[0].GetSymbol().GetStartAddress().GetLoadAddress(target)
                    base = struct.unpack("<Q", process.ReadMemory(ram_ptr, 8, error))[0]
                    (out / f"ram_{frame}.bin").write_bytes(process.ReadMemory(base, 0x01800000, error))
                    print(f"slippi_ram: frame {frame} -> {out / f'ram_{frame}.bin'}", flush=True)
                    wanted.discard(frame)
            process.Continue()
    finally:
        process.Kill()
        shutil.rmtree(work, ignore_errors=True)
    if wanted:
        print(f"slippi_ram: frames never reached: {sorted(wanted)}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
