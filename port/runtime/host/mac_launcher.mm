// macOS dashboard and launcher (AppKit + SceneKit). Melee-styled: dark grid backdrop, yellow
// angled section headers, italic display type. Shows the signed-in player's ranked profile and
// recent games, manages controllers (ports, remapping) and display settings, then starts play.
// Disc images can be dropped onto the window.
// SPDX-License-Identifier: GPL-2.0-or-later
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <SceneKit/SceneKit.h>
#import <objc/runtime.h>
#include "dashboard.h"
#include "gc_diagram.h"
#include "host.h"
#include "input_config.h"
#include "controller_pairing.h"
#import <GameController/GameController.h>
#include "mac_launcher.h"
#include "slippi_login.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// One line describing a controller's connection and measured report rate, for the dashboards.
static std::string controller_rate_line(const host::ControllerInfo& pad) {
  char b[160];
  if (pad.is_gamecube_adapter) {
    std::string ports;
    for (int p = 0; p < 4; ++p) if (pad.adapter_ports & (1u << p)) ports += (ports.empty() ? "port " : ", ") + std::to_string(p + 1);
    if (pad.report_hz <= 0) std::snprintf(b, sizeof b, "USB · %s · measuring the polling rate…", ports.empty() ? "no controller plugged in" : ports.c_str());
    else if (pad.report_hz >= 900) std::snprintf(b, sizeof b, "USB · %s · polling at %.0f Hz (1 ms)", ports.empty() ? "no controller plugged in" : ports.c_str(), pad.report_hz);
    else std::snprintf(b, sizeof b, "USB · %s · polling at %.0f Hz; the host kept the adapter's default, try another USB port or hub", ports.empty() ? "no controller plugged in" : ports.c_str(), pad.report_hz);
    return b;
  }
  if (pad.report_hz <= 0) std::snprintf(b, sizeof b, "%s · move a stick to measure the report rate", pad.wired ? "Wired" : "Bluetooth");
  else std::snprintf(b, sizeof b, "%s · reports at %.0f Hz (%.1f ms)", pad.wired ? "Wired" : "Bluetooth", pad.report_hz, 1000.0 / pad.report_hz);
  return b;
}

@class MULauncherWindow;
@class MUControllerEditor;
// Menu bar extra (NSStatusItem): the Dashdance mark in the menu bar with the player's rank, rating and
// record, the last games, and the actions that make sense from anywhere: Play, show the dashboard,
// sign out, quit. Built once per process; the dashboard feeds it, the game keeps it.
@interface MUStatusBar : NSObject
@property(nonatomic) NSStatusItem* item;
@property(nonatomic, weak) MULauncherWindow* launcher;
@property(nonatomic) host::Dashboard dashboard;
@property(nonatomic) BOOL playing;
- (void)refresh;
@end
static MUStatusBar* g_status = nil;

// Help menu links (menu items need a target outside the responder chain).
@interface MULinks : NSObject
@end
@implementation MULinks
- (void)openGitHub:(id)sender { [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:@"https://github.com/TheAndersMadsen/dashdance"]]; }
- (void)openSlippiSite:(id)sender { [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:@"https://slippi.gg"]]; }
- (void)quit:(id)sender {
  if (NSApp.modalWindow) [NSApp stopModalWithCode:NSModalResponseCancel];
  else host::request_exit(0);
}
@end
static MULinks* g_links = nil;

