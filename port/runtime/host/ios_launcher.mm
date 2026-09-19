// iOS / iPadOS / visionOS dashboard and launcher (UIKit + SceneKit). Melee-styled: dark grid
// backdrop, yellow angled section headers, italic display type. Shows the signed-in player's
// ranked profile and recent games, manages controllers and display settings, then starts play.
// SPDX-License-Identifier: GPL-2.0-or-later
#import <UIKit/UIKit.h>
#import <SceneKit/SceneKit.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include "dashboard.h"
#include "gc_diagram.h"
#include "host.h"
#include "input_config.h"
#include "controller_pairing.h"
#import <GameController/GameController.h>
#include "mac_launcher.h"
#include "slippi_login.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
UIColor* rgb(CGFloat r, CGFloat g, CGFloat b, CGFloat a = 1) { return [UIColor colorWithRed:r green:g blue:b alpha:a]; }
// Theme: Melee's menu yellow on deep blue, violet for the Dashdance mark. One place for every colour.
UIColor* kYellow() { return rgb(0.97, 0.79, 0.28); }
UIColor* kRed() { return rgb(0.89, 0.27, 0.17); }
UIColor* kMarkViolet() { return rgb(0.49, 0.36, 1.00); }
UIColor* kGlassTint() { return rgb(0.30, 0.40, 1.0, 0.10); }
// Liquid Glass (iOS 26): glass cards and buttons; older systems get the material fallback.
bool glass_available() {
#if TARGET_OS_VISION
  return false;
#else
  if (@available(iOS 26.0, *)) return true;
  return false;
#endif
}
// Haptics: one helper, fired next to the user action, never in a burst.
void haptic_impact() {
#if !TARGET_OS_VISION
  UIImpactFeedbackGenerator* g = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleMedium]; [g prepare]; [g impactOccurred];
#endif
}
void haptic_notify(bool success) {
#if !TARGET_OS_VISION
  UINotificationFeedbackGenerator* g = [[UINotificationFeedbackGenerator alloc] init]; [g prepare]; [g notificationOccurred:success ? UINotificationFeedbackTypeSuccess : UINotificationFeedbackTypeError];
#endif
}
// The Dashdance mark ships in the bundle (AppMark.png, from the icon's glyph layer); MELEE_MARK overrides the path (for the bare binary).
UIImage* slippi_mark() {
  NSString* path = [NSBundle.mainBundle pathForResource:@"AppMark" ofType:@"png"];
  if (const char* env = std::getenv("MELEE_MARK")) path = [NSString stringWithUTF8String:env];
  UIImage* image = path ? [UIImage imageWithContentsOfFile:path] : nil;
  return [image imageWithRenderingMode:UIImageRenderingModeAlwaysTemplate];
}
NSString* ns(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()]; }
UIFont* meleeFont(CGFloat size, UIFontWeight weight) {
  UIFont* base = [UIFont systemFontOfSize:size weight:weight];
  UIFontDescriptor* d = [base.fontDescriptor fontDescriptorWithSymbolicTraits:UIFontDescriptorTraitItalic | UIFontDescriptorTraitBold];
  return d ? [UIFont fontWithDescriptor:d size:size] : base;
}
std::vector<std::string> documents_discs(NSURL** documents_out) {
  std::vector<std::string> discs;
  NSArray<NSURL*>* documents = [[NSFileManager defaultManager] URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask];
  if (documents.count == 0) return discs;
  if (documents_out) *documents_out = documents.firstObject;
  NSArray<NSURL*>* entries = [[NSFileManager defaultManager] contentsOfDirectoryAtURL:documents.firstObject includingPropertiesForKeys:nil options:0 error:nil];
  for (NSURL* entry in entries) {
    NSString* extension = entry.pathExtension.lowercaseString;
    if ([extension isEqualToString:@"iso"] || [extension isEqualToString:@"gcm"]) discs.push_back(std::string(entry.fileSystemRepresentation));
  }
  return discs;
}
int display_max_hz() {
#if TARGET_OS_VISION
  return 90;
#else
  UIScreen* screen = nil;   // the screen the app's scene is on (iPhone Duo has two), not a global main screen
  for (UIScene* s in UIApplication.sharedApplication.connectedScenes) if ([s isKindOfClass:UIWindowScene.class]) { screen = ((UIWindowScene*)s).screen; break; }
  return (int)(screen ?: UIScreen.mainScreen).maximumFramesPerSecond;
#endif
}
}  // namespace

// ---- 3D mark: a metallic ring with a core, slowly turning (SceneKit, transparent background)
@interface MUHeroView : SCNView
@end
@implementation MUHeroView
- (instancetype)initWithSize:(CGFloat)size {
  self = [super initWithFrame:CGRectMake(0, 0, size, size) options:nil];
  self.translatesAutoresizingMaskIntoConstraints = NO;
  [self.widthAnchor constraintEqualToConstant:size].active = YES;
  [self.heightAnchor constraintEqualToConstant:size].active = YES;
  self.backgroundColor = UIColor.clearColor;
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
  SCNNode* key = [SCNNode node]; key.light = [SCNLight light]; key.light.type = SCNLightTypeOmni; key.light.intensity = 900; key.light.color = UIColor.whiteColor; key.position = SCNVector3Make(3, 4, 5);
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
  self.layer.shadowColor = rgb(0.97, 0.79, 0.28).CGColor; self.layer.shadowOpacity = 0.35; self.layer.shadowRadius = size * 0.3; self.layer.shadowOffset = CGSizeZero;
  return self;
}
@end

// ---- Melee-style backdrop: dark blue gradient with a faint perspective grid and a drifting glow
@interface MUBackdropView : UIView
@end
@implementation MUBackdropView { CAGradientLayer* _gradient; CAShapeLayer* _grid; CAGradientLayer* _glow; }
- (instancetype)initWithFrame:(CGRect)frame {
  self = [super initWithFrame:frame];
  self.userInteractionEnabled = NO; self.translatesAutoresizingMaskIntoConstraints = NO;
  _gradient = [CAGradientLayer layer];
  _gradient.colors = @[(id)rgb(0.05, 0.07, 0.20).CGColor, (id)rgb(0.02, 0.02, 0.08).CGColor];
  _gradient.startPoint = CGPointMake(0.5, 0); _gradient.endPoint = CGPointMake(0.5, 1);
  [self.layer addSublayer:_gradient];
  _grid = [CAShapeLayer layer];
  _grid.strokeColor = rgb(0.45, 0.55, 1.0, 0.10).CGColor; _grid.lineWidth = 1; _grid.fillColor = nil;
  [self.layer addSublayer:_grid];
  _glow = [CAGradientLayer layer];
  _glow.type = kCAGradientLayerRadial;
  _glow.colors = @[(id)rgb(0.97, 0.79, 0.28, 0.22).CGColor, (id)rgb(0.97, 0.79, 0.28, 0).CGColor];
  _glow.startPoint = CGPointMake(0.5, 0.5); _glow.endPoint = CGPointMake(1, 1);
  [self.layer addSublayer:_glow];
  return self;
}
- (void)layoutSubviews {
  [super layoutSubviews];
  [CATransaction begin]; [CATransaction setDisableActions:YES];
  _gradient.frame = self.bounds;
  const CGFloat w = self.bounds.size.width, h = self.bounds.size.height;
  UIBezierPath* p = [UIBezierPath bezierPath];
  for (CGFloat x = 0; x <= w; x += 56) { [p moveToPoint:CGPointMake(x, 0)]; [p addLineToPoint:CGPointMake(x, h)]; }
  for (CGFloat y = 0; y <= h; y += 56) { [p moveToPoint:CGPointMake(0, y)]; [p addLineToPoint:CGPointMake(w, y)]; }
  _grid.path = p.CGPath; _grid.frame = self.bounds;
  const CGFloat d = MAX(w, h) * 0.9;
  _glow.bounds = CGRectMake(0, 0, d, d); _glow.position = CGPointMake(w * 0.85, h * 0.12);
  [CATransaction commit];
  if (![_glow animationForKey:@"drift"]) {
    CABasicAnimation* drift = [CABasicAnimation animationWithKeyPath:@"position"];
    drift.fromValue = [NSValue valueWithCGPoint:_glow.position];
    drift.toValue = [NSValue valueWithCGPoint:CGPointMake(w * 0.2, h * 0.35)];
    drift.duration = 16; drift.autoreverses = YES; drift.repeatCount = HUGE_VALF;
    drift.timingFunction = [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseInEaseOut];
    [_glow addAnimation:drift forKey:@"drift"];
  }
}
@end

@interface MULauncherController : UIViewController
+ (UIButtonConfiguration*)glassConfiguration:(BOOL)prominent;
@end

// ---- Controller editor: a live GameCube controller, bindings, deadzones, trigger point and rumble
@interface MUDiagramUIView : UIView
@property(nonatomic, copy) void (^onPick)(int part);
- (host::DiagramState&)state;
@end
@implementation MUDiagramUIView { host::DiagramState _state; }
- (instancetype)initWithFrame:(CGRect)frame {
  self = [super initWithFrame:frame];
  self.backgroundColor = UIColor.clearColor; self.opaque = NO; self.contentMode = UIViewContentModeRedraw;
  return self;
}
- (host::DiagramState&)state { return _state; }
- (void)drawRect:(CGRect)rect { host::gc_diagram_draw(UIGraphicsGetCurrentContext(), self.bounds, _state); }
- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  const CGPoint p = [touches.anyObject locationInView:self];
  const int part = host::gc_diagram_hit(self.bounds, p, _state.directions);
  if (part != host::DP_NONE && self.onPick) self.onPick(part);
}
@end

// The width of the longest word in `text` at `font`: a wrapping label must never be narrower than this, or a word breaks
// in half on a narrow phone ("Sharpe / n").
static CGFloat longest_word_width(NSString* text, UIFont* font) {
  CGFloat widest = 0;
  for (NSString* word in [text componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet])
    if (word.length) widest = MAX(widest, ceil([word sizeWithAttributes:@{NSFontAttributeName: font}].width));
  return widest;
}
static void keep_words_whole(UILabel* l) {
  const CGFloat w = longest_word_width(l.text ?: @"", l.font);
  if (w > 0) { NSLayoutConstraint* c = [l.widthAnchor constraintGreaterThanOrEqualToConstant:w + 1]; c.priority = UILayoutPriorityRequired - 1; c.active = YES; }
  [l setContentCompressionResistancePriority:UILayoutPriorityDefaultHigh forAxis:UILayoutConstraintAxisHorizontal];
}
@interface MURemapController : UIViewController
@property(nonatomic) host::ControllerConfig config;
@property(nonatomic, copy) NSString* controllerName;
@property(nonatomic) MUDiagramUIView* diagram;
@property(nonatomic) NSMutableArray<UIButton*>* rows;
@property(nonatomic) UILabel* hint;
@property(nonatomic) UISlider* stickSlider;
@property(nonatomic) UISlider* cstickSlider;
@property(nonatomic) UISlider* triggerSlider;
@property(nonatomic) UILabel* stickValue;
@property(nonatomic) UILabel* cstickValue;
@property(nonatomic) UILabel* triggerValue;
@property(nonatomic) UISwitch* swapSwitch;
@property(nonatomic) UISwitch* rumbleSwitch;
@property(nonatomic) CADisplayLink* link;
@property(nonatomic) int capturing;
@property(nonatomic) BOOL armed;
@property(nonatomic) BOOL sequence;
@property(nonatomic) UIStackView* columns;   // the picture and the settings: side by side when wide
@end

