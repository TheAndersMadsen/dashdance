// Power and thermal management on Apple platforms: while a game runs, ask the system for latency-critical
// scheduling (no timer coalescing, no App Nap) and keep the display and system awake; log thermal-state
// changes so throttling shows up next to the frame timings it explains.
// SPDX-License-Identifier: GPL-2.0-or-later
#import <Foundation/Foundation.h>
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#import <AppKit/AppKit.h>
#else
#import <UIKit/UIKit.h>
#import <AVFoundation/AVFoundation.h>
#import <QuartzCore/QuartzCore.h>
#endif
#import <Network/Network.h>
#include "host.h"
#include "gx_metal.h"
#include "input_config.h"
#include <algorithm>
#include <atomic>
#include <cstdio>

#if !TARGET_OS_OSX && !TARGET_OS_VISION
// Holds the display at its highest refresh rate while a game runs. A ProMotion iPhone or iPad otherwise lowers the panel
// to 60 Hz for 60 fps content, and a finished frame can then wait up to 16.7 ms for its slot instead of 8.3 ms. The link
// lives on its own thread, so it keeps ticking while the game's main thread sleeps between frames.
@interface MUDisplayPacer : NSObject
@property(nonatomic) NSThread* thread;
@property(nonatomic) NSInteger hz;
@end
@implementation MUDisplayPacer
- (void)tick:(CADisplayLink*)link {}
- (void)run {
  @autoreleasepool {
    CADisplayLink* link = [CADisplayLink displayLinkWithTarget:self selector:@selector(tick:)];
    const float hz = (float)self.hz;
    link.preferredFrameRateRange = CAFrameRateRangeMake(hz, hz, hz);
    [link addToRunLoop:NSRunLoop.currentRunLoop forMode:NSRunLoopCommonModes];
    while (!NSThread.currentThread.cancelled) {
      @autoreleasepool { [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.25]]; }
    }
    [link invalidate];
  }
}
@end
#endif

namespace host {
namespace {
id g_activity = nil;
std::atomic<bool> g_low_power{false}, g_bluetooth_audio{false};
id g_power_observer = nil, g_route_observer = nil;
std::atomic<int> g_net_kind{-1};   // -1 not known yet, 0 offline, 1 wired, 2 Wi-Fi, 3 mobile data, 4 other (VPN and such)
nw_path_monitor_t g_path_monitor = nil;
void network_monitor_start() {
  if (g_path_monitor) return;
  g_path_monitor = nw_path_monitor_create();
  nw_path_monitor_set_queue(g_path_monitor, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
  dispatch_semaphore_t first = dispatch_semaphore_create(0);
  nw_path_monitor_set_update_handler(g_path_monitor, ^(nw_path_t path) {
    int kind = 0;
    if (nw_path_get_status(path) == nw_path_status_satisfied)
      kind = nw_path_uses_interface_type(path, nw_interface_type_wired) ? 1 : nw_path_uses_interface_type(path, nw_interface_type_wifi) ? 2
           : nw_path_uses_interface_type(path, nw_interface_type_cellular) ? 3 : 4;
    const bool was_unknown = g_net_kind.load() < 0;
    g_net_kind = kind;
    if (was_unknown) dispatch_semaphore_signal(first);
  });
  nw_path_monitor_start(g_path_monitor);
  dispatch_semaphore_wait(first, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.25 * NSEC_PER_SEC)));   // the first answer takes milliseconds; the checklist should include it
}
#if !TARGET_OS_OSX && !TARGET_OS_VISION   // Vision Pro composites every window at the headset's fixed rate: nothing to raise
MUDisplayPacer* g_pacer = nil;
NSInteger screen_max_hz() {
  UIScreen* screen = nil;   // the screen the app's scene is on, not a global main screen (iPhone Duo has two)
  for (UIScene* sc in UIApplication.sharedApplication.connectedScenes) if ([sc isKindOfClass:UIWindowScene.class]) { screen = ((UIWindowScene*)sc).screen; break; }
  return MAX((NSInteger)60, (screen ?: UIScreen.mainScreen).maximumFramesPerSecond);
}
#endif
#if !TARGET_OS_OSX
bool route_is_wireless() {
  for (AVAudioSessionPortDescription* port in AVAudioSession.sharedInstance.currentRoute.outputs) {
    NSString* t = port.portType;
    if ([t isEqualToString:AVAudioSessionPortBluetoothA2DP] || [t isEqualToString:AVAudioSessionPortBluetoothLE] ||
        [t isEqualToString:AVAudioSessionPortBluetoothHFP] || [t isEqualToString:AVAudioSessionPortAirPlay]) return true;
  }
  return false;
}
#endif
id g_thermal_observer = nil;
const char* thermal_name(NSProcessInfoThermalState state) {
  switch (state) {
    case NSProcessInfoThermalStateNominal: return "nominal";
    case NSProcessInfoThermalStateFair: return "fair";
    case NSProcessInfoThermalStateSerious: return "serious (throttling)";
    case NSProcessInfoThermalStateCritical: return "critical (heavy throttling)";
  }
  return "unknown";
}
}  // namespace

