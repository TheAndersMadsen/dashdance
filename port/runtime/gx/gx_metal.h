// Native Metal renderer for decoded GX frames: the D3D12 backend's design (EFB render
// target at an integer scale, Dolphin-derived integer-math shaders, EFB copies kept as
// GPU textures, letterboxed present) on Metal, for macOS, iOS and visionOS.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "gx_core.h"
#include "overlay.h"
#include <cstdint>
#include <functional>
#include <string>

namespace gx {
struct MetalOptions {
  int efb_scale = 0;         // internal resolution multiplier; 0 = auto (integer scale covering the window)
  bool vsync = true;
  bool widescreen = false;   // present at 16:9 (Slippi widescreen code on)
  float sharpness = 0.0f;    // 0..1 contrast-adaptive sharpening in the present pass
  int anisotropy = 16;
  int ssaa = 1;              // 2 = 4x supersampling
  int upscaler = 0;          // 0 = off, 1 = MetalFX spatial (balanced), 2 = MetalFX spatial (quality): the EFB
                             // is rendered at half the window's auto scale and reconstructed to full size at
                             // present; ignored (plain blit) where MetalFX is unsupported or downscaling
  std::string capture_path;  // write a PPM of the presented EFB region at capture_frame
  uint32_t capture_frame = 0;
  uint32_t capture_every = 0;
  std::string cache_dir;     // pipelines.bin lives here: every pipeline seen, precompiled in the background next launch
};

// `layer` is a CAMetalLayer*; `w`/`h` the drawable size in pixels.
Backend* create_metal_backend(void* layer, int w, int h, const MetalOptions& options);
void metal_resize(Backend* backend, int w, int h);
void metal_set_options(Backend* backend, const MetalOptions& options);
// Caps on the internal-resolution multiplier, applied to both auto and explicit scales and re-evaluated
// every frame: `device_cap` for the device class (phones: 2), `thermal_cap` from the thermal state
// (serious: 2, critical: 1). 0 = no cap.
void metal_scale_caps(int device_cap, int thermal_cap);
uint64_t metal_frames_presented(Backend* backend);
// Drawn over every presented frame (touch controls); the provider runs on the render thread.
using OverlayProvider = std::function<bool(host::OverlayFrame&)>;
void metal_set_overlay(Backend* backend, OverlayProvider provider);
}  // namespace gx
