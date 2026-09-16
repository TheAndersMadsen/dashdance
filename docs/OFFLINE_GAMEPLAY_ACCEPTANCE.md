# Offline gameplay acceptance

This gate answers one narrow question: did the portable offline executable
really navigate Melee into a VS match?  Process survival, requested retraces,
GX command/draw counts, DVD traffic, and music are useful diagnostics but are
not gameplay acceptance evidence.

## Two independent oracles

The menu path is read from GALE01 guest memory without writing it.  The pinned
`doldecomp/melee` state machine (`05a1394faea2aac458e4bdd030621d8a5631ae62`)
defines `state_machine` at `0x80479D30`.  Its current mode is byte `+0`; its
current mode-state id is byte `+3` (`0x80479D33`).  Telemetry names these
`mode_byte` and `state_byte`; its legacy packed value is unambiguously
`combined = (state_byte << 8) | mode_byte`.  `GameModeKind` and
`gmVsMode_StateId` give this ordered offline path:

| Milestone | Mode byte:state byte | Packed combined | Meaning |
| --- | --- | --- | --- |
| boot | `28:00` | `0x0028` | Melee boot/memory-card scene |
| main menu | `01:00` | `0x0001` | top-level menu mode |
| VS CSS | `02:00` | `0x0002` | offline VS character select |
| SSS | `02:01` | `0x0102` | offline VS stage select |
| in game | `02:02` | `0x0202` | offline VS simulation |
| results | `02:04` | `0x0402` | offline VS results |

Slippi's custom **online** in-game state is `02:08` (`combined=0x0802`).  It
must not satisfy the offline `02:02` milestone.

Actual match evidence comes separately from the raw stream in the generated
Slippi replay:

- `0x35`: command-size table and replay creation;
- `0x36`: game start;
- `0x37` and `0x38`: pre/post-frame events reaching a positive game frame;
- `0x3C`: frame bookend;
- `0x39`: game end, required only when accepting through results.

The checker requires a same-frame `0x37` -> `0x38` -> `0x3C` chain in raw-event
order at a positive frame.  Independent positive maxima do not pass.  A bounded
run stopped during a live match may have no `0x39`; shutdown still finalizes the
partial replay, and `--through in-game` accepts it only if that coherent frame
chain is present.  `--through results` additionally requires both scene `02:04`
and one `0x39` after all frame data.

## Deterministic input

`port/scripts/offline_vs_acceptance.txt` preserves the exact absolute
navigation from the upstream-proven `vs_match.txt`, then uses an `@match`
section.  The offline recorder starts that section once the first post-frame
event for frame 1 is observed.  Scripted movement is never itself treated as
proof that a match was reached.

The portable `InputScript` trace has an independent copy of the original
Win32 parser/sampler as a unit-test oracle.  Both implementations agree for
every retrace through 3000, with and without an artificial match start.  In
particular, a port mentioned anywhere in a script is connected from boot, and
same-frame entries retain file-order, per-port last-entry-wins behavior.

## Checker

The launch must include `--expect-scene 0x0202`.  After an authorized caller
runs the game with isolated profile/card/cache and replay directories:

```sh
python3 tools/check_offline_gameplay.py \
  --log /isolated/cache/melee_port.log \
  --replay /isolated/cache/replays \
  --through in-game \
  --min-game-frame 1
```

The log must also show strict AOT with zero interpreter fallback, successful
headless exit, and the runtime GCT load at the pinned `0x8065CC80` address.
Those are prerequisites, not substitutes for the two gameplay oracles.  The
checker also requires the adjacent `launch.json` and `result.json`, the verified
binary sidecar, exact build/runtime Sys identity, `scene_observed=true`, and
closed log/replay artifacts bound by path, byte size, and SHA-256.  Milestones
come from the structured scene timeline and must exactly match the hashed log's
anchored `accept: scene` lines.