@implementation MURemapController
- (UILabel*)text:(NSString*)t size:(CGFloat)size weight:(UIFontWeight)weight alpha:(CGFloat)alpha {
  UILabel* l = [[UILabel alloc] init];
  l.text = t; l.font = [UIFont systemFontOfSize:size weight:weight]; l.textColor = [UIColor colorWithWhite:1 alpha:alpha]; l.numberOfLines = 0;
  return l;
}
- (UIButton*)action:(NSString*)title symbol:(NSString*)symbol prominent:(BOOL)prominent selector:(SEL)selector {
  UIButtonConfiguration* c = [MULauncherController glassConfiguration:prominent];
  c.title = title; c.image = [UIImage systemImageNamed:symbol]; c.imagePadding = 6; c.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
  if (prominent) { c.baseBackgroundColor = kYellow(); c.baseForegroundColor = UIColor.blackColor; } else c.baseForegroundColor = UIColor.whiteColor;
  UIButton* b = [UIButton buttonWithConfiguration:c primaryAction:nil];
  [b addTarget:self action:selector forControlEvents:UIControlEventTouchUpInside];
  return b;
}
- (UIView*)row:(NSString*)name control:(UIView*)control value:(UILabel*)value {
  UIStackView* row = [[UIStackView alloc] init]; row.axis = UILayoutConstraintAxisHorizontal; row.spacing = 12; row.alignment = UIStackViewAlignmentCenter;
  UILabel* l = [self text:name size:15 weight:UIFontWeightRegular alpha:1];
  [l setContentHuggingPriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
  keep_words_whole(l);
  [row addArrangedSubview:l]; [row addArrangedSubview:control];
  if ([control isKindOfClass:UISlider.class]) {   // sliders shrink on narrow phones before any label does
    NSLayoutConstraint* prefer = [control.widthAnchor constraintEqualToConstant:170]; prefer.priority = UILayoutPriorityDefaultLow; prefer.active = YES;
    [control.widthAnchor constraintGreaterThanOrEqualToConstant:90].active = YES;
  }
  if (value) {
    value.font = [UIFont monospacedDigitSystemFontOfSize:14 weight:UIFontWeightMedium]; value.textColor = [UIColor colorWithWhite:1 alpha:0.75]; value.textAlignment = NSTextAlignmentRight;
    [value.widthAnchor constraintEqualToConstant:52].active = YES;
    [row addArrangedSubview:value];
  }
  return row;
}
- (UISlider*)slider:(float)min max:(float)max value:(float)value {
  UISlider* s = [[UISlider alloc] init];
  s.minimumValue = min; s.maximumValue = max; s.value = value; s.tintColor = kYellow();
  [s addTarget:self action:@selector(slidersChanged) forControlEvents:UIControlEventValueChanged];
  return s;
}
- (void)viewDidLoad {
  [super viewDidLoad];
  self.view.backgroundColor = rgb(0.05, 0.06, 0.16);
  self.capturing = -1;
  UIScrollView* scroll = [[UIScrollView alloc] init];
  scroll.translatesAutoresizingMaskIntoConstraints = NO;
  [self.view addSubview:scroll];
  UIStackView* stack = [[UIStackView alloc] init];
  stack.axis = UILayoutConstraintAxisVertical; stack.spacing = 12; stack.translatesAutoresizingMaskIntoConstraints = NO;
  [scroll addSubview:stack];
  UIStackView* left = [[UIStackView alloc] init]; left.axis = UILayoutConstraintAxisVertical; left.spacing = 12;
  UIStackView* right = [[UIStackView alloc] init]; right.axis = UILayoutConstraintAxisVertical; right.spacing = 12;
  [stack addArrangedSubview:left]; [stack addArrangedSubview:right];
  self.columns = stack;
  [NSLayoutConstraint activateConstraints:@[
    [scroll.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor], [scroll.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
    [scroll.leadingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.leadingAnchor], [scroll.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor],
    [stack.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor constant:24], [stack.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor constant:-24],
    [stack.leadingAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.leadingAnchor constant:24], [stack.trailingAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.trailingAnchor constant:-24]]];
  UILabel* title = [[UILabel alloc] init];
  title.text = @"CONTROLLER"; title.font = meleeFont(26, UIFontWeightBold); title.textColor = kYellow(); title.numberOfLines = 0;
  self.hint = [self text:@"" size:13 weight:UIFontWeightRegular alpha:0.65];
  self.diagram = [[MUDiagramUIView alloc] init];
  [self.diagram.heightAnchor constraintEqualToAnchor:self.diagram.widthAnchor multiplier:1.0 / host::kDiagramAspect].active = YES;
  __weak MURemapController* weakSelf = self;
  self.diagram.onPick = ^(int part) { [weakSelf startCaptureAt:part sequence:NO]; };
  UILabel* device = [self text:self.controllerName ?: @"" size:13 weight:UIFontWeightSemibold alpha:0.55];   // which pad this is, when several are connected
  for (UIView* v in @[title, device, self.hint, self.diagram]) [left addArrangedSubview:v];
  UIStackView* top = [[UIStackView alloc] init]; top.axis = UILayoutConstraintAxisHorizontal; top.spacing = 10; top.distribution = UIStackViewDistributionFillEqually;
  [top addArrangedSubview:[self action:@"Map all buttons" symbol:@"list.number" prominent:NO selector:@selector(mapAll)]];
  [top addArrangedSubview:[self action:@"Test rumble" symbol:@"waveform" prominent:NO selector:@selector(testRumble)]];
  [left addArrangedSubview:top];
  self.rows = [NSMutableArray array];
  for (int i = 0; i < host::GC_CTL_COUNT; ++i) {
    UIButtonConfiguration* c = [MULauncherController glassConfiguration:NO];
    c.cornerStyle = UIButtonConfigurationCornerStyleLarge; c.baseForegroundColor = UIColor.whiteColor;
    c.contentInsets = NSDirectionalEdgeInsetsMake(11, 16, 11, 16);
    UIButton* b = [UIButton buttonWithConfiguration:c primaryAction:nil];
    b.tag = i; b.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeading;
    [b addTarget:self action:@selector(rowTapped:) forControlEvents:UIControlEventTouchUpInside];
    [self.rows addObject:b]; [right addArrangedSubview:b];
  }
  const host::ControllerMap m = _config.map;
  self.stickSlider = [self slider:0 max:60 value:m.stick_deadzone]; self.stickValue = [[UILabel alloc] init];
  self.cstickSlider = [self slider:0 max:60 value:m.cstick_deadzone]; self.cstickValue = [[UILabel alloc] init];
  self.triggerSlider = [self slider:20 max:100 value:m.trigger_press]; self.triggerValue = [[UILabel alloc] init];
  [right addArrangedSubview:[self row:@"Stick deadzone" control:self.stickSlider value:self.stickValue]];
  [right addArrangedSubview:[self row:@"C-stick deadzone" control:self.cstickSlider value:self.cstickValue]];
  [right addArrangedSubview:[self row:@"Trigger press point" control:self.triggerSlider value:self.triggerValue]];
  self.swapSwitch = [[UISwitch alloc] init]; self.swapSwitch.on = m.swap_sticks; self.swapSwitch.onTintColor = kYellow();
  [self.swapSwitch addTarget:self action:@selector(togglesChanged) forControlEvents:UIControlEventValueChanged];
  self.rumbleSwitch = [[UISwitch alloc] init]; self.rumbleSwitch.on = m.rumble; self.rumbleSwitch.onTintColor = kYellow();
  [self.rumbleSwitch addTarget:self action:@selector(togglesChanged) forControlEvents:UIControlEventValueChanged];
  [right addArrangedSubview:[self row:@"Swap sticks" control:self.swapSwitch value:nil]];
  [right addArrangedSubview:[self row:@"Rumble" control:self.rumbleSwitch value:nil]];
  UIStackView* bottom = [[UIStackView alloc] init]; bottom.axis = UILayoutConstraintAxisHorizontal; bottom.spacing = 10; bottom.distribution = UIStackViewDistributionFillEqually;
  [bottom addArrangedSubview:[self action:@"Reset to defaults" symbol:@"arrow.counterclockwise" prominent:NO selector:@selector(resetDefaults)]];
  [bottom addArrangedSubview:[self action:@"Done" symbol:@"checkmark" prominent:YES selector:@selector(finish)]];
  [right addArrangedSubview:bottom];
  [self refresh];
  self.link = [CADisplayLink displayLinkWithTarget:self selector:@selector(tick)];
  [self.link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
}
- (void)viewWillLayoutSubviews {
  [super viewWillLayoutSubviews];
  const CGSize size = self.view.bounds.size;
  const BOOL wide = size.width > size.height && size.width >= 640;   // landscape phones, wide sheets, Vision Pro windows
  self.columns.axis = wide ? UILayoutConstraintAxisHorizontal : UILayoutConstraintAxisVertical;
  self.columns.distribution = wide ? UIStackViewDistributionFillEqually : UIStackViewDistributionFill;
  self.columns.alignment = wide ? UIStackViewAlignmentTop : UIStackViewAlignmentFill;
  self.columns.spacing = wide ? 28 : 12;
}
- (void)viewDidDisappear:(BOOL)animated { [super viewDidDisappear:animated]; [self.link invalidate]; self.link = nil; }
- (void)refresh {
  for (UIButton* b in self.rows) {
    const int i = (int)b.tag;
    UIButtonConfiguration* c = b.configuration;
    NSString* bound = self.capturing == i ? @"Press a button…" : ns(host::physical_input_name(_config.map.binding[i]));
    NSMutableAttributedString* t = [[NSMutableAttributedString alloc] initWithString:[NSString stringWithUTF8String:host::kGcControlNames[i]]
                                                                          attributes:@{NSFontAttributeName: [UIFont systemFontOfSize:17 weight:UIFontWeightSemibold], NSForegroundColorAttributeName: UIColor.whiteColor}];
    [t appendAttributedString:[[NSAttributedString alloc] initWithString:[@"    " stringByAppendingString:bound]
                                                              attributes:@{NSFontAttributeName: [UIFont systemFontOfSize:15], NSForegroundColorAttributeName: self.capturing == i ? kYellow() : [UIColor colorWithWhite:1 alpha:0.6]}]];
    c.attributedTitle = t;
    c.baseBackgroundColor = self.capturing == i ? rgb(0.97, 0.79, 0.28, 0.25) : [UIColor colorWithWhite:1 alpha:0.08];
    b.configuration = c;
  }
  self.hint.text = self.capturing >= 0 ? [NSString stringWithFormat:@"Press the button you want for %s.", host::kGcControlNames[self.capturing]]
                                       : @"Tap a button on the controller or in the list, then press the input you want. Map all goes through every button in order. Changes are saved as you go.";
  self.stickValue.text = [NSString stringWithFormat:@"%d%%", _config.map.stick_deadzone];
  self.cstickValue.text = [NSString stringWithFormat:@"%d%%", _config.map.cstick_deadzone];
  self.triggerValue.text = [NSString stringWithFormat:@"%d%%", _config.map.trigger_press];
}
- (void)tick {
  host::DiagramState& s = self.diagram.state;
  host::ControllerLiveState live;
  if (host::window_controller_state(_config.guid, live)) host::gc_diagram_from_pad(s, _config.map, live);
  else { s.stick_deadzone = _config.map.stick_deadzone / 100.0f; s.cstick_deadzone = _config.map.cstick_deadzone / 100.0f; }
  s.selected = self.capturing;
  s.pulse = 0.5f + 0.5f * (float)std::sin(CACurrentMediaTime() * 6.0);
  if (self.capturing >= 0) {
    const int input = host::window_capture_input(_config.guid);
    if (!self.armed) { if (input == host::kUnbound) self.armed = YES; }   // wait for the previous press to be released
    else if (input != host::kUnbound) [self assign:input];
  }
  [self.diagram setNeedsDisplay];
}
- (void)assign:(int)input {
  for (int j = 0; j < host::GC_CTL_COUNT; ++j) if (j != self.capturing && _config.map.binding[j] == input) _config.map.binding[j] = host::kUnbound;
  _config.map.binding[self.capturing] = input;
  host::upsert_controller_config(_config);
  haptic_impact();
  if (self.sequence && self.capturing + 1 < host::GC_CTL_COUNT) { self.capturing += 1; self.armed = NO; }
  else { self.capturing = -1; self.sequence = NO; }
  [self refresh];
}
- (void)startCaptureAt:(int)part sequence:(BOOL)sequence {
  if (part < 0 || part >= host::GC_CTL_COUNT) return;
  self.capturing = part; self.sequence = sequence; self.armed = NO;
  [self refresh];
}
- (void)rowTapped:(UIButton*)sender { [self startCaptureAt:(int)sender.tag sequence:NO]; }
- (void)mapAll { [self startCaptureAt:0 sequence:YES]; }
- (void)slidersChanged {
  _config.map.stick_deadzone = (int)std::lround(self.stickSlider.value);
  _config.map.cstick_deadzone = (int)std::lround(self.cstickSlider.value);
  _config.map.trigger_press = (int)std::lround(self.triggerSlider.value);
  host::upsert_controller_config(_config);
  [self refresh];
}
- (void)togglesChanged {
  _config.map.swap_sticks = self.swapSwitch.on;
  _config.map.rumble = self.rumbleSwitch.on;
  host::upsert_controller_config(_config);
}
- (void)testRumble { host::window_test_rumble(_config.guid); haptic_impact(); }
- (void)resetDefaults {
  _config.map = host::ControllerMap::defaults();
  host::upsert_controller_config(_config);
  self.stickSlider.value = _config.map.stick_deadzone; self.cstickSlider.value = _config.map.cstick_deadzone; self.triggerSlider.value = _config.map.trigger_press;
  [self.swapSwitch setOn:NO animated:YES]; [self.rumbleSwitch setOn:YES animated:YES];
  self.capturing = -1; self.sequence = NO;
  [self refresh];
}
- (void)finish { [self dismissViewControllerAnimated:YES completion:nil]; }
@end

// QA aid (MELEE_TEXT_AUDIT=1): logs every piece of text that is cut off, needs more lines than its box has, or sits
// outside the window, so layouts can be checked on every device and orientation without eyeballing screenshots.
static void text_audit_view(UIView* v, UIWindow* w, const char* where, int& found) {
  if (v.hidden || v.alpha < 0.01) return;
  if ([v isKindOfClass:UILabel.class]) {
    UILabel* l = (UILabel*)v;
    NSAttributedString* text = l.attributedText;
    const CGSize b = l.bounds.size;
    if (text.length && b.width > 1 && b.height > 1) {
      BOOL clipped;
      UIFont* font = [text attribute:NSFontAttributeName atIndex:0 effectiveRange:nil] ?: l.font;
      if (l.numberOfLines == 1) clipped = !l.adjustsFontSizeToFitWidth && [text boundingRectWithSize:CGSizeMake(CGFLOAT_MAX, CGFLOAT_MAX) options:NSStringDrawingUsesLineFragmentOrigin context:nil].size.width > b.width + 1.5;
      else clipped = [text boundingRectWithSize:CGSizeMake(b.width, CGFLOAT_MAX) options:NSStringDrawingUsesLineFragmentOrigin context:nil].size.height > b.height + 2.0;
      const BOOL brokenWord = l.numberOfLines != 1 && longest_word_width(text.string, font) > b.width + 1.0;   // "Sharpe / n"
      clipped = clipped || brokenWord;
      const CGRect r = [l convertRect:l.bounds toView:nil];
      const BOOL outside = CGRectGetMinX(r) < -1 || CGRectGetMaxX(r) > w.bounds.size.width + 1;
      // Where the text actually sits inside the label, then whether it runs into the rounded ends or the edge of the nearest
      // coloured or rounded shape around it (a badge, a capsule, a card).
      const CGFloat tw = MIN(b.width, [text boundingRectWithSize:CGSizeMake(l.numberOfLines == 1 ? CGFLOAT_MAX : b.width, CGFLOAT_MAX) options:NSStringDrawingUsesLineFragmentOrigin context:nil].size.width);
      const CGFloat tx = l.textAlignment == NSTextAlignmentCenter ? (b.width - tw) / 2 : l.textAlignment == NSTextAlignmentRight ? b.width - tw : 0;
      CGRect textRect = [l convertRect:CGRectMake(tx, 0, tw, b.height) toView:nil];
      BOOL crowded = NO;
      for (UIView* a = l; a && a != w; a = a.superview) {
        const BOOL shape = a.layer.cornerRadius > 0 || (a.backgroundColor && CGColorGetAlpha(a.backgroundColor.CGColor) > 0.05);
        if (!shape) continue;
        const CGFloat inset = MAX(4.0, MIN(a.layer.cornerRadius, a.bounds.size.height / 2) * 0.5);
        const CGRect safe = CGRectInset([a convertRect:a.bounds toView:nil], inset, 0);
        crowded = CGRectGetMinX(textRect) < CGRectGetMinX(safe) - 0.5 || CGRectGetMaxX(textRect) > CGRectGetMaxX(safe) + 0.5;
        break;
      }
      if (clipped || outside || crowded) {
        ++found;
        fprintf(stderr, "text-audit [%s] %s%s%s \"%s\" box %.0fx%.0f x %.0f..%.0f\n", where, clipped ? "clipped" : "", outside ? " outside-window" : "", crowded ? " touches-its-shape" : "",
                text.string.UTF8String, b.width, b.height, CGRectGetMinX(r), CGRectGetMaxX(r));
      }
    }
  }
  for (UIView* s in v.subviews) text_audit_view(s, w, where, found);
}
static void text_audit(UIWindow* w, const char* where) {
  if (!w) return;
  int found = 0;
  text_audit_view(w, w, where, found);
  fprintf(stderr, "text-audit [%s] done: %d problems, window %.0fx%.0f\n", where, found, w.bounds.size.width, w.bounds.size.height);
}

// "Connect a controller": pairing-mode steps per controller family, a shortcut to Settings, and a live confirmation the
// moment a new controller shows up. Apple does not let apps pair Bluetooth controllers themselves.
@interface MUPairingController : UIViewController
@property(nonatomic) UILabel* steps; @property(nonatomic) UIImageView* guideIcon;
@property(nonatomic) UILabel* status; @property(nonatomic) UIActivityIndicatorView* spinner; @property(nonatomic) UIImageView* check;
@property(nonatomic) NSTimer* timer;
@property(nonatomic) std::string baseline;
@property(nonatomic, copy) void (^completion)(void);
@end

@implementation MUPairingController
- (UILabel*)text:(NSString*)t size:(CGFloat)size weight:(UIFontWeight)weight alpha:(CGFloat)alpha {
  UILabel* l = [[UILabel alloc] init];
  l.text = t; l.font = [UIFont systemFontOfSize:size weight:weight]; l.textColor = [UIColor colorWithWhite:1 alpha:alpha]; l.numberOfLines = 0;
  return l;
}
- (UIView*)step:(int)number text:(NSString*)text {
  UIStackView* row = [[UIStackView alloc] init]; row.axis = UILayoutConstraintAxisHorizontal; row.spacing = 10; row.alignment = UIStackViewAlignmentFirstBaseline;
  UILabel* n = [self text:[NSString stringWithFormat:@"%d", number] size:17 weight:UIFontWeightHeavy alpha:1]; n.textColor = kYellow();
  [n setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
  [row addArrangedSubview:n]; [row addArrangedSubview:[self text:text size:17 weight:UIFontWeightSemibold alpha:1]];
  return row;
}
- (UIButton*)action:(NSString*)title symbol:(NSString*)symbol prominent:(BOOL)prominent selector:(SEL)selector {
  UIButtonConfiguration* c = [MULauncherController glassConfiguration:prominent];
  c.title = title; c.image = [UIImage systemImageNamed:symbol]; c.imagePadding = 6; c.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
  c.contentInsets = NSDirectionalEdgeInsetsMake(12, 18, 12, 18);
  if (prominent) { c.baseBackgroundColor = kYellow(); c.baseForegroundColor = UIColor.blackColor; } else c.baseForegroundColor = UIColor.whiteColor;
  UIButton* b = [UIButton buttonWithConfiguration:c primaryAction:nil];
  [b addTarget:self action:selector forControlEvents:UIControlEventTouchUpInside];
  return b;
}
- (void)viewDidLoad {
  [super viewDidLoad];
  self.view.backgroundColor = rgb(0.05, 0.06, 0.16);
  UIScrollView* scroll = [[UIScrollView alloc] init]; scroll.translatesAutoresizingMaskIntoConstraints = NO;
  [self.view addSubview:scroll];
  UIStackView* stack = [[UIStackView alloc] init]; stack.axis = UILayoutConstraintAxisVertical; stack.spacing = 12; stack.translatesAutoresizingMaskIntoConstraints = NO;
  [scroll addSubview:stack];
  [NSLayoutConstraint activateConstraints:@[
    [scroll.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor], [scroll.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
    [scroll.leadingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.leadingAnchor], [scroll.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor],
    [stack.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor constant:24], [stack.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor constant:-32],
    [stack.centerXAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.centerXAnchor],
    [stack.widthAnchor constraintLessThanOrEqualToAnchor:scroll.frameLayoutGuide.widthAnchor constant:-48]]];
  // 640 pt wide where there is room, the full width minus margins otherwise; strong enough that wrapping text cannot squeeze it.
  NSLayoutConstraint* readable = [stack.widthAnchor constraintEqualToConstant:640]; readable.priority = 998; readable.active = YES;
  UILabel* title = [[UILabel alloc] init];
  title.text = @"CONNECT A CONTROLLER"; title.font = meleeFont(26, UIFontWeightBold); title.textColor = kYellow(); title.numberOfLines = 0;
  [stack addArrangedSubview:title];
  [stack addArrangedSubview:[self text:@"A wireless controller pairs once. After that it connects by itself whenever you turn it on, and shows up here within a second." size:14 weight:UIFontWeightRegular alpha:0.7]];
  [stack setCustomSpacing:24 afterView:stack.arrangedSubviews.lastObject];

  [stack addArrangedSubview:[self step:1 text:@"Put the controller in pairing mode"]];
  NSMutableArray* names = [NSMutableArray array];
  for (int i = 0; i < host::kPairingGuideCount; ++i) [names addObject:[NSString stringWithUTF8String:host::kPairingGuides[i].name]];
  UISegmentedControl* picker = [[UISegmentedControl alloc] initWithItems:names];
  picker.selectedSegmentIndex = 0;
#if !TARGET_OS_VISION   // visionOS draws its own glass segments; custom tints make the labels unreadable there
  picker.selectedSegmentTintColor = kYellow();
  [picker setTitleTextAttributes:@{NSForegroundColorAttributeName: UIColor.blackColor} forState:UIControlStateSelected];
  [picker setTitleTextAttributes:@{NSForegroundColorAttributeName: UIColor.whiteColor} forState:UIControlStateNormal];
#endif
  [picker addTarget:self action:@selector(guideChanged:) forControlEvents:UIControlEventValueChanged];
  [stack addArrangedSubview:picker];
  UIStackView* guide = [[UIStackView alloc] init]; guide.axis = UILayoutConstraintAxisHorizontal; guide.spacing = 12; guide.alignment = UIStackViewAlignmentTop;
  self.guideIcon = [[UIImageView alloc] init]; self.guideIcon.tintColor = kYellow(); self.guideIcon.contentMode = UIViewContentModeScaleAspectFit;
  self.guideIcon.preferredSymbolConfiguration = [UIImageSymbolConfiguration configurationWithPointSize:24 weight:UIImageSymbolWeightRegular];
  [self.guideIcon.widthAnchor constraintEqualToConstant:32].active = YES;
  self.steps = [self text:@"" size:15 weight:UIFontWeightRegular alpha:0.85];
  [guide addArrangedSubview:self.guideIcon]; [guide addArrangedSubview:self.steps];
  [stack addArrangedSubview:guide];
  [stack setCustomSpacing:24 afterView:guide];

  [stack addArrangedSubview:[self step:2 text:@"Pick it in Bluetooth settings"]];
  [stack addArrangedSubview:[self text:@"Open Settings, tap Bluetooth, and tap the controller under Other Devices." size:15 weight:UIFontWeightRegular alpha:0.85]];
  UIStackView* open = [[UIStackView alloc] init]; open.axis = UILayoutConstraintAxisHorizontal; open.alignment = UIStackViewAlignmentLeading;
  [open addArrangedSubview:[self action:@"Open Settings" symbol:@"arrow.up.forward.app" prominent:NO selector:@selector(openSettings)]];
  [open addArrangedSubview:[[UIView alloc] init]];
  [stack addArrangedSubview:open];
  [stack setCustomSpacing:24 afterView:open];

  [stack addArrangedSubview:[self step:3 text:@"Come back and play"]];
  UIStackView* statusRow = [[UIStackView alloc] init]; statusRow.axis = UILayoutConstraintAxisHorizontal; statusRow.spacing = 10; statusRow.alignment = UIStackViewAlignmentCenter;
  self.spinner = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium]; self.spinner.color = UIColor.whiteColor; [self.spinner startAnimating];
  self.check = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"checkmark.circle.fill"]]; self.check.tintColor = rgb(0.30, 0.85, 0.45); self.check.hidden = YES;
  self.status = [self text:@"Waiting for a controller…" size:16 weight:UIFontWeightMedium alpha:0.9];
  for (UIView* v in @[self.spinner, self.check, self.status]) [statusRow addArrangedSubview:v];
  [stack addArrangedSubview:statusRow];
  [stack setCustomSpacing:28 afterView:statusRow];
  [stack addArrangedSubview:[self text:@"Using a cable? A USB-C controller works the moment you plug it in (iPhone 15 and later, and iPads with USB-C). The on-screen controls hide themselves while a controller is connected. GameCube controllers on an adapter need a Mac." size:13 weight:UIFontWeightRegular alpha:0.6]];
  UIButton* done = [self action:@"Done" symbol:@"checkmark" prominent:YES selector:@selector(finish)];
  [stack addArrangedSubview:done];
  [self guideChanged:picker];
}
- (void)guideChanged:(UISegmentedControl*)sender {
  const host::PairingGuide& g = host::kPairingGuides[MAX(0, MIN(host::kPairingGuideCount - 1, (int)sender.selectedSegmentIndex))];
  self.steps.text = [NSString stringWithUTF8String:g.steps];
  self.guideIcon.image = [UIImage systemImageNamed:[NSString stringWithUTF8String:g.symbol]] ?: [UIImage systemImageNamed:@"gamecontroller"];
}
- (void)openSettings {
  // iOS has no public link to the Bluetooth page; this one opens it where the system allows and Settings otherwise.
  [UIApplication.sharedApplication openURL:[NSURL URLWithString:@"App-Prefs:Bluetooth"] options:@{} completionHandler:^(BOOL ok) {
    if (!ok) [UIApplication.sharedApplication openURL:[NSURL URLWithString:UIApplicationOpenSettingsURLString] options:@{} completionHandler:nil];
  }];
}
- (void)viewDidAppear:(BOOL)animated {
  [super viewDidAppear:animated];
  std::string s;
  for (const host::ControllerInfo& p : host::window_list_controllers()) s += p.guid + ";";
  self.baseline = s;
#if !TARGET_OS_VISION
  [GCController startWirelessControllerDiscoveryWithCompletionHandler:nil];   // MFi pads that support discovery pair without Settings
#endif
  self.timer = [NSTimer timerWithTimeInterval:0.5 target:self selector:@selector(tick) userInfo:nil repeats:YES];
  [NSRunLoop.mainRunLoop addTimer:self.timer forMode:NSRunLoopCommonModes];
}
- (void)viewWillDisappear:(BOOL)animated {
  [super viewWillDisappear:animated];
#if !TARGET_OS_VISION
  [GCController stopWirelessControllerDiscovery];
#endif
  [self.timer invalidate]; self.timer = nil;
  if (self.completion) self.completion();
}
- (void)tick {
  for (const host::ControllerInfo& p : host::window_list_controllers()) {
    if (self.baseline.find(p.guid + ";") != std::string::npos) continue;
    self.baseline += p.guid + ";";
    [self.spinner stopAnimating]; self.spinner.hidden = YES; self.check.hidden = NO;
    self.status.text = [NSString stringWithFormat:@"%@ is connected and ready to play.", [NSString stringWithUTF8String:p.name.c_str()]];
    self.status.textColor = UIColor.whiteColor;
#if !TARGET_OS_VISION
    [[[UINotificationFeedbackGenerator alloc] init] notificationOccurred:UINotificationFeedbackTypeSuccess];
#endif
  }
}
- (void)finish { [self dismissViewControllerAnimated:YES completion:nil]; }
@end

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

// ---- Dashboard
@interface MULauncherController () <UIDocumentPickerDelegate, UITextFieldDelegate>
@property(nonatomic) host::LauncherSettings* settings;
@property(nonatomic) host::Dashboard dashboard;
@property(nonatomic) BOOL done, playPressed, busy;
@property(nonatomic) UIScrollView* scroll;
@property(nonatomic) UIStackView* cardColumns; @property(nonatomic) NSLayoutConstraint* maxWidth;   // two columns of cards when wide
@property(nonatomic) UIStackView* stack;
@property(nonatomic) NSArray<UIView*>* entrance;
@property(nonatomic, copy) NSString* startupError;
// hero
@property(nonatomic) UILabel* playerChip;
// steps
@property(nonatomic) UIView* stepsCard; @property(nonatomic) NSArray<UILabel*>* stepLabels; @property(nonatomic) NSArray<UIImageView*>* stepIcons;
// disc
@property(nonatomic) UILabel* discLabel; @property(nonatomic) UILabel* hintLabel;
// account
@property(nonatomic) UIView* accountCard; @property(nonatomic) UILabel* accountLabel; @property(nonatomic) UIStackView* signInRows; @property(nonatomic) UITextField* emailField; @property(nonatomic) UITextField* passwordField; @property(nonatomic) UIButton* signInButton; @property(nonatomic) UIButton* signOutButton;
// ranked
@property(nonatomic) UIView* rankedCard; @property(nonatomic) UILabel* rankLabel; @property(nonatomic) UILabel* ratingLabel; @property(nonatomic) UILabel* recordLabel; @property(nonatomic) UIView* winBar; @property(nonatomic) NSLayoutConstraint* winBarWidth; @property(nonatomic) UILabel* placementLabel; @property(nonatomic) UILabel* mainsLabel;
// games
@property(nonatomic) UIView* gamesCard; @property(nonatomic) UIStackView* gamesStack;
// controllers
@property(nonatomic) UIStackView* readinessStack; @property(nonatomic) std::string readinessSignature; @property(nonatomic) UIStackView* controllersStack; @property(nonatomic) NSTimer* controllerTimer; @property(nonatomic) NSUInteger controllerCount; @property(nonatomic) std::string controllerSignature;
// display
@property(nonatomic) UILabel* regionLabel;
@property(nonatomic) UISegmentedControl* scaleControl; @property(nonatomic) UISegmentedControl* anisoControl; @property(nonatomic) UISegmentedControl* upscalerControl; @property(nonatomic) UISwitch* vsyncSwitch; @property(nonatomic) UISwitch* widescreenSwitch; @property(nonatomic) UISlider* sharpnessSlider; @property(nonatomic) UISwitch* onlineSwitch; @property(nonatomic) UISegmentedControl* delayControl;
@property(nonatomic) UISlider* overlaySlider; @property(nonatomic) UISlider* overlayScaleSlider;
@property(nonatomic) UIButton* playButton;
@end

@implementation MULauncherController
- (void)viewDidLoad {
  [super viewDidLoad];
  self.view.backgroundColor = rgb(0.03, 0.03, 0.09);
  MUBackdropView* backdrop = [[MUBackdropView alloc] initWithFrame:self.view.bounds];
  [self.view addSubview:backdrop];
  [NSLayoutConstraint activateConstraints:@[[backdrop.topAnchor constraintEqualToAnchor:self.view.topAnchor], [backdrop.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
                                            [backdrop.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor], [backdrop.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor]]];
  self.scroll = [[UIScrollView alloc] init];
  self.scroll.translatesAutoresizingMaskIntoConstraints = NO;
#if !TARGET_OS_VISION
  self.scroll.keyboardDismissMode = UIScrollViewKeyboardDismissModeInteractive;
#endif
  [self.view addSubview:self.scroll];
  self.stack = [[UIStackView alloc] init];
  self.stack.axis = UILayoutConstraintAxisVertical; self.stack.spacing = 16; self.stack.translatesAutoresizingMaskIntoConstraints = NO;
  UIView* content = self.scroll;   // the stack's host: a glass container on iOS 26, so the cards render as one glass pass
#if !TARGET_OS_VISION
  if (@available(iOS 26.0, *)) {
    UIGlassContainerEffect* container = [[UIGlassContainerEffect alloc] init];
    container.spacing = 12;
    UIVisualEffectView* cv = [[UIVisualEffectView alloc] initWithEffect:container];
    cv.translatesAutoresizingMaskIntoConstraints = NO;
    [self.scroll addSubview:cv];
    [NSLayoutConstraint activateConstraints:@[[cv.topAnchor constraintEqualToAnchor:self.scroll.contentLayoutGuide.topAnchor], [cv.bottomAnchor constraintEqualToAnchor:self.scroll.contentLayoutGuide.bottomAnchor],
                                              [cv.leadingAnchor constraintEqualToAnchor:self.scroll.frameLayoutGuide.leadingAnchor], [cv.trailingAnchor constraintEqualToAnchor:self.scroll.frameLayoutGuide.trailingAnchor]]];
    content = cv.contentView;
  }
#endif
  [content addSubview:self.stack];
  [NSLayoutConstraint activateConstraints:@[
    [self.scroll.topAnchor constraintEqualToAnchor:self.view.topAnchor], [self.scroll.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
    [self.scroll.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor], [self.scroll.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
    // Cards are centred on the display itself, not on the safe area, so two columns split evenly around iPhone Duo's fold
    // even when system controls sit along one edge; they still never enter the safe-area insets.
    [self.stack.leadingAnchor constraintGreaterThanOrEqualToAnchor:self.view.safeAreaLayoutGuide.leadingAnchor constant:20],
    [self.stack.trailingAnchor constraintLessThanOrEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor constant:-20],
    [self.stack.topAnchor constraintEqualToAnchor:self.scroll.contentLayoutGuide.topAnchor constant:44], [self.stack.bottomAnchor constraintEqualToAnchor:self.scroll.contentLayoutGuide.bottomAnchor constant:-48],
    [self.stack.centerXAnchor constraintEqualToAnchor:self.scroll.frameLayoutGuide.centerXAnchor]]];
  self.maxWidth = [self.stack.widthAnchor constraintLessThanOrEqualToConstant:640]; self.maxWidth.active = YES;
  NSLayoutConstraint* width = [self.stack.widthAnchor constraintEqualToAnchor:self.scroll.frameLayoutGuide.widthAnchor constant:-40];
  width.priority = UILayoutPriorityDefaultHigh; width.active = YES;
  [self.stack.widthAnchor constraintLessThanOrEqualToAnchor:self.scroll.frameLayoutGuide.widthAnchor constant:-32].active = YES;   // never wider than a phone
  UITapGestureRecognizer* tap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(dismissKeyboard)];
  tap.cancelsTouchesInView = NO; [self.view addGestureRecognizer:tap];

  UIView* hero = [self buildHero];
  self.stepsCard = [self buildSteps];
  UIView* disc = [self buildDisc];
  self.accountCard = [self buildAccount];
  self.rankedCard = [self buildRanked];
  self.gamesCard = [self buildGames];
  UIView* controllers = [self buildControllers];
  UIView* readiness = [self buildReadiness];
  UIView* display = [self buildDisplay];
  UIView* touch = [self buildTouch];
  UIView* regionCard = [self buildRegion];
  self.playButton = [self button:@"PLAY" symbol:@"play.fill" prominent:YES];
  [self.playButton addTarget:self action:@selector(play) forControlEvents:UIControlEventTouchUpInside];
  UILabel* footer = [[UILabel alloc] init];
  footer.text = @"Needs your own Super Smash Bros. Melee NTSC 1.02 disc image. Nothing from the game ships with the app. Unofficial; not affiliated with the Slippi team or Nintendo.";
  footer.font = [UIFont preferredFontForTextStyle:UIFontTextStyleCaption1]; footer.textColor = [UIColor colorWithWhite:1 alpha:0.45]; footer.numberOfLines = 0; footer.textAlignment = NSTextAlignmentCenter;
  UIStackView* leftCards = [[UIStackView alloc] init]; leftCards.axis = UILayoutConstraintAxisVertical; leftCards.spacing = 16;
  UIStackView* rightCards = [[UIStackView alloc] init]; rightCards.axis = UILayoutConstraintAxisVertical; rightCards.spacing = 16;
  for (UIView* v in @[self.stepsCard, self.rankedCard, self.gamesCard, self.accountCard]) [leftCards addArrangedSubview:v];   // you
  for (UIView* v in @[readiness, disc, controllers, display, touch, regionCard]) [rightCards addArrangedSubview:v];                      // the setup
  self.cardColumns = [[UIStackView alloc] initWithArrangedSubviews:@[leftCards, rightCards]];
  self.cardColumns.axis = UILayoutConstraintAxisVertical; self.cardColumns.spacing = 16;
  for (UIView* v in @[hero, self.cardColumns, self.playButton, footer]) [self.stack addArrangedSubview:v];
  [self.stack setCustomSpacing:28 afterView:hero];
  self.entrance = @[hero, self.stepsCard, self.rankedCard, self.gamesCard, self.accountCard, disc, controllers, display, touch, regionCard, self.playButton];
  for (UIView* v in self.entrance) { v.alpha = 0; v.transform = CGAffineTransformMakeTranslation(0, 24); }
  [self refreshDisc]; [self refreshAccount]; [self refreshControllers]; [self refreshSteps]; [self refreshReadiness];
  [self loadDashboard];
  self.controllerTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 target:self selector:@selector(controllerTick) userInfo:nil repeats:YES];
#if !TARGET_OS_VISION
  [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(keyboardChanged:) name:UIKeyboardWillChangeFrameNotification object:nil];
#endif
}
- (void)viewDidAppear:(BOOL)animated {
  [super viewDidAppear:animated];
  NSTimeInterval delay = 0.1;
  for (UIView* v in self.entrance) {
    [UIView animateWithDuration:0.7 delay:delay usingSpringWithDamping:0.82 initialSpringVelocity:0.4 options:UIViewAnimationOptionAllowUserInteraction animations:^{ v.alpha = 1; v.transform = CGAffineTransformIdentity; } completion:nil];
    delay += 0.06;
  }
  if (const char* scroll = std::getenv("MELEE_LAUNCHER_SCROLL"))   // screenshot aid: start scrolled down by N points
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ [self.scroll setContentOffset:CGPointMake(0, std::atof(scroll)) animated:NO]; });
  if (const char* orientation = std::getenv("MELEE_ORIENTATION"))   // screenshot aid: "portrait" or "landscape"
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.3 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
#if !TARGET_OS_VISION
      if (@available(iOS 16.0, *)) {
        const UIInterfaceOrientationMask mask = std::strcmp(orientation, "landscape") == 0 ? UIInterfaceOrientationMaskLandscapeRight : UIInterfaceOrientationMaskPortrait;
        [self.view.window.windowScene requestGeometryUpdateWithPreferences:[[UIWindowSceneGeometryPreferencesIOS alloc] initWithInterfaceOrientations:mask] errorHandler:nil];
      }
#endif
    });