namespace {
NSMenu* build_main_menu() {
  NSMenu* menubar = [[NSMenu alloc] init];
  NSMenuItem* appItem = [[NSMenuItem alloc] init]; [menubar addItem:appItem];
  NSMenu* app = [[NSMenu alloc] initWithTitle:@"Dashdance"];
  [app addItemWithTitle:@"About Dashdance" action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
  [app addItem:[NSMenuItem separatorItem]];
  [app addItemWithTitle:@"Hide Dashdance" action:@selector(hide:) keyEquivalent:@"h"];
  NSMenuItem* hideOthers = [app addItemWithTitle:@"Hide Others" action:@selector(hideOtherApplications:) keyEquivalent:@"h"];
  hideOthers.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
  [app addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
  [app addItem:[NSMenuItem separatorItem]];
  NSMenuItem* quit = [app addItemWithTitle:@"Quit Dashdance" action:@selector(quit:) keyEquivalent:@"q"];
  quit.target = g_links;
  appItem.submenu = app;
  NSMenuItem* windowItem = [[NSMenuItem alloc] init]; [menubar addItem:windowItem];
  NSMenu* window = [[NSMenu alloc] initWithTitle:@"Window"];
  [window addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
  [window addItemWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""];
  NSMenuItem* fs = [window addItemWithTitle:@"Enter Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
  fs.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
  windowItem.submenu = window;
  NSApp.windowsMenu = window;
  NSMenuItem* helpItem = [[NSMenuItem alloc] init]; [menubar addItem:helpItem];
  NSMenu* help = [[NSMenu alloc] initWithTitle:@"Help"];
  NSMenuItem* gh = [help addItemWithTitle:@"Dashdance on GitHub" action:@selector(openGitHub:) keyEquivalent:@""]; gh.target = g_links;
  NSMenuItem* sg = [help addItemWithTitle:@"Slippi.gg" action:@selector(openSlippiSite:) keyEquivalent:@""]; sg.target = g_links;
  helpItem.submenu = help;
  NSApp.helpMenu = help;
  return menubar;
}
void prepare_application() {
  [NSApplication sharedApplication];
  [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
  static bool launched = false;
  if (!launched) { g_links = [[MULinks alloc] init]; NSApp.mainMenu = build_main_menu(); [NSApp finishLaunching]; launched = true; }
  [NSApp activateIgnoringOtherApps:YES];
}
NSColor* rgb(CGFloat r, CGFloat g, CGFloat b, CGFloat a = 1) { return [NSColor colorWithSRGBRed:r green:g blue:b alpha:a]; }
// Theme: Melee's menu yellow on deep blue, violet for the Dashdance mark. One place for every colour.
NSColor* kYellow() { return rgb(0.97, 0.79, 0.28); }
NSColor* kMarkViolet() { return rgb(0.49, 0.36, 1.00); }
NSColor* kGlassTint() { return rgb(0.30, 0.40, 1.0, 0.10); }
// The Dashdance mark ships in the bundle (AppMark.png, the icon's glyph layer); MELEE_MARK overrides the path (for the bare binary).
NSImage* slippi_mark() {
  NSString* path = [NSBundle.mainBundle pathForResource:@"AppMark" ofType:@"png"];
  if (const char* env = std::getenv("MELEE_MARK")) path = [NSString stringWithUTF8String:env];
  NSImage* image = path ? [[NSImage alloc] initWithContentsOfFile:path] : nil;
  [image setTemplate:YES];
  return image;
}
NSColor* kInk() { return rgb(0.10, 0.08, 0.02); }
static const void* const kSliderLabelKey = &kSliderLabelKey;    // associated-object keys for the slider value labels
static const void* const kSliderFormatKey = &kSliderFormatKey;
static const void* const kSliderScaleKey = &kSliderScaleKey;
NSColor* kGreen() { return rgb(0.30, 0.85, 0.45); }
NSColor* kRed() { return rgb(0.89, 0.27, 0.17); }
// stringWithUTF8String returns nil for invalid UTF-8 (replay names and tags are Shift-JIS on the console),
// and a nil label string is an AppKit assertion that aborts the launcher. Fall back to Shift-JIS, then Latin-1.
NSString* ns(const std::string& s) {
  if (NSString* utf8 = [NSString stringWithUTF8String:s.c_str()]) return utf8;
  if (NSString* sjis = [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSShiftJISStringEncoding]) return sjis;
  return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSISOLatin1StringEncoding] ?: @"";
}
NSImage* symbol(NSString* name, CGFloat size, NSFontWeight weight) {
  NSImage* image = [NSImage imageWithSystemSymbolName:name accessibilityDescription:nil];
  return [image imageWithSymbolConfiguration:[NSImageSymbolConfiguration configurationWithPointSize:size weight:weight]];
}
NSFont* meleeFont(CGFloat size) {
  NSFont* base = [NSFont systemFontOfSize:size weight:NSFontWeightBlack];
  NSFontDescriptor* d = [base.fontDescriptor fontDescriptorWithSymbolicTraits:NSFontDescriptorTraitItalic | NSFontDescriptorTraitBold];
  NSFont* f = d ? [NSFont fontWithDescriptor:d size:size] : nil;
  return f ?: base;
}
NSTextField* label(NSString* text, CGFloat size, NSFontWeight weight, CGFloat alpha) {
  NSTextField* l = [NSTextField wrappingLabelWithString:text];
  l.font = [NSFont systemFontOfSize:size weight:weight]; l.textColor = [NSColor colorWithWhite:1 alpha:alpha]; l.selectable = NO;
  [l setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
  return l;
}
int display_max_hz(NSScreen* screen) {
  if (@available(macOS 12.0, *)) { if (screen.maximumFramesPerSecond > 0) return (int)screen.maximumFramesPerSecond; }
  return 60;
}
}  // namespace

// ---- 3D mark: metallic ring around a gold core, slowly turning
@interface MUHeroView : SCNView
@end
@implementation MUHeroView
- (instancetype)initWithSize:(CGFloat)size {
  self = [super initWithFrame:NSMakeRect(0, 0, size, size) options:nil];
  self.translatesAutoresizingMaskIntoConstraints = NO;
  [self.widthAnchor constraintEqualToConstant:size].active = YES;
  [self.heightAnchor constraintEqualToConstant:size].active = YES;
  self.backgroundColor = NSColor.clearColor;
  self.antialiasingMode = SCNAntialiasingModeMultisampling4X;
  self.autoenablesDefaultLighting = NO;
  SCNScene* scene = [SCNScene scene];
  self.scene = scene;
  SCNNode* root = [SCNNode node];
  SCNTorus* torus = [SCNTorus torusWithRingRadius:1.0 pipeRadius:0.34];
  torus.ringSegmentCount = 96; torus.pipeSegmentCount = 48;
  SCNMaterial* metal = [SCNMaterial material];
  metal.lightingModelName = SCNLightingModelPhysicallyBased;
  metal.diffuse.contents = rgb(0.96, 0.96, 1.0); metal.metalness.contents = @0.9; metal.roughness.contents = @0.22;
  torus.materials = @[metal];
  SCNNode* ring = [SCNNode nodeWithGeometry:torus];
  ring.eulerAngles = SCNVector3Make(M_PI_2 * 0.92, 0, 0);
  SCNSphere* core = [SCNSphere sphereWithRadius:0.42];
  SCNMaterial* gold = [SCNMaterial material];
  gold.lightingModelName = SCNLightingModelPhysicallyBased;
  gold.diffuse.contents = kYellow(); gold.metalness.contents = @0.85; gold.roughness.contents = @0.3; gold.emission.contents = rgb(0.5, 0.35, 0.05);
  core.materials = @[gold];
  SCNNode* coreNode = [SCNNode nodeWithGeometry:core];
  [root addChildNode:ring]; [root addChildNode:coreNode];
  [scene.rootNode addChildNode:root];
  SCNNode* key = [SCNNode node]; key.light = [SCNLight light]; key.light.type = SCNLightTypeOmni; key.light.intensity = 900; key.light.color = NSColor.whiteColor; key.position = SCNVector3Make(3, 4, 5);
  SCNNode* fill = [SCNNode node]; fill.light = [SCNLight light]; fill.light.type = SCNLightTypeOmni; fill.light.intensity = 400; fill.light.color = rgb(0.6, 0.55, 1.0); fill.position = SCNVector3Make(-4, -2, 3);
  SCNNode* ambient = [SCNNode node]; ambient.light = [SCNLight light]; ambient.light.type = SCNLightTypeAmbient; ambient.light.intensity = 250; ambient.light.color = rgb(0.4, 0.4, 0.6);
  [scene.rootNode addChildNode:key]; [scene.rootNode addChildNode:fill]; [scene.rootNode addChildNode:ambient];
  SCNNode* cam = [SCNNode node]; cam.camera = [SCNCamera camera]; cam.camera.fieldOfView = 34; cam.position = SCNVector3Make(0, 0.6, 5.2);
  [cam lookAt:SCNVector3Zero];
  [scene.rootNode addChildNode:cam];
  self.pointOfView = cam;
  [root runAction:[SCNAction repeatActionForever:[SCNAction rotateByX:0 y:M_PI * 2 z:0 duration:14]]];
  [coreNode runAction:[SCNAction repeatActionForever:[SCNAction sequence:@[[SCNAction scaleTo:1.12 duration:1.6], [SCNAction scaleTo:1.0 duration:1.6]]]]];
  [ring runAction:[SCNAction repeatActionForever:[SCNAction sequence:@[[SCNAction rotateByX:0.25 y:0 z:0 duration:3.0], [SCNAction rotateByX:-0.25 y:0 z:0 duration:3.0]]]]];
  return self;
}
@end

// ---- backdrop: dark blue gradient, faint grid, drifting gold glow (layer-backed)
@interface MUBackdropView : NSView
@end
@implementation MUBackdropView { CAGradientLayer* _gradient; CAShapeLayer* _grid; CAGradientLayer* _glow; }
- (instancetype)initWithFrame:(NSRect)frame {
  self = [super initWithFrame:frame];
  self.wantsLayer = YES; self.translatesAutoresizingMaskIntoConstraints = NO;
  _gradient = [CAGradientLayer layer];
  _gradient.colors = @[(id)rgb(0.02, 0.02, 0.08).CGColor, (id)rgb(0.05, 0.07, 0.20).CGColor];
  _gradient.startPoint = CGPointMake(0.5, 0); _gradient.endPoint = CGPointMake(0.5, 1);
  [self.layer addSublayer:_gradient];
  _grid = [CAShapeLayer layer];
  _grid.strokeColor = rgb(0.45, 0.55, 1.0, 0.10).CGColor; _grid.lineWidth = 1; _grid.fillColor = nil;
  [self.layer addSublayer:_grid];
  _glow = [CAGradientLayer layer];
  _glow.type = kCAGradientLayerRadial;
  _glow.colors = @[(id)rgb(0.97, 0.79, 0.28, 0.20).CGColor, (id)rgb(0.97, 0.79, 0.28, 0).CGColor];
  _glow.startPoint = CGPointMake(0.5, 0.5); _glow.endPoint = CGPointMake(1, 1);
  [self.layer addSublayer:_glow];
  return self;
}
- (void)layout {
  [super layout];
  [CATransaction begin]; [CATransaction setDisableActions:YES];
  _gradient.frame = self.bounds;
  const CGFloat w = self.bounds.size.width, h = self.bounds.size.height;
  CGMutablePathRef p = CGPathCreateMutable();
  for (CGFloat x = 0; x <= w; x += 56) { CGPathMoveToPoint(p, nil, x, 0); CGPathAddLineToPoint(p, nil, x, h); }
  for (CGFloat y = 0; y <= h; y += 56) { CGPathMoveToPoint(p, nil, 0, y); CGPathAddLineToPoint(p, nil, w, y); }
  _grid.path = p; CGPathRelease(p); _grid.frame = self.bounds;
  const CGFloat d = MAX(w, h) * 0.9;
  _glow.bounds = CGRectMake(0, 0, d, d); _glow.position = CGPointMake(w * 0.85, h * 0.88);
  [CATransaction commit];
  if (![_glow animationForKey:@"drift"]) {
    CABasicAnimation* drift = [CABasicAnimation animationWithKeyPath:@"position"];
    drift.fromValue = [NSValue valueWithPoint:NSPointFromCGPoint(_glow.position)];
    drift.toValue = [NSValue valueWithPoint:NSMakePoint(w * 0.2, h * 0.65)];
    drift.duration = 16; drift.autoreverses = YES; drift.repeatCount = HUGE_VALF;
    drift.timingFunction = [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseInEaseOut];
    [_glow addAnimation:drift forKey:@"drift"];
  }
}
@end

// ---- a vertical stack whose children always span its full width (AppKit's width alignment only equalises them)
@interface MUColumn : NSStackView
@end
@implementation MUColumn
- (instancetype)init {
  self = [super init];
  self.orientation = NSUserInterfaceLayoutOrientationVertical; self.alignment = NSLayoutAttributeLeading; self.spacing = 10;
  return self;
}
- (void)addArrangedSubview:(NSView*)view {
  [super addArrangedSubview:view];
  [NSLayoutConstraint activateConstraints:@[[view.leadingAnchor constraintEqualToAnchor:self.leadingAnchor], [view.trailingAnchor constraintEqualToAnchor:self.trailingAnchor]]];
}
@end
@interface MUFlippedView : NSView
@end
@implementation MUFlippedView
- (BOOL)isFlipped { return YES; }
@end
API_AVAILABLE(macos(26.0))
@interface MUGlassContainer : NSGlassEffectContainerView
@end
@implementation MUGlassContainer
- (BOOL)isFlipped { return YES; }
@end

// ---- angled yellow section header (Melee menu bar)
@interface MUHeaderView : NSView
@end
@implementation MUHeaderView { CAShapeLayer* _shape; }
- (instancetype)initWithTitle:(NSString*)title symbol:(NSString*)name {
  self = [super initWithFrame:NSZeroRect];
  self.wantsLayer = YES; self.translatesAutoresizingMaskIntoConstraints = NO;
  [self.heightAnchor constraintEqualToConstant:28].active = YES;
  _shape = [CAShapeLayer layer]; _shape.fillColor = kYellow().CGColor;
  [self.layer addSublayer:_shape];
  NSImageView* icon = [NSImageView imageViewWithImage:symbol(name, 12, NSFontWeightBold)];
  icon.contentTintColor = kInk(); icon.translatesAutoresizingMaskIntoConstraints = NO;
  NSTextField* l = [NSTextField labelWithString:title];
  l.font = meleeFont(13); l.textColor = kInk(); l.translatesAutoresizingMaskIntoConstraints = NO;
  [self addSubview:icon]; [self addSubview:l];
  [NSLayoutConstraint activateConstraints:@[[icon.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:14], [icon.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
                                            [l.leadingAnchor constraintEqualToAnchor:icon.trailingAnchor constant:7], [l.centerYAnchor constraintEqualToAnchor:self.centerYAnchor]]];
  return self;
}
- (void)layout {
  [super layout];
  const CGFloat w = self.bounds.size.width, h = self.bounds.size.height;
  // Melee's angled bar covers 70% of the card and always reaches past its title, so the words never run off the yellow.
  CGFloat end = w * 0.7;
  for (NSView* sub in self.subviews) if ([sub isKindOfClass:NSTextField.class]) end = MAX(end, NSMaxX(sub.frame) + 12 + h * 0.5);
  end = MIN(end, w);
  CGMutablePathRef p = CGPathCreateMutable();
  CGPathMoveToPoint(p, nil, 0, 0); CGPathAddLineToPoint(p, nil, end, 0); CGPathAddLineToPoint(p, nil, end - 12, h); CGPathAddLineToPoint(p, nil, 0, h); CGPathCloseSubpath(p);
  [CATransaction begin]; [CATransaction setDisableActions:YES]; _shape.path = p; [CATransaction commit];
  CGPathRelease(p);
}
@end

@class MULauncherWindow;
@interface MUDropView : NSView
@property(nonatomic, weak) MULauncherWindow* owner;
@end

@interface MULauncherWindow : NSObject <NSWindowDelegate, NSTextFieldDelegate>
@property(nonatomic) host::LauncherSettings* settings;
@property(nonatomic) host::Dashboard dashboard;
@property(nonatomic) NSWindow* window;
@property(nonatomic) NSStackView* stack;
@property(nonatomic) NSScrollView* scroll;
@property(nonatomic) NSStackView* cardColumns; @property(nonatomic) NSLayoutConstraint* maxWidth; @property(nonatomic) NSArray<NSLayoutConstraint*>* stackedWidths;   // two columns of cards when wide
@property(nonatomic) id editor;   // the open controller editor sheet
@property(nonatomic) NSArray<NSView*>* entrance;
@property(nonatomic, copy) NSString* startupError;
@property(nonatomic) BOOL busy, closed;
@property(nonatomic) NSTimer* timer;
// hero
@property(nonatomic) NSTextField* playerChip; @property(nonatomic) NSBox* chipBox;
// steps
@property(nonatomic) NSView* stepsCard; @property(nonatomic) NSArray<NSTextField*>* stepLabels; @property(nonatomic) NSArray<NSImageView*>* stepIcons;
// disc
@property(nonatomic) NSTextField* discName; @property(nonatomic) NSTextField* discHint; @property(nonatomic) NSButton* playButton;
// account
@property(nonatomic) NSTextField* accountLabel; @property(nonatomic) NSTextField* emailField; @property(nonatomic) NSSecureTextField* passwordField; @property(nonatomic) NSButton* signInButton; @property(nonatomic) NSProgressIndicator* spinner; @property(nonatomic) NSButton* signOutButton; @property(nonatomic) NSStackView* signInRows;
// ranked / games
@property(nonatomic) NSView* rankedCard; @property(nonatomic) NSTextField* rankLabel; @property(nonatomic) NSTextField* ratingLabel; @property(nonatomic) NSTextField* recordLabel; @property(nonatomic) NSView* winTrack; @property(nonatomic) NSView* winBar; @property(nonatomic) NSLayoutConstraint* winBarWidth; @property(nonatomic) NSTextField* placementLabel; @property(nonatomic) NSTextField* mainsLabel;
@property(nonatomic) NSView* gamesCard; @property(nonatomic) NSStackView* gamesStack;
// controllers
@property(nonatomic) NSStackView* readinessStack; @property(nonatomic) std::string readinessSignature; @property(nonatomic) NSStackView* controllersStack; @property(nonatomic) NSUInteger controllerCount; @property(nonatomic) std::string controllerSignature; @property(nonatomic) unsigned tickCount; @property(nonatomic, copy) NSString* remapGuid; @property(nonatomic) int capturing; @property(nonatomic) BOOL armed; @property(nonatomic) NSArray<NSButton*>* remapButtons;
// display
@property(nonatomic) NSSwitch* discordSwitch; @property(nonatomic) NSSwitch* discordRankSwitch; @property(nonatomic) NSTextField* regionLabel;
@property(nonatomic) NSSegmentedControl* scaleControl; @property(nonatomic) NSSegmentedControl* anisoControl; @property(nonatomic) NSSegmentedControl* upscalerControl; @property(nonatomic) NSSwitch* vsyncSwitch; @property(nonatomic) NSSwitch* fullscreenSwitch; @property(nonatomic) NSSwitch* widescreenSwitch; @property(nonatomic) NSSlider* sharpness; @property(nonatomic) NSSwitch* onlineSwitch; @property(nonatomic) NSSegmentedControl* delayControl;
- (void)acceptDroppedDisc:(NSString*)path;
- (void)openEditor:(MUControllerEditor*)editor;
- (void)play;
- (void)signOut;
@end

// ---- Controller editor (sheet): a live GameCube controller, bindings, deadzones, trigger point and rumble; or the keyboard layout
@interface MUDiagramView : NSView
@property(nonatomic, copy) void (^onPick)(int part);
- (host::DiagramState&)state;
@end
@implementation MUDiagramView { host::DiagramState _state; }
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { return YES; }
- (host::DiagramState&)state { return _state; }
- (void)drawRect:(NSRect)dirty { host::gc_diagram_draw(NSGraphicsContext.currentContext.CGContext, self.bounds, _state); }
- (void)mouseDown:(NSEvent*)event {
  const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
  const int part = host::gc_diagram_hit(self.bounds, p, _state.directions);
  if (part != host::DP_NONE && self.onPick) self.onPick(part);
}
@end

// QA aid (MELEE_TEXT_AUDIT=1): logs every label, button and picker whose text is cut off or sits outside the window.
static void text_audit_view(NSView* v, NSWindow* w, const char* where, int& found) {
  if (v.hidden || v.alphaValue < 0.01) return;
  NSString* text = nil; BOOL clipped = NO;
  const NSSize b = v.bounds.size;
  if ([v isKindOfClass:NSTextField.class] && !((NSTextField*)v).editable) {
    NSTextField* t = (NSTextField*)v; text = t.stringValue;
    if (text.length && b.width > 1) {
      if (t.cell.wraps) clipped = [t.cell cellSizeForBounds:NSMakeRect(0, 0, b.width, CGFLOAT_MAX)].height > b.height + 1.5;
      else clipped = [t.cell cellSizeForBounds:NSMakeRect(0, 0, CGFLOAT_MAX, CGFLOAT_MAX)].width > b.width + 1.5;
    }
  } else if ([v isKindOfClass:NSButton.class] && ![v isKindOfClass:NSPopUpButton.class]) {
    NSButton* bt = (NSButton*)v; text = bt.title;
    if (text.length && b.width > 1) clipped = bt.intrinsicContentSize.width > b.width + 1.5;
  } else if ([v isKindOfClass:NSSegmentedControl.class]) {
    NSSegmentedControl* sc = (NSSegmentedControl*)v;
    text = sc.segmentCount ? [sc labelForSegment:0] : nil;
    if (b.width > 1) clipped = sc.intrinsicContentSize.width > b.width + 1.5;
  }
  if (text.length) {
    const NSRect r = [v convertRect:v.bounds toView:nil];
    const BOOL outside = NSMinX(r) < -1 || NSMaxX(r) > w.contentView.bounds.size.width + 1;
    BOOL crowded = NO;
    if ([v isKindOfClass:NSTextField.class]) {   // text running into the rounded ends of the capsule or box around it
      for (NSView* a = v.superview; a && a != w.contentView; a = a.superview) {
        CGFloat radius = a.layer.cornerRadius;
        if ([a isKindOfClass:NSBox.class] && ((NSBox*)a).boxType == NSBoxCustom) radius = MAX(radius, ((NSBox*)a).cornerRadius);
        if (radius <= 0) continue;
        const CGFloat inset = MAX(4.0, MIN(radius, a.bounds.size.height / 2) * 0.5);
        const NSRect safe = NSInsetRect([a convertRect:a.bounds toView:nil], inset, 0);
        const CGFloat tw = MIN(b.width, [((NSTextField*)v).cell cellSizeForBounds:NSMakeRect(0, 0, CGFLOAT_MAX, CGFLOAT_MAX)].width);
        const CGFloat tx = ((NSTextField*)v).alignment == NSTextAlignmentCenter ? NSMidX(r) - tw / 2 : NSMinX(r);
        crowded = tx < NSMinX(safe) - 0.5 || tx + tw > NSMaxX(safe) + 0.5;
        break;
      }
    }
    if (clipped || outside || crowded) {
      ++found;
      fprintf(stderr, "text-audit [%s] %s%s%s \"%s\" box %.0fx%.0f x %.0f..%.0f\n", where, clipped ? "clipped" : "", outside ? " outside-window" : "", crowded ? " touches-its-shape" : "",
              text.UTF8String, b.width, b.height, NSMinX(r), NSMaxX(r));
    }
  }
  for (NSView* s in v.subviews) text_audit_view(s, w, where, found);
}
static void text_audit(NSWindow* w, const char* where) {
  if (!w) return;
  int found = 0;
  [w.contentView layoutSubtreeIfNeeded];
  text_audit_view(w.contentView, w, where, found);
  fprintf(stderr, "text-audit [%s] done: %d problems, window %.0fx%.0f\n", where, found, w.contentView.bounds.size.width, w.contentView.bounds.size.height);
}
// Sheets need room: grow the dashboard window, within its screen, before attaching one that would not fit.
static void fit_window_for_sheet(NSWindow* parent, NSSize sheet) {
  if (parent.styleMask & NSWindowStyleMaskFullScreen) return;
  const NSRect screen = (parent.screen ?: NSScreen.mainScreen).visibleFrame;
  NSRect f = parent.frame;
  const CGFloat w = MAX(f.size.width, MIN(sheet.width + 48, screen.size.width)), h = MAX(f.size.height, MIN(sheet.height + 96, screen.size.height));
  if (w == f.size.width && h == f.size.height) return;
  NSRect g = NSMakeRect(f.origin.x - (w - f.size.width) / 2, f.origin.y - (h - f.size.height), w, h);
  g.origin.x = MAX(screen.origin.x, MIN(g.origin.x, NSMaxX(screen) - w)); g.origin.y = MAX(screen.origin.y, MIN(g.origin.y, NSMaxY(screen) - h));
  [parent setFrame:g display:YES animate:NO];   // animation does not run inside the dashboard's modal session
}
// "Connect a controller": pairing-mode steps per controller family, a shortcut to Bluetooth settings, and a live
// confirmation the moment a new controller shows up. Apple does not let apps pair Bluetooth controllers themselves.
@interface MUPairingSheet : NSObject
@property(nonatomic) NSWindow* sheet;
@property(nonatomic, copy) void (^completion)(void);
@property(nonatomic) NSTextField* steps; @property(nonatomic) NSImageView* guideIcon;
@property(nonatomic) NSTextField* status; @property(nonatomic) NSProgressIndicator* spinner; @property(nonatomic) NSImageView* check;
@property(nonatomic) NSTimer* timer;
@property(nonatomic) std::string baseline;
- (void)presentOn:(NSWindow*)parent;
@end

@implementation MUPairingSheet
static NSString* pairing_guids() {
  std::string s;
  for (const host::ControllerInfo& p : host::window_list_controllers()) s += p.guid + ";";
  return [NSString stringWithUTF8String:s.c_str()];
}
static NSView* pairing_step(int number, NSString* text) {
  NSStackView* row = [[NSStackView alloc] init]; row.orientation = NSUserInterfaceLayoutOrientationHorizontal; row.spacing = 10; row.alignment = NSLayoutAttributeFirstBaseline;
  NSTextField* n = label([NSString stringWithFormat:@"%d", number], 15, NSFontWeightHeavy, 1); n.textColor = kYellow();
  NSTextField* t = label(text, 15, NSFontWeightSemibold, 1);
  [row addArrangedSubview:n]; [row addArrangedSubview:t];
  return row;
}
static NSTextField* pairing_body(NSString* text) {
  NSTextField* f = [NSTextField wrappingLabelWithString:text];
  f.font = [NSFont systemFontOfSize:13]; f.textColor = [NSColor colorWithWhite:1 alpha:0.72]; f.preferredMaxLayoutWidth = 560;
  return f;
}
static NSButton* pairing_button(NSString* title, NSString* sym, id target, SEL action, BOOL prominent) {
  NSButton* b = [NSButton buttonWithTitle:title target:target action:action];
  b.image = symbol(sym, 12, NSFontWeightSemibold); b.imagePosition = NSImageLeading; b.controlSize = NSControlSizeLarge; b.bezelStyle = NSBezelStyleRounded;
  if (prominent) { b.bezelColor = kYellow(); b.contentTintColor = kInk(); }
  if (@available(macOS 26.0, *)) b.bezelStyle = NSBezelStyleGlass;
  return b;
}
- (instancetype)init {
  self = [super init];
  self.sheet = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 620, 560) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
  self.sheet.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
  self.sheet.backgroundColor = rgb(0.05, 0.06, 0.15);
  NSStackView* col = [[MUColumn alloc] init]; col.spacing = 12; col.translatesAutoresizingMaskIntoConstraints = NO;
  NSTextField* title = [NSTextField labelWithString:@"CONNECT A CONTROLLER"]; title.font = meleeFont(22); title.textColor = kYellow();
  [col addArrangedSubview:title];
  [col addArrangedSubview:pairing_body(@"A wireless controller pairs once. After that it connects by itself whenever you turn it on, and shows up in Dashdance within a second.")];
  [col setCustomSpacing:20 afterView:col.arrangedSubviews.lastObject];

  [col addArrangedSubview:pairing_step(1, @"Put the controller in pairing mode")];
  NSSegmentedControl* picker = [[NSSegmentedControl alloc] init];
  picker.segmentCount = host::kPairingGuideCount; picker.trackingMode = NSSegmentSwitchTrackingSelectOne; picker.controlSize = NSControlSizeLarge;
  for (int i = 0; i < host::kPairingGuideCount; ++i) [picker setLabel:[NSString stringWithUTF8String:host::kPairingGuides[i].name] forSegment:i];
  picker.selectedSegment = 0; picker.target = self; picker.action = @selector(guideChanged:);
  [col addArrangedSubview:picker];
  NSStackView* guide = [[NSStackView alloc] init]; guide.orientation = NSUserInterfaceLayoutOrientationHorizontal; guide.spacing = 12; guide.alignment = NSLayoutAttributeTop;
  self.guideIcon = [[NSImageView alloc] init]; self.guideIcon.contentTintColor = kYellow();
  [self.guideIcon.widthAnchor constraintEqualToConstant:30].active = YES;
  self.steps = pairing_body(@""); self.steps.preferredMaxLayoutWidth = 520;
  [guide addArrangedSubview:self.guideIcon]; [guide addArrangedSubview:self.steps];
  [col addArrangedSubview:guide];
  [col setCustomSpacing:20 afterView:guide];

  [col addArrangedSubview:pairing_step(2, @"Pick it in Bluetooth settings")];
  [col addArrangedSubview:pairing_body(@"It appears under Nearby Devices within a few seconds. Click Connect.")];
  [col addArrangedSubview:pairing_button(@"Open Bluetooth Settings", @"arrow.up.forward.app", self, @selector(openBluetooth), NO)];
  [col setCustomSpacing:20 afterView:col.arrangedSubviews.lastObject];

  [col addArrangedSubview:pairing_step(3, @"Play")];
  NSStackView* statusRow = [[NSStackView alloc] init]; statusRow.orientation = NSUserInterfaceLayoutOrientationHorizontal; statusRow.spacing = 10;
  self.spinner = [[NSProgressIndicator alloc] init]; self.spinner.style = NSProgressIndicatorStyleSpinning; self.spinner.controlSize = NSControlSizeSmall; [self.spinner startAnimation:nil];
  self.check = [NSImageView imageViewWithImage:symbol(@"checkmark.circle.fill", 18, NSFontWeightBold)]; self.check.contentTintColor = rgb(0.30, 0.85, 0.45); self.check.hidden = YES;
  self.status = label(@"Waiting for a controller…", 14, NSFontWeightMedium, 0.85);
  for (NSView* v in @[self.spinner, self.check, self.status]) [statusRow addArrangedSubview:v];
  [col addArrangedSubview:statusRow];
  [col setCustomSpacing:24 afterView:statusRow];
  [col addArrangedSubview:pairing_body(@"Using a cable? A USB controller works the moment you plug it in. GameCube controllers: plug a Wii U / Switch GameCube adapter (WUP-028, or a Mayflash in Wii U mode) into USB; Dashdance reads it directly at 1000 Hz.")];

  NSButton* done = pairing_button(@"Done", @"checkmark", self, @selector(close), YES); done.keyEquivalent = @"\r";
  done.translatesAutoresizingMaskIntoConstraints = NO;
  NSView* content = self.sheet.contentView;
  [content addSubview:col]; [content addSubview:done];
  [NSLayoutConstraint activateConstraints:@[
    [col.topAnchor constraintEqualToAnchor:content.topAnchor constant:26], [col.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:30],
    [col.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-30],
    [done.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-24], [done.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-20]]];
  [self guideChanged:picker];
  return self;
}
- (void)guideChanged:(NSSegmentedControl*)sender {
  const host::PairingGuide& g = host::kPairingGuides[MAX(0, MIN(host::kPairingGuideCount - 1, (int)sender.selectedSegment))];
  self.steps.stringValue = [NSString stringWithUTF8String:g.steps];
  self.guideIcon.image = symbol([NSString stringWithUTF8String:g.symbol], 22, NSFontWeightRegular) ?: symbol(@"gamecontroller", 22, NSFontWeightRegular);
}
- (void)openBluetooth {
  [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:@"x-apple.systempreferences:com.apple.BluetoothSettings"]];
}
- (void)presentOn:(NSWindow*)parent {
  self.baseline = pairing_guids().UTF8String;
  [GCController startWirelessControllerDiscoveryWithCompletionHandler:nil];   // MFi pads that support discovery pair without Settings
  self.timer = [NSTimer timerWithTimeInterval:0.5 target:self selector:@selector(tick) userInfo:nil repeats:YES];
  [NSRunLoop.mainRunLoop addTimer:self.timer forMode:NSRunLoopCommonModes];   // the dashboard runs modally
  fit_window_for_sheet(parent, self.sheet.frame.size);
  [parent beginSheet:self.sheet completionHandler:nil];
}
- (void)tick {
  for (const host::ControllerInfo& p : host::window_list_controllers()) {
    if (self.baseline.find(p.guid + ";") != std::string::npos) continue;
    self.baseline += p.guid + ";";
    [self.spinner stopAnimation:nil]; self.spinner.hidden = YES; self.check.hidden = NO;
    self.status.stringValue = [NSString stringWithFormat:@"%@ is connected and ready to play.", [NSString stringWithUTF8String:p.name.c_str()]];
    self.status.textColor = NSColor.whiteColor;
  }
}
- (void)close {
  [GCController stopWirelessControllerDiscovery];
  [self.timer invalidate]; self.timer = nil;
  if (self.sheet.sheetParent) [self.sheet.sheetParent endSheet:self.sheet];
  [self.sheet orderOut:nil];
  if (self.completion) self.completion();
}
@end

@interface MUControllerEditor : NSObject
@property(nonatomic) NSWindow* sheet;
@property(nonatomic, copy) NSString* guid;   // nil: the keyboard
@property(nonatomic, copy) void (^completion)(void);
@property(nonatomic) MUDiagramView* diagram;
@property(nonatomic) NSMutableArray<NSButton*>* rows;
@property(nonatomic) NSTextField* hint;
@property(nonatomic) NSSlider* stickSlider; @property(nonatomic) NSSlider* cstickSlider; @property(nonatomic) NSSlider* triggerSlider; @property(nonatomic) NSSlider* modifierSlider;
@property(nonatomic) NSTextField* stickValue; @property(nonatomic) NSTextField* cstickValue; @property(nonatomic) NSTextField* triggerValue; @property(nonatomic) NSTextField* modifierValue;
@property(nonatomic) NSSwitch* swapSwitch; @property(nonatomic) NSSwitch* rumbleSwitch;
@property(nonatomic) NSTimer* timer;
@property(nonatomic) id monitor;
@property(nonatomic) int capturing;
@property(nonatomic) BOOL armed; @property(nonatomic) BOOL sequence;
- (instancetype)initWithGuid:(NSString*)guid name:(NSString*)name;
- (void)presentOn:(NSWindow*)parent;
@end

@implementation MUControllerEditor { bool _held[512]; }
- (BOOL)isKeyboard { return self.guid == nil; }
- (int)controlCount { return self.isKeyboard ? host::KB_COUNT : host::GC_CTL_COUNT; }
- (host::ControllerConfig)config {
  host::ControllerConfig c;
  if (const host::ControllerConfig* e = host::controller_config_for(self.guid.UTF8String ?: "")) c = *e;
  c.guid = self.guid.UTF8String ?: "";
  return c;
}
- (NSSlider*)slider:(double)min max:(double)max value:(double)value {
  NSSlider* s = [NSSlider sliderWithValue:value minValue:min maxValue:max target:self action:@selector(slidersChanged)];
  s.continuous = YES; [s.widthAnchor constraintEqualToConstant:190].active = YES;
  return s;
}
- (NSTextField*)valueLabel {
  NSTextField* l = [NSTextField labelWithString:@""];
  l.font = [NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightMedium]; l.textColor = [NSColor colorWithWhite:1 alpha:0.75]; l.alignment = NSTextAlignmentRight;
  [l.widthAnchor constraintEqualToConstant:44].active = YES;
  return l;
}
- (NSStackView*)labeled:(NSString*)text control:(NSView*)control value:(NSView*)value {
  NSStackView* row = [[NSStackView alloc] init]; row.orientation = NSUserInterfaceLayoutOrientationHorizontal; row.spacing = 10; row.alignment = NSLayoutAttributeCenterY;
  NSTextField* l = [NSTextField labelWithString:text]; l.font = [NSFont systemFontOfSize:13]; l.textColor = NSColor.whiteColor;
  NSView* spacer = [[NSView alloc] init]; [spacer setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
  [row addArrangedSubview:l]; [row addArrangedSubview:spacer]; [row addArrangedSubview:control];
  if (value) [row addArrangedSubview:value];
  return row;
}
- (NSButton*)plainButton:(NSString*)title symbol:(NSString*)name action:(SEL)action {
  NSButton* b = [NSButton buttonWithTitle:title target:self action:action];
  b.image = symbol(name, 12, NSFontWeightSemibold); b.imagePosition = NSImageLeading; b.controlSize = NSControlSizeLarge; b.bezelStyle = NSBezelStyleRounded;
  if (@available(macOS 26.0, *)) b.bezelStyle = NSBezelStyleGlass;
  return b;
}
- (instancetype)initWithGuid:(NSString*)guid name:(NSString*)name {
  self = [super init];
  self.guid = guid; self.capturing = -1; std::memset(_held, 0, sizeof _held);
  const BOOL keyboard = guid == nil;
  const NSRect frame = NSMakeRect(0, 0, 920, 700);
  self.sheet = [[NSWindow alloc] initWithContentRect:frame styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
  self.sheet.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
  self.sheet.backgroundColor = rgb(0.05, 0.06, 0.15);
  NSView* content = self.sheet.contentView;

  NSStackView* left = [[MUColumn alloc] init]; left.spacing = 12; left.translatesAutoresizingMaskIntoConstraints = NO;
  NSTextField* title = [NSTextField labelWithString:keyboard ? @"KEYBOARD" : @"CONTROLLER"];
  title.font = meleeFont(22); title.textColor = kYellow();
  NSTextField* device = label(keyboard ? @"" : name, 12, NSFontWeightSemibold, 0.55);   // which pad this is, when several are connected
  device.hidden = keyboard;
  self.hint = label(@"", 12, NSFontWeightRegular, 0.7);
  self.diagram = [[MUDiagramView alloc] init];
  self.diagram.translatesAutoresizingMaskIntoConstraints = NO;
  [self.diagram.heightAnchor constraintEqualToAnchor:self.diagram.widthAnchor multiplier:1.0 / host::kDiagramAspect].active = YES;
  __weak MUControllerEditor* weakSelf = self;
  self.diagram.onPick = ^(int part) { [weakSelf startCapture:part sequence:NO]; };
  for (NSView* v in @[title, device, self.hint, self.diagram]) [left addArrangedSubview:v];
  if (keyboard) {
    self.modifierSlider = [self slider:20 max:90 value:host::keyboard_map().modifier_percent]; self.modifierValue = [self valueLabel];
    [left addArrangedSubview:[self labeled:@"Modifier stick amount" control:self.modifierSlider value:self.modifierValue]];
  } else {
    const host::ControllerMap m = [self config].map;
    self.stickSlider = [self slider:0 max:60 value:m.stick_deadzone]; self.stickValue = [self valueLabel];
    self.cstickSlider = [self slider:0 max:60 value:m.cstick_deadzone]; self.cstickValue = [self valueLabel];
    self.triggerSlider = [self slider:20 max:100 value:m.trigger_press]; self.triggerValue = [self valueLabel];
    [left addArrangedSubview:[self labeled:@"Stick deadzone" control:self.stickSlider value:self.stickValue]];
    [left addArrangedSubview:[self labeled:@"C-stick deadzone" control:self.cstickSlider value:self.cstickValue]];
    [left addArrangedSubview:[self labeled:@"Trigger press point" control:self.triggerSlider value:self.triggerValue]];
    self.swapSwitch = [[NSSwitch alloc] init]; self.swapSwitch.state = m.swap_sticks ? NSControlStateValueOn : NSControlStateValueOff; self.swapSwitch.target = self; self.swapSwitch.action = @selector(togglesChanged);
    self.rumbleSwitch = [[NSSwitch alloc] init]; self.rumbleSwitch.state = m.rumble ? NSControlStateValueOn : NSControlStateValueOff; self.rumbleSwitch.target = self; self.rumbleSwitch.action = @selector(togglesChanged);
    [left addArrangedSubview:[self labeled:@"Swap sticks" control:self.swapSwitch value:nil]];
    [left addArrangedSubview:[self labeled:@"Rumble" control:self.rumbleSwitch value:nil]];
  }
  NSStackView* buttons = [[NSStackView alloc] init]; buttons.orientation = NSUserInterfaceLayoutOrientationHorizontal; buttons.spacing = 8;
  [buttons addArrangedSubview:[self plainButton:keyboard ? @"Map all keys" : @"Map all buttons" symbol:@"list.number" action:@selector(mapAll)]];
  if (!keyboard) [buttons addArrangedSubview:[self plainButton:@"Test rumble" symbol:@"waveform" action:@selector(testRumble)]];
  [buttons addArrangedSubview:[self plainButton:@"Reset to defaults" symbol:@"arrow.counterclockwise" action:@selector(resetDefaults)]];
  [left addArrangedSubview:buttons];

  NSScrollView* scroll = [[NSScrollView alloc] init];
  scroll.drawsBackground = NO; scroll.hasVerticalScroller = YES; scroll.translatesAutoresizingMaskIntoConstraints = NO;
  NSView* doc = [[MUFlippedView alloc] init]; doc.translatesAutoresizingMaskIntoConstraints = NO;
  scroll.documentView = doc;
  NSStackView* list = [[MUColumn alloc] init]; list.spacing = 6; list.translatesAutoresizingMaskIntoConstraints = NO;
  [doc addSubview:list];
  self.rows = [NSMutableArray array];
  for (int i = 0; i < [self controlCount]; ++i) {
    NSButton* b = [NSButton buttonWithTitle:@"" target:self action:@selector(rowClicked:)];
    b.bezelStyle = NSBezelStyleRounded; b.controlSize = NSControlSizeLarge; b.tag = i; b.alignment = NSTextAlignmentLeft;
    if (@available(macOS 26.0, *)) b.bezelStyle = NSBezelStyleGlass;
    [list addArrangedSubview:b]; [self.rows addObject:b];
  }
  NSButton* done = [NSButton buttonWithTitle:@"Done" target:self action:@selector(close)];
  done.bezelStyle = NSBezelStyleRounded; done.controlSize = NSControlSizeLarge; done.bezelColor = kYellow(); done.contentTintColor = kInk();
  if (@available(macOS 26.0, *)) done.bezelStyle = NSBezelStyleGlass;
  done.translatesAutoresizingMaskIntoConstraints = NO;
  [content addSubview:left]; [content addSubview:scroll]; [content addSubview:done];
  [NSLayoutConstraint activateConstraints:@[
    [left.topAnchor constraintEqualToAnchor:content.topAnchor constant:22], [left.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:24],
    [left.widthAnchor constraintEqualToConstant:480],
    [scroll.topAnchor constraintEqualToAnchor:content.topAnchor constant:22], [scroll.leadingAnchor constraintEqualToAnchor:left.trailingAnchor constant:22],
    [scroll.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-24], [scroll.bottomAnchor constraintEqualToAnchor:done.topAnchor constant:-14],
    [done.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-24], [done.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-18],
    [doc.widthAnchor constraintEqualToAnchor:scroll.contentView.widthAnchor],
    [list.topAnchor constraintEqualToAnchor:doc.topAnchor], [list.bottomAnchor constraintEqualToAnchor:doc.bottomAnchor],
    [list.leadingAnchor constraintEqualToAnchor:doc.leadingAnchor], [list.trailingAnchor constraintEqualToAnchor:doc.trailingAnchor]]];
  [self refresh];
  return self;
}
- (void)presentOn:(NSWindow*)parent {
  __weak MUControllerEditor* weakSelf = self;
  self.monitor = [NSEvent addLocalMonitorForEventsMatchingMask:(NSEventMaskKeyDown | NSEventMaskKeyUp | NSEventMaskFlagsChanged)
                                                       handler:^NSEvent*(NSEvent* e) { MUControllerEditor* me = weakSelf; return me ? [me handleKey:e] : e; }];
  self.timer = [NSTimer timerWithTimeInterval:1.0 / 60.0 target:self selector:@selector(tick) userInfo:nil repeats:YES];
  [NSRunLoop.mainRunLoop addTimer:self.timer forMode:NSRunLoopCommonModes];   // the dashboard runs modally
  fit_window_for_sheet(parent, self.sheet.frame.size);
  [parent beginSheet:self.sheet completionHandler:nil];
}
- (NSEvent*)handleKey:(NSEvent*)e {
  if (e.window != self.sheet) return e;
  const int code = host::scancode_from_mac_keycode(e.keyCode);
  if (e.type == NSEventTypeFlagsChanged) {
    NSEventModifierFlags mask = 0;
    switch (e.keyCode) {
      case 56: case 60: mask = NSEventModifierFlagShift; break;
      case 59: case 62: mask = NSEventModifierFlagControl; break;
      case 58: case 61: mask = NSEventModifierFlagOption; break;
      case 55: case 54: mask = NSEventModifierFlagCommand; break;
      case 57: mask = NSEventModifierFlagCapsLock; break;
    }
    const bool down = mask && (e.modifierFlags & mask);
    if (code > 0 && code < 512) _held[code] = down;
    if (down && self.isKeyboard && self.capturing >= 0) [self assignKey:code];
    return e;
  }
  if (e.type == NSEventTypeKeyDown && e.keyCode == 53) {   // Escape: stop assigning, or close
    if (self.capturing >= 0) { self.capturing = -1; self.sequence = NO; [self refresh]; } else [self close];
    return nil;
  }
  if (!self.isKeyboard || (e.modifierFlags & NSEventModifierFlagCommand)) return e;
  if (e.type == NSEventTypeKeyDown) {
    if (e.isARepeat) return nil;
    if (code > 0 && code < 512) _held[code] = true;
    if (self.capturing >= 0) [self assignKey:code];
    return nil;   // keys belong to the layout while this sheet is up
  }
  if (code > 0 && code < 512) _held[code] = false;
  return nil;
}
- (void)assignKey:(int)code {
  if (code <= 0 || self.capturing < 0) return;
  host::KeyboardMap k = host::keyboard_map();
  for (int j = 0; j < host::KB_COUNT; ++j) if (j != self.capturing && k.key[j] == code) k.key[j] = 0;   // one key, one control
  k.key[self.capturing] = code;
  host::set_keyboard_map(k);
  [self advance];
}
- (void)advance {
  if (self.sequence && self.capturing + 1 < [self controlCount]) { self.capturing += 1; self.armed = NO; }
  else { self.capturing = -1; self.sequence = NO; }
  [self refresh];
}
- (void)tick {
  host::DiagramState& s = self.diagram.state;
  if (self.isKeyboard) host::gc_diagram_from_keyboard(s, host::keyboard_map(), _held, 512);
  else {
    const host::ControllerConfig cfg = [self config];
    host::ControllerLiveState live;
    if (host::window_controller_state(cfg.guid, live)) host::gc_diagram_from_pad(s, cfg.map, live);
    else { s.stick_deadzone = cfg.map.stick_deadzone / 100.0f; s.cstick_deadzone = cfg.map.cstick_deadzone / 100.0f; }
    if (self.capturing >= 0) {
      const int input = host::window_capture_input(cfg.guid);
      if (!self.armed) { if (input == host::kUnbound) self.armed = YES; }   // wait for the previous press to be released
      else if (input != host::kUnbound) {
        host::ControllerConfig c = cfg;
        for (int j = 0; j < host::GC_CTL_COUNT; ++j) if (j != self.capturing && c.map.binding[j] == input) c.map.binding[j] = host::kUnbound;
        c.map.binding[self.capturing] = input;
        host::upsert_controller_config(c);
        [self advance];
      }
    }
  }
  s.selected = self.capturing < host::DP_COUNT ? self.capturing : host::DP_NONE;
  s.pulse = 0.5f + 0.5f * (float)std::sin(CACurrentMediaTime() * 6.0);
  self.diagram.needsDisplay = YES;
}
- (void)refresh {
  const host::ControllerConfig cfg = [self config];
  const host::KeyboardMap& k = host::keyboard_map();
  for (NSButton* b in self.rows) {
    const int i = (int)b.tag;
    NSString* name = ns(self.isKeyboard ? host::kKeyboardControlNames[i] : host::kGcControlNames[i]);
    NSString* bound = self.capturing == i ? (self.isKeyboard ? @"Press a key…" : @"Press a button…")
                                          : ns(self.isKeyboard ? host::key_name(k.key[i]) : host::physical_input_name(cfg.map.binding[i]));
    NSMutableAttributedString* t = [[NSMutableAttributedString alloc] initWithString:name attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold], NSForegroundColorAttributeName: NSColor.whiteColor}];
    [t appendAttributedString:[[NSAttributedString alloc] initWithString:[@"    " stringByAppendingString:bound]
                                                              attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:13], NSForegroundColorAttributeName: self.capturing == i ? kYellow() : [NSColor colorWithWhite:1 alpha:0.6]}]];
    b.attributedTitle = t;
  }
  if (self.capturing >= 0) {
    NSString* name = ns(self.isKeyboard ? host::kKeyboardControlNames[self.capturing] : host::kGcControlNames[self.capturing]);
    self.hint.stringValue = [NSString stringWithFormat:@"%@ for %@. Escape stops.", self.isKeyboard ? @"Press the key" : @"Press the button", name];
  } else {
    self.hint.stringValue = @"Click a control on the picture or in the list, then press the input you want. Changes are saved as you go.";
  }
  if (self.isKeyboard) self.modifierValue.stringValue = [NSString stringWithFormat:@"%d%%", k.modifier_percent];
  else {
    self.stickValue.stringValue = [NSString stringWithFormat:@"%d%%", cfg.map.stick_deadzone];
    self.cstickValue.stringValue = [NSString stringWithFormat:@"%d%%", cfg.map.cstick_deadzone];
    self.triggerValue.stringValue = [NSString stringWithFormat:@"%d%%", cfg.map.trigger_press];
  }
}
- (void)slidersChanged {
  if (self.isKeyboard) {
    host::KeyboardMap k = host::keyboard_map(); k.modifier_percent = (int)std::lround(self.modifierSlider.doubleValue); host::set_keyboard_map(k);
  } else {
    host::ControllerConfig c = [self config];
    c.map.stick_deadzone = (int)std::lround(self.stickSlider.doubleValue);
    c.map.cstick_deadzone = (int)std::lround(self.cstickSlider.doubleValue);
    c.map.trigger_press = (int)std::lround(self.triggerSlider.doubleValue);
    host::upsert_controller_config(c);
  }
  [self refresh];
}
- (void)togglesChanged {
  host::ControllerConfig c = [self config];
  c.map.swap_sticks = self.swapSwitch.state == NSControlStateValueOn;
  c.map.rumble = self.rumbleSwitch.state == NSControlStateValueOn;
  host::upsert_controller_config(c);
}
- (void)testRumble { host::window_test_rumble(self.guid.UTF8String ?: ""); }
- (void)resetDefaults {
  if (self.isKeyboard) { host::set_keyboard_map(host::KeyboardMap::defaults()); self.modifierSlider.doubleValue = host::keyboard_map().modifier_percent; }
  else {
    host::ControllerConfig c = [self config]; c.map = host::ControllerMap::defaults(); host::upsert_controller_config(c);
    self.stickSlider.doubleValue = c.map.stick_deadzone; self.cstickSlider.doubleValue = c.map.cstick_deadzone; self.triggerSlider.doubleValue = c.map.trigger_press;
    self.swapSwitch.state = NSControlStateValueOff; self.rumbleSwitch.state = NSControlStateValueOn;
  }
  self.capturing = -1; self.sequence = NO;
  [self refresh];
}
- (void)mapAll { [self startCapture:0 sequence:YES]; }
- (void)rowClicked:(NSButton*)sender { [self startCapture:(int)sender.tag sequence:NO]; }
- (void)startCapture:(int)control sequence:(BOOL)sequence {
  if (control < 0 || control >= [self controlCount]) return;
  self.capturing = control; self.sequence = sequence; self.armed = NO;
  [self refresh];
}
- (void)close {
  [self.timer invalidate]; self.timer = nil;
  if (self.monitor) { [NSEvent removeMonitor:self.monitor]; self.monitor = nil; }
  if (self.sheet.sheetParent) [self.sheet.sheetParent endSheet:self.sheet];
  [self.sheet orderOut:nil];
  if (self.completion) self.completion();
}
@end

@implementation MUDropView
- (instancetype)initWithFrame:(NSRect)frame { self = [super initWithFrame:frame]; [self registerForDraggedTypes:@[NSPasteboardTypeFileURL]]; return self; }
- (BOOL)isFlipped { return YES; }
- (NSURL*)discURLIn:(id<NSDraggingInfo>)sender {
  NSArray* urls = [sender.draggingPasteboard readObjectsForClasses:@[NSURL.class] options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
  for (NSURL* url in urls) { NSString* ext = url.pathExtension.lowercaseString; if ([ext isEqualToString:@"iso"] || [ext isEqualToString:@"gcm"]) return url; }
  return nil;
}
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender { return [self discURLIn:sender] ? NSDragOperationCopy : NSDragOperationNone; }
- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
  NSURL* url = [self discURLIn:sender]; if (!url) return NO;
  [self.owner acceptDroppedDisc:[NSString stringWithUTF8String:url.fileSystemRepresentation]]; return YES;
}
@end

@implementation MULauncherWindow
- (instancetype)initWithSettings:(host::LauncherSettings*)settings error:(NSString*)error {
  self = [super init];
  self.settings = settings; self.startupError = error; self.capturing = -1;
  NSRect frame = NSMakeRect(0, 0, 720, 900);
  self.window = [[NSWindow alloc] initWithContentRect:frame styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable | NSWindowStyleMaskFullSizeContentView backing:NSBackingStoreBuffered defer:NO];
  self.window.title = @"Dashdance"; self.window.titlebarAppearsTransparent = YES; self.window.titleVisibility = NSWindowTitleHidden;
  self.window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
  self.window.backgroundColor = rgb(0.03, 0.03, 0.09); self.window.minSize = NSMakeSize(560, 520);
  self.window.delegate = self; self.window.releasedWhenClosed = NO;
  [self.window center];
  MUDropView* content = [[MUDropView alloc] initWithFrame:frame];
  content.owner = self; content.wantsLayer = YES;
  self.window.contentView = content;
  MUBackdropView* backdrop = [[MUBackdropView alloc] initWithFrame:frame];
  [content addSubview:backdrop];
  NSScrollView* scroll = [[NSScrollView alloc] init];
  self.scroll = scroll;
  scroll.translatesAutoresizingMaskIntoConstraints = NO; scroll.drawsBackground = NO; scroll.hasVerticalScroller = YES; scroll.automaticallyAdjustsContentInsets = NO;
  NSView* doc = [[MUFlippedView alloc] init]; doc.translatesAutoresizingMaskIntoConstraints = NO;   // the scroll document
  NSView* column = doc;                                                                               // where the cards live
  if (@available(macOS 26.0, *)) {
    // One glass container around the column: nearby glass cards merge and render as a batch.
    MUGlassContainer* container = [[MUGlassContainer alloc] init];
    container.translatesAutoresizingMaskIntoConstraints = NO; container.spacing = 12;
    NSView* inner = [[MUFlippedView alloc] init]; inner.translatesAutoresizingMaskIntoConstraints = NO;
    container.contentView = inner;
    [NSLayoutConstraint activateConstraints:@[[inner.topAnchor constraintEqualToAnchor:container.topAnchor], [inner.bottomAnchor constraintEqualToAnchor:container.bottomAnchor],
                                              [inner.leadingAnchor constraintEqualToAnchor:container.leadingAnchor], [inner.trailingAnchor constraintEqualToAnchor:container.trailingAnchor]]];
    doc = container; column = inner;
  }
  scroll.documentView = doc;
  [content addSubview:scroll];
  self.stack = [[MUColumn alloc] init];
  self.stack.spacing = 14; self.stack.translatesAutoresizingMaskIntoConstraints = NO;
  [column addSubview:self.stack];
  [NSLayoutConstraint activateConstraints:@[
    [backdrop.topAnchor constraintEqualToAnchor:content.topAnchor], [backdrop.bottomAnchor constraintEqualToAnchor:content.bottomAnchor], [backdrop.leadingAnchor constraintEqualToAnchor:content.leadingAnchor], [backdrop.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
    [scroll.topAnchor constraintEqualToAnchor:content.topAnchor], [scroll.bottomAnchor constraintEqualToAnchor:content.bottomAnchor], [scroll.leadingAnchor constraintEqualToAnchor:content.leadingAnchor], [scroll.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
    [doc.widthAnchor constraintEqualToAnchor:scroll.contentView.widthAnchor],
    [self.stack.topAnchor constraintEqualToAnchor:doc.topAnchor constant:40], [self.stack.bottomAnchor constraintEqualToAnchor:doc.bottomAnchor constant:-36],
    [self.stack.centerXAnchor constraintEqualToAnchor:doc.centerXAnchor]]];
  self.maxWidth = [self.stack.widthAnchor constraintLessThanOrEqualToConstant:620]; self.maxWidth.active = YES;
  NSLayoutConstraint* width = [self.stack.widthAnchor constraintEqualToAnchor:doc.widthAnchor constant:-48]; width.priority = NSLayoutPriorityDefaultHigh; width.active = YES;

  NSView* hero = [self buildHero];
  self.stepsCard = [self buildSteps];
  self.rankedCard = [self buildRanked];
  self.gamesCard = [self buildGames];
  NSView* account = [self buildAccount];
  NSView* disc = [self buildDisc];
  NSView* controllers = [self buildControllers];
  NSView* readiness = [self buildReadiness];
  NSView* display = [self buildDisplay];
  NSView* discordCard = [self buildDiscord];
  NSView* regionCard = [self buildRegion];
  self.playButton = [NSButton buttonWithTitle:@"  PLAY  " target:self action:@selector(play)];
  self.playButton.bezelStyle = NSBezelStyleRounded; self.playButton.controlSize = NSControlSizeLarge; self.playButton.keyEquivalent = @"\r";
  if (@available(macOS 26.0, *)) self.playButton.bezelStyle = NSBezelStyleGlass;
  self.playButton.bezelColor = kYellow(); self.playButton.font = meleeFont(18); self.playButton.contentTintColor = kInk();
  self.playButton.image = symbol(@"play.fill", 15, NSFontWeightBold); self.playButton.imagePosition = NSImageLeading;
  [self.playButton.heightAnchor constraintEqualToConstant:44].active = YES;
  NSTextField* footer = label(@"Needs your own Super Smash Bros. Melee NTSC 1.02 disc image. Nothing from the game ships with the app. Unofficial; not affiliated with the Slippi team or Nintendo.", 11, NSFontWeightRegular, 0.45);
  footer.alignment = NSTextAlignmentCenter;
  NSStackView* leftCards = [[MUColumn alloc] init]; leftCards.spacing = 14;
  NSStackView* rightCards = [[MUColumn alloc] init]; rightCards.spacing = 14;
  for (NSView* v in @[self.stepsCard, self.rankedCard, self.gamesCard, account]) [leftCards addArrangedSubview:v];                 // you
  for (NSView* v in @[readiness, disc, controllers, display, regionCard, discordCard]) [rightCards addArrangedSubview:v];                    // the setup
  self.cardColumns = [NSStackView stackViewWithViews:@[leftCards, rightCards]];
  self.cardColumns.orientation = NSUserInterfaceLayoutOrientationVertical; self.cardColumns.alignment = NSLayoutAttributeLeading; self.cardColumns.spacing = 14;
  self.stackedWidths = @[[leftCards.widthAnchor constraintEqualToAnchor:self.cardColumns.widthAnchor], [rightCards.widthAnchor constraintEqualToAnchor:self.cardColumns.widthAnchor]];
  [NSLayoutConstraint activateConstraints:self.stackedWidths];
  for (NSView* v in @[hero, self.cardColumns, self.playButton, footer]) [self.stack addArrangedSubview:v];
  [self.stack setCustomSpacing:26 afterView:hero];
  self.entrance = @[hero, self.stepsCard, self.rankedCard, self.gamesCard, account, disc, controllers, display, regionCard, discordCard, self.playButton];
  for (NSView* v in self.entrance) v.alphaValue = 0;
  [self refreshDisc]; [self refreshAccount]; [self refreshControllers]; [self refreshSteps]; [self refreshReadiness];
  [self updateColumns];
  [self loadDashboard];
  self.timer = [NSTimer timerWithTimeInterval:0.03 target:self selector:@selector(tick) userInfo:nil repeats:YES];
  [NSRunLoop.mainRunLoop addTimer:self.timer forMode:NSModalPanelRunLoopMode];   // the launcher runs modally
  [NSRunLoop.mainRunLoop addTimer:self.timer forMode:NSDefaultRunLoopMode];
  return self;
}
- (void)animateIn {
  self.window.contentView.wantsLayer = YES;
  NSTimeInterval delay = 0.05;
  for (NSView* v in self.entrance) {
    v.wantsLayer = YES;
    CABasicAnimation* rise = [CABasicAnimation animationWithKeyPath:@"transform.translation.y"];
    rise.fromValue = @(-22); rise.toValue = @0; rise.duration = 0.7; rise.beginTime = CACurrentMediaTime() + delay;
    rise.timingFunction = [CAMediaTimingFunction functionWithControlPoints:0.2 :0.9 :0.25 :1.0]; rise.fillMode = kCAFillModeBackwards;
    CABasicAnimation* fade = [CABasicAnimation animationWithKeyPath:@"opacity"];
    fade.fromValue = @0; fade.toValue = @1; fade.duration = 0.5; fade.beginTime = rise.beginTime; fade.fillMode = kCAFillModeBackwards;
    v.alphaValue = 1;
    [v.layer addAnimation:rise forKey:@"rise"]; [v.layer addAnimation:fade forKey:@"fade"];
    delay += 0.06;
  }
}

// ---- building blocks
// A card: Liquid Glass on macOS 26 (tinted toward the backdrop's blue), a translucent box otherwise.
- (NSView*)card { return [self cardTinted:kGlassTint()]; }
- (NSView*)cardTinted:(NSColor*)tint {
  if (@available(macOS 26.0, *)) {
    NSGlassEffectView* g = [[NSGlassEffectView alloc] init];
    g.cornerRadius = 20; g.tintColor = tint; g.style = NSGlassEffectViewStyleRegular;
    NSView* host = [[NSView alloc] init];
    g.contentView = host;
    return g;
  }
  NSBox* box = [[NSBox alloc] init];
  box.boxType = NSBoxCustom; box.cornerRadius = 16; box.borderWidth = 1;
  box.borderColor = rgb(0.5, 0.6, 1.0, 0.16); box.fillColor = rgb(0.06, 0.07, 0.16, 0.82); box.contentViewMargins = NSMakeSize(0, 0);
  return box;
}
- (NSView*)hostOf:(NSView*)card {
  if ([card isKindOfClass:NSBox.class]) return ((NSBox*)card).contentView;
  if (@available(macOS 26.0, *)) { if ([card isKindOfClass:NSGlassEffectView.class]) return ((NSGlassEffectView*)card).contentView; }
  return card;
}
- (NSStackView*)stackIn:(NSView*)card header:(NSString*)title symbol:(NSString*)name {
  NSView* host = [self hostOf:card];
  NSStackView* s = [[MUColumn alloc] init];
  s.translatesAutoresizingMaskIntoConstraints = NO;
  [host addSubview:s];
  [NSLayoutConstraint activateConstraints:@[[s.topAnchor constraintEqualToAnchor:host.topAnchor constant:14], [s.bottomAnchor constraintEqualToAnchor:host.bottomAnchor constant:-16],
                                            [s.leadingAnchor constraintEqualToAnchor:host.leadingAnchor constant:16], [s.trailingAnchor constraintEqualToAnchor:host.trailingAnchor constant:-16]]];
  if (host != card) {   // the glass view sizes itself around its content view
    host.translatesAutoresizingMaskIntoConstraints = NO;
    [NSLayoutConstraint activateConstraints:@[[host.topAnchor constraintEqualToAnchor:card.topAnchor], [host.bottomAnchor constraintEqualToAnchor:card.bottomAnchor],
                                              [host.leadingAnchor constraintEqualToAnchor:card.leadingAnchor], [host.trailingAnchor constraintEqualToAnchor:card.trailingAnchor]]];
  }
  [s addArrangedSubview:[[MUHeaderView alloc] initWithTitle:title symbol:name]];
  return s;
}
// A slider with its value shown next to it (formatted by `format`, value scaled by `scale`).
- (NSStackView*)sliderRow:(NSString*)text symbol:(NSString*)name slider:(NSSlider*)slider format:(NSString*)format scale:(double)scale {
  NSTextField* value = [NSTextField labelWithString:@""];
  value.font = [NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightMedium]; value.textColor = [NSColor colorWithWhite:1 alpha:0.7]; value.alignment = NSTextAlignmentRight;
  [value.widthAnchor constraintEqualToConstant:46].active = YES;
  value.stringValue = [NSString stringWithFormat:format, slider.doubleValue * scale];
  objc_setAssociatedObject(slider, kSliderLabelKey, value, OBJC_ASSOCIATION_RETAIN); objc_setAssociatedObject(slider, kSliderFormatKey, format, OBJC_ASSOCIATION_COPY); objc_setAssociatedObject(slider, kSliderScaleKey, @(scale), OBJC_ASSOCIATION_RETAIN);
  slider.target = self; slider.action = @selector(sliderChanged:); slider.continuous = YES;
  NSStackView* pair = [[NSStackView alloc] init]; pair.orientation = NSUserInterfaceLayoutOrientationHorizontal; pair.spacing = 8;
  [pair addArrangedSubview:slider]; [pair addArrangedSubview:value];
  [slider.widthAnchor constraintEqualToConstant:150].active = YES;
  return [self row:text symbol:name control:pair];
}
- (void)sliderChanged:(NSSlider*)slider {
  NSTextField* value = objc_getAssociatedObject(slider, kSliderLabelKey); NSString* format = objc_getAssociatedObject(slider, kSliderFormatKey); NSNumber* scale = objc_getAssociatedObject(slider, kSliderScaleKey);
  if (value && format) value.stringValue = [NSString stringWithFormat:format, slider.doubleValue * scale.doubleValue];
}
- (NSStackView*)row:(NSString*)text symbol:(NSString*)name control:(NSView*)control {
  NSStackView* row = [[NSStackView alloc] init];
  row.orientation = NSUserInterfaceLayoutOrientationHorizontal; row.spacing = 10; row.alignment = NSLayoutAttributeCenterY;
  NSImageView* icon = [NSImageView imageViewWithImage:symbol(name, 13, NSFontWeightMedium)];
  icon.contentTintColor = kYellow(); [icon.widthAnchor constraintEqualToConstant:20].active = YES;
  NSTextField* l = [NSTextField labelWithString:text]; l.font = [NSFont systemFontOfSize:13]; l.textColor = NSColor.whiteColor;
  NSView* spacer = [[NSView alloc] init]; [spacer setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
  [row addArrangedSubview:icon]; [row addArrangedSubview:l]; [row addArrangedSubview:spacer]; [row addArrangedSubview:control];
  if ([control isKindOfClass:NSSlider.class]) [control.widthAnchor constraintEqualToConstant:180].active = YES;
  return row;
}
// Buttons are glass on macOS 26, rounded otherwise.
- (NSButton*)button:(NSString*)title symbol:(NSString*)name action:(SEL)action {
  NSButton* b = [NSButton buttonWithTitle:title target:self action:action];
  b.image = symbol(name, 12, NSFontWeightSemibold); b.imagePosition = NSImageLeading; b.controlSize = NSControlSizeLarge;
  b.bezelStyle = NSBezelStyleRounded;
  if (@available(macOS 26.0, *)) b.bezelStyle = NSBezelStyleGlass;
  return b;
}
- (NSSwitch*)toggle:(BOOL)on { NSSwitch* s = [[NSSwitch alloc] init]; s.state = on ? NSControlStateValueOn : NSControlStateValueOff; s.controlSize = NSControlSizeSmall; return s; }
- (NSSegmentedControl*)segments:(NSArray<NSString*>*)items selected:(NSInteger)index {
  NSSegmentedControl* s = [NSSegmentedControl segmentedControlWithLabels:items trackingMode:NSSegmentSwitchTrackingSelectOne target:nil action:nil];
  s.selectedSegment = index; s.controlSize = NSControlSizeRegular;
  return s;
}

// ---- sections
- (NSView*)buildHero {
  NSStackView* hero = [[NSStackView alloc] init];
  hero.orientation = NSUserInterfaceLayoutOrientationVertical; hero.alignment = NSLayoutAttributeCenterX; hero.spacing = 6;
  if (NSImage* mark = slippi_mark()) {
    const CGFloat size = 124;
    NSView* disc;
    NSImageView* iv = [NSImageView imageViewWithImage:mark];
    iv.contentTintColor = NSColor.whiteColor; iv.imageScaling = NSImageScaleProportionallyUpOrDown; iv.translatesAutoresizingMaskIntoConstraints = NO;
    if (@available(macOS 26.0, *)) {
      NSGlassEffectView* g = [[NSGlassEffectView alloc] init];
      g.cornerRadius = size / 2; g.tintColor = [kMarkViolet() colorWithAlphaComponent:0.22];
      NSView* host = [[NSView alloc] init]; g.contentView = host; disc = g;
      host.translatesAutoresizingMaskIntoConstraints = NO;
      [NSLayoutConstraint activateConstraints:@[[host.topAnchor constraintEqualToAnchor:g.topAnchor], [host.bottomAnchor constraintEqualToAnchor:g.bottomAnchor], [host.leadingAnchor constraintEqualToAnchor:g.leadingAnchor], [host.trailingAnchor constraintEqualToAnchor:g.trailingAnchor]]];
      [host addSubview:iv];
    } else {
      NSBox* box = [[NSBox alloc] init]; box.boxType = NSBoxCustom; box.cornerRadius = size / 2; box.borderWidth = 1; box.borderColor = [NSColor colorWithWhite:1 alpha:0.25]; box.fillColor = [kMarkViolet() colorWithAlphaComponent:0.35]; box.contentViewMargins = NSMakeSize(0, 0);
      [box.contentView addSubview:iv]; disc = box;
    }
    disc.translatesAutoresizingMaskIntoConstraints = NO;
    [NSLayoutConstraint activateConstraints:@[[disc.widthAnchor constraintEqualToConstant:size], [disc.heightAnchor constraintEqualToConstant:size],
                                              [iv.centerXAnchor constraintEqualToAnchor:disc.centerXAnchor], [iv.centerYAnchor constraintEqualToAnchor:disc.centerYAnchor],
                                              [iv.widthAnchor constraintEqualToConstant:80], [iv.heightAnchor constraintEqualToConstant:80]]];
    iv.wantsLayer = YES;
    CABasicAnimation* breathe = [CABasicAnimation animationWithKeyPath:@"transform.scale"];
    breathe.fromValue = @1.0; breathe.toValue = @1.05; breathe.duration = 2.4; breathe.autoreverses = YES; breathe.repeatCount = HUGE_VALF;
    breathe.timingFunction = [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseInEaseOut];
    [iv.layer addAnimation:breathe forKey:@"breathe"];
    [hero addArrangedSubview:disc];
  } else {
    [hero addArrangedSubview:[[MUHeroView alloc] initWithSize:110]];
  }
  NSTextField* title = [NSTextField labelWithString:@"Dashdance"];
  title.font = meleeFont(46); title.textColor = NSColor.whiteColor;
  title.wantsLayer = YES; title.layer.shadowColor = kYellow().CGColor; title.layer.shadowOpacity = 0.5; title.layer.shadowRadius = 14; title.layer.shadowOffset = CGSizeZero;
  NSTextField* sub = [NSTextField labelWithString:@"SUPER SMASH BROS. MELEE  ·  SLIPPI ONLINE  ·  NATIVE"];
  sub.font = [NSFont systemFontOfSize:11 weight:NSFontWeightSemibold]; sub.textColor = [NSColor colorWithWhite:1 alpha:0.6];
  self.chipBox = [[NSBox alloc] init]; self.chipBox.boxType = NSBoxCustom; self.chipBox.cornerRadius = 13; self.chipBox.borderWidth = 0; self.chipBox.fillColor = kYellow(); self.chipBox.contentViewMargins = NSMakeSize(0, 0);
  self.playerChip = [NSTextField labelWithString:@""]; self.playerChip.font = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold]; self.playerChip.textColor = kInk(); self.playerChip.translatesAutoresizingMaskIntoConstraints = NO;
  [self.chipBox.contentView addSubview:self.playerChip];
  [NSLayoutConstraint activateConstraints:@[[self.playerChip.topAnchor constraintEqualToAnchor:self.chipBox.contentView.topAnchor constant:5], [self.playerChip.bottomAnchor constraintEqualToAnchor:self.chipBox.contentView.bottomAnchor constant:-5],
                                            [self.playerChip.leadingAnchor constraintEqualToAnchor:self.chipBox.contentView.leadingAnchor constant:12], [self.playerChip.trailingAnchor constraintEqualToAnchor:self.chipBox.contentView.trailingAnchor constant:-12]]];
  self.chipBox.hidden = YES;
  [hero addArrangedSubview:title]; [hero addArrangedSubview:sub]; [hero addArrangedSubview:self.chipBox];
  [hero setCustomSpacing:12 afterView:sub];
  return hero;
}
- (NSView*)buildSteps {
  NSView* card = [self card];
  NSStackView* s = [self stackIn:card header:@"GET STARTED" symbol:@"flag.checkered"];
  NSArray* names = @[@"Choose your Melee disc image", @"Sign in to Slippi Online", @"Connect a controller (GameCube adapter, Bluetooth or USB pad, or keyboard)", @"Press Play"];
  NSMutableArray* labels = [NSMutableArray array]; NSMutableArray* icons = [NSMutableArray array];
  for (NSString* n in names) {
    NSStackView* row = [[NSStackView alloc] init]; row.orientation = NSUserInterfaceLayoutOrientationHorizontal; row.spacing = 10; row.alignment = NSLayoutAttributeCenterY;
    NSImageView* icon = [NSImageView imageViewWithImage:symbol(@"circle", 15, NSFontWeightSemibold)];
    icon.contentTintColor = [NSColor colorWithWhite:1 alpha:0.4]; [icon.widthAnchor constraintEqualToConstant:20].active = YES;
    NSTextField* l = label(n, 13, NSFontWeightRegular, 0.9);
    [row addArrangedSubview:icon]; [row addArrangedSubview:l]; [s addArrangedSubview:row];
    [labels addObject:l]; [icons addObject:icon];
  }
  self.stepLabels = labels; self.stepIcons = icons;
  return card;
}
- (NSView*)buildRanked {
  NSView* card = [self cardTinted:rgb(0.97, 0.79, 0.28, 0.12)];
  if ([card isKindOfClass:NSBox.class]) ((NSBox*)card).borderColor = rgb(0.97, 0.79, 0.28, 0.4);
  NSStackView* s = [self stackIn:card header:@"RANKED" symbol:@"trophy"];
  NSStackView* top = [[NSStackView alloc] init]; top.orientation = NSUserInterfaceLayoutOrientationHorizontal; top.alignment = NSLayoutAttributeFirstBaseline;
  self.rankLabel = [NSTextField labelWithString:@""]; self.rankLabel.font = meleeFont(30); self.rankLabel.textColor = kYellow();
  self.ratingLabel = [NSTextField labelWithString:@""]; self.ratingLabel.font = [NSFont monospacedDigitSystemFontOfSize:20 weight:NSFontWeightSemibold]; self.ratingLabel.textColor = NSColor.whiteColor;
  NSView* spacer = [[NSView alloc] init]; [spacer setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
  [top addArrangedSubview:self.rankLabel]; [top addArrangedSubview:spacer]; [top addArrangedSubview:self.ratingLabel];
  self.recordLabel = label(@"", 13, NSFontWeightMedium, 0.9);
  self.winTrack = [[NSView alloc] init]; self.winTrack.wantsLayer = YES; self.winTrack.layer.backgroundColor = [NSColor colorWithWhite:1 alpha:0.12].CGColor; self.winTrack.layer.cornerRadius = 4; self.winTrack.layer.masksToBounds = YES;
  [self.winTrack.heightAnchor constraintEqualToConstant:8].active = YES;
  self.winBar = [[NSView alloc] init]; self.winBar.wantsLayer = YES; self.winBar.layer.backgroundColor = kGreen().CGColor; self.winBar.translatesAutoresizingMaskIntoConstraints = NO;
  [self.winTrack addSubview:self.winBar];
  self.winBarWidth = [self.winBar.widthAnchor constraintEqualToAnchor:self.winTrack.widthAnchor multiplier:0.0];
  [NSLayoutConstraint activateConstraints:@[[self.winBar.leadingAnchor constraintEqualToAnchor:self.winTrack.leadingAnchor], [self.winBar.topAnchor constraintEqualToAnchor:self.winTrack.topAnchor], [self.winBar.bottomAnchor constraintEqualToAnchor:self.winTrack.bottomAnchor], self.winBarWidth]];
  self.placementLabel = label(@"", 12, NSFontWeightRegular, 0.7);
  self.mainsLabel = label(@"", 12, NSFontWeightRegular, 0.85);
  for (NSView* v in @[top, self.recordLabel, self.winTrack, self.placementLabel, self.mainsLabel]) [s addArrangedSubview:v];
  return card;
}
- (NSView*)buildGames {
  NSView* card = [self card];
  NSStackView* s = [self stackIn:card header:@"RECENT GAMES" symbol:@"clock.arrow.circlepath"];
  self.gamesStack = [[MUColumn alloc] init]; self.gamesStack.spacing = 7;
  [s addArrangedSubview:self.gamesStack];
  return card;
}
- (NSView*)buildAccount {
  NSView* card = [self card];
  NSStackView* s = [self stackIn:card header:@"SLIPPI ONLINE ACCOUNT" symbol:@"person.crop.circle"];
  self.accountLabel = label(@"", 13, NSFontWeightRegular, 1);
  [s addArrangedSubview:self.accountLabel];
  self.signInRows = [[MUColumn alloc] init]; self.signInRows.spacing = 8;
  self.emailField = [[NSTextField alloc] init]; self.emailField.placeholderString = @"Email"; self.emailField.controlSize = NSControlSizeLarge; self.emailField.bezelStyle = NSTextFieldRoundedBezel; self.emailField.delegate = self;
  self.passwordField = [[NSSecureTextField alloc] init]; self.passwordField.placeholderString = @"Password"; self.passwordField.controlSize = NSControlSizeLarge; self.passwordField.bezelStyle = NSTextFieldRoundedBezel; self.passwordField.delegate = self;
  NSStackView* buttons = [[NSStackView alloc] init]; buttons.orientation = NSUserInterfaceLayoutOrientationHorizontal; buttons.spacing = 8;
  self.signInButton = [self button:@"Sign In" symbol:@"person.badge.key" action:@selector(signIn)];
  self.spinner = [[NSProgressIndicator alloc] init]; self.spinner.style = NSProgressIndicatorStyleSpinning; self.spinner.controlSize = NSControlSizeSmall; self.spinner.displayedWhenStopped = NO;
  NSButton* reset = [NSButton buttonWithTitle:@"Forgot password" target:self action:@selector(forgotPassword)]; reset.bezelStyle = NSBezelStyleInline; reset.controlSize = NSControlSizeSmall;
  NSButton* site = [NSButton buttonWithTitle:@"Create an account at slippi.gg" target:self action:@selector(openSlippi)]; site.bezelStyle = NSBezelStyleInline; site.controlSize = NSControlSizeSmall;
  [buttons addArrangedSubview:self.signInButton]; [buttons addArrangedSubview:self.spinner]; [buttons addArrangedSubview:reset]; [buttons addArrangedSubview:site];
  for (NSView* v in @[self.emailField, self.passwordField, buttons]) [self.signInRows addArrangedSubview:v];
  [s addArrangedSubview:self.signInRows];
  self.signOutButton = [self button:@"Sign Out" symbol:@"rectangle.portrait.and.arrow.right" action:@selector(signOut)];
  NSStackView* out = [[NSStackView alloc] init]; out.orientation = NSUserInterfaceLayoutOrientationHorizontal; [out addArrangedSubview:self.signOutButton];
  [s addArrangedSubview:out];
  return card;
}
- (NSView*)buildDisc {
  NSView* card = [self card];
  NSStackView* s = [self stackIn:card header:@"GAME DISC" symbol:@"opticaldisc"];
  self.discName = label(@"", 16, NSFontWeightSemibold, 1);
  self.discHint = label(@"", 12, NSFontWeightRegular, 0.6);
  NSStackView* buttons = [[NSStackView alloc] init]; buttons.orientation = NSUserInterfaceLayoutOrientationHorizontal;
  [buttons addArrangedSubview:[self button:@"Choose Disc Image…" symbol:@"folder" action:@selector(chooseDisc)]];
  [s addArrangedSubview:self.discName]; [s addArrangedSubview:self.discHint]; [s addArrangedSubview:buttons];
  return card;
}
- (NSView*)buildControllers {
  NSView* card = [self card];
  NSStackView* s = [self stackIn:card header:@"CONTROLLERS" symbol:@"gamecontroller"];
  self.controllersStack = [[MUColumn alloc] init]; self.controllersStack.spacing = 8;
  [s addArrangedSubview:self.controllersStack];
  [s addArrangedSubview:[self button:@"Connect a Controller…" symbol:@"dot.radiowaves.left.and.right" action:@selector(connectController)]];
  [s addArrangedSubview:label(@"Connect a Controller walks you through pairing a PlayStation, Xbox, Switch Pro or other Bluetooth controller. A Wii U / Switch GameCube adapter (WUP-028, or a Mayflash in Wii U mode) is read directly over USB and asked to poll at 1000 Hz; the rate shown is what your port actually delivers. Configure opens a live view of the controller: remap buttons, set stick deadzones, the trigger press point and rumble. The keyboard layout is configured the same way.", 11, NSFontWeightRegular, 0.6)];
  return card;
}
- (NSView*)buildReadiness {
  NSView* card = [self card];
  NSStackView* s = [self stackIn:card header:@"READY TO COMPETE" symbol:@"trophy"];
  self.readinessStack = [[MUColumn alloc] init]; self.readinessStack.spacing = 7;
  [s addArrangedSubview:self.readinessStack];
  return card;
}
// What in this setup costs latency, checked about once a second (display, full screen, network, controller, delay).
- (void)refreshReadiness {
  if (!self.readinessStack) return;
  const int delay = self.delayControl ? [self selectedDelay] : self.settings->online_delay;
  const bool fullscreen = self.fullscreenSwitch ? self.fullscreenSwitch.state == NSControlStateValueOn : self.settings->fullscreen;
  const std::vector<host::ReadinessItem> items = host::competitive_readiness(self.settings->display_hz, fullscreen, delay);
  std::string sig;
  for (const host::ReadinessItem& i : items) sig += (i.ok ? "1" : "0") + i.text + ";";
  if (sig == self.readinessSignature) return;
  self.readinessSignature = sig;
  for (NSView* v in self.readinessStack.arrangedSubviews) [v removeFromSuperview];
  for (const host::ReadinessItem& i : items) {
    NSStackView* row = [[NSStackView alloc] init]; row.orientation = NSUserInterfaceLayoutOrientationHorizontal; row.spacing = 8; row.alignment = NSLayoutAttributeFirstBaseline;
    NSImageView* icon = [NSImageView imageViewWithImage:symbol(i.ok ? @"checkmark.circle.fill" : @"exclamationmark.triangle.fill", 12, NSFontWeightSemibold)];
    icon.contentTintColor = i.ok ? rgb(0.30, 0.85, 0.45) : kYellow(); [icon.widthAnchor constraintEqualToConstant:18].active = YES;
    NSTextField* text = [NSTextField wrappingLabelWithString:ns(i.text)];
    text.font = [NSFont systemFontOfSize:12]; text.textColor = [NSColor colorWithWhite:1 alpha:i.ok ? 0.75 : 0.95]; text.preferredMaxLayoutWidth = 520;
    [text setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    [row addArrangedSubview:icon]; [row addArrangedSubview:text];
    [self.readinessStack addArrangedSubview:row];
  }
}
- (NSView*)buildDisplay {
  NSView* card = [self card];
  NSStackView* s = [self stackIn:card header:@"DISPLAY & PERFORMANCE" symbol:@"speedometer"];
  id<MTLDevice> gpu = MTLCreateSystemDefaultDevice();
  NSScreen* screen = NSScreen.mainScreen;
  NSString* info = [NSString stringWithFormat:@"%@  ·  %d Hz display%@  ·  thermal %s  ·  60 Hz simulation, each frame shown on the next refresh", gpu ? gpu.name : @"Metal", display_max_hz(screen), display_max_hz(screen) > 60 ? @" (ProMotion)" : @"", host::thermal_state_name()];
  [s addArrangedSubview:label(info, 12, NSFontWeightRegular, 0.7)];
  const int scales[] = {0, 1, 2, 3, 4, 6, 8}; NSInteger scaleIndex = 0;
  for (int i = 0; i < 7; ++i) if (scales[i] == self.settings->scale) scaleIndex = i;
  self.scaleControl = [self segments:@[@"Auto", @"1×", @"2×", @"3×", @"4×", @"6×", @"8×"] selected:scaleIndex];
  [s addArrangedSubview:[self row:@"Internal resolution" symbol:@"square.resize" control:self.scaleControl]];
  self.anisoControl = [self segments:@[@"Off", @"4×", @"16×"] selected:self.settings->anisotropy >= 16 ? 2 : self.settings->anisotropy >= 4 ? 1 : 0];
  [s addArrangedSubview:[self row:@"Anisotropic filtering" symbol:@"square.stack.3d.up" control:self.anisoControl]];
  self.upscalerControl = [self segments:@[@"Off", @"MetalFX", @"MetalFX+"] selected:MAX(0, MIN(2, self.settings->upscaler))];
  [s addArrangedSubview:[self row:@"MetalFX upscaling (renders at half resolution, reconstructs)" symbol:@"wand.and.rays" control:self.upscalerControl]];
  self.vsyncSwitch = [self toggle:self.settings->vsync];
  [s addArrangedSubview:[self row:@"Display sync (off = lowest latency, may tear)" symbol:@"waveform.path" control:self.vsyncSwitch]];
  self.fullscreenSwitch = [self toggle:self.settings->fullscreen];
  [s addArrangedSubview:[self row:@"Start full screen (lowest latency; ⌥⏎ toggles)" symbol:@"arrow.up.left.and.arrow.down.right" control:self.fullscreenSwitch]];
  self.widescreenSwitch = [self toggle:self.settings->widescreen];
  [s addArrangedSubview:[self row:@"Widescreen (16:9)" symbol:@"rectangle.ratio.16.to.9" control:self.widescreenSwitch]];
  self.sharpness = [NSSlider sliderWithValue:self.settings->sharpness minValue:0 maxValue:1 target:nil action:nil];
  [s addArrangedSubview:[self sliderRow:@"Sharpen" symbol:@"sparkles" slider:self.sharpness format:@"%.0f%%" scale:100]];
  self.onlineSwitch = [self toggle:self.settings->online];
  [s addArrangedSubview:[self row:@"Slippi Online services" symbol:@"network" control:self.onlineSwitch]];
  NSMutableArray<NSString*>* delays = [@[@"1", @"2", @"3", @"4"] mutableCopy];   // plus the exact value when a larger one was set in the in-game menu
  if (self.settings->online_delay > 4) [delays addObject:[NSString stringWithFormat:@"%d", self.settings->online_delay]];
  self.delayControl = [self segments:delays selected:self.settings->online_delay > 4 ? 4 : MAX(0, self.settings->online_delay - 1)];
  [s addArrangedSubview:[self row:@"Online input delay (frames)" symbol:@"timer" control:self.delayControl]];
  [s addArrangedSubview:label(@"Each frame of delay adds 16.7 ms. 1 is the lowest latency on a stable, nearby connection; 2 is Slippi's default and rolls back less on Wi-Fi.", 11, NSFontWeightRegular, 0.6)];
  NSStackView* presetRow = [[NSStackView alloc] init]; presetRow.orientation = NSUserInterfaceLayoutOrientationHorizontal; presetRow.spacing = 10; presetRow.alignment = NSLayoutAttributeCenterY;
  [presetRow addArrangedSubview:[self button:@"Competitive preset" symbol:@"bolt.fill" action:@selector(applyCompetitivePreset)]];
  [presetRow addArrangedSubview:label(@"Full screen, 2× resolution (lowest latency that still looks crisp), 16× filtering, display sync, 4:3, no sharpening.", 11, NSFontWeightRegular, 0.6)];
  [s addArrangedSubview:presetRow];
  return card;
}

- (NSView*)buildDiscord {
  NSView* card = [self card];
  NSStackView* s = [self stackIn:card header:@"DISCORD" symbol:@"bubble.left.and.bubble.right"];
  self.discordSwitch = [self toggle:self.settings->discord_enabled];
  [s addArrangedSubview:[self row:@"Show what I'm playing on Discord" symbol:@"gamecontroller" control:self.discordSwitch]];
  self.discordRankSwitch = [self toggle:self.settings->discord_show_rank];
  [s addArrangedSubview:[self row:@"Show my rank" symbol:@"trophy" control:self.discordRankSwitch]];
  [s addArrangedSubview:label(@"Your status follows the game: menus, the queue, your opponent, then the stage, characters, live stocks and set score, with your rank badge and a link to your slippi.gg profile. It goes through the Discord app on this Mac, so there is nothing to sign in to, and nothing happens when Discord is not running.", 11, NSFontWeightRegular, 0.6)];
  return card;
}
- (NSView*)buildRegion {
  NSView* card = [self card];
  NSStackView* s = [self stackIn:card header:@"MATCHMAKING REGION" symbol:@"globe"];
  self.regionLabel = label(@"Checking your public IPv4 address…", 13, NSFontWeightMedium, 1);
  [s addArrangedSubview:self.regionLabel];
  [s addArrangedSubview:label(@"Slippi's matchmaking places you by the region of this address, looked up at ipgeolocation.io. If the lookup lands far from you, you get matched far from home. Check it; if it is wrong, send ipgeolocation the correction request (copied with your address filled in).", 11, NSFontWeightRegular, 0.6)];
  NSStackView* buttons = [[NSStackView alloc] init]; buttons.orientation = NSUserInterfaceLayoutOrientationHorizontal; buttons.spacing = 8;
  [buttons addArrangedSubview:[self button:@"Check my region" symbol:@"location.magnifyingglass" action:@selector(openRegionCheck)]];
  [buttons addArrangedSubview:[self button:@"Copy correction request" symbol:@"doc.on.doc" action:@selector(copyRegionReport)]];
  [buttons addArrangedSubview:[self button:@"Contact form" symbol:@"envelope" action:@selector(openRegionContact)]];
  [s addArrangedSubview:buttons];
  return card;
}
- (void)refreshRegion {
  const host::Dashboard& d = self.dashboard;
  if (!d.public_ipv4.empty()) self.regionLabel.stringValue = [NSString stringWithFormat:@"Your public IPv4 address: %s", d.public_ipv4.c_str()];
  else if (!d.ipv4_error.empty()) self.regionLabel.stringValue = [NSString stringWithFormat:@"Could not determine your public IPv4 address (%s).", d.ipv4_error.c_str()];
}
- (void)openRegionCheck { [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:@"https://ipgeolocation.io/what-is-my-ip/"]]; }
- (void)openRegionContact { [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:@"https://ipgeolocation.io/contact.html"]]; }
- (void)copyRegionReport {
  const std::string ip = self.dashboard.public_ipv4.empty() ? "<insert IP here>" : self.dashboard.public_ipv4;
  NSString* text = [NSString stringWithFormat:@"Hi,\nYour service reports my IP (%s) to be at <X location>, but I'm actually located at <Y location>. Could you correct this?\nThank you.", ip.c_str()];
  [NSPasteboard.generalPasteboard clearContents]; [NSPasteboard.generalPasteboard setString:text forType:NSPasteboardTypeString];
  self.regionLabel.stringValue = [NSString stringWithFormat:@"Copied a correction request for %s. Paste it into the ipgeolocation contact form.", ip.c_str()];
}

// ---- state
- (void)refreshSteps {
  const BOOL disc = !self.settings->iso.empty(), account = self.dashboard.signed_in, pad = self.controllerCount > 0;
  const BOOL states[4] = {disc, account, pad, NO};
  for (int i = 0; i < 4; ++i) {
    self.stepIcons[i].image = symbol(states[i] ? @"checkmark.circle.fill" : (i == 2 ? @"circle.dashed" : @"circle"), 15, NSFontWeightSemibold);
    self.stepIcons[i].contentTintColor = states[i] ? kGreen() : [NSColor colorWithWhite:1 alpha:0.4];
    self.stepLabels[i].alphaValue = states[i] ? 0.5 : 1;
  }
  self.stepsCard.hidden = disc && account;
}
- (void)refreshAccount {
  if (!self.settings) return;
  slippi::login::Account account;
  bool signed_in = slippi::login::read_user_file(self.settings->slippi_dir, account);
  if (!signed_in && _dashboard.profile_loaded && _dashboard.signed_in) { signed_in = true; account.display_name = _dashboard.name; account.connect_code = _dashboard.code; }   // sample data
  _dashboard.signed_in = signed_in;
  if (signed_in) {
    self.settings->account_name = account.display_name; self.settings->account_code = account.connect_code;
    if (_dashboard.name.empty()) { _dashboard.name = account.display_name; _dashboard.code = account.connect_code; }
    self.accountLabel.stringValue = [NSString stringWithFormat:@"Signed in as %s  (%s). Ranked stats and your connect code stay saved on this Mac.", account.display_name.c_str(), account.connect_code.c_str()];
    self.playerChip.stringValue = [NSString stringWithFormat:@"%s  %s", account.display_name.c_str(), account.connect_code.c_str()];
    self.chipBox.hidden = NO;
  } else {
    self.settings->account_name.clear(); self.settings->account_code.clear();
    self.accountLabel.stringValue = @"Sign in with your Slippi account to play online and see your ranked stats. Offline play works without it.";
    self.chipBox.hidden = YES;
  }
  self.signInRows.hidden = signed_in; self.signOutButton.hidden = !signed_in;
  self.rankedCard.hidden = !signed_in;
  [self refreshRanked]; [self refreshSteps];
}
- (void)refreshRanked {
  if (g_status) { g_status.dashboard = self.dashboard; [g_status refresh]; }
  const host::Dashboard& d = self.dashboard;
  if (self.settings) { self.settings->rank = d.profile_loaded && d.profile.ranked ? d.rank() : ""; self.settings->rating = d.profile.rating; }
  self.rankLabel.stringValue = ns(d.rank()); self.ratingLabel.stringValue = ns(d.rating());
  self.recordLabel.stringValue = d.profile_loaded ? ns(d.record()) : (d.profile_error.empty() ? @"Loading ranked profile…" : ns(d.profile_error));
  self.winBarWidth.active = NO;
  self.winBarWidth = [self.winBar.widthAnchor constraintEqualToAnchor:self.winTrack.widthAnchor multiplier:MAX(0.0, MIN(1.0, d.win_rate()))];
  self.winBarWidth.active = YES;
  self.placementLabel.stringValue = ns(d.placement());
  std::string mains;
  for (const std::string& m : d.mains()) mains += (mains.empty() ? "Mains: " : "   ") + m;
  self.mainsLabel.stringValue = ns(mains);
  self.placementLabel.hidden = self.placementLabel.stringValue.length == 0; self.mainsLabel.hidden = mains.empty();
  if (d.profile_loaded && d.profile.ranked) self.playerChip.stringValue = [NSString stringWithFormat:@"%s  %s  ·  %s", d.name.c_str(), d.code.c_str(), d.rank().c_str()];
}
- (void)refreshGames {
  for (NSView* v in self.gamesStack.arrangedSubviews) [v removeFromSuperview];
  std::vector<host::GameRow> rows = self.dashboard.rows();
  self.gamesCard.hidden = rows.empty();
  for (const host::GameRow& r : rows) {
    NSStackView* row = [[NSStackView alloc] init]; row.orientation = NSUserInterfaceLayoutOrientationHorizontal; row.spacing = 10; row.alignment = NSLayoutAttributeCenterY;
    NSStackView* text = [[NSStackView alloc] init]; text.orientation = NSUserInterfaceLayoutOrientationVertical; text.alignment = NSLayoutAttributeLeading; text.spacing = 1;
    [text addArrangedSubview:label(ns(r.title), 13, NSFontWeightSemibold, 1)]; [text addArrangedSubview:label(ns(r.subtitle), 11, NSFontWeightRegular, 0.6)];
    NSView* spacer = [[NSView alloc] init]; [spacer setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSTextField* result = [NSTextField labelWithString:ns(r.result)];
    result.font = meleeFont(13); result.textColor = r.win ? kGreen() : r.loss ? kRed() : [NSColor colorWithWhite:1 alpha:0.6];
    [row addArrangedSubview:text]; [row addArrangedSubview:spacer]; [row addArrangedSubview:result];
    [self.gamesStack addArrangedSubview:row];
  }
}
- (NSStackView*)controllerRow:(NSString*)iconName title:(NSString*)title subtitle:(NSString*)subtitle {
  NSStackView* row = [[NSStackView alloc] init]; row.orientation = NSUserInterfaceLayoutOrientationHorizontal; row.spacing = 10; row.alignment = NSLayoutAttributeCenterY;
  NSImageView* icon = [NSImageView imageViewWithImage:symbol(iconName, 15, NSFontWeightMedium)];
  icon.contentTintColor = kYellow(); [icon.widthAnchor constraintEqualToConstant:22].active = YES;
  NSStackView* text = [[NSStackView alloc] init]; text.orientation = NSUserInterfaceLayoutOrientationVertical; text.alignment = NSLayoutAttributeLeading; text.spacing = 1;
  [text addArrangedSubview:label(title, 13, NSFontWeightSemibold, 1)];
  [text addArrangedSubview:label(subtitle, 11, NSFontWeightRegular, 0.6)];
  [text setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
  [row addArrangedSubview:icon]; [row addArrangedSubview:text];
  return row;
}
- (NSString*)keyboardSummary {
  const host::KeyboardMap& k = host::keyboard_map();
  return [NSString stringWithFormat:@"Stick %s %s %s %s  ·  A %s  ·  B %s  ·  hold %s to walk and tilt",
          host::key_name(k.key[host::KB_STICK_UP]).c_str(), host::key_name(k.key[host::KB_STICK_LEFT]).c_str(), host::key_name(k.key[host::KB_STICK_DOWN]).c_str(),
          host::key_name(k.key[host::KB_STICK_RIGHT]).c_str(), host::key_name(k.key[host::GC_CTL_A]).c_str(), host::key_name(k.key[host::GC_CTL_B]).c_str(),
          host::key_name(k.key[host::KB_MODIFIER]).c_str()];
}
- (void)refreshControllers {
  std::vector<host::ControllerInfo> pads = host::window_list_controllers();
  self.controllerCount = pads.size();
  for (NSView* v in self.controllersStack.arrangedSubviews) [v removeFromSuperview];
  NSStackView* keyboard = [self controllerRow:@"keyboard" title:@"Keyboard" subtitle:[self keyboardSummary]];
  [keyboard addArrangedSubview:[self button:@"Configure…" symbol:@"slider.horizontal.3" action:@selector(configureKeyboard)]];
  [self.controllersStack addArrangedSubview:keyboard];
  for (const host::ControllerInfo& pad : pads) {
    NSStackView* row = [self controllerRow:(pad.is_gamecube_adapter ? @"cable.connector" : @"gamecontroller.fill") title:ns(pad.name) subtitle:ns(controller_rate_line(pad))];
    if (!pad.is_gamecube_adapter) {
      NSString* guid = ns(pad.guid);
      NSPopUpButton* port = [[NSPopUpButton alloc] init]; port.controlSize = NSControlSizeRegular;
      [port addItemsWithTitles:@[@"Auto port", @"Port 1", @"Port 2", @"Port 3", @"Port 4"]];
      [port selectItemAtIndex:MAX(0, MIN(4, pad.assigned_port))];
      port.target = self; port.action = @selector(portChanged:); objc_setAssociatedObject(port, "guid", guid, OBJC_ASSOCIATION_COPY);
      NSButton* configure = [self button:@"Configure…" symbol:@"slider.horizontal.3" action:@selector(configureController:)];
      objc_setAssociatedObject(configure, "guid", guid, OBJC_ASSOCIATION_COPY);
      objc_setAssociatedObject(configure, "name", ns(pad.name), OBJC_ASSOCIATION_COPY);
      [row addArrangedSubview:port]; [row addArrangedSubview:configure];
    }
    [self.controllersStack addArrangedSubview:row];
  }
  if (pads.empty()) [self.controllersStack addArrangedSubview:label(@"Pair or plug in a controller and it appears here.", 12, NSFontWeightRegular, 0.55)];
  [self refreshSteps];
}
- (void)tick {
  if (self.closed) return;
  if (++self.tickCount % 33 == 0) {   // about once a second: controllers coming and going, and their measured rates
    std::string sig;
    for (const host::ControllerInfo& p : host::window_list_controllers()) sig += p.guid + ":" + std::to_string((int)(p.report_hz / 10)) + ":" + std::to_string(p.adapter_ports) + ";";
    if (sig != self.controllerSignature) { self.controllerSignature = sig; [self refreshControllers]; }
    [self refreshReadiness];
  }
}
- (void)portChanged:(NSPopUpButton*)sender {
  NSString* guid = objc_getAssociatedObject(sender, "guid");
  host::ControllerConfig cfg; if (const host::ControllerConfig* c = host::controller_config_for(guid.UTF8String)) cfg = *c;
  cfg.guid = guid.UTF8String; cfg.port = (int)sender.indexOfSelectedItem;
  host::upsert_controller_config(cfg);
  [self refreshControllers];
}
- (void)configureController:(NSButton*)sender {
  [self openEditor:[[MUControllerEditor alloc] initWithGuid:objc_getAssociatedObject(sender, "guid") name:objc_getAssociatedObject(sender, "name")]];
}
- (void)connectController {
  if (self.editor) return;
  MUPairingSheet* pairing = [[MUPairingSheet alloc] init];
  self.editor = pairing;
  __weak MULauncherWindow* weakSelf = self;
  pairing.completion = ^{ MULauncherWindow* me = weakSelf; if (!me) return; me.editor = nil; [me refreshControllers]; };
  [pairing presentOn:self.window];
}
- (void)configureKeyboard { [self openEditor:[[MUControllerEditor alloc] initWithGuid:nil name:@"Keyboard"]]; }
- (void)openEditor:(MUControllerEditor*)editor {
  if (self.editor) return;
  self.editor = editor;
  __weak MULauncherWindow* weakSelf = self;
  editor.completion = ^{ MULauncherWindow* me = weakSelf; if (!me) return; me.editor = nil; [me refreshControllers]; };
  [editor presentOn:self.window];
}
- (void)refreshDisc {
  if (self.settings->iso.empty()) {
    self.discName.stringValue = @"No disc chosen";
    self.discHint.stringValue = self.startupError.length ? self.startupError : @"Choose your Melee NTSC 1.02 image (.iso/.gcm), or drop it onto this window.";
    self.playButton.enabled = NO;
  } else {
    NSString* path = ns(self.settings->iso);
    NSDictionary* attrs = [[NSFileManager defaultManager] attributesOfItemAtPath:path error:nil];
    self.discName.stringValue = path.lastPathComponent;
    self.discHint.stringValue = [NSString stringWithFormat:@"%.2f GB · %@%@", [attrs fileSize] / 1e9, [path stringByAbbreviatingWithTildeInPath], self.startupError.length ? [@"\n" stringByAppendingString:self.startupError] : @""];
    self.playButton.enabled = YES;
  }
  [self refreshSteps];
}
- (void)loadDashboard {
  std::string slippi_dir = self.settings->slippi_dir, replay_dir = self.settings->replay_dir;
  __weak MULauncherWindow* weakSelf = self;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    host::Dashboard d;
    host::dashboard_load_games(replay_dir, d, 8);
    host::dashboard_load_profile(slippi_dir, d);
    host::dashboard_load_network(d);
    dispatch_async(dispatch_get_main_queue(), ^{
      MULauncherWindow* s = weakSelf; if (!s || s.closed || !s.settings) return;
      host::Dashboard merged = d; merged.signed_in = s.dashboard.signed_in || d.profile_loaded;
      s.dashboard = merged; [s refreshAccount]; [s refreshGames]; [s refreshRegion];
      if (g_status) { g_status.dashboard = s.dashboard; [g_status refresh]; }
    });
  });
}

// ---- actions
- (void)controlTextDidEndEditing:(NSNotification*)note {
  NSNumber* movement = note.userInfo[@"NSTextMovement"];
  if (movement.integerValue != NSReturnTextMovement) return;
  if (note.object == self.emailField) [self.window makeFirstResponder:self.passwordField]; else [self signIn];
}
- (void)setBusy:(BOOL)busy {
  _busy = busy; self.signInButton.enabled = !busy;
  if (busy) [self.spinner startAnimation:nil]; else [self.spinner stopAnimation:nil];
}
- (void)signIn {
  if (self.busy) return;
  std::string email = self.emailField.stringValue.UTF8String ?: "", password = self.passwordField.stringValue.UTF8String ?: "";
  if (email.empty() || password.empty()) { self.accountLabel.stringValue = @"Enter your Slippi email and password."; return; }
  self.busy = YES; self.accountLabel.stringValue = @"Signing in…";
  std::string dir = self.settings->slippi_dir;
  __weak MULauncherWindow* weakSelf = self;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    slippi::login::Account account; slippi::login::Session session; std::string error;
    bool ok = slippi::login::sign_in_session(email, password, account, session, error) && slippi::login::write_user_file(dir, account, error);
    if (ok) slippi::login::write_session(dir, session);
    dispatch_async(dispatch_get_main_queue(), ^{
      MULauncherWindow* s = weakSelf; if (!s || s.closed || !s.settings) return;
      s.busy = NO;
      if (ok) { s.passwordField.stringValue = @""; s->_dashboard.name = account.display_name; s->_dashboard.code = account.connect_code; [s refreshAccount]; [s loadDashboard]; }
      else s.accountLabel.stringValue = ns(error);
    });
  });
}
- (void)forgotPassword {
  std::string email = self.emailField.stringValue.UTF8String ?: "";
  if (email.empty()) { self.accountLabel.stringValue = @"Enter your email first, then click Forgot password."; return; }
  self.accountLabel.stringValue = @"Sending a password reset email…";
  __weak MULauncherWindow* weakSelf = self;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    std::string error; bool ok = slippi::login::send_password_reset(email, error);
    dispatch_async(dispatch_get_main_queue(), ^{ MULauncherWindow* s = weakSelf; if (!s || s.closed) return; s.accountLabel.stringValue = ok ? @"Password reset email sent. Check your inbox." : ns(error); });
  });
}
- (void)openSlippi { [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:@"https://slippi.gg"]]; }
- (void)signOut {
  if (!self.settings) return;
  slippi::login::remove_user_file(self.settings->slippi_dir); slippi::login::remove_session(self.settings->slippi_dir);
  self.dashboard = host::Dashboard(); [self refreshAccount]; [self refreshGames];
}
- (void)acceptDroppedDisc:(NSString*)path { self.settings->iso = std::string(path.fileSystemRepresentation); self.startupError = nil; [self refreshDisc]; }
- (void)chooseDisc {
  NSOpenPanel* panel = [NSOpenPanel openPanel];
  panel.title = @"Choose Disc Image"; panel.prompt = @"Choose"; panel.canChooseDirectories = NO; panel.allowsMultipleSelection = NO;
  if ([panel runModal] != NSModalResponseOK) return;
  [self acceptDroppedDisc:[NSString stringWithUTF8String:panel.URL.fileSystemRepresentation]];
}
- (void)applyCompetitivePreset {
  self.scaleControl.selectedSegment = 2; self.anisoControl.selectedSegment = 2;   // 2x: the lowest-latency resolution that still looks crisp
  self.vsyncSwitch.state = NSControlStateValueOn; self.fullscreenSwitch.state = NSControlStateValueOn; self.widescreenSwitch.state = NSControlStateValueOff;
  self.sharpness.doubleValue = 0; [self sliderChanged:self.sharpness];
}
// The online delay the dashboard shows: 1-4, or the exact larger value set in the in-game menu (the fifth segment).
- (int)selectedDelay {
  const NSInteger i = self.delayControl.selectedSegment;
  return i >= 4 ? MAX(5, self.settings->online_delay) : (int)MAX(0, i) + 1;
}
- (void)play {
  // A Configure or Connect sheet must not keep its timers, key capture and controller discovery running into the match
  // (Play from the menu bar works while a sheet is open).
  if (self.editor) { [self.editor close]; self.editor = nil; }
  self.settings->discord_enabled = self.discordSwitch.state == NSControlStateValueOn;
  self.settings->discord_show_rank = self.discordRankSwitch.state == NSControlStateValueOn;
  const int scales[] = {0, 1, 2, 3, 4, 6, 8};
  self.settings->scale = scales[MAX(0, MIN(6, self.scaleControl.selectedSegment))];
  self.settings->anisotropy = self.anisoControl.selectedSegment == 2 ? 16 : self.anisoControl.selectedSegment == 1 ? 4 : 1;
  self.settings->upscaler = (int)MAX(0, MIN(2, self.upscalerControl.selectedSegment));
  self.settings->vsync = self.vsyncSwitch.state == NSControlStateValueOn;
  self.settings->fullscreen = self.fullscreenSwitch.state == NSControlStateValueOn;
  self.settings->widescreen = self.widescreenSwitch.state == NSControlStateValueOn;
  self.settings->online = self.onlineSwitch.state == NSControlStateValueOn;
  if (self.delayControl) self.settings->online_delay = [self selectedDelay];
  self.settings->sharpness = (float)self.sharpness.doubleValue;
  [NSApp stopModalWithCode:NSModalResponseOK];
}
// A wide window (full screen, a large display) puts the cards in two columns; a narrow one keeps one readable column.
- (void)windowDidResize:(NSNotification*)notification { [self updateColumns]; }
- (void)updateColumns {
  const BOOL wide = self.window.contentView.bounds.size.width >= 1100;
  if (!self.cardColumns || (self.cardColumns.orientation == NSUserInterfaceLayoutOrientationHorizontal) == wide) return;
  if (wide) {
    [NSLayoutConstraint deactivateConstraints:self.stackedWidths];
    self.cardColumns.orientation = NSUserInterfaceLayoutOrientationHorizontal; self.cardColumns.alignment = NSLayoutAttributeTop;
    self.cardColumns.distribution = NSStackViewDistributionFillEqually; self.cardColumns.spacing = 18;
    self.maxWidth.constant = 1240;
  } else {
    self.cardColumns.orientation = NSUserInterfaceLayoutOrientationVertical; self.cardColumns.alignment = NSLayoutAttributeLeading;
    self.cardColumns.distribution = NSStackViewDistributionFill; self.cardColumns.spacing = 14;
    [NSLayoutConstraint activateConstraints:self.stackedWidths];
    self.maxWidth.constant = 620;
  }
}
- (void)windowWillClose:(NSNotification*)notification { [NSApp stopModalWithCode:NSModalResponseCancel]; }
@end

