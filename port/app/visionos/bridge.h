// AR Mode: the C ABI the Swift app shares with the game (see ar_bridge.h).
#import "../../runtime/gx/ar_bridge.h"
// Swift reattaches SDL's existing UIWindow to the recreated main scene before restoring it.
void dashdance_vision_window_created(void* uiwindow);
void dashdance_vision_restore_window(void);
