# iPhone Duo development workflow

Dashdance uses UIKit in Objective-C++ for its dashboard, SDL for its window and input,
and Metal for the game. The project-local skills in `.agents/skills/iphone-duo-*`
are also available to Claude Code through `.claude/skills` symlinks.

## Choose the skills for the change

Start with [readiness](../.agents/skills/iphone-duo-readiness/SKILL.md) for any Duo task.

| Work | Read next | Dashdance entry points |
|---|---|---|
| Game viewport, resizing, HUD, touch controls | [games](../.agents/skills/iphone-duo-games/SKILL.md), [adaptive layout](../.agents/skills/iphone-duo-adaptive-layout/SKILL.md) | `window_sdl.cpp`, `window_fold_apple.mm`, `overlay.*`, `game_menu.*`, `gx_metal.mm` |
| Dashboard, controller editor, sheets, visible UI review | [design review](../.agents/skills/iphone-duo-design-review/SKILL.md), [adaptive layout](../.agents/skills/iphone-duo-adaptive-layout/SKILL.md) | `ios_launcher.mm` |
| Native navigation, toolbars or tabs | [vertical bars](../.agents/skills/iphone-duo-vertical-bars/SKILL.md) | UIKit containers in `ios_launcher.mm` |
| Pane composition | [dual-pane patterns](../.agents/skills/iphone-duo-dual-pane-patterns/SKILL.md) | Dashboard columns and controller editor |
| Hinge events, scene lifecycle or display transitions | [hinge and scenes](../.agents/skills/iphone-duo-hinge-and-scenes/SKILL.md) | `window_fold_apple.mm`, UIKit/SDL lifecycle |
| A future camera feature | [camera](../.agents/skills/iphone-duo-camera/SKILL.md) | Only when capture is in scope |
| A future Flutter or React Native client | [Flutter](../.agents/skills/iphone-duo-flutter/SKILL.md) or [React Native](../.agents/skills/iphone-duo-react-native/SKILL.md) | These are not Dashdance's current stack |

The [API index](../.agents/reference/api-index.md), [device facts](../.agents/reference/device-facts.md)
and [sources](../.agents/reference/sources.md) accompany the skills. Confirm concrete API usage against
the installed UIKit headers and Apple's documentation; Swift examples need translation to Objective-C++.
Check SDK guards as well as runtime availability when supporting older toolchains.

## Apply the guidance to this game

- Keep Melee's selected 4:3 or 16:9 presentation and gameplay intact. Use the existing themed padding
  where necessary; the generic suggestion to change camera aspect ratio does not authorize changing Melee.
- Keep renderer, hit testing, touch layout and artwork aligned through `host::window_game_rect`.
  Read the current view's bounds, scale and each safe-area edge independently after resizing.
- Use active division and occlusion regions for custom controls. Verify the queried coordinate space
  and avoid adding region margins twice. Inspect the existing `window_fold_apple.mm` bridge before adding one.
- Keep HUD and touch targets sized in points, clear of the fold and system controls. Preserve navigation,
  settings and the running game across display and pose changes.
- Keep UIKit work on the main thread and blocking work off the simulation thread. Preserve
  `UIRequiresFullScreen = false` and behavior on ordinary iPhones, iPads and Vision Pro.

## Verification and evidence

1. Record Xcode version, simulator runtime, SDK and deployment target. Build with `./setup.sh <disc> --ios`
   (regenerates the manifest). Build other affected targets per `AGENTS.md`.
2. In Xcode Device Hub, check closed/outer, open/inner, partially folded book and tabletop poses,
   portrait and landscape, plus Split View on both sides. Change pose while the app is running.
3. Inspect dashboard, controller editor, gameplay, touch controls, HUD and menu for the affected change.
   Run the dashboard with `SIMCTL_CHILD_MELEE_TEXT_AUDIT=1` and, for representative content,
   `SIMCTL_CHILD_MELEE_DASHBOARD_SAMPLE=1`. Inspect screenshots as well as audit results.
4. Capture the actual active display. Use `xcrun simctl io <device> enumerate`, then
   `xcrun simctl io <device> screenshot --display=<display-UUID> <output.png>`.
   The default screenshot can capture the inactive outer display while the game is on the inner one.
   Prefix Xcode-only commands with `DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer`.
