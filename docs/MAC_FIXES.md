# Mac fixes log

Fixes found by playing Dashdance on a MacBook Pro (M4 Pro, macOS 26) with a Mayflash adapter in Wii U mode.
Each fix is its own commit on `christopher/mac-fixes`, so any subset can go upstream as a PR.

How the loop works: play, then run `/fix-logs`. It reads the session logs and crash reports nobody has
reviewed, logs what broke here, fixes it one commit at a time, rebuilds, and adds a bullet to `docs/PR.md`.
A fix counts as confirmed once it has been played through again.

## Development tooling

- 2026-09-27: Installed the ten MIT-licensed iPhone Duo skills at upstream revision
  `b8b9d15b4dc987d0c35940265e07f4128ba79361`, with shared references and Claude Code symlinks.
  `CLAUDE.md` routes Duo work through the [UIKit/Metal workflow](IPHONE_DUO.md).
  Validated upstream file equality, skill metadata and local links; this changes guidance, not runtime code.

## Open issues

| # | Found | What happens | Evidence | Notes |
|---|---|---|---|---|
| | | None right now. | | |

## Fixed

| # | What was wrong | Fix | Commit |
|---|---|---|---|
| 9 | Fresh installs failed at "Generating the port": setup.sh cloned doldecomp/melee at upstream master, which has moved past the pinned revision bootstrap_port.py requires (issue #3, Tahoe 26.5.1). | setup.sh now checks the decomp checkout out at the pinned revision (fetch by commit, detach) and the pin error names the remedy: run ./setup.sh. | see git log |
| 10 | Esc-menu Volume muted music at 0%, but jukebox music stayed at full level from 10% upward: the mixer scaled game samples before adding music at its own gain. | Apply the master volume to the jukebox contribution on Mac and Windows. A synthetic HPS track tests 0%, 10%, 50%, 100% and independent Music gain; the player confirmed the live fix. | see git log |
| 14 | Direct games dropped 30.7 to 32.0 s after connecting on a healthy link (six times on 2026-09-19 against The__Phenom2009: `session-20260919-114240.log`, `-122015`, `-122449`, `-122828`; report `winner -3, end 7`). Both players dial each other. When a player's real port differs from the one matchmaking advertised (their router remaps it), the attempt to the advertised port never connects, sits alone under its own address key and expires at ENet's 30 s cap; `all peers gone` was judged per address, so the expiry marked the player inactive and the netplay loop closed the healthy connection. Reproduced with two local instances, one dialling a dead port for the other: the victim's log matches the real sessions line for line. | A player is gone only when no connected peer of theirs is left under any address. Same test now plays through the 30 s mark; the normal two-connection case still closes its duplicate and stays in sync (2100 checksums, 0 mismatched). In the real sessions this Mac was the side that got closed, so the opponent's client needs the same fix: the logic is inherited from Slippi Dolphin. `slippi: peer ...` and `peer health` lines, `MELEE_NET_DROP_CONNECT=1` and `--local-peer` with a wrong port are the tools. | see git log |
| 15 | A GameCube controller on an Input Integrity Lossless Adapter in PC (XInput) mode played, but the dashboard listed it as "Xbox One Wireless Controller · Bluetooth · move a stick to measure the report rate" and the Ready to compete card could only say "move a stick". The adapter (VID 045E PID 028E, four Xbox 360 interfaces, every endpoint at bInterval 1) is claimed by Apple's `XboxGamepad.dext`; SDL names every pad from that driver by its category, so the name never matched the framework's "Lossless adapter [XInput]" and the measured rate was never attached; `attachedToDevice` is false for USB pads on the Mac, so it read as Bluetooth. `session-20260929-205533.log`. | On the Mac, `controller_rate.mm` counts input reports from USB HID gamepads through IOHIDManager (a shared open next to the system driver): 3001 reports in 3.00 s from this adapter, so 1000 Hz shows the moment it is plugged in, and a device seen over USB is wired. Reports are paired with SDL gamepads by name first and by order for the leftovers, the framework's product string is shown instead of SDL's guess, and "Lossless" in the name marks a GameCube controller for the rate line and the readiness card. Both adapter modes are documented on the dashboards; the Switch/Dolphin mode already went through the WUP-028 path. Both dashboards log every controller row they show (`dashboard: controller row: ...`), and `--iso` launches open the controllers before the readiness lines are written. Verified in the installed app: `Lossless adapter [XInput] · USB · GameCube controller · polling at 1000 Hz (1.0 ms)`. iOS 18+ supports wired Xbox controllers, so XInput mode should also connect on an iPad; not tried on a device. The adapter's LED colours are not documented on the vendor's site beyond green/red for Switch/Dolphin mode; a purple LED in XInput mode coincided with an unchanged descriptor set, driver stack and 1000 Hz report stream, and Ghidra shows Apple's `XboxGamepad.dext` never writes to a 360-protocol device (`Xbox360Gamepad::setReport` returns unsupported; `XboxHIDDevice::Start_Impl` only sets up the IN pipe at the endpoint interval), so the host cannot be driving it and rumble cannot work in this mode. The companion app (Google Drive) could not be fetched without a browser session. | see git log |
| 16 | The app died whenever a controller connected after launch: `Dashdance-2026-09-29-205439.ips` (the player's session `session-20260929-204951.log` ends with `controller: Lossless adapter [XInput] connected`) and four `melee_port_mac` reports between 21:06 and 21:15 while reproducing with `--iso`, all `BUG IN _GCDevicePhysicalInput: Recursive or concurrent mutation detected` inside GameController. `controller_rate.mm` set the controller's `handlerQueue` to a private queue and installed a change handler from inside the connect notification, which the framework posts during its own event transaction for the new device; a controller present at launch was configured outside a transaction and survived. | Handlers stay on the main queue and are installed on the next main-queue turn after the notification. Reproduced 4/4 before (no simulated frame ever ran), then a `--iso` launch with the adapter connecting after start ran through boot with the pad opened and no crash report. | see git log |
| 1 | GameCube adapter never read on this Mac: `ReadPipeTO` on the interrupt pipe returns `kIOReturnBadArgument`, so the reader thread gave up after 20 failures. | Fall back to blocking `ReadPipe` when timed reads are rejected; abort the pipe on close so the blocking read wakes. | see git log |
| 2 | No way to quit from full screen except the Dock. | Cmd+Q requests exit from the SDL event loop. | see git log |
| 3 | HUD showed any adapter rate below 900 Hz as "125 Hz" (the Mayflash overclocks to ~540 Hz). | Show the measured rate. | see git log |
| 6 | No way to see ping during a match (it was only written to the log every 10 s). | HUD shows "ping N ms" while connected to an online opponent. Not yet seen in a live match. | see git log |
| 5 | Launcher crashed on open (`+[NSTextField wrappingLabelWithString:]` assertion in `refreshGames`, 2026-09-16 10:46). `ns()` used `stringWithUTF8String`, which returns nil for invalid UTF-8, and replay names are Shift-JIS on console. | `ns()` falls back to Shift-JIS, then Latin-1. Launcher stayed up after the fix; the original crash was not reproduced on demand, so the cause is inferred from the stack. | see git log |
| 7 | Desync against Dolphin players whenever the UCF 0.84 shield drop fired (seen twice on 2026-09-16: `Game_20260916T121422` frame 3791, Marth spot-dodged instead of dropping through Stadium's platform; `Game_20260916T131538` frame 1058, a shield out of landing lag came a frame late, then the drop was missed). The UCF code returns to its caller's return address + 8 (`lwz r7,28(r1); addi r7,r7,8; mtlr r7; blr`) to skip the caller's `li r3,1`, so the spot-dodge check reports "no input" and the next check (shield or drop) runs. The recompiler turns every `blr` into a C++ `return`, so the caller always resumed right after the call: the check reported "handled" with no state change, and the fighter lost that frame's input. | `analyze.py` recognises returns to the return address + N and the emitter resumes the caller there (`if (c.lr == ret+N) goto`). Headless re-simulations of both replays now match Slippi Dolphin on every player-frame (7840 and 2372), and a Slippi Dolphin replay from 2025-10-12 still matches its recording (16916). The full decision is now unit-tested against the translated code: `tools/mac/shield_drop_test.sh` drives `ftCo_8009980C`/`ftCo_80099894`/`ftCo_8009A080` with the Game_20260916T121422 stick dive (-0.39, -0.60, -0.74, -0.96) and the disc thresholds (spot dodge y <= -0.7 within 4 frames, UCF skip y in (-0.8, -0.7] with the |x|,|y| >= ~0.63 diagonal, platform pass y <= -0.66 within 6 frames) and expects the resim outcomes (25 checks). Online desyncs now also name their first domino: a remote-input gap (newest remote input older than the simulated frame) is logged when it opens and again on the next checksum mismatch. Not yet confirmed in a live match. | see git log |
| 8 | Crash mid-match, `guest call depth exceeded` in `HSD_JObjSetupMatrixSub` (2026-09-16 09:36, 10:44, 14:23, 15:37; `session-20260916-140128.log`, `session-20260916-152215.log` and its `.ram`). The 15:37 RAM showed the JObj has no parent and the guest stack was 16 frames deep, so the game was not recursing: the host counter in `ppc::call` had drifted to 20000. It counted with ++/-- around the call, and guest `longjmp` (a C++ exception) skipped the decrement for every frame it unwound. Stage animation lookups (`grAnime_801C8318`: setjmp, `HSD_ForeachAnim`, a callback that longjmps) do that every frame a background animates; a Battlefield replay leaked 1 to 801 in 11019 frames, and the count carries across games. | `CallDepthScope` guard decrements on unwind. The same replay stays at depth 1 with an identical re-simulated recording; the dispatch test runs 25000 longjmps (old code faults). The frame log line shows `call depth N`. Not yet confirmed in play. | 2626ae4 |
| 11 | Cmd+Q during play crashed on exit (2026-09-25 09:18 report): AppKit's `terminate:` called `exit()` inside SDL's event pump, bypassing the game's worker shutdown. Static destruction then called `std::terminate()` while background threads were still running. The report does not identify which thread was destroyed first. | Both Mac Quit menus now cancel the launcher modal or request the game's normal exit, which joins workers before returning from main. Confirmed in play: Cmd+Q quit without a crash. | see git log |
| 12 | At 8x on an M1 Pro, GPU work fell behind 60 Hz. The render queue skipped every presentation while backlogged, leaving the last menu image on screen even as game input and audio advanced. The 2026-09-25 session log recorded 300, then 600 frames drained without presenting. | Present at least every eighth queued frame without changing the selected scale. On Mac, show an in-game warning after eight consecutive frames with at least eight queued behind them, and clear it after 120 backlog-free frames. A fullscreen scripted M1 Pro run stayed at 8x and presented 140 frames over 1200 simulated frames; sustained overload filled the 64-frame queue, adding 13.6 ms/frame of queue wait by frame 1200. This makes the display visibly update but does not make 8x sustainable at 60 Hz on this machine; lower the resolution manually for smooth play. The player confirmed the warning looks good in the running build; online play still needs a playtest. | see git log |
| 13 | Seven iOS launcher crashes on 2026-09-27 23:14-23:25 (`viewDidLayoutSubviews` formatting a string, one stack overflow entering `buildDisplay`): all from an intermediate Duo-session build whose source never landed; the final Duo commits (2cc8232, 9780146) restructured exactly that display card. | Reviewed and re-verified: the merged build launches clean on the iPhone Duo simulator through the same paths (viewDidLoad, buildDisplay, viewDidLayoutSubviews) with sample dashboard data. The current format arguments are all safe (`%@` on a copied property, `%s` on a static literal). | see git log |

| 4 | No session logs at all: the first `host::log` ran before `main_mac` set the log path, so the file opened relative to cwd (`/` from Finder), failed, and was never retried. Crashes left no trace. | Buffer lines until the path is set; one timestamped log per launch in `~/Library/Application Support/Dashdance/Logs/`, newest 50 kept, `latest.log` symlink. | see git log |

## Investigating a desync

`tools/mac/desync.py <replay.slp>` does the whole loop and writes `reports/desync/<replay>/report.md`:

1. Slippi Dolphin re-simulates the replay from its recorded inputs (`slippi_frames.py`, a patched copy of the
   Launcher's playback Dolphin, resync off). Every pre-frame and post-frame field is diffed against Dashdance's
   recording: first divergent frame, fields, and the inputs leading up to it. The online frame is replay frame + 124.
2. Dashdance re-simulates the same replay headless (`dashdance_resim.py`, the playback code set translated into
   `build/mac-playback*`). Reproducing its own recording means a deterministic bug in the translated game or HLE;
   matching Slippi means an online-only path (or a bug already fixed).
3. Guest RAM entering the divergent frame, from both (`slippi_ram.py` attaches lldb to Dolphin;
   `MELEE_RAM_DUMP_FRAMES` in the playback build), compared by `ramdiff.py`: fighters by decomp field name, bone
   animation, player blocks, pointers normalised. Identical state means the bug is in code run during that frame.
4. Slippi's frames around the divergence as PNGs.

For fix 7 the state entering the frame was identical and the inputs identical, which pointed at control flow in
that frame; reading the translated `ftCo_80099794` next to the UCF cave found the return-address trick.

## Setup notes that are not code

- `install.sh` does not work as-is: its `--depth 1` clone lacks the upstream commit `bootstrap_port.py` checks, and `setup.sh` clones doldecomp/melee at HEAD instead of the pinned revision. Workaround: full clone, and `git -C deps/melee checkout 05a1394faea2aac458e4bdd030621d8a5631ae62`.
- 1000 Hz polling without a driver (the README claim) did not work here: `SetPipePolicy` is rejected, and with no driver Apple's HID driver owns the adapter exclusively. The legacy GCAdapterDriver.kext (Permissive Security, SIP off) gives ~540 Hz, which looks like the Mayflash hardware cap.

## iPhone Duo reserved-region correction — 2026-09-27

The Apple window bridge calculated pixel scale from output slots immediately after clearing them,
so safe-area and fold geometry stayed at 1× on Retina displays. It also projected each camera region
onto every edge, potentially consuming most of the touch-control area. Derive scale from the Metal
view and drawable, intersect camera regions with the view, and reserve only their nearest edge beyond
the existing safe inset. Verified with seven 3× geometry cases and successful iOS simulator/macOS
builds. Live fold transitions and camera avoidance across all poses remain unverified.

## iOS dashboard redesign — 2026-09-27

The Duo outer-display baseline clipped the identity and section headings, and Play sat below all
settings. Replaced the oversized hero with compact branding, moved Play and setup first, and made
settings and history rows wrap within safe-area columns. Columns now use UIKit's local reserved
region coordinates and the actual available width. Rounded glass cards retain Melee's yellow
headers; primary actions use a filled yellow surface for readable dark text. The seven-choice
resolution picker is a native menu. Launcher display information uses its own scene, and entrance
and Play animations respect Reduce Motion. See `docs/IPHONE_DUO.md` for the simulator evidence
and remaining pose checks. macOS and iOS simulator builds pass.
