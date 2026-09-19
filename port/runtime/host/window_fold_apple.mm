// iPhone Duo fold: query UIKit's reserved division region through the SDL window's
// UIKit view (iOS 27.1+; a no-op everywhere else, including macOS and pre-27 iOS).
// SPDX-License-Identifier: GPL-2.0-or-later
#include <TargetConditionals.h>

#if defined(__APPLE__) && TARGET_OS_IPHONE
#import <UIKit/UIKit.h>

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