@implementation MUStatusBar
- (instancetype)init {
  self = [super init];
  self.item = [NSStatusBar.systemStatusBar statusItemWithLength:NSVariableStatusItemLength];
  NSImage* mark = slippi_mark();
  if (mark) { mark.size = NSMakeSize(18, 18); self.item.button.image = mark; self.item.button.imagePosition = NSImageLeading; }
  else self.item.button.title = @"Dashdance";
  self.item.button.toolTip = @"Dashdance";
  [self refresh];
  return self;
}
- (NSMenuItem*)info:(NSString*)text {
  NSMenuItem* i = [[NSMenuItem alloc] initWithTitle:text action:nil keyEquivalent:@""]; i.enabled = NO; return i;
}
- (void)refresh {
  NSMenu* menu = [[NSMenu alloc] init];
  menu.autoenablesItems = NO;
  const host::Dashboard& d = self.dashboard;
  if (d.signed_in) {
    if (self.item.button.image) self.item.button.title = d.profile_loaded && d.profile.ranked ? ns("  " + d.rank()) : @"";
    NSMenuItem* who = [self info:[NSString stringWithFormat:@"%s  %s", d.name.c_str(), d.code.c_str()]];
    who.attributedTitle = [[NSAttributedString alloc] initWithString:who.title attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold]}];
    [menu addItem:who];
    if (d.profile_loaded) {
      [menu addItem:[self info:[NSString stringWithFormat:@"%s  ·  %s", d.rank().c_str(), d.rating().c_str()]]];
      [menu addItem:[self info:ns(d.record())]];
      if (!d.placement().empty()) [menu addItem:[self info:ns(d.placement())]];
      std::string mains; for (const std::string& m : d.mains()) mains += (mains.empty() ? "" : "   ") + m;
      if (!mains.empty()) [menu addItem:[self info:ns(mains)]];
    } else {
      [menu addItem:[self info:d.profile_error.empty() ? @"Loading ranked profile…" : ns(d.profile_error)]];
    }
    std::vector<host::GameRow> rows = d.rows();
    if (!rows.empty()) {
      [menu addItem:[NSMenuItem separatorItem]];
      NSMenuItem* recent = [[NSMenuItem alloc] initWithTitle:@"Recent Games" action:nil keyEquivalent:@""];
      NSMenu* sub = [[NSMenu alloc] init]; sub.autoenablesItems = NO;
      for (size_t i = 0; i < rows.size() && i < 8; ++i) [sub addItem:[self info:[NSString stringWithFormat:@"%s  %s  ·  %s", rows[i].result.c_str(), rows[i].title.c_str(), rows[i].subtitle.c_str()]]];
      recent.submenu = sub; [menu addItem:recent];
    }
  } else {
    if (self.item.button.image) self.item.button.title = @"";
    [menu addItem:[self info:@"Not signed in to Slippi"]];
  }
  [menu addItem:[NSMenuItem separatorItem]];
  NSMenuItem* play = [[NSMenuItem alloc] initWithTitle:self.playing ? @"Playing…" : @"Play" action:@selector(playFromMenu:) keyEquivalent:@""];
  play.target = self; play.enabled = !self.playing && self.launcher != nil; [menu addItem:play];
  NSMenuItem* show = [[NSMenuItem alloc] initWithTitle:@"Show Dashdance" action:@selector(showApp:) keyEquivalent:@""];
  show.target = self; [menu addItem:show];
  if (d.signed_in && !self.playing && self.launcher) {
    NSMenuItem* out = [[NSMenuItem alloc] initWithTitle:@"Sign Out of Slippi" action:@selector(signOutFromMenu:) keyEquivalent:@""];
    out.target = self; [menu addItem:out];
  }
  [menu addItem:[NSMenuItem separatorItem]];
  NSMenuItem* site = [[NSMenuItem alloc] initWithTitle:@"Slippi.gg" action:@selector(openSlippiSite:) keyEquivalent:@""]; site.target = g_links; [menu addItem:site];
  NSMenuItem* quit = [[NSMenuItem alloc] initWithTitle:@"Quit Dashdance" action:@selector(quit:) keyEquivalent:@"q"]; quit.target = g_links; [menu addItem:quit];
  self.item.menu = menu;
}
- (void)playFromMenu:(id)sender { if (self.launcher) [self.launcher play]; }
- (void)showApp:(id)sender { [NSApp activateIgnoringOtherApps:YES]; for (NSWindow* w in NSApp.windows) if (w.isVisible) [w makeKeyAndOrderFront:nil]; }
- (void)signOutFromMenu:(id)sender { if (self.launcher) [self.launcher signOut]; }
@end