#if TARGET_OS_VISION
  if (const char* size = std::getenv("MELEE_WINDOW_SIZE"))   // screenshot aid: "<width>x<height>" in points
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.3 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
      double w = 0, h = 0;
      if (std::sscanf(size, "%lfx%lf", &w, &h) == 2)
        [self.view.window.windowScene requestGeometryUpdateWithPreferences:[[UIWindowSceneGeometryPreferencesVision alloc] initWithSize:CGSizeMake(w, h)] errorHandler:nil];
    });
#endif
  if (std::getenv("MELEE_TEXT_AUDIT"))   // QA aid: log text that does not fit, after the other aids have opened their screens
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(5.0 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ text_audit(self.view.window, "ios"); });
  if (std::getenv("MELEE_OPEN_PAIRING"))   // screenshot aid: the Connect a Controller screen
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2.0 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ [self connectController]; });
  if (const char* which = std::getenv("MELEE_OPEN_EDITOR"))   // screenshot aid: "pad:<guid>:<name>" opens that controller's editor
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2.0 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
      const std::string w = which;
      if (w.rfind("pad:", 0) != 0) return;
      const size_t colon = w.find(':', 4);
      MURemapController* rm = [[MURemapController alloc] init];
      host::ControllerConfig cfg;
      cfg.guid = w.substr(4, colon == std::string::npos ? std::string::npos : colon - 4);
      if (const host::ControllerConfig* c = host::controller_config_for(cfg.guid.c_str())) cfg = *c;
      rm.config = cfg; rm.controllerName = [NSString stringWithUTF8String:(colon == std::string::npos ? "Controller" : w.substr(colon + 1)).c_str()];
      rm.modalPresentationStyle = UIModalPresentationFormSheet;
      [self presentViewController:rm animated:NO completion:nil];
    });
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.2 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
    CABasicAnimation* breathe = [CABasicAnimation animationWithKeyPath:@"transform.scale"];
    breathe.fromValue = @1.0; breathe.toValue = @1.02; breathe.duration = 1.6; breathe.autoreverses = YES; breathe.repeatCount = HUGE_VALF;
    breathe.timingFunction = [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseInEaseOut];
    [self.playButton.layer addAnimation:breathe forKey:@"breathe"];
  });
}

