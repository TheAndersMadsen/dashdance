// iPhone Duo fold: query UIKit's reserved division region through the SDL window's
// UIKit view (iOS 27.1+; a no-op everywhere else, including macOS and pre-27 iOS).
// SPDX-License-Identifier: GPL-2.0-or-later
#include <TargetConditionals.h>

#if defined(__APPLE__) && TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#include <algorithm>
#include <cstring>

// Returns true and fills x0,y0,x1,y1 (client pixels) when a division region is
// currently active — the curved centre band of the inner display while partially
// folded. The frame already includes UIKit's margins for interactive content.
bool window_fold_division_apple(void* uiwindow, float pixels_per_point, float* out) {
  if (@available(iOS 27.1, *)) {
    UIWindow* window = (__bridge UIWindow*)uiwindow;
    if (!window) return false;
    UIView* view = window.rootViewController.view ?: window;
    if (view.bounds.size.width <= 0 || view.bounds.size.height <= 0) return false;
    NSArray<UIViewReservedRegion*>* regions = [view reservedRegionsOfKind:[UIViewReservedRegionKind divisionRegionKind]];
    for (UIViewReservedRegion* region in regions) {
      if (!region.isActive) continue;
      const CGRect f = region.frame;
      if (f.size.width <= 0 || f.size.height <= 0) continue;
      out[0] = f.origin.x * pixels_per_point;
      out[1] = f.origin.y * pixels_per_point;
      out[2] = (f.origin.x + f.size.width) * pixels_per_point;
      out[3] = (f.origin.y + f.size.height) * pixels_per_point;
      return true;
    }
  }
  return false;
}
#else
bool window_fold_division_apple(void*, float, float*) { return false; }
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE
// iPhone Duo: the inner display rotates regardless of the app's supported orientations, and
// SDL's size bookkeeping can lag that rotation — leaving the Metal view at its old frame while
// the scene window has already changed shape (a portrait layout drawn into the top-left corner
// of a landscape screen). Sync the view to the window and report the authoritative geometry:
// pixel size from the layer's drawable, point size from the view bounds, insets from UIKit.
// Returns false when no Metal view is found (callers keep their SDL-based values).
bool window_sync_apple(void* uiwindow, float* out) {
  if (@available(iOS 16.0, *)) {
    UIWindow* window = (__bridge UIWindow*)uiwindow;
    if (!window || window.bounds.size.width <= 0 || window.bounds.size.height <= 0) return false;
    // Depth-first search for the SDL Metal view (the subview backed by a CAMetalLayer).
    UIView* metal_view = nil;
    NSArray<UIView*>* frontier = @[window];
    for (int depth = 0; depth <= 6 && !metal_view && frontier.count; ++depth) {
      NSMutableArray<UIView*>* next = nil;
      for (UIView* view in frontier) {
        if ([view.layer isKindOfClass:[CAMetalLayer class]]) { metal_view = view; break; }
        if (view.subviews.count) {
          if (!next) next = [NSMutableArray array];
          [next addObjectsFromArray:view.subviews];
        }
      }
      frontier = next;
    }
    if (!metal_view) return false;
    metal_view.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    metal_view.frame = metal_view.superview ? metal_view.superview.bounds : window.bounds;
    CAMetalLayer* layer = (CAMetalLayer*)metal_view.layer;
    CGSize drawable = layer.drawableSize;
    const UIEdgeInsets insets = metal_view.safeAreaInsets;
    const float px_per_pt = out[0] && out[2] ? (float)drawable.width / (float)metal_view.bounds.size.width : 1.0f;
    out[0] = (float)drawable.width;   out[1] = (float)drawable.height;   // client pixels
    out[2] = (float)metal_view.bounds.size.width;                        // points
    out[3] = (float)metal_view.bounds.size.height;
    out[4] = insets.top * px_per_pt;                                     // safe insets in pixels
    out[5] = insets.left * px_per_pt;
    out[6] = insets.right * px_per_pt;
    out[7] = insets.bottom * px_per_pt;
    out[8] = out[9] = out[10] = out[11] = 0.0f;                          // extra occlusion insets, pixels
    // Occlusion regions (the outer camera, or the under-display camera while it streams) on top
    // of the safe area: only the part that reaches further inward than the safe inset counts, so
    // nothing is double-counted and the layout can dodge the camera when it activates.
    if (@available(iOS 27.1, *)) {
      const CGRect bounds = metal_view.bounds;
      for (UIViewReservedRegion* region in [metal_view reservedRegionsOfKind:[UIViewReservedRegionKind occlusionRegionKind]]) {
        if (!region.isActive) continue;
        const CGRect f = region.frame;
        out[9] = std::max(out[9], (float)(CGRectGetMaxX(f) - insets.left) * px_per_pt);                     // reaches in from the left
        out[10] = std::max(out[10], (float)(insets.right - CGRectGetMinX(f)) * px_per_pt);                  // from the right
        out[8] = std::max(out[8], (float)(CGRectGetMaxY(f) - insets.top) * px_per_pt);                      // from the top
        out[11] = std::max(out[11], (float)(insets.bottom - (bounds.size.height - CGRectGetMinY(f))) * px_per_pt);  // from the bottom
      }
      out[9] = std::max(0.0f, out[9]); out[10] = std::max(0.0f, out[10]);
      out[8] = std::max(0.0f, out[8]); out[11] = std::max(0.0f, out[11]);
    }
    return drawable.width > 0 && drawable.height > 0 && out[2] > 0 && out[3] > 0;
  }
  return false;
}
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE
// Test aid (MELEE_ORIENTATION=landscape|portrait): ask the scene to rotate, exactly as the
// launcher's aid does, so the game path can be exercised in every pose on the simulator.
bool window_request_orientation_apple(void* uiwindow, const char* orientation) {
  if (@available(iOS 16.0, *)) {
    UIWindow* window = (__bridge UIWindow*)uiwindow;
    if (!window || !orientation || !*orientation) return false;
    const UIInterfaceOrientationMask mask = std::strcmp(orientation, "landscape") == 0
        ? UIInterfaceOrientationMaskLandscapeRight : UIInterfaceOrientationMaskPortrait;
    [window.windowScene requestGeometryUpdateWithPreferences:
        [[UIWindowSceneGeometryPreferencesIOS alloc] initWithInterfaceOrientations:mask] errorHandler:nil];
    return true;
  }
  return false;
}
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE
#include <atomic>
#include <cstdlib>
namespace host { void log(const char* format, ...); }
// Hinge breadcrumb (iPhone Duo only): logs coarse pose changes and the angle at 15° steps into
// the session log, so a pose-dependent bug report says which pose it happened in. Layout never
// reads the angle — that is what the reserved-region queries are for.
void window_hinge_watch_apple(void* uiwindow) {
  if (@available(iOS 27.1, *)) {
    UIWindow* window = (__bridge UIWindow*)uiwindow;
    if (!window) return;
    static UIHingeInteraction* interaction = nil;   // retained for the process lifetime
    interaction = [[UIHingeInteraction alloc] initWithUpdateHandler:^(UIHingeInteraction*, UIHingeInteractionUpdate* update) {
      if (!update.hinge) {
        static std::atomic<bool> none_reported{false};
        if (!none_reported.exchange(true)) host::log("hinge: none on this device");
        return;
      }
      static std::atomic<int> last_status{0};
      static std::atomic<int> last_angle_step{-999};
      const int status = (int)update.hinge.status;
      const int step = (int)(update.hinge.angle / 15.0);
      if (status != last_status.exchange(status) || step != last_angle_step.exchange(step)) {
        static const char* names[] = {"unknown", "closed", "partially open", "fully open"};
        host::log("hinge: %s (%.0f degrees)", names[status >= 1 && status <= 3 ? status : 0], update.hinge.angle);
      }
    }];
    UIView* view = window.rootViewController.view ?: window;
    [view addInteraction:interaction];
  }
}
#endif
