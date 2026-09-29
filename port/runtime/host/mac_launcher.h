// Native launcher (AppKit on macOS, UIKit on iOS/visionOS): pick the disc, a few
// settings, Play. Nothing from the game ships with the app; the user's own image is used.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>
namespace host {
struct LauncherSettings {
  std::string iso;             // remembered disc image; empty until chosen/imported
  bool widescreen = false;
  bool flash_failed_lcancel = false;
  float sharpness = 0.0f;      // 0..1
  float overlay_opacity = 1.0f;// on-screen controller (touch devices)
  float overlay_scale = 1.0f;  // 0.7..1.4
  bool online = true;          // Slippi Online services
  int online_delay = 2;        // Slippi Online input delay frames (1..9): each frame adds 16.7 ms; 2 is Slippi's default
  int scale = 0;               // internal resolution multiplier, 0 = auto
  int anisotropy = 16;         // 1, 4, 16
  int upscaler = 0;            // 0 off, 1 MetalFX spatial (balanced), 2 (quality)
  bool vsync = true;           // display sync; off = uncapped presentation
  bool fullscreen = true;
  int volume = 70;             // 0..100
  bool discord_enabled = true, discord_show_rank = true;   // Discord Rich Presence (macOS), through Dashdance's Discord application
  std::string rank; float rating = 0.0f;                  // the player's tier and rating, once the dashboard fetched them
  bool hud = false;            // performance HUD in game      // macOS: start in full screen (measured: ~10 ms display latency versus ~25 ms in a window, which costs a compositor frame)
  int display_hz = 60;         // informational: the display's maximum refresh rate
  std::string replay_dir;      // for the recent-games list
  std::string gpu_name;        // informational
  std::string slippi_dir;      // where this app keeps its own user.json (native sign-in)
  std::string account_name, account_code;   // current login, if any
  bool account_from_launcher = false;       // login comes from the Slippi Launcher's file (macOS)
};
// Shows the launcher and blocks until Play (true) or quit (false). `error` explains why the
// remembered disc could not be used, if that happened.
bool launcher_run(LauncherSettings& settings, const std::string& error);
void mac_show_error(const std::string& title, const std::string& detail);
}  // namespace host
