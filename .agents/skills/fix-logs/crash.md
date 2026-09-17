# Fixing a crash

A guest crash ends the session log with `FATAL: guest fault: <reason> (<value>) in <function>`. Just above it
is a block of diagnostics:

- recent function entries (a ring of the last 64);
- all registers;
- **guest call stack** (the PowerPC back chain; deep recursion shows as one line with a count);
- `guest RAM at the fault written to .../Logs/session-<stamp>.ram`. The newest three are kept.

A macOS `.ips` report with `SIGABRT` from `std::terminate` inside `exit` at the same time is the clean fatal
exit, not a second crash.

## 1. Read the fault

- Find the function in the decomp (`deps/melee/src`) and its caller in the call stack.
- Registers that look like pointers (`80xxxxxx`) are usually the object being worked on. Inspect them:

```bash
tools/mac/crashram.py "~/Library/Application Support/Dashdance/Logs/session-<stamp>.ram" <ADDR>
```

It prints the words at ADDR, every RAM word that points at ADDR (with the symbol, or the fighter field that
holds it), and for an `HSD_JObj` the decoded fields and parent chain, flagging a cycle.

## 2. Offline or online only?

The game's replay (`Replays/Game_<timestamp>.slp`) holds every frame the game finalized before the crash. The
last few frames of an online game are usually missing.

```bash
tools/mac/desync.py "<replay>" --no-ram --no-frames   # did it desync first? does headless Dashdance crash too?
tools/mac/dashdance_resim.py "<replay>" --ram-frames <F1>,<F2>
```

- **Headless Dashdance crashes too**: the crash is deterministic. Narrow it down with RAM snapshots at earlier
  frames (`crashram.py` on each) and `MELEE_WATCH_ADDR` on the corrupted field.
- **It doesn't** (the frames it has run clean): suspect what only a live game does. Rollback restores the
  guest RAM regions in `slippi_online.cpp` `Savestate`; every rollback is logged as
  `slippi: rollback to frame X from frame Y`. Check whether one happened shortly before the fault.

Test the crash diagnostics themselves with `MELEE_TEST_FATAL_RETRACE=<retrace>`, which raises a guest fault
once that retrace is reached.

## Known so far

- `guest call depth exceeded` is not always real recursion. Compare the guest call stack in the log: if it is
  shallow, the host counter leaked (fix 8 in `docs/MAC_FIXES.md`: a guest `longjmp` skipped the decrement).
  The `[frame N]` line logs `call depth N` every 60 frames; it should stay flat, so watch it grow in a headless
  replay (`dashdance_resim.py`, the log lands next to `--out`) to find the frames that leak.
- Before blaming an object, check its fields with `crashram.py`: for fix 8 the "self-parented JObj" guess from
  r3 = r31 was wrong (its parent was 0).
