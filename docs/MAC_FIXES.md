# Mac fixes log

Fixes found by playing Dashdance on a MacBook Pro (M4 Pro, macOS 26) with a Mayflash adapter in Wii U mode.
Each fix is its own commit on `christopher/mac-fixes`, so any subset can go upstream as a PR.

How the loop works: play, then run `/fix-logs`. It reads the session logs and crash reports nobody has
reviewed, logs what broke here, fixes it one commit at a time, rebuilds, and adds a bullet to `docs/PR.md`.
A fix counts as confirmed once it has been played through again.

## Open issues

| # | Found | What happens | Evidence | Notes |
|---|---|---|---|---|
| 1 | 2026-09-16 | Crash mid-match (SIGABRT from `ppc::fatal` inside recursive guest calls `f_80373078`/`f_8036F1F8`). Happened twice (09:36, 10:44). | `~/Library/Logs/DiagnosticReports/Dashdance-2026-09-16-093630.ips`, `-104424.ips` | Same class as Hero88go/melee-unlocked#5. The fatal reason string was never logged (fix 4); the next occurrence will have it in `Logs/`. Also: `host::die` calls `exit()`, and a static destructor then hits `std::terminate`, so a clean fatal exit becomes an abort. |
| 2 | 2026-09-16 | Desync in unranked, Sheik (us) vs Marth on Pokémon Stadium, about 63 s in. Checksums agreed through online frame 3908, then mismatched from 3915 on; the game ended as a no contest (end 7) at replay frame 3796. No rollbacks in the preceding ~90 frames. | `Logs/session-20260916-120928.log` lines 2117+, `Replays/Game_20260916T121422.slp` | The replay pins the moment: remote Marth dash-dances on the side platform (y=25), shields at 3789 while the stick rotates from left to down (y -0.39, -0.60, -0.74, -0.96), and our simulation spot-dodges at 3792. That is the shield-drop input (UCF widens it), so the opponent's Dolphin almost certainly dropped through the platform. Suspects: floating-point edge behaviour in the translated shield-drop / UCF comparison, or how remote stick bytes reach the pad buffers. Next: watch the replay in Slippi Launcher (Dolphin re-simulates from the recorded inputs) at 1:03 to confirm, then build a headless repro from the .slp inputs. |

## Fixed

| # | What was wrong | Fix | Commit |
|---|---|---|---|
| 1 | GameCube adapter never read on this Mac: `ReadPipeTO` on the interrupt pipe returns `kIOReturnBadArgument`, so the reader thread gave up after 20 failures. | Fall back to blocking `ReadPipe` when timed reads are rejected; abort the pipe on close so the blocking read wakes. | see git log |
| 2 | No way to quit from full screen except the Dock. | Cmd+Q requests exit from the SDL event loop. | see git log |
| 3 | HUD showed any adapter rate below 900 Hz as "125 Hz" (the Mayflash overclocks to ~540 Hz). | Show the measured rate. | see git log |
| 6 | No way to see ping during a match (it was only written to the log every 10 s). | HUD shows "ping N ms" while connected to an online opponent. Not yet seen in a live match. | see git log |
| 5 | Launcher crashed on open (`+[NSTextField wrappingLabelWithString:]` assertion in `refreshGames`, 2026-09-16 10:46). `ns()` used `stringWithUTF8String`, which returns nil for invalid UTF-8, and replay names are Shift-JIS on console. | `ns()` falls back to Shift-JIS, then Latin-1. Launcher stayed up after the fix; the original crash was not reproduced on demand, so the cause is inferred from the stack. | see git log |
| 4 | No session logs at all: the first `host::log` ran before `main_mac` set the log path, so the file opened relative to cwd (`/` from Finder), failed, and was never retried. Crashes left no trace. | Buffer lines until the path is set; one timestamped log per launch in `~/Library/Application Support/Dashdance/Logs/`, newest 50 kept, `latest.log` symlink. | see git log |

## Setup notes that are not code

- `install.sh` does not work as-is: its `--depth 1` clone lacks the upstream commit `bootstrap_port.py` checks, and `setup.sh` clones doldecomp/melee at HEAD instead of the pinned revision. Workaround: full clone, and `git -C deps/melee checkout 05a1394faea2aac458e4bdd030621d8a5631ae62`.
- 1000 Hz polling without a driver (the README claim) did not work here: `SetPipePolicy` is rejected, and with no driver Apple's HID driver owns the adapter exclusively. The legacy GCAdapterDriver.kext (Permissive Security, SIP off) gives ~540 Hz, which looks like the Mayflash hardware cap.