5. Record pass/fail/not tested for every relevant case, with screenshots and logs. A static audit or
   successful compile is not evidence of pose handling. Follow `docs/PERFORMANCE.md` for scripted-match
   checks; do not measure performance during a build.

## Observed baseline — 2026-09-27

- Built with Xcode 27.1 and the iOS 27.1 simulator SDK using `setup.sh --ios`; deployment target is iOS 17.0.
- Installed on the iPhone Duo simulator running iOS 27.1. The dashboard recognized the copied Melee disc.
- An offline launch rendered Melee on the inner display with the touch overlay visible.
- The dashboard screenshot showed truncated title and section text; this needs a layout review.
- Fold transitions, state continuity, both Split View placements, the complete orientation matrix,
  text-audit results and scripted-match performance have **not** been verified in this session.
- Device Hub UI automation timed out; simulator screenshots and launches used `simctl`.

This is a launch baseline, not a completed Duo readiness audit.

## Dashboard redesign — 2026-09-27

Read all ten installed skills before the redesign. Readiness, design review, adaptive layout,
dual-pane patterns and games informed the changes; the vertical-bars and hinge/scenes review
confirmed that the current custom dashboard and SDL scene owner should remain. Camera capture,
Flutter and React Native instructions do not apply to this native UIKit/Metal client.

The dashboard now puts Play and disc/controller setup first, with a compact identity header.
Regular-width windows fit two readable columns inside the asymmetric safe area; active vertical
fold regions set their gap and widths. Settings use full-width controls and a native resolution
menu. Section headers and recent-game titles wrap; cards retain a minimum corner radius.
Entrance and Play animations respect Reduce Motion. The launcher's display information comes
from its own window scene.

The Xcode App Resizability audit confirmed a launch screen, all four iPad orientations and
`UIRequiresFullScreen=false`. SDL owns the existing scene lifecycle. The separate `apple_power.mm`
global-screen fallback remains deferred: power setup currently precedes SDL window creation, so
removing that fallback requires lifecycle sequencing and display-change handling.

Validation used Xcode 27.1 / iOS 27.1 SDK, deployment target iOS 17. Both `setup.sh --ios`
and `tools/mac/rebuild.sh` passed. Screenshots/logs are in local `reports/duo-redesign/`
(ignored because runtime evidence can contain game material).

- Duo outer dashboard: portrait 466×678 pt and landscape 678×466 pt; zero text-audit problems
  with representative sample data. Both screenshots inspected.
- iPhone 18 Pro / iOS 27.0: portrait 402×874 pt and landscape 874×402 pt; zero text-audit
  problems. Both screenshots inspected. These captures precede only the final primary-button
  surface change from glass to filled for contrast.
- iPad Pro 11-inch M5 / iOS 27.0: the two-column portrait dashboard at 834×1210 pt reports
  zero text-audit problems and was visually inspected. The landscape request retained portrait
  geometry, so iPad landscape remains unverified.
- The scripted offline acceptance run completed 4200 retraces but exited 5: it reached character
  selection and never observed required match scene `0x0202`. A passing match/performance result
  is therefore **not established** (`match-final.log`). No builds or simulators ran during this check.
- Live inner/outer transitions, book/tabletop/tent poses, both Split View placements, controller
  editor interactions, VoiceOver/Dynamic Type and Vision Pro remain unverified. Device Hub UI
  automation timed out; successful static layout checks do not establish fold-pose readiness.

## Upstream provenance and updates

All ten skill files and three shared references are unmodified copies from
[mirzaaghazadeh/iphone-duo-skills](https://github.com/mirzaaghazadeh/iphone-duo-skills)
at commit `b8b9d15b4dc987d0c35940265e07f4128ba79361` (installed 2026-09-27).
The MIT license is retained in [the reference directory](../.agents/reference/iphone-duo-skills.LICENSE)
and `THIRD_PARTY_NOTICES.md`. `.claude/reference` preserves the skills' relative reference links.

When updating, review the upstream diff, replace the skills and shared references together, retain
the license, validate sibling/reference links through both agent directories, and update the pinned
revision here. Keep Dashdance-specific rules in this workflow and `CLAUDE.md`, so upstream files stay comparable.
