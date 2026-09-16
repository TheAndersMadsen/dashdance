# Mac fixes from playtesting

Found by playing Dashdance on an M4 Pro MacBook Pro (macOS 26) with a Mayflash adapter in Wii U mode.
Details and evidence for each are in `docs/MAC_FIXES.md`.

## What this changes

- Read the GameCube adapter when macOS rejects timed reads on its interrupt pipe (it was never read at all on this Mac).
- Quit with Cmd+Q from full screen.
- Show the adapter's measured polling rate in the HUD instead of rounding anything under 900 Hz to "125 Hz".
- Write a log file for every Mac session (no Mac session ever wrote one, so crashes left no trace).
- Keep the launcher from crashing when a replay's player name isn't valid UTF-8.
- Show live ping in the performance HUD during online matches.
- Add `tools/mac/` scripts: rebuild, capture a report, list unreviewed logs, probe the adapter, and render replay frames with Slippi's playback Dolphin (`slippi_frames.py`) as desync ground truth.

## Still open

- Mid-match crash in recursive guest calls (`f_80373078` / `f_8036F1F8`), same class as Hero88go/melee-unlocked#5.
- Shield-drop desync: Dashdance spot-dodges where Melee drops through the platform (first divergent frame 3791 in the logged replay).

## Before opening

- Drop the "Fork workflow" section from `CLAUDE.md`, `.agents/skills/fix-logs`, `.claude/skills/fix-logs` and this file.