Loaded-code provenance is fail-closed as well.  `loaded_module_records` is the
total one-based load-event count and may exceed the 128 retained-identity cap;
the retained `modules` must be unique, ordered by `first_event`, internally
consistent, and account for every event because `unrecorded_modules` must be
zero.  At least one complete, untruncated `SlippiCSS.dat` identity is required.
Its normalized SHA-256 and byte count must match the direct immutable
`GameFiles/GALE01/SlippiCSS.dat` input, so an unrelated GameFile load cannot
satisfy the dynamic-code provenance gate.

## Earlier Mac logs are not an acceptance pass

The isolated neutral-600 log
(`d2df6832f14a4011fc7e14a488760596dccd3fe049d615aef1d51bd69e01c5b2`)
and scripted-2400 log
(`7cb389d9145c4b5c37bdc285ec3216029d1f4a2af6fb1f7c0e7c86d699569398`)
both verified boot and the GCT load, but neither recorded scene bytes.  The
2400 run created a card and loaded additional assets, yet reported 0 replays
and never emitted a `0x35` recording command.  Neither log establishes CSS,
SSS, or gameplay regardless of its frame/GX totals.

## macOS notes (2026-09-16)

The headless binary now builds on macOS: `runtime_headless` links
`discord_rpc.cpp` and `slippi_login.cpp` (plus the Foundation framework) that
`exi_slippi.cpp` already references, and `--hidden` is accepted by
`melee_port_mac` to run without showing a window. Frame captures were also
fixed: they used to read the EFB after the game's next-frame clear had erased
it (all-black images); the readback is now encoded into the presenting frame's
command buffer before the clear, and works in drain mode with hidden windows.

Known issue: the scripted navigation in `offline_vs_acceptance.txt` stops
progressing on the Slippi main menu (mode `0x01`) on today's macOS builds.
Scene telemetry shows the boot-scene dialog answered and the main menu reached
(`0x28` → `0x01` at retrace 582 with a fresh card), but later entries (cursor
moves, A, START) leave the menu untouched while the same input pipeline exits
the boot scene correctly. Interactive (physical) input on the same menus works.
Until this is resolved, run the acceptance path interactively or investigate
the pad-injection path used once the Slippi main menu scene is active.

## Scene-gated script blocks (2026-09-16)

`@scene MODE STATE` blocks were added to the input script format: entries in a
block are keyed on the guest state-machine bytes (0x80479D30/33) instead of
absolute retraces, with frame numbers counted from the first retrace the gate
matches. This makes menu navigation immune to boot-length variance (the
fresh-card "Create Game Data?" flow used to swallow time-keyed entries).
`@match` entries remain match-relative and ungated; while a gate is active,
ungated time-keyed entries are suspended. The runtime PADRead injection now
passes the guest scene bytes to the script sampler on all three front ends.

A second diagnostic, `MELEE_FAKE_LOGIN=1`, makes the offline 0xB9 handler
report a logged-in state (name "PLAYER", code "#0001") so the Slippi main menu
unlocks without online services. Combined with `--hidden` and frame captures,
these tools exposed the full menu flow headlessly: boot card dialog → Slippi
main menu → Online Play (Ranked/Unranked/Direct/Teams/Party) → the online
character select renders and navigates correctly.

## Current blocker: memory-card save-completion notice

With online services enabled, the scripted flow answers "Yes" on the fresh-card
prompt; the game creates the card file correctly (90,176-byte .gci written) and
shows the "Game Data has been created" notice with a progress bar — but the
notice does not auto-dismiss (observed for 3,400+ frames). The save-completion
callback or timer the notice waits on is not being delivered by the card HLE.
Until it is, the offline VS acceptance (which needs the classic main menu,
reachable only after this notice) cannot complete headlessly. The card HLE
(`port/runtime/hle/hle_card.cpp`) should be checked for the
save-completion/interrupt delivery path against the SIRC/CARD timeline.
