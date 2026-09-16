# Mac fixes from playtesting

Found by playing Dashdance on an M4 Pro MacBook Pro (macOS 26) with a Mayflash adapter in Wii U mode.
Details and evidence for each are in `docs/MAC_FIXES.md`.

## Fixes

- Read the GameCube adapter when macOS rejects timed reads on its interrupt pipe (it was never read at all on this Mac).
- Quit with Cmd+Q from full screen.
- Show the adapter's measured polling rate in the HUD instead of rounding anything under 900 Hz to "125 Hz".
- Keep the launcher from crashing when a replay's player name isn't valid UTF-8.
- Fix desyncs against Dolphin players when UCF's shield drop fires. The Gecko code returns past its call site
  (return address + 8), and the recompiler now resumes the caller there instead of right after the call.
- The headless executable links again (it called Discord presence, which only the app has).

## Features

- Show live ping in the performance HUD during online matches.

## Logging

- Write a log file for every Mac session (no Mac session ever wrote one, so crashes left no trace).
- On a guest crash, log the guest call stack and all registers, and save guest RAM next to the session log.
  `MELEE_TEST_FATAL_RETRACE=N` triggers a fake crash to test this.
- Log every rollback (frame rolled back to, and from).

## Debugging tools we built

All in `tools/mac/`, written up for agents in the `fix-logs` skill (`desync.md`, `crash.md`).

- `rebuild.sh`, `report.sh`, `unreviewed.sh`, `gcadapter_probe.cpp`: rebuild and install, capture a bug
  report, list logs nobody has read, probe the adapter.
- `desync.py`: one command to find a desync. It finds the first frame where a replay stops matching real Melee,
  checks whether Dashdance repeats it offline, compares memory at that frame, and renders what Melee shows.
- `slippi_frames.py`: renders any frame range of a replay with Slippi's own Dolphin, as ground truth.
- `dashdance_resim.py`: plays a replay back in headless Dashdance on the Mac. This builds on the existing replay
  playback, which was Windows-only: `bootstrap_port.py --playback`, headless `--replay`, and
  `MELEE_RAM_DUMP_FRAMES` to save memory at chosen frames.
- `slippi_ram.py`: saves Slippi Dolphin's game memory at chosen frames, through the debugger.
- `ramdiff.py`: compares Dashdance's and Dolphin's memory field by field (fighters, animation).
- `crashram.py`: shows what a crash's broken object is and what points at it.
- `slp.py`: small replay reader the others share.

## Still open

- Mid-match crash in recursive guest calls (`f_80373078` / `f_8036F1F8`), same class as Hero88go/melee-unlocked#5.

## Before opening

- Drop the "Fork workflow" section from `CLAUDE.md`, `.agents/skills/fix-logs`, `.claude/skills/fix-logs` and this file.
