// Xelee mode bridge: the C ABI between the game (Metal renderer, app flow) and the visionOS
// immersive renderer (Swift). Plain C so both the C++ build and Swift's bridging header read it.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// The mirror the Metal renderer publishes for the Vision Pro quad. Three slot textures; the
// quad's frame loop picks the newest finished sequence and waits on `done` on the GPU.
// `read` is the reverse direction: the quad pass signals the sequence it consumed, and the game
// waits for it before re-rendering into a slot (the standard triple-buffer handshake, GPU-side).
typedef struct DashdanceMrFrame {
  uint64_t sequence;        // newest finished frame; slot k holds sequence s where (s - 1) % 3 == k
  int32_t width, height;    // slot texture size in pixels (the game picture, no letterbox)
  void* game[3];            // id<MTLTexture> BGRA8Unorm, sRGB bytes
  void* overlay[3];         // id<MTLTexture> RGBA8Unorm premultiplied (in-game menu / HUD)
  void* done;               // id<MTLSharedEvent>, notified with `sequence` per finished frame
  void* read;               // id<MTLSharedEvent>, waited with (sequence - 3) by the game
  float aspect;             // picture aspect (width / height)
} DashdanceMrFrame;

// 1 when the game's Metal renderer is running with the mirror on.
int dashdance_mr_active(void);
// Thread-safe copy into *out; returns 0 (and zeroes *out) when the mirror is not running.
int dashdance_mr_snapshot(DashdanceMrFrame* out);

// visionOS app flow (Swift @main drives it; stubs elsewhere). Swift registers the two hooks at
// startup; the game flow requests the immersive space after Play and waits for it to open.
void dashdance_vision_set_space_hooks(void (*request_open)(void), int (*is_open)(void));
void dashdance_vision_request_space(void);
int dashdance_vision_space_opened(void);
// The whole launcher + game flow on the calling thread (the main thread), then exit()s.
void dashdance_vision_main(void);
// Xelee mode: the game thread asks Swift to host the dashboard, then hands the controller over.
void dashdance_vision_set_dashboard_hook(void (*show)(void));
void dashdance_vision_show_dashboard(void);
void* dashdance_vision_take_controller(void);

#ifdef __cplusplus
}
#endif
