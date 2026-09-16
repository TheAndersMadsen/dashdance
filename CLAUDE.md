# Dashdance — guide for coding agents and new contributors

Dashdance is a native macOS / iPadOS / iOS / visionOS client for Super Smash Bros. Melee with Slippi
Online, built by statically recompiling the game's PowerPC code to C++ and running it on a host
runtime with a Metal renderer. Read this file first; it tells you how the tree is laid out, how to
build, and the rules that are not obvious from the code.

## Fork workflow (Metroxe/dashdance, branch `christopher/mac-fixes`)

This checkout is Christopher's playtest fork. He plays, things break, and fixes land here one commit at a
time until they go upstream as one PR. This section is fork-only: drop it from the PR.

- Remotes: `fork` is Metroxe/dashdance (push here), `origin` is TheAndersMadsen/dashdance (upstream; never push).
- Work on `christopher/mac-fixes`. One fix per commit. Push to `fork` after each fix.
- `/fix-logs` (`.agents/skills/fix-logs/`) reviews unreviewed session logs and crash reports and fixes what they show.
- `docs/MAC_FIXES.md` is the issue and fix log. `docs/PR.md` is the running PR description: every fix adds one
  plain bullet there in the same change. Keep it short.
- Rebuild with `tools/mac/rebuild.sh`, never a bare cmake build (the input manifest must be regenerated after an edit).
- Capture evidence by hand with `tools/mac/report.sh "what happened"`; list unreviewed logs with `tools/mac/unreviewed.sh`.
- Desync ground truth: `tools/mac/slippi_frames.py <replay.slp> START END` renders those frames with Slippi's playback Dolphin
  (re-simulated from inputs, resync off) into `reports/slippi-frames/`, plus `resim.slp` with Slippi's action states and positions.
- The disc path is in `.disc-path`; `deps/melee` must stay at the pinned decomp commit `05a1394f`.

## One-command setup

Players use `install.sh` (the one line in the README): it clones into ~/Dashdance, asks for the disc, runs `setup.sh`, copies the app to
Applications and opens it. Developers call `setup.sh` directly:

```bash
./setup.sh /path/to/melee.iso            # macOS app -> dist/Dashdance.app
./setup.sh /path/to/melee.iso --ios      # iPad/iPhone Simulator app (needs Xcode)
./setup.sh /path/to/melee.iso --visionos # Vision Pro Simulator app (needs Xcode)
./setup.sh /path/to/melee.iso --device   # dist/Dashdance.ipa for AltStore/SideStore/Sideloadly; --team auto signs and installs over USB
```

There is no store or public-release distribution and there must not be: the built app contains the
translated game. `tools/release.sh` and `.github/workflows/release.yml` build DMG/IPA release assets for a
private fork only (both refuse public repositories).

The script installs Homebrew packages (cmake ninja python), clones doldecomp/melee into
`deps/melee`, extracts `main.dol` from the disc (`tools/extract_dol.py`), fetches the pinned Aurora
dependency, generates the port, builds, packages and opens the app. Nothing from the game is
committed; the disc stays where it is.

## Layout