// ---- building blocks
// A card: Liquid Glass on iOS 26 (tinted toward the backdrop's blue, not interactive: cards are surfaces, buttons respond), material otherwise.
- (UIView*)panel {
  UIVisualEffectView* card;
#if !TARGET_OS_VISION
  if (@available(iOS 26.0, *)) {
    UIGlassEffect* glass = [UIGlassEffect effectWithStyle:UIGlassEffectStyleRegular];
    glass.tintColor = kGlassTint();
    card = [[UIVisualEffectView alloc] initWithEffect:glass];
    card.layer.cornerRadius = 22; card.layer.cornerCurve = kCACornerCurveContinuous; card.clipsToBounds = YES;
    return card;
  }
#endif
  card = [[UIVisualEffectView alloc] initWithEffect:[UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemUltraThinMaterialDark]];
  card.layer.cornerRadius = 18; card.layer.cornerCurve = kCACornerCurveContinuous; card.clipsToBounds = YES;
  card.layer.borderWidth = 1; card.layer.borderColor = rgb(0.5, 0.6, 1.0, 0.14).CGColor;
  return card;
}
// A circular glass disc for the hero mark (interactive: it reacts to touch like a control).
- (UIView*)glassDisc:(CGFloat)size {
  UIView* disc;
#if !TARGET_OS_VISION
  if (@available(iOS 26.0, *)) {
    UIGlassEffect* glass = [UIGlassEffect effectWithStyle:UIGlassEffectStyleRegular];
    glass.tintColor = [kMarkViolet() colorWithAlphaComponent:0.22]; glass.interactive = YES;
    disc = [[UIVisualEffectView alloc] initWithEffect:glass];
  } else
#endif
  {
    disc = [[UIVisualEffectView alloc] initWithEffect:[UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemThinMaterialDark]];
    disc.layer.borderWidth = 1; disc.layer.borderColor = [UIColor colorWithWhite:1 alpha:0.25].CGColor;
  }
  disc.translatesAutoresizingMaskIntoConstraints = NO;
  [disc.widthAnchor constraintEqualToConstant:size].active = YES; [disc.heightAnchor constraintEqualToConstant:size].active = YES;
  disc.layer.cornerRadius = size / 2; disc.clipsToBounds = YES;
  return disc;
}
- (UIStackView*)stackIn:(UIView*)card {
  UIView* host = [card isKindOfClass:UIVisualEffectView.class] ? ((UIVisualEffectView*)card).contentView : card;
  UIStackView* s = [[UIStackView alloc] init];
  s.axis = UILayoutConstraintAxisVertical; s.spacing = 12; s.translatesAutoresizingMaskIntoConstraints = NO;
  [host addSubview:s];
  [NSLayoutConstraint activateConstraints:@[[s.topAnchor constraintEqualToAnchor:host.topAnchor constant:16], [s.bottomAnchor constraintEqualToAnchor:host.bottomAnchor constant:-18],
                                            [s.leadingAnchor constraintEqualToAnchor:host.leadingAnchor constant:18], [s.trailingAnchor constraintEqualToAnchor:host.trailingAnchor constant:-18]]];
  return s;
}
// Melee's angled yellow menu bar as a section header.
- (UIView*)header:(NSString*)text symbol:(NSString*)symbol {
  UIView* bar = [[UIView alloc] init];
  bar.translatesAutoresizingMaskIntoConstraints = NO;
  [bar.heightAnchor constraintEqualToConstant:30].active = YES;
  CAShapeLayer* shape = [CAShapeLayer layer];
  shape.fillColor = kYellow().CGColor;
  bar.layer.mask = nil;
  [bar.layer insertSublayer:shape atIndex:0];
  UILabel* l = [[UILabel alloc] init];
  l.text = text; l.font = meleeFont(15, UIFontWeightBold); l.textColor = rgb(0.10, 0.08, 0.02);
  l.translatesAutoresizingMaskIntoConstraints = NO;
  UIImageView* icon = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:symbol]];
  icon.tintColor = rgb(0.10, 0.08, 0.02); icon.contentMode = UIViewContentModeScaleAspectFit; icon.translatesAutoresizingMaskIntoConstraints = NO;
  icon.preferredSymbolConfiguration = [UIImageSymbolConfiguration configurationWithPointSize:13 weight:UIImageSymbolWeightBold];
  [bar addSubview:icon]; [bar addSubview:l];
  [NSLayoutConstraint activateConstraints:@[[icon.leadingAnchor constraintEqualToAnchor:bar.leadingAnchor constant:16], [icon.centerYAnchor constraintEqualToAnchor:bar.centerYAnchor], [icon.widthAnchor constraintEqualToConstant:18],
                                            [l.leadingAnchor constraintEqualToAnchor:icon.trailingAnchor constant:8], [l.centerYAnchor constraintEqualToAnchor:bar.centerYAnchor], [l.trailingAnchor constraintLessThanOrEqualToAnchor:bar.trailingAnchor constant:-20]]];
  objc_setAssociatedObject(bar, "shape", shape, OBJC_ASSOCIATION_RETAIN);
  return bar;
}
- (void)viewWillLayoutSubviews {
  [super viewWillLayoutSubviews];
  // Wide screens (iPad in landscape, 13-inch iPads, Vision Pro windows) put the cards in two columns; phones and narrow
  // windows keep one readable column in the same order.
  // A regular-width environment with room for two readable columns: iPhone Duo open, iPads, large iPhones in landscape, Vision Pro windows.
  const BOOL wide = self.traitCollection.horizontalSizeClass == UIUserInterfaceSizeClassRegular && self.view.bounds.size.width >= 800;
  if (self.cardColumns && (self.cardColumns.axis == UILayoutConstraintAxisHorizontal) != wide) {
    const BOOL atTop = self.scroll.contentOffset.y <= -self.scroll.adjustedContentInset.top + 1;   // a window resized while showing the top keeps showing the top
    if (atTop) dispatch_async(dispatch_get_main_queue(), ^{ [self.scroll setContentOffset:CGPointMake(0, -self.scroll.adjustedContentInset.top) animated:NO]; });
    self.cardColumns.axis = wide ? UILayoutConstraintAxisHorizontal : UILayoutConstraintAxisVertical;
    self.cardColumns.distribution = wide ? UIStackViewDistributionFillEqually : UIStackViewDistributionFill;
    self.cardColumns.alignment = wide ? UIStackViewAlignmentTop : UIStackViewAlignmentFill;
    self.maxWidth.constant = wide ? 1180 : 640;
  }
}
- (void)viewDidLayoutSubviews {
  [super viewDidLayoutSubviews];
  // Wrapping labels learn their real width after layout; without this a label nested in stacks keeps one line and ends in "…".
  BOOL relayout = NO;
  for (UIView* v in [self allSubviewsOf:self.stack]) {
    if (![v isKindOfClass:UILabel.class]) continue;
    UILabel* l = (UILabel*)v;
    if (l.numberOfLines != 0 || l.bounds.size.width < 1 || fabs(l.preferredMaxLayoutWidth - l.bounds.size.width) < 0.5) continue;
    l.preferredMaxLayoutWidth = l.bounds.size.width; relayout = YES;
  }
  if (relayout) [self.view setNeedsLayout];
  // angled header bars follow their width
  for (UIView* v in [self allSubviewsOf:self.stack]) {
    CAShapeLayer* shape = objc_getAssociatedObject(v, "shape");
    if (!shape) continue;
    const CGFloat w = v.bounds.size.width, h = v.bounds.size.height;
    // Melee's angled bar covers 72% of the card, and always reaches past its title so the words never run off the yellow.
    CGFloat end = w * 0.72;
    for (UIView* sub in v.subviews) if ([sub isKindOfClass:UILabel.class]) end = MAX(end, CGRectGetMaxX(sub.frame) + 14 + h * 0.5);
    end = MIN(end, w);
    UIBezierPath* p = [UIBezierPath bezierPath];
    [p moveToPoint:CGPointMake(0, 0)]; [p addLineToPoint:CGPointMake(end, 0)]; [p addLineToPoint:CGPointMake(end - 14, h)]; [p addLineToPoint:CGPointMake(0, h)]; [p closePath];
    shape.path = p.CGPath;
  }
}
- (NSArray<UIView*>*)allSubviewsOf:(UIView*)v { NSMutableArray* a = [NSMutableArray array]; for (UIView* s in v.subviews) { [a addObject:s]; [a addObjectsFromArray:[self allSubviewsOf:s]]; } return a; }
- (UILabel*)label:(NSString*)text size:(CGFloat)size weight:(UIFontWeight)weight alpha:(CGFloat)alpha {
  UILabel* l = [[UILabel alloc] init];
  l.text = text; l.font = [UIFont systemFontOfSize:size weight:weight]; l.textColor = [UIColor colorWithWhite:1 alpha:alpha]; l.numberOfLines = 0;
  return l;
}
// A slider with its value shown next to it, formatted by `format` (e.g. "%.0f%%" with scale 100).
- (UIView*)sliderRow:(NSString*)text symbol:(NSString*)symbol slider:(UISlider*)slider format:(NSString*)format scale:(float)scale {
  UILabel* value = [self label:@"" size:14 weight:UIFontWeightMedium alpha:0.7];
  value.font = [UIFont monospacedDigitSystemFontOfSize:14 weight:UIFontWeightMedium]; value.textAlignment = NSTextAlignmentRight;
  [value.widthAnchor constraintEqualToConstant:52].active = YES;
  __weak UILabel* weakValue = value;   // the slider retains the action; the action must not retain the slider
  [slider addAction:[UIAction actionWithHandler:^(UIAction* a) { UISlider* sl = (UISlider*)a.sender; weakValue.text = [NSString stringWithFormat:format, sl.value * scale]; }] forControlEvents:UIControlEventValueChanged];
  value.text = [NSString stringWithFormat:format, slider.value * scale];
  UIStackView* pair = [[UIStackView alloc] init]; pair.axis = UILayoutConstraintAxisHorizontal; pair.spacing = 8; pair.alignment = UIStackViewAlignmentCenter;
  [pair addArrangedSubview:slider]; [pair addArrangedSubview:value];
  NSLayoutConstraint* prefer = [slider.widthAnchor constraintEqualToConstant:150]; prefer.priority = UILayoutPriorityDefaultLow; prefer.active = YES;   // gives way on narrow phones
  [slider.widthAnchor constraintGreaterThanOrEqualToConstant:90].active = YES;
  return [self row:text symbol:symbol control:pair];
}
- (UIView*)row:(NSString*)text symbol:(NSString*)symbol control:(UIView*)control {
  UIStackView* row = [[UIStackView alloc] init];
  row.axis = UILayoutConstraintAxisHorizontal; row.spacing = 12; row.alignment = UIStackViewAlignmentCenter;
  UIImageView* icon = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:symbol]];
  icon.tintColor = kYellow(); icon.contentMode = UIViewContentModeScaleAspectFit;
  icon.preferredSymbolConfiguration = [UIImageSymbolConfiguration configurationWithPointSize:15 weight:UIImageSymbolWeightMedium];
  [icon.widthAnchor constraintEqualToConstant:24].active = YES; [icon setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
  UILabel* l = [self label:text size:16 weight:UIFontWeightRegular alpha:1];
  [l setContentHuggingPriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
  [l setContentCompressionResistancePriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];   // on a phone the label wraps; the switch keeps its size
  keep_words_whole(l);                                                                                                // ...but only between words
  [control setContentCompressionResistancePriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
  [row addArrangedSubview:icon]; [row addArrangedSubview:l]; [row addArrangedSubview:control];
  if ([control isKindOfClass:UISlider.class]) {   // sliders shrink on narrow phones before any label does
    NSLayoutConstraint* prefer = [control.widthAnchor constraintEqualToConstant:170]; prefer.priority = UILayoutPriorityDefaultLow; prefer.active = YES;
    [control.widthAnchor constraintGreaterThanOrEqualToConstant:90].active = YES;
  }
  return row;
}
// Buttons are glass on iOS 26 (prominent glass for Play, tinted Melee yellow); filled/gray otherwise.
+ (UIButtonConfiguration*)glassConfiguration:(BOOL)prominent {
#if !TARGET_OS_VISION
  if (@available(iOS 26.0, *)) return prominent ? [UIButtonConfiguration prominentGlassButtonConfiguration] : [UIButtonConfiguration glassButtonConfiguration];
#endif
  return prominent ? [UIButtonConfiguration filledButtonConfiguration] : [UIButtonConfiguration grayButtonConfiguration];
}
- (UIButton*)button:(NSString*)title symbol:(NSString*)symbol prominent:(BOOL)prominent {
  UIButtonConfiguration* c = [MULauncherController glassConfiguration:prominent];
  c.title = title; c.cornerStyle = UIButtonConfigurationCornerStyleCapsule; c.image = [UIImage systemImageNamed:symbol]; c.imagePadding = 8;
  c.preferredSymbolConfigurationForImage = [UIImageSymbolConfiguration configurationWithPointSize:16 weight:UIImageSymbolWeightBold];
  c.contentInsets = NSDirectionalEdgeInsetsMake(prominent ? 18 : 12, 20, prominent ? 18 : 12, 20);
  if (prominent) { c.baseBackgroundColor = kYellow(); c.baseForegroundColor = rgb(0.1, 0.08, 0.02); } else c.baseForegroundColor = UIColor.whiteColor;
  UIButton* b = [UIButton buttonWithConfiguration:c primaryAction:nil];
  b.titleLabel.font = prominent ? meleeFont(22, UIFontWeightBold) : [UIFont systemFontOfSize:16 weight:UIFontWeightSemibold];
  if (prominent && !glass_available()) { b.layer.shadowColor = kYellow().CGColor; b.layer.shadowOpacity = 0.45; b.layer.shadowRadius = 18; b.layer.shadowOffset = CGSizeMake(0, 6); }
  return b;
}
- (UITextField*)field:(NSString*)placeholder symbol:(NSString*)symbol secure:(BOOL)secure {
  UITextField* f = [[UITextField alloc] init];
  f.attributedPlaceholder = [[NSAttributedString alloc] initWithString:placeholder attributes:@{NSForegroundColorAttributeName: [UIColor colorWithWhite:1 alpha:0.35]}];
  f.textColor = UIColor.whiteColor; f.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
  f.backgroundColor = [UIColor colorWithWhite:1 alpha:0.08]; f.layer.cornerRadius = 12; f.layer.cornerCurve = kCACornerCurveContinuous;
  f.secureTextEntry = secure; f.autocapitalizationType = UITextAutocapitalizationTypeNone; f.autocorrectionType = UITextAutocorrectionTypeNo; f.delegate = self;
  UIImageView* icon = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:symbol]];
  icon.tintColor = [UIColor colorWithWhite:1 alpha:0.5]; icon.contentMode = UIViewContentModeCenter; icon.frame = CGRectMake(0, 0, 40, 46);
  f.leftView = icon; f.leftViewMode = UITextFieldViewModeAlways;
  [f.heightAnchor constraintEqualToConstant:46].active = YES;
  return f;
}
- (UISegmentedControl*)segments:(NSArray<NSString*>*)items selected:(NSInteger)index {
  UISegmentedControl* s = [[UISegmentedControl alloc] initWithItems:items];
  s.selectedSegmentIndex = index; s.selectedSegmentTintColor = kYellow();
  [s setTitleTextAttributes:@{NSForegroundColorAttributeName: UIColor.whiteColor} forState:UIControlStateNormal];
  [s setTitleTextAttributes:@{NSForegroundColorAttributeName: rgb(0.1, 0.08, 0.02)} forState:UIControlStateSelected];
  return s;
}

