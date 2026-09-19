// iPhone Duo fold: query UIKit's reserved division region through the SDL window's
// UIKit view (iOS 27.1+; a no-op everywhere else, including macOS and pre-27 iOS).
// SPDX-License-Identifier: GPL-2.0-or-later
#include <TargetConditionals.h>

#if defined(__APPLE__) && TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
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
    out[0] = (float)drawable.width;   out[1] = (float)drawable.height;   // client pixels
    out[2] = (float)metal_view.bounds.size.width;                        // points
    out[3] = (float)metal_view.bounds.size.height;
    out[4] = insets.top * out[0] / out[2];                               // safe insets in pixels
    out[5] = insets.left * out[0] / out[2];
    out[6] = insets.right * out[0] / out[2];
    out[7] = insets.bottom * out[0] / out[2];
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