| Path | What lives there |
|---|---|
| `port/app/main_mac.cpp` | The app entry point for all Apple platforms: options, launcher/dashboard, game loop start. |
| `port/app/ios/`, `port/app/macos/` | Info.plist templates. |
| `port/app/icons/AppIcon.icon` | Icon Composer bundle (Dashdance mark as a glass layer). `tools/package_macos_app.sh` and CMake compile it with `actool`. |
| `port/runtime/host/` | Host services: window + input (`window_sdl.cpp`), dashboards (`mac_launcher.mm`, `ios_launcher.mm`), controller mapping, deadzones, trigger point, rumble and the keyboard layout (`input_config.*`), the Connect a Controller pairing steps (`controller_pairing.h`), the live GameCube controller in the controller editors (`gc_diagram.*`, drawing the GPL-3.0 ControllerOverlays artwork layers in `port/app/art/controller`; overlay coordinates are in the artwork's 3828 x 2689 units), dashboard model (`dashboard.*`), retrace/timing (`host.cpp`), touch overlay (`overlay.*`), GameCube adapter (`gc_adapter_iokit.cpp` on macOS: IOKit with a 1 ms pipe policy; `gc_adapter_libusb.cpp` elsewhere), controller report-rate measurement (`controller_rate.mm`), in-game menu and HUD (`game_menu.*`, drawn through the Metal overlay's glyph atlas), local notifications (`notify_apple.mm`). |
| `port/runtime/gx/` | The GX (GameCube GPU) translation and the Metal renderer (`gx_metal.mm`, `gx_msl.cpp`). |
| `port/runtime/hle/` | High-level emulation of the GameCube SDK the game calls (disc, pads, audio, memory cards) and Slippi's EXI device, login (`slippi_login.*`), replay parsing (`slippi_history.*`), Discord Rich Presence over the local IPC socket (`discord_rpc.*`). |
| `port/runtime/ppc/` | Guest CPU context, memory, interpreter fallback. |
| `port/recomp/` | The recompiler configuration; `hle_list.txt` names the guest functions replaced by host code. |
| `port/slippi_sys/` | Vendored Slippi game files (Sys folder). |
| `tools/` | Build helpers: `bootstrap_port.py` (generate/configure/build), `extract_dol.py`, `package_macos_app.sh`, `make_icons.py`. |
| `docs/` | Longer notes and README images. |

## Build rules that bite

- **Provenance gate.** Any edit under `port/` invalidates the generated manifest. Before building run
  `python3 tools/bootstrap_port.py --decomp-root <decomp> --dol <main.dol> --build-dir <build-dir> --gct-base 0x8065CC80 --macos-arch arm64 --stage generate`
  (or `./setup.sh`, which does it). Otherwise `port_verify_generated` fails with a one-line error.
- **Toolchains.** macOS builds run with the Command Line Tools (`unset DEVELOPER_DIR`). iOS and
  visionOS builds need `export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer`. Mixing
  them produces precompiled-header mismatches.
- **Simulator runtimes.** Keep `xcode-select` on the Command Line Tools and prefix Xcode-only commands instead, e.g.
  `DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer xcodebuild -downloadPlatform visionOS`; without the prefix it fails
  with "requires Xcode". With the visionOS 26 runtime, create the simulator from the `Apple-Vision-Pro-4K` device type.
- **HLE hooks are matched by symbol name.** A name missing from `port/recomp/hle_list.txt` is silently
  recompiled instead of hooked (this is how a `longjmp` bug once corrupted guest memory).
- **Never build while measuring performance**, and shut down Simulators first: both distort the
  per-frame numbers.

## Running and diagnosing

- `build/<dir>/port/melee_port_mac --help` lists every flag. Useful: `--iso`, `--fullscreen`,
  `--no-vsync`, `--window WxH`, `--offline`, `--profile-dir`, `--cache-dir`, `--log-file`.
- Every 60 frames the log has `[frame N] ... sim: X ms/frame (worst Y) | <cost slots>`; frames over
  16.7 ms log `sim frame N took ...` with the cost slots that explain it.
- `MELEE_METAL_LATENCY=1` logs XFB-copy-to-panel latency from Metal's presented timestamps and GPU
  time. `MELEE_METAL_COMPILE_LOG=1` logs every shader compile. `MELEE_RENDER_THREAD=0`,
  `MELEE_REALTIME=0`, `MELEE_METAL_SYNC_COMPILE=1` turn the render thread, real-time scheduling and
  background shader compilation off for A/B tests.
- `MELEE_DASHBOARD_SAMPLE=1` fills the dashboard with sample data; `MELEE_LAUNCHER_SCROLL=<pt>` starts
  it scrolled; `MELEE_MARK=<glyph.png>` shows the Dashdance mark when running the bare binary.
