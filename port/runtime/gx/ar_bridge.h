// AR Mode bridge: the C ABI between the game (Metal renderer, app flow) and the visionOS
// immersive renderer (Swift). Plain C so both the C++ build and Swift's bridging header read it.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// The mirror the Metal renderer publishes for the Vision Pro quad. Acquiring a snapshot pins
// its newest slot until dashdance_ar_release_frame, including repeated reads of that sequence.
// The consumer waits on `done` on the GPU, then releases the lease after its GPU work completes.
// The producer skips a pinned slot instead of waiting for the headset.
typedef struct DashdanceArFrame {
  uint64_t sequence;        // newest encoded frame; its slot is sequence % 3
  uint64_t lease;           // acquired ownership; release exactly once after the last GPU read
  int32_t width, height;    // slot texture size in pixels (the game picture, no letterbox)
  void* game[3];            // id<MTLTexture> BGRA8Unorm, sRGB bytes; only the newest slot is leased
  void* overlay[3];         // id<MTLTexture> BGRA8Unorm premultiplied (in-game menu / HUD)
  void* done;               // id<MTLSharedEvent>, notified with `sequence` per finished frame
  float aspect;             // displayed picture aspect (4:3 or 16:9, independent of texture size)
} DashdanceArFrame;

// 1 when the game's Metal renderer is running with the mirror on.
int dashdance_ar_active(void);
// Thread-safe acquisition into *out; returns 0 (and zeroes *out) when no frame is available.
// Early exits before submitting GPU work must also release the acquired lease.
int dashdance_ar_snapshot(DashdanceArFrame* out);
void dashdance_ar_release_frame(uint64_t lease);

// visionOS app flow (Swift @main drives it; stubs elsewhere). Swift registers the two hooks at
// startup; the game flow requests the immersive space after Play and waits for it to open.
void dashdance_vision_set_space_hooks(void (*request_open)(void), int (*is_open)(void));
void dashdance_vision_request_space(void);
int dashdance_vision_space_opened(void);
// SwiftUI's C entry stores the process's real argc/argv (simctl launch arguments, --iso,
// --script, ...), runs the app, and RootView starts the game flow with those arguments.
void dashdance_vision_store_args(int argc, char** argv);
void dashdance_vision_main(void);
// AR Mode: the game thread asks Swift to host the dashboard, then hands the controller over.
void dashdance_vision_set_dashboard_hook(void (*show)(void));
void dashdance_vision_show_dashboard(void);
void* dashdance_vision_take_controller(void);
// AR Mode: the quad's touch surface. Swift feeds the quad's spatial events through this: finger
// identifies the pinch, phase is 0 down / 1 move / 2 up, and u,v are the position across the game
// picture (0..1, v = 0 at the top). A pinch on the picture plays; off it, it places the screen.
void dashdance_ar_touch(int64_t finger, int phase, float u, float v);

#ifdef __cplusplus
}
#endif