namespace host {
void mac_show_error(const std::string& title, const std::string& detail) {
  @autoreleasepool {
    prepare_application();
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = [NSString stringWithUTF8String:title.c_str()];
    alert.informativeText = [NSString stringWithUTF8String:detail.c_str()];
    [alert runModal];
  }
}
bool launcher_run(LauncherSettings& settings, const std::string& error) {
  @autoreleasepool {
    prepare_application();
    settings.display_hz = display_max_hz(NSScreen.mainScreen);
    if (id<MTLDevice> gpu = MTLCreateSystemDefaultDevice()) settings.gpu_name = gpu.name.UTF8String;
    MULauncherWindow* launcher = [[MULauncherWindow alloc] initWithSettings:&settings error:error.empty() ? nil : [NSString stringWithUTF8String:error.c_str()]];
    if (!g_status && !std::getenv("MELEE_NO_STATUS_ITEM")) g_status = [[MUStatusBar alloc] init];
    if (g_status) { g_status.launcher = launcher; g_status.playing = NO; g_status.dashboard = launcher.dashboard; [g_status refresh]; }
    if (g_status && std::getenv("MELEE_OPEN_STATUS_MENU"))   // screenshot aid: pop the menu bar extra open after the dashboard has loaded
      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(3.0 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ [g_status.item.button performClick:nil]; });
    [launcher.window makeKeyAndOrderFront:nil];
    [launcher animateIn];
    if (std::getenv("MELEE_TEXT_AUDIT"))   // QA aid: log text that does not fit, after the other aids have opened their sheets
      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(5.0 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        text_audit(launcher.window, "dashboard");
        if (NSWindow* sheet = launcher.window.attachedSheet) text_audit(sheet, "sheet");
      });
    if (std::getenv("MELEE_OPEN_PAIRING"))   // screenshot aid: the Connect a Controller sheet
      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2.0 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ [launcher connectController]; });
    if (const char* which = std::getenv("MELEE_OPEN_EDITOR")) {   // screenshot aid: "keyboard", or "pad:<guid>:<name>"
      const std::string w = which;
      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2.0 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        if (w == "keyboard") [launcher openEditor:[[MUControllerEditor alloc] initWithGuid:nil name:@"Keyboard"]];
        else if (w.rfind("pad:", 0) == 0) {
          const size_t colon = w.find(':', 4);
          [launcher openEditor:[[MUControllerEditor alloc] initWithGuid:ns(w.substr(4, colon == std::string::npos ? std::string::npos : colon - 4)) name:ns(colon == std::string::npos ? "Controller" : w.substr(colon + 1))]];
        }
      });
    }
    if (const char* size = std::getenv("MELEE_LAUNCHER_SIZE")) {   // screenshot aid: "<width>x<height>" in points
      double w = 0, h = 0;
      if (std::sscanf(size, "%lfx%lf", &w, &h) == 2) { NSRect f = launcher.window.frame; f.size = NSMakeSize(w, h); [launcher.window setFrame:f display:YES]; [launcher.window center]; [launcher updateColumns]; }
    }
    if (const char* scroll = std::getenv("MELEE_LAUNCHER_SCROLL"))   // screenshot aid: start scrolled down by N points
      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        NSScrollView* sv = launcher.scroll;   // held directly: the glass container reparents its content view
        [launcher.window layoutIfNeeded];
        if (sv) { [sv.contentView scrollToPoint:NSMakePoint(0, std::atof(scroll))]; [sv reflectScrolledClipView:sv.contentView]; } });
    NSModalResponse response = [NSApp runModalForWindow:launcher.window];
    launcher.closed = YES;
    if (g_status) { g_status.launcher = nil; g_status.playing = response == NSModalResponseOK; [g_status refresh]; }
    [launcher.timer invalidate];
    launcher.settings = nullptr;   // an in-flight sign-in must not write into main's settings afterwards
    [launcher.window orderOut:nil];
    return response == NSModalResponseOK;
  }
}
}  // namespace host