// ---- sections
- (UIView*)buildHero {
  UIStackView* hero = [[UIStackView alloc] init];
  hero.axis = UILayoutConstraintAxisVertical; hero.alignment = UIStackViewAlignmentCenter; hero.spacing = 8;
  if (UIImage* mark = slippi_mark()) {
    UIView* disc = [self glassDisc:132];
    UIImageView* iv = [[UIImageView alloc] initWithImage:mark];
    iv.tintColor = UIColor.whiteColor; iv.contentMode = UIViewContentModeScaleAspectFit; iv.translatesAutoresizingMaskIntoConstraints = NO;
    UIView* host = [disc isKindOfClass:UIVisualEffectView.class] ? ((UIVisualEffectView*)disc).contentView : disc;
    [host addSubview:iv];
    [NSLayoutConstraint activateConstraints:@[[iv.centerXAnchor constraintEqualToAnchor:host.centerXAnchor], [iv.centerYAnchor constraintEqualToAnchor:host.centerYAnchor],
                                              [iv.widthAnchor constraintEqualToConstant:84], [iv.heightAnchor constraintEqualToConstant:84]]];
    CABasicAnimation* breathe = [CABasicAnimation animationWithKeyPath:@"transform.scale"];
    breathe.fromValue = @1.0; breathe.toValue = @1.05; breathe.duration = 2.4; breathe.autoreverses = YES; breathe.repeatCount = HUGE_VALF;
    breathe.timingFunction = [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseInEaseOut];
    [iv.layer addAnimation:breathe forKey:@"breathe"];
    [hero addArrangedSubview:disc];
  } else {
    [hero addArrangedSubview:[[MUHeroView alloc] initWithSize:120]];
  }
  UILabel* title = [[UILabel alloc] init];
  title.text = @"Dashdance"; title.font = meleeFont(52, UIFontWeightBlack); title.textColor = UIColor.whiteColor;
  title.layer.shadowColor = kYellow().CGColor; title.layer.shadowOpacity = 0.5; title.layer.shadowRadius = 14; title.layer.shadowOffset = CGSizeZero;
  UILabel* sub = [self label:@"SUPER SMASH BROS. MELEE  ·  SLIPPI ONLINE  ·  NATIVE" size:12 weight:UIFontWeightSemibold alpha:0.6];
  // The player badge: a yellow capsule with real padding. Long names shrink the text slightly, then truncate, and the
  // capsule never grows past the screen, so letters never run into its rounded ends.
  UIView* chipWrap = [[UIView alloc] init];
  chipWrap.backgroundColor = kYellow(); chipWrap.layer.cornerRadius = 16; chipWrap.layer.cornerCurve = kCACornerCurveContinuous;
  self.playerChip = [[UILabel alloc] init];
  self.playerChip.font = [UIFont systemFontOfSize:15 weight:UIFontWeightSemibold]; self.playerChip.textColor = rgb(0.1, 0.08, 0.02); self.playerChip.textAlignment = NSTextAlignmentCenter;
  self.playerChip.adjustsFontSizeToFitWidth = YES; self.playerChip.minimumScaleFactor = 0.8; self.playerChip.lineBreakMode = NSLineBreakByTruncatingMiddle;
  self.playerChip.translatesAutoresizingMaskIntoConstraints = NO;
  [chipWrap addSubview:self.playerChip];
  [NSLayoutConstraint activateConstraints:@[[chipWrap.heightAnchor constraintEqualToConstant:32],
                                            [self.playerChip.centerYAnchor constraintEqualToAnchor:chipWrap.centerYAnchor],
                                            [self.playerChip.leadingAnchor constraintEqualToAnchor:chipWrap.leadingAnchor constant:18], [self.playerChip.trailingAnchor constraintEqualToAnchor:chipWrap.trailingAnchor constant:-18]]];
  [chipWrap.widthAnchor constraintLessThanOrEqualToConstant:600].active = YES;
  chipWrap.hidden = YES;
  objc_setAssociatedObject(self.playerChip, "badge", chipWrap, OBJC_ASSOCIATION_ASSIGN);
  [hero addArrangedSubview:title]; [hero addArrangedSubview:sub]; [hero addArrangedSubview:chipWrap];
  [chipWrap.widthAnchor constraintLessThanOrEqualToAnchor:hero.widthAnchor].active = YES;   // a long name shrinks or truncates inside the capsule instead of widening it
  [hero setCustomSpacing:14 afterView:sub];
  return hero;
}
- (UIView*)buildSteps {
  UIView* card = [self panel];
  UIStackView* s = [self stackIn:card];
  [s addArrangedSubview:[self header:@"GET STARTED" symbol:@"flag.checkered"]];
  NSArray* names = @[@"Add your Melee disc image", @"Sign in to Slippi Online", @"Connect a controller (touch works too)", @"Press Play"];
  NSMutableArray* labels = [NSMutableArray array]; NSMutableArray* icons = [NSMutableArray array];
  for (NSString* n in names) {
    UIStackView* row = [[UIStackView alloc] init]; row.axis = UILayoutConstraintAxisHorizontal; row.spacing = 12; row.alignment = UIStackViewAlignmentCenter;
    UIImageView* icon = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"circle"]];
    icon.tintColor = [UIColor colorWithWhite:1 alpha:0.4]; icon.contentMode = UIViewContentModeScaleAspectFit;
    icon.preferredSymbolConfiguration = [UIImageSymbolConfiguration configurationWithPointSize:18 weight:UIImageSymbolWeightSemibold];
    [icon.widthAnchor constraintEqualToConstant:24].active = YES; [icon setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    UILabel* l = [self label:n size:16 weight:UIFontWeightRegular alpha:0.9];
    [row addArrangedSubview:icon]; [row addArrangedSubview:l]; [s addArrangedSubview:row];
    [labels addObject:l]; [icons addObject:icon];
  }
  self.stepLabels = labels; self.stepIcons = icons;
  return card;
}
- (UIView*)buildDisc {
  UIView* card = [self panel];
  UIStackView* s = [self stackIn:card];
  [s addArrangedSubview:[self header:@"GAME DISC" symbol:@"opticaldisc"]];
  self.discLabel = [self label:@"" size:20 weight:UIFontWeightSemibold alpha:1];
  self.hintLabel = [self label:@"" size:13 weight:UIFontWeightRegular alpha:0.6];
  UIButton* import_ = [self button:@"Import Disc Image" symbol:@"square.and.arrow.down" prominent:NO];
  [import_ addTarget:self action:@selector(importDisc) forControlEvents:UIControlEventTouchUpInside];
  [s addArrangedSubview:self.discLabel]; [s addArrangedSubview:self.hintLabel]; [s addArrangedSubview:import_];
  return card;
}
- (UIView*)buildAccount {
  UIView* card = [self panel];
  UIStackView* s = [self stackIn:card];
  [s addArrangedSubview:[self header:@"SLIPPI ONLINE ACCOUNT" symbol:@"person.crop.circle"]];
  self.accountLabel = [self label:@"" size:16 weight:UIFontWeightRegular alpha:1];
  [s addArrangedSubview:self.accountLabel];
  self.signInRows = [[UIStackView alloc] init]; self.signInRows.axis = UILayoutConstraintAxisVertical; self.signInRows.spacing = 10;
  self.emailField = [self field:@"Email" symbol:@"envelope" secure:NO]; self.emailField.keyboardType = UIKeyboardTypeEmailAddress; self.emailField.textContentType = UITextContentTypeUsername; self.emailField.returnKeyType = UIReturnKeyNext;
  self.passwordField = [self field:@"Password" symbol:@"key" secure:YES]; self.passwordField.textContentType = UITextContentTypePassword; self.passwordField.returnKeyType = UIReturnKeyGo;
  self.signInButton = [self button:@"Sign In" symbol:@"person.badge.key" prominent:NO];
  [self.signInButton addTarget:self action:@selector(signIn) forControlEvents:UIControlEventTouchUpInside];
  UIButton* reset = [UIButton buttonWithType:UIButtonTypeSystem];
  [reset setTitle:@"Forgot password · Create an account at slippi.gg" forState:UIControlStateNormal];
  reset.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote]; reset.tintColor = kYellow();
  [reset addTarget:self action:@selector(forgotPassword) forControlEvents:UIControlEventTouchUpInside];
  for (UIView* v in @[self.emailField, self.passwordField, self.signInButton, reset]) [self.signInRows addArrangedSubview:v];
  [s addArrangedSubview:self.signInRows];
  self.signOutButton = [self button:@"Sign Out" symbol:@"rectangle.portrait.and.arrow.right" prominent:NO];
  [self.signOutButton addTarget:self action:@selector(signOut) forControlEvents:UIControlEventTouchUpInside];
  [s addArrangedSubview:self.signOutButton];
  return card;
}
- (UIView*)buildRanked {
  UIView* card = [self panel];
  ((UIView*)card).layer.borderColor = rgb(0.97, 0.79, 0.28, 0.35).CGColor;
  UIStackView* s = [self stackIn:card];
  [s addArrangedSubview:[self header:@"RANKED" symbol:@"trophy"]];
  UIStackView* top = [[UIStackView alloc] init]; top.axis = UILayoutConstraintAxisHorizontal; top.alignment = UIStackViewAlignmentFirstBaseline; top.spacing = 12;
  self.rankLabel = [[UILabel alloc] init]; self.rankLabel.font = meleeFont(34, UIFontWeightBlack); self.rankLabel.textColor = kYellow();
  self.ratingLabel = [[UILabel alloc] init]; self.ratingLabel.font = [UIFont monospacedDigitSystemFontOfSize:22 weight:UIFontWeightSemibold]; self.ratingLabel.textColor = UIColor.whiteColor;
  UIView* spacer = [[UIView alloc] init]; [spacer setContentHuggingPriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
  [top addArrangedSubview:self.rankLabel]; [top addArrangedSubview:spacer]; [top addArrangedSubview:self.ratingLabel];
  self.recordLabel = [self label:@"" size:15 weight:UIFontWeightMedium alpha:0.9];
  UIView* track = [[UIView alloc] init]; track.backgroundColor = [UIColor colorWithWhite:1 alpha:0.12]; track.layer.cornerRadius = 4; track.clipsToBounds = YES;
  [track.heightAnchor constraintEqualToConstant:8].active = YES;
  self.winBar = [[UIView alloc] init]; self.winBar.backgroundColor = rgb(0.30, 0.85, 0.45); self.winBar.translatesAutoresizingMaskIntoConstraints = NO;
  [track addSubview:self.winBar];
  self.winBarWidth = [self.winBar.widthAnchor constraintEqualToAnchor:track.widthAnchor multiplier:0.0];
  [NSLayoutConstraint activateConstraints:@[[self.winBar.leadingAnchor constraintEqualToAnchor:track.leadingAnchor], [self.winBar.topAnchor constraintEqualToAnchor:track.topAnchor], [self.winBar.bottomAnchor constraintEqualToAnchor:track.bottomAnchor], self.winBarWidth]];
  self.placementLabel = [self label:@"" size:14 weight:UIFontWeightRegular alpha:0.7];
  self.mainsLabel = [self label:@"" size:14 weight:UIFontWeightRegular alpha:0.85];
  for (UIView* v in @[top, self.recordLabel, track, self.placementLabel, self.mainsLabel]) [s addArrangedSubview:v];
  return card;
}
- (UIView*)buildGames {
  UIView* card = [self panel];
  UIStackView* s = [self stackIn:card];
  [s addArrangedSubview:[self header:@"RECENT GAMES" symbol:@"clock.arrow.circlepath"]];
  self.gamesStack = [[UIStackView alloc] init]; self.gamesStack.axis = UILayoutConstraintAxisVertical; self.gamesStack.spacing = 8;
  [s addArrangedSubview:self.gamesStack];
  return card;
}
- (UIView*)buildControllers {
  UIView* card = [self panel];
  UIStackView* s = [self stackIn:card];
  [s addArrangedSubview:[self header:@"CONTROLLERS" symbol:@"gamecontroller"]];
  self.controllersStack = [[UIStackView alloc] init]; self.controllersStack.axis = UILayoutConstraintAxisVertical; self.controllersStack.spacing = 10;
  [s addArrangedSubview:self.controllersStack];
  UIButton* connect = [self button:@"Connect a Controller" symbol:@"dot.radiowaves.left.and.right" prominent:NO];
  [connect addTarget:self action:@selector(connectController) forControlEvents:UIControlEventTouchUpInside];
  [s addArrangedSubview:connect];
  UILabel* help = [self label:@"Connect a Controller walks you through pairing a PlayStation, Xbox, Switch Pro or other Bluetooth controller; USB-C controllers work as soon as they are plugged in. Connected controllers appear here with their measured report rate, a GameCube port and Configure. The on-screen controller hides itself while a controller is connected. GameCube adapters need a Mac: iOS does not give apps raw USB access." size:13 weight:UIFontWeightRegular alpha:0.6];
  [s addArrangedSubview:help];
  return card;
}
- (UIView*)buildReadiness {
  UIView* card = [self panel];
  UIStackView* s = [self stackIn:card];
  [s addArrangedSubview:[self header:@"READY TO COMPETE" symbol:@"trophy"]];
  self.readinessStack = [[UIStackView alloc] init]; self.readinessStack.axis = UILayoutConstraintAxisVertical; self.readinessStack.spacing = 8;
  [s addArrangedSubview:self.readinessStack];
  return card;
}
// What in this setup costs latency, checked about once a second (display, network, controller, audio, delay, heat).
- (void)refreshReadiness {
  if (!self.readinessStack) return;
  const int delay = self.delayControl ? [self selectedDelay] : self.settings->online_delay;
  const std::vector<host::ReadinessItem> items = host::competitive_readiness(display_max_hz(), false, delay);
  std::string sig;
  for (const host::ReadinessItem& i : items) sig += (i.ok ? "1" : "0") + i.text + ";";
  if (sig == self.readinessSignature) return;
  self.readinessSignature = sig;
  for (UIView* v in self.readinessStack.arrangedSubviews) [v removeFromSuperview];
  for (const host::ReadinessItem& i : items) {
    UIStackView* row = [[UIStackView alloc] init]; row.axis = UILayoutConstraintAxisHorizontal; row.spacing = 10; row.alignment = UIStackViewAlignmentFirstBaseline;
    UIImageView* icon = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:i.ok ? @"checkmark.circle.fill" : @"exclamationmark.triangle.fill"]];
    icon.tintColor = i.ok ? rgb(0.30, 0.85, 0.45) : kYellow(); icon.contentMode = UIViewContentModeScaleAspectFit;
    icon.preferredSymbolConfiguration = [UIImageSymbolConfiguration configurationWithPointSize:14 weight:UIImageSymbolWeightSemibold];
    [icon.widthAnchor constraintEqualToConstant:20].active = YES; [icon setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    UILabel* text = [self label:ns(i.text) size:14 weight:UIFontWeightRegular alpha:i.ok ? 0.75 : 0.95];
    text.lineBreakMode = NSLineBreakByWordWrapping;
    [text setContentCompressionResistancePriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisVertical];
    [row addArrangedSubview:icon]; [row addArrangedSubview:text];
    [self.readinessStack addArrangedSubview:row];
  }
}
- (UIView*)buildDisplay {
  UIView* card = [self panel];
  UIStackView* s = [self stackIn:card];
  [s addArrangedSubview:[self header:@"DISPLAY & PERFORMANCE" symbol:@"speedometer"]];
  id<MTLDevice> gpu = MTLCreateSystemDefaultDevice();
  NSString* info = [NSString stringWithFormat:@"%@  ·  %d Hz display  ·  thermal %s  ·  60 Hz simulation, frames shown on the next refresh", gpu ? gpu.name : @"Metal", display_max_hz(), host::thermal_state_name()];
  UILabel* infoLabel = [self label:info size:13 weight:UIFontWeightRegular alpha:0.7];
  [s addArrangedSubview:infoLabel];
  const int scales[] = {0, 1, 2, 3, 4, 6, 8}; NSInteger scaleIndex = 0;
  for (int i = 0; i < 7; ++i) if (scales[i] == self.settings->scale) scaleIndex = i;
  self.scaleControl = [self segments:@[@"Auto", @"1×", @"2×", @"3×", @"4×", @"6×", @"8×"] selected:scaleIndex];
  [s addArrangedSubview:[self label:@"Internal resolution" size:14 weight:UIFontWeightMedium alpha:0.8]];
  [s addArrangedSubview:self.scaleControl];
  self.anisoControl = [self segments:@[@"Off", @"4×", @"16×"] selected:self.settings->anisotropy >= 16 ? 2 : self.settings->anisotropy >= 4 ? 1 : 0];
  [s addArrangedSubview:[self row:@"Anisotropic filtering" symbol:@"square.stack.3d.up" control:self.anisoControl]];
  self.upscalerControl = [self segments:@[@"Off", @"MetalFX", @"MetalFX+"] selected:MAX(0, MIN(2, self.settings->upscaler))];
  [s addArrangedSubview:[self row:@"MetalFX upscaling (renders at half resolution, reconstructs)" symbol:@"wand.and.rays" control:self.upscalerControl]];
  self.vsyncSwitch = [[UISwitch alloc] init]; self.vsyncSwitch.on = self.settings->vsync; self.vsyncSwitch.onTintColor = kYellow();
  [s addArrangedSubview:[self row:@"Display sync (off = lowest latency, may tear)" symbol:@"waveform.path" control:self.vsyncSwitch]];
  self.widescreenSwitch = [[UISwitch alloc] init]; self.widescreenSwitch.on = self.settings->widescreen; self.widescreenSwitch.onTintColor = kYellow();
  [s addArrangedSubview:[self row:@"Widescreen (16:9)" symbol:@"rectangle.ratio.16.to.9" control:self.widescreenSwitch]];
  self.sharpnessSlider = [[UISlider alloc] init]; self.sharpnessSlider.value = self.settings->sharpness; self.sharpnessSlider.tintColor = kYellow();
  [s addArrangedSubview:[self sliderRow:@"Sharpen" symbol:@"sparkles" slider:self.sharpnessSlider format:@"%.0f%%" scale:100]];
  self.onlineSwitch = [[UISwitch alloc] init]; self.onlineSwitch.on = self.settings->online; self.onlineSwitch.onTintColor = kYellow();
  [s addArrangedSubview:[self row:@"Slippi Online services" symbol:@"network" control:self.onlineSwitch]];
  NSMutableArray<NSString*>* delays = [@[@"1", @"2", @"3", @"4"] mutableCopy];   // plus the exact value when a larger one was set in the in-game menu
  if (self.settings->online_delay > 4) [delays addObject:[NSString stringWithFormat:@"%d", self.settings->online_delay]];
  self.delayControl = [self segments:delays selected:self.settings->online_delay > 4 ? 4 : MAX(0, self.settings->online_delay - 1)];
  [s addArrangedSubview:[self row:@"Online input delay (frames)" symbol:@"timer" control:self.delayControl]];
  [s addArrangedSubview:[self label:@"Each frame of delay adds 16.7 ms. 1 is the lowest latency on a stable, nearby connection; 2 is Slippi's default and rolls back less on Wi-Fi. For online play a USB-C Ethernet adapter beats Wi-Fi." size:12 weight:UIFontWeightRegular alpha:0.6]];
  UIButton* preset = [self button:@"Competitive preset" symbol:@"bolt.fill" prominent:NO];
  [preset addTarget:self action:@selector(applyCompetitivePreset) forControlEvents:UIControlEventTouchUpInside];
  // Button on its own line with its explanation underneath: the title never hyphenates on a narrow phone.
  UIStackView* presetRow = [[UIStackView alloc] init]; presetRow.axis = UILayoutConstraintAxisVertical; presetRow.spacing = 6; presetRow.alignment = UIStackViewAlignmentLeading;
  [presetRow addArrangedSubview:preset]; [presetRow addArrangedSubview:[self label:@"2× resolution (lowest latency that still looks crisp), 16× filtering, display sync on, 4:3, no sharpening." size:12 weight:UIFontWeightRegular alpha:0.6]];
  [s addArrangedSubview:presetRow];
  return card;
}
- (UIView*)buildTouch {
  UIView* card = [self panel];
  UIStackView* s = [self stackIn:card];
  [s addArrangedSubview:[self header:@"ON-SCREEN CONTROLS" symbol:@"hand.tap"]];
  self.overlaySlider = [[UISlider alloc] init]; self.overlaySlider.value = self.settings->overlay_opacity; self.overlaySlider.tintColor = kYellow();
  [s addArrangedSubview:[self sliderRow:@"Opacity" symbol:@"circle.lefthalf.filled" slider:self.overlaySlider format:@"%.0f%%" scale:100]];
  self.overlayScaleSlider = [[UISlider alloc] init]; self.overlayScaleSlider.minimumValue = 0.7; self.overlayScaleSlider.maximumValue = 1.4; self.overlayScaleSlider.value = self.settings->overlay_scale; self.overlayScaleSlider.tintColor = kYellow();
  [s addArrangedSubview:[self sliderRow:@"Size" symbol:@"arrow.up.left.and.arrow.down.right" slider:self.overlayScaleSlider format:@"%.2f×" scale:1]];
  return card;
}