const char* thermal_state_name() { return thermal_name(NSProcessInfo.processInfo.thermalState); }
namespace {
int g_device_cap = 0;
// Phones have the least thermal headroom: cap the internal resolution at 2x, and when the system reports
// serious or critical heat drop to 2x / 1x so the frame rate holds instead of the resolution.
void apply_caps() {
  const NSProcessInfoThermalState st = NSProcessInfo.processInfo.thermalState;
  const int thermal = st == NSProcessInfoThermalStateCritical ? 1 : st == NSProcessInfoThermalStateSerious ? 2 : 0;
  gx::metal_scale_caps(g_device_cap, thermal);
}
}  // namespace

std::vector<ReadinessItem> competitive_readiness(int display_hz, bool fullscreen, int online_delay) {
  network_monitor_start();
  std::vector<ReadinessItem> v;
  char b[200];
  // The wait is this display's own refresh interval, never a number from some other screen.
  const double wait_ms = 1000.0 / std::max(display_hz, 1);
  if (display_hz >= 100) { std::snprintf(b, sizeof b, "%d Hz display: a finished frame waits at most %.1f ms for the next refresh", display_hz, wait_ms); v.push_back({true, b}); }
  else { std::snprintf(b, sizeof b, "%d Hz display: frames wait up to %.1f ms for a refresh; a faster display shortens that", display_hz, wait_ms); v.push_back({false, b}); }
#if TARGET_OS_OSX
  v.push_back(fullscreen ? ReadinessItem{true, "Starts in full screen: frames skip the window compositor"}
                         : ReadinessItem{false, "Starts in a window: the compositor can hold each frame an extra refresh"});
#else
  (void)fullscreen;
#endif
  if (NSProcessInfo.processInfo.lowPowerModeEnabled) v.push_back({false, "Low Power Mode is on: it limits the display and slows the CPU"});
  switch (g_net_kind.load()) {
    case 1: v.push_back({true, "Wired network: the steadiest connection for online play"}); break;
    case 2: v.push_back({false, "Wi-Fi: a wired Ethernet connection rolls back less"}); break;
    case 3: v.push_back({false, "Mobile data: expect more rollback; Wi-Fi or Ethernet is steadier"}); break;
    case 0: v.push_back({false, "No network connection: online play is unavailable"}); break;
    case 4: v.push_back({true, "Connected to the network"}); break;
    default: break;
  }
  bool adapter = false, pad = false, wired = false, gamecube = false;
  double adapter_hz = 0, pad_hz = 0;
  for (const ControllerInfo& c : window_list_controllers()) {
    if (c.is_gamecube_adapter) { adapter = true; adapter_hz = c.report_hz; }
    else { pad = true; pad_hz = std::max(pad_hz, c.report_hz); wired = wired || c.wired; gamecube = gamecube || c.gamecube_controller; }
  }
  if (adapter) {
    if (adapter_hz >= 900) { std::snprintf(b, sizeof b, "GameCube adapter polling at %.0f Hz", adapter_hz); v.push_back({true, b}); }
    else if (adapter_hz > 0) { std::snprintf(b, sizeof b, "GameCube adapter at %.0f Hz: another USB port usually gives 1000 Hz", adapter_hz); v.push_back({false, b}); }
    else v.push_back({true, "GameCube adapter connected (measuring its polling rate)"});
  } else if (pad && gamecube) {
    if (pad_hz >= 900) { std::snprintf(b, sizeof b, "GameCube controller through the Lossless Adapter at %.0f Hz", pad_hz); v.push_back({true, b}); }
    else if (pad_hz > 0) { std::snprintf(b, sizeof b, "GameCube controller through the Lossless Adapter at %.0f Hz: it can do 1000 Hz, try another USB port", pad_hz); v.push_back({false, b}); }
    else v.push_back({true, "GameCube controller through the Lossless Adapter: move a stick to measure its report rate"});
  } else if (pad) {
    if (pad_hz <= 0) v.push_back({true, "Controller connected: move a stick to measure its report rate"});
    else if (wired) { std::snprintf(b, sizeof b, "Wired controller reporting at %.0f Hz", pad_hz); v.push_back({true, b}); }
    else { std::snprintf(b, sizeof b, "Bluetooth controller at %.0f Hz: a wired USB controller responds sooner", pad_hz); v.push_back({false, b}); }
  } else {
#if TARGET_OS_OSX
    v.push_back({false, "No controller: playing on the keyboard"});
#else
    v.push_back({false, "Touch controls only: a controller is far more precise"});
#endif
  }
#if !TARGET_OS_OSX
  if (route_is_wireless()) v.push_back({false, "Bluetooth audio lags the picture: use wired or built-in sound"});
#endif
  if (online_delay <= 1) v.push_back({true, "Online input delay 1 frame: the lowest latency"});
  else if (online_delay == 2) v.push_back({true, "Online input delay 2 frames: Slippi's default"});
  else { std::snprintf(b, sizeof b, "Online input delay %d frames adds %.0f ms: 2 is Slippi's default", online_delay, online_delay * 16.667); v.push_back({false, b}); }
  const NSProcessInfoThermalState heat = NSProcessInfo.processInfo.thermalState;
  if (heat == NSProcessInfoThermalStateSerious || heat == NSProcessInfoThermalStateCritical)
    v.push_back({false, "Device is hot: resolution steps down to hold 60 frames per second"});
  return v;
}
const char* latency_warning() {
  const bool lpm = g_low_power.load(), bt = g_bluetooth_audio.load();
  return lpm && bt ? "Low Power Mode on  ·  Bluetooth audio lags" : lpm ? "Low Power Mode on" : bt ? "Bluetooth audio lags" : "";
}
void audio_session_report() {
#if !TARGET_OS_OSX
  @autoreleasepool {
    AVAudioSession* session = AVAudioSession.sharedInstance;
    g_bluetooth_audio = route_is_wireless();
    NSString* route = session.currentRoute.outputs.firstObject.portName ?: @"none";
    log("audio session: IO buffer %.1f ms (asked 5.0), output latency %.1f ms, route %s%s", session.IOBufferDuration * 1000.0, session.outputLatency * 1000.0,
        route.UTF8String, g_bluetooth_audio.load() ? " (wireless: sound will lag the picture; wired or built-in audio is faster)" : "");
  }
#endif
}
void power_play_begin() {
  @autoreleasepool {
    g_low_power = NSProcessInfo.processInfo.lowPowerModeEnabled;
    if (!g_power_observer)
      g_power_observer = [NSNotificationCenter.defaultCenter addObserverForName:NSProcessInfoPowerStateDidChangeNotification object:nil queue:nil usingBlock:^(NSNotification*) {
        g_low_power = NSProcessInfo.processInfo.lowPowerModeEnabled;
        log("power: Low Power Mode %s", g_low_power.load() ? "on (the system limits the display and the CPU)" : "off");
      }];
    if (g_low_power.load()) log("power: Low Power Mode is on: the system limits the display to 60 Hz and slows the CPU, which adds latency");
#if TARGET_OS_OSX
    if (!g_activity)
      g_activity = [NSProcessInfo.processInfo beginActivityWithOptions:NSActivityUserInitiated | NSActivityLatencyCritical | NSActivityIdleDisplaySleepDisabled | NSActivityIdleSystemSleepDisabled
                                                                reason:@"Playing Super Smash Bros. Melee"];
#else
    UIApplication.sharedApplication.idleTimerDisabled = YES;   // the game is played with a controller: never dim the screen
    if (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone) g_device_cap = 2;
#if !TARGET_OS_VISION
    if (!g_pacer) {
      g_pacer = [[MUDisplayPacer alloc] init];
      g_pacer.hz = screen_max_hz();
      g_pacer.thread = [[NSThread alloc] initWithTarget:g_pacer selector:@selector(run) object:nil];
      g_pacer.thread.name = @"display pacer"; g_pacer.thread.qualityOfService = NSQualityOfServiceUtility;
      [g_pacer.thread start];
      log("display: holding the panel at %ld Hz while playing, so a finished frame waits at most %.1f ms for its refresh", (long)g_pacer.hz, 1000.0 / g_pacer.hz);
    }
#endif
    // Audio: a small hardware buffer so sound lands with the frame that made it (iOS defaults to about 20 ms), and no
    // system alert sounds cutting into a match. SDL activates the session when it opens the device; these preferences apply then.
    AVAudioSession* session = AVAudioSession.sharedInstance;
    [session setPreferredSampleRate:48000 error:nil];
    [session setPreferredIOBufferDuration:0.005 error:nil];
#if !TARGET_OS_VISION
    [session setPrefersNoInterruptionsFromSystemAlerts:YES error:nil];
#endif
    g_bluetooth_audio = route_is_wireless();
    if (!g_route_observer)
      g_route_observer = [NSNotificationCenter.defaultCenter addObserverForName:AVAudioSessionRouteChangeNotification object:nil queue:nil usingBlock:^(NSNotification*) {
        const bool wireless = route_is_wireless();
        if (wireless != g_bluetooth_audio.load()) log("audio: output is now %s", wireless ? "wireless (sound lags the picture)" : "wired or built-in");
        g_bluetooth_audio = wireless;
      }];
#endif
    apply_caps();
    log("power: latency-critical activity on, display sleep off, thermal state %s", thermal_state_name());
    if (!g_thermal_observer)
      g_thermal_observer = [NSNotificationCenter.defaultCenter addObserverForName:NSProcessInfoThermalStateDidChangeNotification object:nil queue:nil
                                                                        usingBlock:^(NSNotification*) { log("power: thermal state now %s", thermal_state_name()); apply_caps(); }];
  }
}

void power_play_end() {
  @autoreleasepool {
#if TARGET_OS_OSX
    if (g_activity) { [NSProcessInfo.processInfo endActivity:g_activity]; g_activity = nil; }
#else
    UIApplication.sharedApplication.idleTimerDisabled = NO;
#if !TARGET_OS_VISION
    if (g_pacer) { [g_pacer.thread cancel]; g_pacer = nil; }   // back to the system's adaptive refresh: saves battery in menus
#endif
    if (g_route_observer) { [NSNotificationCenter.defaultCenter removeObserver:g_route_observer]; g_route_observer = nil; }
#endif
    if (g_power_observer) { [NSNotificationCenter.defaultCenter removeObserver:g_power_observer]; g_power_observer = nil; }
    if (g_thermal_observer) { [NSNotificationCenter.defaultCenter removeObserver:g_thermal_observer]; g_thermal_observer = nil; }
  }
}
}  // namespace host
