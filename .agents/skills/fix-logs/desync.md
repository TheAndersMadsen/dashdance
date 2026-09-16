# Fixing a desync

A `DESYNC: checksum mismatch at frame N` means Dashdance and the opponent's Slippi Dolphin simulated the
same inputs differently. Real Melee is the reference. These tools find the frame, the side, the state and
the code. Fix 7 in `docs/MAC_FIXES.md` is a worked example of the whole path.

## 0. Find the replay

The session log that has the `DESYNC` line wrote the game's replay to
`~/Library/Application Support/Dashdance/Replays/Game_<timestamp>.slp`. The timestamp is when the game
started, inside the session. The log counts online frames, and replay frame = online frame - 124.

## 1. Run the pipeline

```bash
tools/mac/desync.py "<replay.slp>"
```

It takes a few minutes and writes `reports/desync/<replay>/` (gitignored). Read `report.md`:

1. **First divergent frame** and the fields that differ, per player. It also shows a table of the frames
   before it, with both simulations' action states and the recorded inputs. The first player listed may be
   a knock-on. A lone `post.instance` difference on the other player is one.
2. **Does Dashdance reproduce it offline?**
   - *Yes*: headless Dashdance, fed the recorded inputs, repeats its own recording. The bug is deterministic
     in the translated game or the HLE. Go to step 2.
   - *No, matches Slippi*: either this build already fixed it, or the cause only happens in live play
     (remote input handling, rollback, timing). Check `git log` since the replay's date. Then read the session
     log around the mismatch for rollbacks and input delay.
3. **Guest RAM entering the frame** (only when it reproduces): fighter fields by decomp name, bone
   animation, and player blocks, from both emulators.
   - *Fields differ*: the state had already diverged. Find who wrote the field (step 2).
   - *Zero differences*: the state and the inputs were identical, so the bug is in code that ran during
     that frame.
4. `frames/frame_<n>.png` is Slippi Dolphin's picture of each frame, for describing what should have happened.

Useful flags: `--reuse` skips the Slippi re-simulation if `resim.slp` exists. `--no-ram`, `--no-dashdance`
and `--no-frames` skip the slow parts.

## 2. Find the code

- Name the action states in `deps/melee/src/melee/ft/kinds/ftCommon/`. `slp.py` names them from
  `forward.h`. For each state, find its IASA/Anim/Phys callbacks, and the `*_CheckInput` functions that
  decide the transition that went differently.
- Read the translated function next to the decomp. Generated C++ is in
  `build/mac/generated/guest/guest_*.cpp` (online build) or `build/mac-playback-replay/generated/guest/`
  (replay build). Search for `void f_<ADDR>`. Gecko caves are spliced into the hooked function; their code
  is in `port/slippi_sys/GameSettings/GALE01r2.ini` (search `C2<addr without 80>`).
- To catch a store to a field in the playback build, set `MELEE_WATCH_ADDR=0x.. MELEE_WATCH_LEN=4`. Pass it
  through `dashdance_resim.run(..., env=...)` or run the headless binary directly. It logs the function and
  the recent calls.
- Classes of bug seen so far:
  - **Control flow the recompiler can't see statically.** Example: fix 7, a Gecko code returning to the
    return address + 8. Suspect it when state and inputs match but a check returns the other answer.
  - Float semantics in `port/runtime/ppc/numeric.*`.
  - HLE behaviour in `port/runtime/hle/`.

Fix in `port/recomp/` or the runtime, add a test under `port/tests/` (see `return_adjust_test.py`), then:

```bash
tools/mac/dashdance_resim.py "<replay.slp>"   # rebuilds the playback build with your change
```

Compare the new `dashdance.slp` with `resim.slp`: `desync.py --reuse --no-ram --no-frames` should report no
divergence. Also re-run one clean Slippi Dolphin replay (for example `~/Documents/Slippi/*.slp`) and check it
still matches its own recording, to catch regressions. Then continue with SKILL.md step 3: rebuild, commit,
docs, push. `rebuild.sh` installs the app so Christopher plays the fix.

## The tools

| Tool | What it does |
|---|---|
| `tools/mac/desync.py REPLAY` | Runs the whole pipeline above. |
| `tools/mac/slippi_frames.py REPLAY START END` | Slippi Dolphin PNG per frame + `resim.slp`, its regenerated replay. |
| `tools/mac/dashdance_resim.py REPLAY [--ram-frames F,..]` | Headless Dashdance re-simulation → `dashdance.slp`, optional RAM snapshots. |
| `tools/mac/slippi_ram.py REPLAY OUT FRAME..` | Slippi Dolphin guest RAM entering those frames (lldb). |
| `tools/mac/ramdiff.py OURS.bin DOLPHIN.bin` | Structured RAM comparison. |
| `tools/mac/slp.py` | Replay reader: all pre/post-frame fields, `diff_records`, `state_name`. |

## Things that bite

- **Slippi playback's "resync" hides desyncs.** It snaps positions back to the recording. Every tool
  turns it off.
- **Frame dumping is disabled in the Mac playback Dolphin** (built without libav). `slippi_frames.py`
  patches a copy in `~/Library/Caches/Dashdance/slippi-frames/`. The copy needs the OpenGL backend (Vulkan
  crashes) and an existing `Dump/Frames` folder. The Launcher's own Dolphin and its user folder are never
  touched.
- **"Waiting for game" is what playback shows after a replay ends**, not a hang.
- **lldb with fastmem on** stops on every deliberate access fault. `slippi_ram.py` turns fastmem off, and
  gives Dolphin the environment (it crashes without `HOME`).
- **Known cosmetic differences.** Post-frame `flags5` 0x80 (offscreen) depends on Dashdance's widescreen
  camera. In RAM: AXDriver voice ids, draw flags, drop shadows, and two player-block words (`ramdiff.py`
  lists them). Heaps sit at slightly different addresses, so compare structures, never raw bytes.
- **The playback build verifies its sources.** Any edit means regeneration, which `dashdance_resim.py` does
  on every run (about 30-60 s).