- (UIView*)buildRegion {
  UIView* card = [self panel];
  UIStackView* s = [self stackIn:card];
  [s addArrangedSubview:[self header:@"MATCHMAKING REGION" symbol:@"globe"]];
  self.regionLabel = [self label:@"Checking your public IPv4 address…" size:15 weight:UIFontWeightMedium alpha:1];
  [s addArrangedSubview:self.regionLabel];
  [s addArrangedSubview:[self label:@"Slippi's matchmaking places you by the region of this address, looked up at ipgeolocation.io. If the lookup lands far from you, you get matched far from home. Check it; if it is wrong, send ipgeolocation the correction request (copied with your address filled in)." size:12 weight:UIFontWeightRegular alpha:0.6]];
  UIButton* check = [self button:@"Check my region" symbol:@"location.magnifyingglass" prominent:NO];
  [check addTarget:self action:@selector(openRegionCheck) forControlEvents:UIControlEventTouchUpInside];
  UIButton* copy = [self button:@"Copy correction request" symbol:@"doc.on.doc" prominent:NO];
  [copy addTarget:self action:@selector(copyRegionReport) forControlEvents:UIControlEventTouchUpInside];
  [s addArrangedSubview:check]; [s addArrangedSubview:copy];
  return card;
}
- (void)refreshRegion {
  const host::Dashboard& d = self.dashboard;
  if (!d.public_ipv4.empty()) self.regionLabel.text = [NSString stringWithFormat:@"Your public IPv4 address: %s", d.public_ipv4.c_str()];
  else if (!d.ipv4_error.empty()) self.regionLabel.text = [NSString stringWithFormat:@"Could not determine your public IPv4 address (%s).", d.ipv4_error.c_str()];
}
- (void)openRegionCheck { [UIApplication.sharedApplication openURL:[NSURL URLWithString:@"https://ipgeolocation.io/what-is-my-ip/"] options:@{} completionHandler:nil]; }
- (void)copyRegionReport {
  const std::string ip = self.dashboard.public_ipv4.empty() ? "<insert IP here>" : self.dashboard.public_ipv4;
  UIPasteboard.generalPasteboard.string = [NSString stringWithFormat:@"Hi,\nYour service reports my IP (%s) to be at <X location>, but I'm actually located at <Y location>. Could you correct this?\nThank you.", ip.c_str()];
  haptic_notify(true);
  self.regionLabel.text = [NSString stringWithFormat:@"Copied a correction request for %s. Paste it into the ipgeolocation contact form (ipgeolocation.io/contact.html).", ip.c_str()];
}