- `MELEE_OPEN_EDITOR=keyboard` (or `pad:<guid>:<name>`) opens the dashboard's controller editor after launch (iOS takes `pad:` only); `MELEE_CONTROLLER_ART=<dir>` loads the controller artwork from another folder; `MELEE_OPEN_PAIRING=1` opens Connect a Controller; `MELEE_TEXT_AUDIT=1` logs every label, button or picker whose text is cut off, needs more lines than its box, leaves the window or runs into the rounded shape around it (`text-audit [...] done: N problems`; run it on each device and orientation after UI changes, and look at the screenshots too); `MELEE_ORIENTATION=portrait|landscape` rotates the iPhone/iPad dashboard (the game follows the device: rotate the Simulator with Command-arrow), `MELEE_WINDOW_SIZE=WxH` sizes the Vision Pro window and `MELEE_LAUNCHER_SIZE=WxH` the Mac dashboard window;
  `MELEE_MENU_PAGE=controls` or `remap` opens that page of the in-game menu together with `MELEE_MENU_OPEN` (screenshots).
- `MELEE_MENU_OPEN=<retrace>` opens the in-game menu at that frame and `MELEE_HUD=1` turns the HUD on (screenshots);
  `MELEE_DUMP_GLYPHS=<file.pgm>` writes the text atlas; `MELEE_DISCORD_APP_ID=<id>` points Discord presence at another application (the built-in id,
  `slippi::discord::kApplicationId`, is Dashdance's own from slippi-rust-extensions PR 36, with its character, stage and rank artwork).
- `MELEE_PAD_FILE=<file>` drives the game from a text file (one line per pad: `p=1 A sx=127`), which
  is how the scripted match tests work without a window in focus.
- On the Simulator, prefix environment variables with `SIMCTL_CHILD_` and pass them to
  `xcrun simctl launch`.

## Design rules

- Apple platform conventions first: Liquid Glass on macOS/iOS 26 (`NSGlassEffectView`, `UIGlassEffect`)
  with material fallbacks, glass buttons, SF Symbols, native controls with visible labels and values,
  haptics only for user actions, a real menu bar on the Mac. See README "Design".
- Melee's own grammar stays: angled yellow section headers, italic display type, dark blue grid.
- Colours and glass tints come from the small theme helpers at the top of each launcher file; do not
  scatter literal colours.
- Keep the simulation thread free of anything that can block (display, GPU, disk, shader compiles).
- Licensing: anything ported, copied or adapted from another project gets a row in `THIRD_PARTY_NOTICES.md` with its
  licence and the evidence (a quoted licence statement or SPDX header, not a GitHub label: every GPLv2 `LICENSE` file
  contains "any later version" as template text). MIT and zlib notices not already in the tree go in that file. Never
  commit code or headers under proprietary terms.
- Online input delay is a player setting (`online_delay`, 1..9, default 2), exposed on both dashboards and the in-game menu; never hardcode it. On iPhone/iPad `power_play_begin` holds the display at full refresh and requests a 5 ms audio buffer (`apple_power.mm`); `latency_warning()` feeds the HUD. `competitive_readiness()` (apple_power.mm) feeds the Ready to compete card on both dashboards and the `readiness:` log lines; keep its wording player-facing. Rollback save states are timed as the `savestate` cost slot. The phase lock stays off: measured as noise.
- iPhone Duo (see docs/TECHNICAL.md): one game rectangle (`host::window_game_rect`) for renderer, touch layout and letterbox artwork; upright, the game never passes the middle of the display (the fold); controls and overlay text are sized in points; dashboard columns switch on size class and centre on the display; no global main screen; `UIRequiresFullScreen` stays off. The README is written for players; put technical material in docs/TECHNICAL.md.
- Every screen adapts: two columns of cards when the window is wide (Mac from 1100 pt, iPad and Vision Pro from 960 pt), a side-by-side controller editor in landscape, and touch controls, HUD and menu inside the safe area. Check portrait and landscape on iPhone and iPad and a narrow and wide window on Mac and Vision Pro with the aids above.

## Verification checklist for a change

1. Build the target you touched (macOS at least; iOS if you touched shared UI or Metal).
2. Run a scripted match and confirm `late` frames stay at zero (see the scripts described in
   `docs/PERFORMANCE.md`).
3. For UI: capture the dashboard (`MELEE_DASHBOARD_SAMPLE=1`) and look at it.
4. Commit with a message that says what changed for the player and why.
