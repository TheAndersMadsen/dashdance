# AR Mode: mixed-reality Melee on Apple Vision Pro

[xelee](https://github.com/rhythmerc/xelee) plays Super Smash Bros. Melee inside your room on a
Meta Quest 3: the game screen floats in space, you place it (and, in xelee's full form, the whole
3D arena) with your hands, menus follow a laser pointer, and sound comes from where the fighters
are. This document is the study that brought that experience to Dashdance on Apple Vision Pro as
**AR Mode** — an alternative mode in which the game and its Slippi online stack are unchanged and
an immersive mixed-reality presentation is added alongside the flat window.

Part 1 documents how xelee works (from its code and docs). Part 2 documents the Vision Pro
platform research. Part 3 is the design and as-built state of Dashdance's implementation.

## Contents

1. [How xelee works](#1-how-xelee-works)
2. [Vision Pro platform research](#2-vision-pro-platform-research)
3. [Dashdance's AR Mode](#3-dashdances-ar-mode)
4. [Phase 2 plan: the 3D arena](#4-phase-2-plan-the-3d-arena)
5. [References](#5-references)

---

## 1. How xelee works

Xelee is built on the same foundations as Dashdance: the doldecomp Melee decompilation translated
to native code (`src/melee`, `src/sysdolphin`), the [aurora](https://github.com/encounter/aurora)
GameCube SDK/GX layer, and a Slippi online stack. On top it adds a mixed-reality layer that is
cleanly split in two:

- **Game-side logic, platform-agnostic** (~6,600 lines in `src/pc/xr_scene.c`, `xr_place.c`,
  `xr_pointer.c`, `spatial_audio.c`, `stage_mix.c`, `stage_ambience.c`). It knows nothing about
  Quest or OpenXR; it emits draw categories, transforms and events through ~40 `aurora_xr_*`
  entry points.
- **The XR renderer** (in xelee's aurora fork, `lib/xr/xr.cpp`, an OpenXR + Dawn/WebGPU
  implementation — Quest-specific and not portable).

### 1.1 The two presentation paths

**The virtual screen** (menus and everything non-fight): the game renders exactly as the flat
build; the finished frame is presented as a quad floating in the room — 0.7 m wide, 0.85 m in
front of the starting head position, always turning to face the head. This path needs nothing
from the game.

**The 3D arena** (fights): xelee's signature. A world vertex reaches an eye as

```
clip_eye = P_eye · V_eye · A · X · V_game⁻¹ · M_pos · v
```

- `M_pos` — the GX position matrix of the draw (model → game camera space). Lighting, skinning,
  texgen and projected shadows all happen before projection, so they replay unchanged.
- `V_game⁻¹` — undoes the fight camera; captured per frame from the fight camera object
  (`cm/camera.c`, the `HSD_CObj` view matrix).
- `A = T(pos) · R_y(yaw) · S(scale)` — the arena placement, applied about a per-stage center
  point in game units (scale = a base meters-per-unit × a per-stage factor; yaw only, never
  pitch/roll).
- `X` — per-moving-piece extra transform for the few stages whose pieces should move in the room.
- `P_eye · V_eye` — the headset's per-eye view/projection in metres.

Draws are tagged by which game camera drew them (world / HUD / mono) via hooks in the game code
(`cm/camera.c`, `if/ifall.c`), and the world draws are re-rendered per eye from the same command
stream ("replay") while the flat frame keeps feeding 2D uses (Pokémon Stadium's screen, EFB
copies). Alpha is depth coverage: pixels the world geometry covers hide the room; translucent
draws add light on top. The HUD is replayed onto its own quad hovering above the stage.

### 1.2 Placement (the "stage spot")

Placement is a pose `T · R_y · S` with a resolution order: this stage's session grab → the
adopted base × stage factor → a default in front of where the head is looking. Gestures (one
hand: pinch-drag; two hands: spread = scale, twist = yaw) are resolved against a plane and an
anchor stores the adopted spot so it survives relaunches. Before each fight the game holds
(no simulation ticks, pads flushed) until the player confirms with A; a dedicated dummy scene
("Place Stage") runs the same flow against a practice stage.

### 1.3 Pointer menus

A ray from the controller tip (or estimated from the hand) intersects the screen quad → 0..1
coords → logical pixels (640×480-ish) → merged into port 1's virtual pad. Menus cooperate: each
menu projects its option boxes through the menu camera (`HSD_CObj` clip matrix) so the pointer
"snap follows" selectable items; a click arms the A press. Grips/triggers don't press Z/L/R
while the laser is on the screen.

### 1.4 Room sensing

- Floor/grounding: a worker casts a grid of downward rays into the room mesh; a contact shadow
  quad (alpha ∝ coverage · R²/(R²+h²), an ambient-occlusion law) is composited between
  passthrough and content so the screen/arena appears to sit on the real floor.
- Anchors persist the adopted spot; a boundary/passthrough layer switch separates MR from VR.
- Room lighting (experimental): a passthrough camera feed feeds a radiance cube → key/fill
  lights + SH ambient applied to the eye-pass shaders, plus a per-stage "grade" tint on
  passthrough. (Built, then removed as shipped feature.)

### 1.5 Spatial audio

Steam Audio binaural pipeline: emitters are tracked through the game object system
(`gobj` procs), voices are tagged at the AX driver level, positions are published once per
drawn frame from the fight camera, and a 5 ms render mixes each voice through distance gain and
an HRTF into a positional bus. The crowd is a ring of six decorrelated seats; music stays
mostly stereo with a virtual speaker pair; synthesized stage ambience (waterfalls, wind,
Doppler on Big Blue) feeds the same buses. Fully output-only — netplay and rollback untouched.

### 1.6 Frame pacing

Display at 120 Hz, game tick every second display frame (lock-step via the VI retrace), a
fixed-latency release slot per frame, Application SpaceWarp (motion-vector reprojection to an
apparent 120 Hz), and deadline-driven dynamic resolution. The relevant takeaway for Vision Pro:
**eyes re-rendered every display frame from the newest game frame** is the portable pattern.

---

## 2. Vision Pro platform research

What follows is the condensed API map (verified against Apple's documentation and the
CompositorServices sample code, visionOS 26 SDK, October 2026).

### 2.1 The compositing model

Vision Pro composites every app's windows and spaces itself. To draw at display rate with your
own Metal engine you use **Compositor Services**: an `ImmersiveSpace` whose content is a
`CompositorLayer` hands you a `LayerRenderer`, and you render directly into its per-frame
textures — bypassing the render server. The system still owns passthrough and everything else.

- In the **mixed** immersion style, your layer's alpha blends with the real room: clear alpha 0
  shows passthrough. This is exactly the mixed-reality canvas xelee gets from OpenXR +
  passthrough layers.
- `upperLimbVisibility(_:)` chooses whether hands appear in front of your content.

### 2.2 The frame protocol (LayerRenderer)

```
while layerRenderer.state != .invalidated {
  if paused { layerRenderer.waitUntilRunning(); continue }
  let frame = layerRenderer.queryNextFrame()          // may return nil; one at a time
  frame.startUpdate()                                  // update phase: simulate, animate
  let timing = frame.predictTiming()                   // predicted deadlines
  ... update scene (no head-dependent work) ...
  frame.endUpdate()
  LayerRenderer.Clock().sleep(until: timing.optimalInputTime)   // pace
  frame.startSubmission()
  let drawable = frame.queryDrawable()                 // 1 (or more, e.g. during capture)
  // per drawable:
  let t = Clock.Instant.epoch.duration(to: drawable.frameTiming.presentationTime).timeInterval
  drawable.deviceAnchor = worldTracking.queryDeviceAnchor(atTimestamp: t)   // predicted head pose
  ... encode Metal ...
  drawable.encodePresent(commandBuffer:)               // presentation event
  commandBuffer.commit()
  frame.endSubmission()
}
```

Rendering details:

- Textures: `drawable.colorTextures[0]` / `depthTextures[0]`; with the default (layered) layout
  this is a **texture array with one slice per eye**; `drawable.views` holds one `View` per eye
  with `transform` (device→eye), a texture-map viewport, and a rasterization rate map.
- A single render pass draws both eyes (`renderTargetArrayLength = drawable.views.count`) with
  **vertex amplification** (`setVertexAmplificationCount`); the vertex shader picks per-eye data
  via `[[view_index]]`.
- Per-eye matrices: `viewMatrix = (deviceAnchor.originFromAnchorTransform * view.transform).inverse`,
  `projection = drawable.computeProjection(convention: .rightUpBack, viewIndex:)`.
- The default configuration is HDR `rgba16Float`; foveation and layout are adjustable through a
  `CompositorLayerConfiguration`. For 8-bit sRGB content, `.rgba8Unorm_sRGB` is a valid choice —
  sRGB bytes pass through with correct blending.
- The layer runs on the app's own thread (`Task { @RendererActor ... }`); the system pauses/
  invalidates it as the space opens and closes.

### 2.3 Input: system spatial events

A fully custom Metal layer gets no UIKit/RealityKit gestures, but the system's **look + pinch**
still works: `layerRenderer.onSpatialEvent` delivers a `SpatialEventCollection` on the main
thread — each event has an `id`, a `phase` (`.active`/`.ended`/`.cancelled`), and either an
`inputDevicePose` (pose of the pointing hand in world space) or a `selectionRay` (origin +
direction). Drag and two-handed gestures are just multiple concurrently-active events. No
ARKit hand-skeleton processing is required for pinch/drag/twist/scale — the Simulator's
"Select to interact" mode drives these same events with the Mac pointer (click = tap, drag =
move, Option = second hand), which is how the mode is tested without a headset.

### 2.4 World tracking and persistence

`ARKitSession` runs providers:

- `WorldTrackingProvider` — predicted device pose (`queryDeviceAnchor(atTimestamp:)`, the one
  call the render loop cannot skip) and **world anchors**. `WorldAnchor` UUIDs *and transforms
  persist across multiple runs of your app* (Apple's documented behaviour); `allAnchors` returns
  the known ones on launch. This is the Vision Pro equivalent of xelee's persistent stage spot.
- Authorizations: `ARKitSession.requestAuthorization(for: WorldTrackingProvider.requiredAuthorizations)`
  (world sensing) before anchoring; `HandTrackingProvider` would additionally need hand-tracking
  authorization (not needed for phase 1 — spatial events cover the gestures).
- `PlaneDetectionProvider` / `SceneReconstructionProvider` (room mesh) exist for grounding
  (phase 2: contact shadows).

Info.plist keys for device builds: `NSWorldSensingUsageDescription` (and
`NSHandsTrackingUsageDescription` when hand tracking is adopted).

### 2.5 Simulator support

The visionOS Simulator opens immersive spaces (rendered into a simulator window), runs
`ARKitSession` world tracking (the device pose follows the simulated camera), persists world
anchors in the simulator container, and delivers spatial events from the Mac pointer. Hand
skeleton tracking itself is not simulated — another reason phase 1 uses spatial events.

### 2.6 Mapping xelee → Vision Pro

| xelee (Quest / OpenXR) | Vision Pro equivalent |
|---|---|
| OpenXR session + swapchains | `ImmersiveSpace` + `CompositorLayer` + `LayerRenderer` |
| Passthrough composition layer | System passthrough in the **mixed** immersion style (alpha blends) |
| Composition quad layers (screen, HUD, lasers) | Rendered in-eye into the layer (Vision Pro has no app-owned compositor quads) |
| Controller/hand pose → laser ray | `onSpatialEvent` selection rays (system look+pinch) |
| `XR_META_environment_raycast` floor + AO shadow | (phase 2) `PlaneDetectionProvider` / `SceneReconstructionProvider` |
| `XR_FB_spatial_entity` anchor persistence | `WorldAnchor` (persists across launches by contract) |
| SpaceWarp / ASW to 120 Hz | Not available; eyes re-render every display frame (90/100 Hz) from the newest 60 Hz game frame |
| Deadline-driven dynamic resolution | `LayerRenderer` paces; internal EFB scale caps already exist (`metal_scale_caps`) |
| Separate 2D panel activity | The existing dashboard window stays open beside the immersive space (mixed style) |

### 2.7 Porting notes: what changes between Quest and Vision Pro

From Apple's guidance for Metal games on visionOS (WWDC24 10093, "Render Metal with passthrough",
the HIG for immersive experiences) and the compositor documentation of both platforms:

- **The compositor owns reprojection on both platforms, differently.** Quest's TimeWarp/ASW
  reprojects the app's eye buffers (and can synthesize frames from motion vectors); Vision Pro's
  render server reprojects your `LayerRenderer` frames against the predicted `deviceAnchor` and
  dims content near real objects in mixed immersion. The app-side consequence is the same: match
  the predicted pose, never lag it. There is no ASW on Vision Pro — the pattern is eyes re-rendered
  every display frame from the newest game state, with the game simulation decoupled (which is what
  the mirror + shared-event design does).
- **No app-owned compositor layers.** Quest apps compose quad layers (screen, HUD, lasers) on top
  of passthrough; on Vision Pro everything is rendered in-eye into the one immersive layer. AR
  Mode therefore composites the in-game menu and the touch controls itself, in its own shader.
- **Windows and spaces are system-managed.** Opening an immersive space hides *other apps''*
  windows but keeps your own visible beside it (mixed style) — Dashdance's dashboard and flat
  window legitimately coexist with the quad. Space open/dismiss is asynchronous; only one
  immersive space per app; `UIApplicationSupportsMultipleScenes` must be true (this bit us —
  see §3.3).
- **Input differs more than rendering.** Quest ships controllers with laser rays; Vision Pro has
  none — the system ray is your eyes, and gestures are look+pinch. Ports either drive a virtual
  cursor from the pinch (xelee's laser) or map the game's own touch controls onto the screen.
  AR Mode does the latter: the quad carries Dashdance's touch layout and pinches become fingers
  (see §3.1), which keeps the game playable with no controller at all. Gamepads (MFi) work
  throughout and take over from the on-screen controls, as on iOS.
- **Anchors: Meta's persistence API vs ARKit's built-in contract.** Quest needs
  `XR_FB_spatial_entity` + a storage layer; visionOS `WorldAnchor` UUIDs and transforms persist
  across launches by contract, and `allAnchors` returns them on launch.
- **Passthrough is a system service.** No layer styling/color-grading hooks like
  `XR_FB_passthrough` styling; what you get is the mixed style, system dimming near objects, and
  (visionOS 26) tint effects on the space — AR Mode keeps the passthrough untouched.
- **Comfort rules are documented, not suggestions.** Content within the field of view, indirect
  gestures, no required head movement, and a stationary frame of reference — a placed screen that
  stays put (and is remembered) is exactly the recommended shape.

---

## 3. Dashdance's AR Mode

![AR Mode on Apple Vision Pro: Melee's Online Play menu floating in the room, with the game's
touch controls drawn around the picture](screenshots/AR_MODE_VISIONOS.jpg)

### 3.1 What it is

An alternative presentation mode on visionOS: after Play, the app opens a mixed-style immersive
space and the game picture appears on a floating screen in the room. The flat window stays open
beside it (dashboard, touch controls, replays — everything keeps working there). The screen can
be **placed with the system pinch**: pinch-drag moves it, one pinched hand twisted turns it,
two pinched hands spread/pinch scale it; releasing adopts the spot into a persisted world
anchor, so the screen is where you left it next launch. **A pinch that lands on the picture
plays**: the quad carries Dashdance's touch layout (buttons, sticks, the MENU chip) and the
pinch becomes a finger there — Melee is playable with no controller at all, and a paired
gamepad takes over exactly as on the flat window. The in-game menu renders on the quad and its
rows are tappable. Input is otherwise whatever already works: a paired gamepad, or the flat
window's touch overlay.

In xelee's terms this is the **virtual screen path** (§1.1a) with placement and persistence —
the foundation the 3D arena (§4) builds on.

### 3.2 How a frame reaches the quad

Dashdance's renderer already renders every game frame into an intermediate EFB texture and
blits the presented region to the window (`gx_metal.mm`, `present_efb`). AR Mode adds a
**mirror**: when enabled, `present_efb` additionally renders

1. the game picture (the XFB source region of the EFB, exactly what the flat blit samples) into
   ring texture `game[i]`, and
2. the in-game menu/HUD overlay (`menu_overlay` at the ring's size — the game rect fills the
   ring, so menu shapes land correctly) into ring texture `overlay[i]` (premultiplied alpha),

for a 3-slot ring (`i = frame_counter % 3`, matching the existing FRAME_SLOTS command buffers).
An `MTLSharedEvent` is notified with the frame sequence once the command buffer completes;
the quad's fragment shader samples `game` under `overlay` (premultiplied). The Swift side gets
the latest sequence/slot/textures through a C bridge and encodes `encodeWaitForEvent` on its own
command buffer — no CPU waits anywhere, simulation and flat presentation unchanged.

Sync: the game signals `done` with the frame sequence once the mirror passes are encoded into
its command buffer, and the quad signals `read` with the sequence it consumed. The game skips a
mirror frame only while the quad is more than two published frames behind — the check compares
against the published sequence, not the frame counter, so a missing or closed space can never
deadlock the game, and the mirror resumes by itself the moment the quad catches up. When the
system occludes the flat window (it happens inside an immersive space) the render thread drains
the flat present but the mirror keeps rendering: on Vision Pro the quad is the primary display.

### 3.3 Where things live

| File | Role |
|---|---|
| `port/runtime/gx/ar_bridge.h` | The C ABI shared by the game, the Metal renderer and Swift (`DashdanceArFrame`, `dashdance_ar_touch`) |
| `port/runtime/gx/gx_metal.{h,mm}` | `MetalOptions::ar_mirror`; the ring, the shared events, `metal_ar_snapshot()`, the touch-event entry |
| `port/runtime/host/game_menu.cpp` | Per-surface menu geometry (`menu_overlay`/`menu_touch(..., surface)`: 0 = flat window, 1 = the quad) |
| `port/runtime/host/window_sdl.cpp` | `game_overlay_ar()` and the quad's touch surface (`ar_touch_resize/event/overlay/read`) — a second instance of the touch layout, sized to the ring, merged into port 1 |
| `port/app/main_mac.cpp` | `--ar` / `MELEE_AR` / the dashboard card / the remembered `ar=` setting; on visionOS requests the immersive space after Play and waits for it before starting the game; `main()` becomes `dashdance_main()` with the entry provided by Swift |
| `port/app/visionos/bridge.h/.c` | The space-open and dashboard hooks (registered by Swift, called by the game flow) |
| `port/app/visionos/DashdanceVisionApp.swift` | The visionOS-only `App`: WindowGroup (dashboard host) + `ImmersiveSpace` in mixed style; `@_cdecl("main")` runs it (a static library gets no `@main` entry) |
| `port/app/visionos/ARScreenRenderer.swift` | The `LayerRenderer` loop, quad shaders, play-vs-place gesture classification, world-anchor persistence |
| `port/CMakeLists.txt` | visionOS: compiles the Swift sources with `swiftc -emit-library -static`, links SwiftUI/CompositorServices/ARKit/`_CompositorServices_SwiftUI` |
| `port/app/ios/Info.plist.in` | `UIApplicationSupportsMultipleScenes` (required to open a space beside the window), `NSWorldSensingUsageDescription`, `NSHandsTrackingUsageDescription` |

Three SDK notes for whoever touches the Swift next: in the current Xcode the SwiftUI surface of
CompositorServices lives in the `_CompositorServices_SwiftUI` module (`CompositorLayer` there is
an `ImmersiveSpaceContent`, not a `View`, and `LayerRenderer.onSpatialEvent` hangs off it), the
Metal importer renames several APIs (`rgba8Unorm_srgb`, `makeTextureView(pixelFormat:)`,
`encodeWaitForEvent(_:value:)`, `drawPrimitives(type:...)`), and the MSL vertex-amplification
attribute is `[[amplification_id]]`. The deployment target is visionOS 2.0.

Also fixed on the way: three iPhone Duo code paths (`UIViewReservedRegion`,
`UIHingeInteraction`) that the visionOS SDK does not declare are now compiled out there
(`ios_launcher.mm`, `window_fold_apple.mm`) — the visionOS build had been broken since the Duo
work landed.

### 3.4 Running it

```bash
./setup.sh /path/to/melee.iso --visionos          # builds and installs into the booted Vision Pro Simulator
SIMCTL_CHILD_MELEE_AR=1 xcrun simctl launch booted app.dashdance.ios
```

In the app, the dashboard's **Display & Performance** card has an **AR Mode (play in the room)**
switch — flip it on and press Play; the choice is remembered in `launcher.ini` (`ar=`). On the
Simulator, use the window's "Select to interact" mode: the pointer is the gaze ray, click =
pinch (drag on the picture plays its controls; drag off it moves the screen; Option-click is the
second hand for twist/scale).

For a **real Vision Pro**:

```bash
./setup.sh /path/to/melee.iso --visionos-device --team auto   # signed, installed over USB, launched
```

Developer Mode must be on; the disc goes into the app's folder in Files; flip AR Mode on in the
dashboard and press Play. `MELEE_AR=1` / `--ar` remain as env/flag overrides for scripted runs.

### 3.5 Verification status

Verified on the **visionOS 27 Simulator** (`xrsimulator`, Apple-Vision-Pro-4K device type),
end to end: dashboard renders (controller connects over Bluetooth, disc card resolves the
image, the AR Mode switch is on the Display & Performance card), Play → the mixed-style
immersive space opens, the game boots and renders on the floating screen in the room with the
in-game overlay composited around it, the mirror handshake stays live for the whole session
(the quad consumes the newest published frame within two publishes), sim time stays at
~0.6–1.3 ms/frame with no late frames, and the flat window keeps presenting beside the space.
With AR Mode off (switch, flag and env all unset), the app behaves exactly as before.

Also verified on macOS and iOS (the same shared renderer/host files): a 600-frame scripted match
with the mirror on runs at 1.6 ms/frame, exit 0, and a captured presented frame is byte-identical
with the mirror on and off — the flat path is untouched.

**Not yet verified on a Vision Pro device** — use `--visionos-device --team auto` for that:
hand-driven placement and play feel, real head-tracked latency, the world-sensing authorization
prompt, and gesture behavior through the real `onSpatialEvent` pipeline. On the Simulator,
gestures are exercisable through the "Select to interact" mode; world-anchor persistence was
exercised only through the default-placement path.

---

## 4. Phase 2 plan: the 3D arena

The full xelee experience — the fight floating in the room — is designed and scoped but not
built. The missing piece is draw categorization and the camera capture (§1.1b):

1. **Capture the fight camera.** xelee hooks `cm/camera.c`. Dashdance doesn't modify game code,
   but it doesn't need to: the fight camera's view matrix lives in guest RAM at an address the
   pinned decomp's map file names. A per-frame guest-memory read (the same mechanism as the
   existing watchpoints) yields `V_game` without touching the recompilation.
2. **Tag draws.** `gx::Frame` already stamps `scene_major/scene_minor` per frame, and every
   `DrawCall` carries its full XF matrix state. Two viable taggers, in order of preference:
   - compare each draw's view matrix (XF regs) against the captured fight camera — matches are
     world draws, everything else (HUD, menus, transitions) renders flat; heuristic, zero game
     patches;
   - or hook the camera-category calls through `port/recomp/hle_list.txt` (symbol replacement),
     the xelee-faithful route.
3. **Re-project.** The Metal backend renders world draws with `view_proj = P_eye·V_eye·A·V_game⁻¹`
   (one per eye, layered/amplified) instead of the game's projection — the same matrix override
   plumbing the sub-frame presenter already has (`submit_frame(frame, overrides)`), extended
   from per-draw position matrices to the view/projection.
4. **Stage placement.** Adopt xelee's `T · R_y · S` about a per-stage center in game units, with
   the placement-hold state machine ported from `xr_place.c` (game-side logic, portable).
5. **Grounding.** `PlaneDetectionProvider` gives the floor height for a contact-shadow quad
   under the arena (xelee's AO law), replacing `XR_META_environment_raycast`.
6. **Spatial audio.** Dashdance's audio path is SDL; the xelee pipeline (emitter tracking →
   voice tagging → per-frame positions → HRTF) is portable, and on visionOS the listener comes
   from `queryDeviceAnchor`. Scope it separately from the rendering work.

---

## 5. References

- xelee: <https://github.com/rhythmerc/xelee> — `docs/quest-xr.md` (the master XR document),
  `docs/xr-stage-placement-ux.md` §A.1 (placement transforms), `src/pc/xr_scene.c` (staging).
- Compositor Services: "Drawing fully immersive content using Metal";
  sample "Interacting with virtual content blended with passthrough" (WWDC24 10092).
- ARKit (visionOS): `ARKitSession`, `WorldTrackingProvider`, `WorldAnchor` (persistence),
  `HandTrackingProvider`; "Tracking specific points in world space".
- Simulator: "Interacting with your app in the visionOS simulator" (gesture simulation).
- Porting patterns: "Bring your iOS or iPadOS game to visionOS" (WWDC24 10093 — hybrid
  window/space Metal games, stereoscopy, head prediction); the Human Interface Guidelines for
  immersive experiences (comfort, immersion levels, passthrough dimming); "Presenting windows and
  spaces" (window visibility beside a space, `UIApplicationSupportsMultipleScenes`).
- Meta compositor references (TimeWarp/Application SpaceWarp) for the ASW comparison in §2.7.