// ---- state
- (void)refreshSteps {
  const BOOL disc = !self.settings->iso.empty(), account = self.dashboard.signed_in, pad = self.controllerCount > 0;
  const BOOL states[4] = {disc, account, pad, NO};
  BOOL allDone = disc && account;
  for (int i = 0; i < 4; ++i) {
    UIImageView* icon = self.stepIcons[i];
    icon.image = [UIImage systemImageNamed:states[i] ? @"checkmark.circle.fill" : (i == 2 ? @"circle.dashed" : @"circle")];
    icon.tintColor = states[i] ? rgb(0.30, 0.85, 0.45) : [UIColor colorWithWhite:1 alpha:0.4];
    self.stepLabels[i].alpha = states[i] ? 0.5 : 0.95;
  }
  self.stepsCard.hidden = allDone;
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
    self.accountLabel.text = [NSString stringWithFormat:@"Signed in as %s  (%s). Ranked stats and your connect code stay saved on this device.", account.display_name.c_str(), account.connect_code.c_str()];
    self.playerChip.text = [NSString stringWithFormat:@"%s  %s", account.display_name.c_str(), account.connect_code.c_str()];
    ((UIView*)objc_getAssociatedObject(self.playerChip, "badge")).hidden = NO;
  } else {
    self.settings->account_name.clear(); self.settings->account_code.clear();
    self.accountLabel.text = @"Sign in with your Slippi account to play online and see your ranked stats. Offline play works without it.";
    ((UIView*)objc_getAssociatedObject(self.playerChip, "badge")).hidden = YES;
  }
  self.signInRows.hidden = signed_in; self.signOutButton.hidden = !signed_in;
  self.rankedCard.hidden = !signed_in;
  [self refreshRanked]; [self refreshSteps];
}
- (void)refreshRanked {
  const host::Dashboard& d = self.dashboard;
  if (self.settings) { self.settings->rank = d.profile_loaded && d.profile.ranked ? d.rank() : ""; self.settings->rating = d.profile.rating; }
  self.rankLabel.text = ns(d.rank());
  self.ratingLabel.text = ns(d.rating());
  self.recordLabel.text = d.profile_loaded ? ns(d.record()) : (d.profile_error.empty() ? @"Loading ranked profile…" : ns(d.profile_error));
  self.winBarWidth.active = NO;
  self.winBarWidth = [self.winBar.widthAnchor constraintEqualToAnchor:self.winBar.superview.widthAnchor multiplier:MAX(0.0, MIN(1.0, d.win_rate()))];
  self.winBarWidth.active = YES;
  self.placementLabel.text = ns(d.placement());
  std::string mains;
  for (const std::string& m : d.mains()) mains += (mains.empty() ? "Mains: " : "   ") + m;
  self.mainsLabel.text = ns(mains);
  self.placementLabel.hidden = self.placementLabel.text.length == 0; self.mainsLabel.hidden = mains.empty();
  if (d.profile_loaded && d.profile.ranked) self.playerChip.text = [NSString stringWithFormat:@"%s  %s  ·  %s", d.name.c_str(), d.code.c_str(), d.rank().c_str()];
}
- (void)refreshGames {
  for (UIView* v in self.gamesStack.arrangedSubviews) [v removeFromSuperview];
  std::vector<host::GameRow> rows = self.dashboard.rows();
  self.gamesCard.hidden = rows.empty();
  for (const host::GameRow& r : rows) {
    UIStackView* row = [[UIStackView alloc] init]; row.axis = UILayoutConstraintAxisHorizontal; row.spacing = 10; row.alignment = UIStackViewAlignmentCenter;
    UIStackView* text = [[UIStackView alloc] init]; text.axis = UILayoutConstraintAxisVertical; text.spacing = 2;
    // Long names wrap onto a second line instead of ending in "…": the labels may grow taller, never get cut.
    UILabel* title = [self label:ns(r.title) size:15 weight:UIFontWeightSemibold alpha:1];
    UILabel* subtitle = [self label:ns(r.subtitle) size:12 weight:UIFontWeightRegular alpha:0.6];
    for (UILabel* l in @[title, subtitle]) {
      l.lineBreakMode = NSLineBreakByWordWrapping;
      [l setContentCompressionResistancePriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisVertical];
      [text addArrangedSubview:l];
    }
    [text setContentCompressionResistancePriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
    UILabel* result = [[UILabel alloc] init];
    result.text = ns(r.result); result.font = meleeFont(14, UIFontWeightBold); result.textAlignment = NSTextAlignmentCenter;
    result.textColor = r.win ? rgb(0.30, 0.85, 0.45) : r.loss ? kRed() : [UIColor colorWithWhite:1 alpha:0.6];
    [result setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    [row addArrangedSubview:text]; [row addArrangedSubview:result];
    [self.gamesStack addArrangedSubview:row];
  }
}
- (void)refreshControllers {
  std::vector<host::ControllerInfo> pads = host::window_list_controllers();
  self.controllerCount = pads.size();
  for (UIView* v in self.controllersStack.arrangedSubviews) [v removeFromSuperview];
  if (pads.empty()) {
    [self.controllersStack addArrangedSubview:[self label:@"No controller connected. Touch controls are ready." size:15 weight:UIFontWeightRegular alpha:0.7]];
  }
  for (size_t i = 0; i < pads.size(); ++i) {
    const host::ControllerInfo& pad = pads[i];
    UIStackView* row = [[UIStackView alloc] init]; row.axis = UILayoutConstraintAxisHorizontal; row.spacing = 10; row.alignment = UIStackViewAlignmentCenter;
    UIImageView* icon = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"gamecontroller.fill"]];
    icon.tintColor = kYellow(); icon.contentMode = UIViewContentModeScaleAspectFit; [icon.widthAnchor constraintEqualToConstant:26].active = YES;
    [icon setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    UIStackView* text = [[UIStackView alloc] init]; text.axis = UILayoutConstraintAxisVertical; text.spacing = 2;
    UILabel* name = [self label:ns(pad.name) size:15 weight:UIFontWeightSemibold alpha:1];
    [text addArrangedSubview:name]; [text addArrangedSubview:[self label:ns(controller_rate_line(pad)) size:12 weight:UIFontWeightRegular alpha:0.6]];
    [text setContentHuggingPriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
    NSMutableArray<UIAction*>* ports = [NSMutableArray array];
    NSString* guid = ns(pad.guid);
    for (int p = 0; p <= 4; ++p) {
      NSString* title = p == 0 ? @"Auto port" : [NSString stringWithFormat:@"Port %d", p];
      UIAction* a = [UIAction actionWithTitle:title image:nil identifier:nil handler:^(UIAction*) {
        host::ControllerConfig cfg; if (const host::ControllerConfig* c = host::controller_config_for(guid.UTF8String)) cfg = *c;
        cfg.guid = guid.UTF8String; cfg.port = p; host::upsert_controller_config(cfg); [self refreshControllers]; }];
      if (p == pad.assigned_port) a.state = UIMenuElementStateOn;
      [ports addObject:a];
    }
    UIButtonConfiguration* pc = [MULauncherController glassConfiguration:NO];
    pc.title = pad.assigned_port ? [NSString stringWithFormat:@"Port %d", pad.assigned_port] : @"Auto"; pc.baseForegroundColor = UIColor.whiteColor; pc.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
    UIButton* portButton = [UIButton buttonWithConfiguration:pc primaryAction:nil];
    portButton.menu = [UIMenu menuWithChildren:ports]; portButton.showsMenuAsPrimaryAction = YES;
    UIButtonConfiguration* rc = [MULauncherController glassConfiguration:NO];
    rc.title = @"Configure"; rc.image = [UIImage systemImageNamed:@"slider.horizontal.3"]; rc.imagePadding = 6; rc.baseForegroundColor = UIColor.whiteColor; rc.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
    UIButton* remap = [UIButton buttonWithConfiguration:rc primaryAction:[UIAction actionWithHandler:^(UIAction*) {
      MURemapController* rm = [[MURemapController alloc] init];
      host::ControllerConfig cfg; if (const host::ControllerConfig* c = host::controller_config_for(guid.UTF8String)) cfg = *c; else { cfg.guid = guid.UTF8String; cfg.port = pad.assigned_port; }
      rm.config = cfg; rm.controllerName = name.text;
      rm.modalPresentationStyle = UIModalPresentationFormSheet;
      [self presentViewController:rm animated:YES completion:nil]; }]];
    [row addArrangedSubview:icon]; [row addArrangedSubview:text];
    // Name and rate on top, the port and Configure buttons sharing the width below: nothing is squeezed on a phone.
    UIStackView* entry = [[UIStackView alloc] initWithArrangedSubviews:@[row]]; entry.axis = UILayoutConstraintAxisVertical; entry.spacing = 8;
    if (!pad.is_gamecube_adapter) {
      UIStackView* actions = [[UIStackView alloc] initWithArrangedSubviews:@[portButton, remap]];
      actions.axis = UILayoutConstraintAxisHorizontal; actions.spacing = 10; actions.distribution = UIStackViewDistributionFillEqually;
      [entry addArrangedSubview:actions];
    }
    [self.controllersStack addArrangedSubview:entry];
  }
  [self refreshSteps];
}
- (void)connectController {
  if (self.presentedViewController) return;
  MUPairingController* pairing = [[MUPairingController alloc] init];
  __weak MULauncherController* weakSelf = self;
  pairing.completion = ^{ [weakSelf refreshControllers]; };
  pairing.modalPresentationStyle = UIModalPresentationFormSheet;
  [self presentViewController:pairing animated:YES completion:nil];
}
- (void)controllerTick {
  // Rebuild the card when a controller comes or goes, or when a measured rate changes (rounded, so it settles).
  std::string sig;
  for (const host::ControllerInfo& p : host::window_list_controllers()) sig += p.guid + ":" + std::to_string((int)(p.report_hz / 10)) + ":" + std::to_string(p.adapter_ports) + ";";
  if (sig != self.controllerSignature) { self.controllerSignature = sig; [self refreshControllers]; }
  [self refreshReadiness];
}
- (void)refreshDisc {
  std::vector<std::string> discs = documents_discs(nullptr);
  if (!self.settings->iso.empty()) { bool present = false; for (const std::string& d : discs) if (d == self.settings->iso) present = true; if (!present) self.settings->iso.clear(); }
  if (self.settings->iso.empty() && !discs.empty()) self.settings->iso = discs.front();
  if (self.settings->iso.empty()) {
    self.discLabel.text = @"No disc image yet";
    self.hintLabel.text = self.startupError.length ? self.startupError : @"Import your .iso/.gcm here, or drop it into this app's folder in Files.";
    self.playButton.enabled = NO;
  } else {
    NSString* path = ns(self.settings->iso);
    NSDictionary* attrs = [[NSFileManager defaultManager] attributesOfItemAtPath:path error:nil];
    self.discLabel.text = path.lastPathComponent;
    self.hintLabel.text = [NSString stringWithFormat:@"%.2f GB · ready to play%@", [attrs fileSize] / 1e9, self.startupError.length ? [@"\n" stringByAppendingString:self.startupError] : @""];
    self.playButton.enabled = YES;
  }
  [self refreshSteps];
}
- (void)loadDashboard {
  std::string slippi_dir = self.settings->slippi_dir, replay_dir = self.settings->replay_dir;
  __weak MULauncherController* weakSelf = self;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    host::Dashboard d;
    host::dashboard_load_games(replay_dir, d, 8);
    host::dashboard_load_profile(slippi_dir, d);
    host::dashboard_load_network(d);
    dispatch_async(dispatch_get_main_queue(), ^{
      MULauncherController* s = weakSelf; if (!s || s.done) return;
      host::Dashboard merged = d; merged.signed_in = s.dashboard.signed_in || d.profile_loaded;
      s.dashboard = merged; [s refreshAccount]; [s refreshGames]; [s refreshRegion];
    });
  });
}

