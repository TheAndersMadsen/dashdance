// Host window services.
#pragma once
#include <functional>
#include <cstdint>

namespace host {
using MessageCallback = std::function<bool(void*, uint32_t, uintptr_t, intptr_t)>;
void window_set_message_callback(MessageCallback cb);
void window_input_capture(bool capture);
struct PadState;
bool window_ui_gamecube_pad(PadState& pad);
using ResizeCallback = std::function<void(int, int)>;
void* window_create(int w, int h, const wchar_t* title, bool visible = true);
void window_set_resize_callback(ResizeCallback cb);
void window_pump();
void window_set_fullscreen(bool enabled);
// Pixels at the top of the window covered by the Dynamic Island, notch or status bar (0 on the Mac).
float window_safe_top_pixels();
// All four safe-area insets in pixels (island, notch, rounded corners, home indicator); zeros on the Mac.
void window_safe_insets(float& top, float& left, float& right, float& bottom);
// iPhone Duo: the active fold division region in client pixels; false when flat or unsupported.
bool window_division_region(float out[4]);
float window_pixels_per_point();
// Where the game's picture goes in a ww x wh pixel window. Held upright it sits under the Dynamic Island and never
// reaches past the middle of the screen, so on iPhone Duo's inner display the fold falls between the game and the
// touch controls; otherwise it is centred at full height. The same rectangle drives the renderer, the touch layout and
// the artwork drawn in the letterbox.
struct GameRect { float x, y, w, h; };
GameRect window_game_rect(float ww, float wh, float aspect);
void window_gamepad_rumble(int port, bool on);   // SDL gamepad on that GameCube port, if any
bool window_take_fullscreen_toggle();   // true once per Alt+Enter press in the game window
double window_refresh_rate();
void window_destroy();
void window_set_title(const wchar_t* title);
bool window_closed();
void window_client_size(int* w, int* h);
// Scripted input: text file with lines "FRAME BUTTON+BUTTON [sx=N] [sy=N] [cx=N] [cy=N]"; state holds
// until the next line. Buttons: A B X Y Z L R START DU DD DL DR. A line with only a frame releases all.
bool input_load_script(const char* path);
void input_mark_match_start();   // confirmed online or recorded offline match frame 1 starts `@match` input
}  // namespace host