// ---- actions
- (void)dismissKeyboard { [self.view endEditing:YES]; }
#if !TARGET_OS_VISION
- (void)keyboardChanged:(NSNotification*)note {
  CGRect end = [note.userInfo[UIKeyboardFrameEndUserInfoKey] CGRectValue];
  CGRect inView = [self.view convertRect:end fromView:nil];
  CGFloat overlap = MAX(0, CGRectGetMaxY(self.view.bounds) - CGRectGetMinY(inView));
  UIEdgeInsets insets = self.scroll.contentInset; insets.bottom = overlap; self.scroll.contentInset = insets; self.scroll.verticalScrollIndicatorInsets = insets;
  if (overlap > 0 && self.passwordField.isFirstResponder) [self.scroll scrollRectToVisible:[self.scroll convertRect:self.signInRows.bounds fromView:self.signInRows] animated:YES];
}
#endif
- (BOOL)textFieldShouldReturn:(UITextField*)field {
  if (field == self.emailField) { [self.passwordField becomeFirstResponder]; return NO; }
  [field resignFirstResponder]; [self signIn]; return YES;
}
- (void)setBusy:(BOOL)busy {
  _busy = busy;
  UIButtonConfiguration* c = self.signInButton.configuration; c.showsActivityIndicator = busy; c.title = busy ? @"Signing In…" : @"Sign In"; self.signInButton.configuration = c; self.signInButton.enabled = !busy;
}
- (void)signIn {
  if (self.busy) return;
  std::string email = self.emailField.text.UTF8String ?: "", password = self.passwordField.text.UTF8String ?: "";
  if (email.empty() || password.empty()) { self.accountLabel.text = @"Enter your Slippi email and password."; return; }
  [self.view endEditing:YES]; self.busy = YES; self.accountLabel.text = @"Signing in…";
  std::string dir = self.settings->slippi_dir;
  __weak MULauncherController* weakSelf = self;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    slippi::login::Account account; slippi::login::Session session; std::string error;
    bool ok = slippi::login::sign_in_session(email, password, account, session, error) && slippi::login::write_user_file(dir, account, error);
    if (ok) slippi::login::write_session(dir, session);
    dispatch_async(dispatch_get_main_queue(), ^{
      MULauncherController* s = weakSelf; if (!s || s.done) return;
      s.busy = NO; haptic_notify(ok);
      if (ok) { s.passwordField.text = @""; s->_dashboard.name = account.display_name; s->_dashboard.code = account.connect_code; [s refreshAccount]; [s loadDashboard]; }
      else s.accountLabel.text = ns(error);
    });
  });
}
- (void)forgotPassword {
  std::string email = self.emailField.text.UTF8String ?: "";
  if (email.empty()) { [UIApplication.sharedApplication openURL:[NSURL URLWithString:@"https://slippi.gg"] options:@{} completionHandler:nil]; return; }
  self.accountLabel.text = @"Sending a password reset email…";
  __weak MULauncherController* weakSelf = self;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    std::string error; bool ok = slippi::login::send_password_reset(email, error);
    dispatch_async(dispatch_get_main_queue(), ^{ MULauncherController* s = weakSelf; if (!s || s.done) return; s.accountLabel.text = ok ? @"Password reset email sent. Check your inbox." : ns(error); });
  });
}
- (void)signOut {
  if (!self.settings) return;
  slippi::login::remove_user_file(self.settings->slippi_dir); slippi::login::remove_session(self.settings->slippi_dir);
  self.dashboard = host::Dashboard(); [self refreshAccount]; [self refreshGames];
}
- (void)importDisc {
  NSMutableArray<UTType*>* types = [NSMutableArray array];
  for (NSString* ext in @[@"iso", @"gcm"]) { UTType* t = [UTType typeWithFilenameExtension:ext]; if (t) [types addObject:t]; }
  [types addObject:UTTypeData];
  UIDocumentPickerViewController* picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:types asCopy:YES];
  picker.delegate = self; [self presentViewController:picker animated:YES completion:nil];
}
- (void)documentPicker:(UIDocumentPickerViewController*)controller didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
  NSURL* documents = nil; documents_discs(&documents);
  if (!documents || urls.count == 0) return;
  NSURL* dest = [documents URLByAppendingPathComponent:urls.firstObject.lastPathComponent];
  NSError* error = nil;
  [[NSFileManager defaultManager] removeItemAtURL:dest error:nil];
  if (![[NSFileManager defaultManager] moveItemAtURL:urls.firstObject toURL:dest error:&error]) self.startupError = [NSString stringWithFormat:@"Could not import: %@", error.localizedDescription];
  else { self.startupError = nil; self.settings->iso = std::string(dest.fileSystemRepresentation); }
  [self refreshDisc];
}
// The online delay the dashboard shows: 1-4, or the exact larger value set in the in-game menu (the fifth segment).
- (int)selectedDelay {
  const NSInteger i = self.delayControl.selectedSegmentIndex;
  return i >= 4 ? MAX(5, self.settings->online_delay) : (int)MAX(0, i) + 1;
}
- (void)applyCompetitivePreset {
  haptic_impact();
  [self.scaleControl setSelectedSegmentIndex:2]; [self.anisoControl setSelectedSegmentIndex:2];   // 2x: the lowest-latency resolution that still looks crisp
  [self.vsyncSwitch setOn:YES animated:YES]; [self.widescreenSwitch setOn:NO animated:YES];
  [self.sharpnessSlider setValue:0 animated:YES]; [self.sharpnessSlider sendActionsForControlEvents:UIControlEventValueChanged];
}
- (void)play {
  haptic_impact();
  const int scales[] = {0, 1, 2, 3, 4, 6, 8};
  self.settings->scale = scales[MAX(0, MIN(6, self.scaleControl.selectedSegmentIndex))];
  self.settings->anisotropy = self.anisoControl.selectedSegmentIndex == 2 ? 16 : self.anisoControl.selectedSegmentIndex == 1 ? 4 : 1;
  self.settings->upscaler = (int)MAX(0, MIN(2, self.upscalerControl.selectedSegmentIndex));
  self.settings->vsync = self.vsyncSwitch.on;
  self.settings->widescreen = self.widescreenSwitch.on;
  self.settings->online = self.onlineSwitch.on;
  if (self.delayControl) self.settings->online_delay = [self selectedDelay];
  self.settings->sharpness = self.sharpnessSlider.value;
  self.settings->overlay_opacity = self.overlaySlider.value;
  self.settings->overlay_scale = self.overlayScaleSlider.value;
  self.playPressed = YES;
  [self.playButton.layer removeAnimationForKey:@"breathe"];
  [UIView animateWithDuration:0.28 animations:^{ self.view.alpha = 0; self.view.transform = CGAffineTransformMakeScale(0.97, 0.97); } completion:nil];
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.3 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ self.done = YES; });
}
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskAll; }
@end

namespace host {
void mac_show_error(const std::string& title, const std::string& detail) { std::fprintf(stderr, "%s: %s\n", title.c_str(), detail.c_str()); }

bool launcher_run(LauncherSettings& settings, const std::string& error) {
  @autoreleasepool {
    UIWindowScene* scene = nil;
    for (int wait = 0; wait < 200 && !scene; ++wait) {
      for (UIScene* s in UIApplication.sharedApplication.connectedScenes) if ([s isKindOfClass:UIWindowScene.class]) { scene = (UIWindowScene*)s; break; }
      if (!scene) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
    }
    if (!scene) std::fprintf(stderr, "launcher: no window scene connected after 10 s\n");
    UIWindow* window = nil;
    if (scene) window = [[UIWindow alloc] initWithWindowScene:scene];
#if !TARGET_OS_VISION
    if (!window) window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
#endif
    if (!window) return false;
    settings.display_hz = display_max_hz();
    MULauncherController* controller = [[MULauncherController alloc] init];
    controller.settings = &settings;
    controller.startupError = error.empty() ? nil : [NSString stringWithUTF8String:error.c_str()];
    window.rootViewController = controller;
    window.windowLevel = UIWindowLevelNormal + 1;
    [window makeKeyAndVisible];
    while (!controller.done) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
    [controller.controllerTimer invalidate];
    window.hidden = YES; window.rootViewController = nil;
    controller.settings = nullptr;
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
    return controller.playPressed;
  }
}
}  // namespace host
