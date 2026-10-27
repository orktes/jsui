// JsuiDemo: iOS host for a JSUI bundle.
//
// One MTKView, driven at display rate (ProMotion: up to 120 Hz) by a
// CADisplayLink whose vsync timestamp is the frame time: animations,
// flings and resampled touch drags step evenly even when the callback
// itself runs a little early or late. Every frame ticks the runtime and, when it has something new, renders with Skia
// Ganesh on Metal straight into the drawable (jsui_render_metal), or with
// the CPU rasteriser plus a blit when Metal is unavailable or `-JsuiRenderer
// cpu` is passed. Touches, hardware keyboards and game controllers
// (GameController.framework, W3C standard mapping) are forwarded.
//
// Launch arguments (NSUserDefaults argument domain):
//   -JsuiURL http://<mac>:5173/bundle.js   load from `jsui dev` (dev mode: live reload)
//   -JsuiRenderer cpu                       force the CPU path
//   -JsuiJsThread off                       run JS on the main thread (default: on, its own thread
//                                           as in React Native; or lockstep)
// Without -JsuiURL the app loads ui/bundle.js from its own bundle.
// ui/state.json, when the app has one ({"key": value, ...}), is shared state
// set before the bundle loads (like jsui-viewer --state key=json).
// <Video> plays with AVFoundation (tools/jsui-viewer/video.mm, shared with
// the desktop viewer); sounds play with the built-in miniaudio backend when
// the library is built with JSUI_AUDIO (scripts/build-ios.sh does).
// JS runs on its own thread by default (JsuiConfig.js_thread): the
// MTKView's frames never wait for it. The view draws every display frame,
// so no on_js_ready wake-up is needed.

#import <GameController/GameController.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "jsui/jsui.h"

extern "C" {
struct ViewerVideos;
ViewerVideos* viewer_videos_create(void* device, void* queue);
void viewer_videos_attach(ViewerVideos* vs, JsuiRuntime* rt);
void viewer_videos_command(void* user, const JsuiVideoCommand* c);
int viewer_videos_tick(ViewerVideos* vs);
void viewer_videos_destroy(ViewerVideos* vs);
}

static void jsuiLog(void*, JsuiLogLevel level, const char* message) {
  static const char* names[] = {"debug", "info", "warn", "error"};
  NSLog(@"[jsui %s] %s", names[level < 4 ? level : 3], message);
}

static double nowMs() { return CACurrentMediaTime() * 1000.0; }

// DOM KeyboardEvent key / code for a UIKey (hardware keyboards, iOS 13.4+).
API_AVAILABLE(ios(13.4))
static void domKey(UIKey* k, std::string* key, std::string* code) {
  switch (k.keyCode) {
    case UIKeyboardHIDUsageKeyboardReturnOrEnter: *key = "Enter"; *code = "Enter"; return;
    case UIKeyboardHIDUsageKeyboardEscape: *key = "Escape"; *code = "Escape"; return;
    case UIKeyboardHIDUsageKeyboardDeleteOrBackspace: *key = "Backspace"; *code = "Backspace"; return;
    case UIKeyboardHIDUsageKeyboardDeleteForward: *key = "Delete"; *code = "Delete"; return;
    case UIKeyboardHIDUsageKeyboardTab: *key = "Tab"; *code = "Tab"; return;
    case UIKeyboardHIDUsageKeyboardSpacebar: *key = " "; *code = "Space"; return;
    case UIKeyboardHIDUsageKeyboardRightArrow: *key = "ArrowRight"; *code = "ArrowRight"; return;
    case UIKeyboardHIDUsageKeyboardLeftArrow: *key = "ArrowLeft"; *code = "ArrowLeft"; return;
    case UIKeyboardHIDUsageKeyboardDownArrow: *key = "ArrowDown"; *code = "ArrowDown"; return;
    case UIKeyboardHIDUsageKeyboardUpArrow: *key = "ArrowUp"; *code = "ArrowUp"; return;
    case UIKeyboardHIDUsageKeyboardHome: *key = "Home"; *code = "Home"; return;
    case UIKeyboardHIDUsageKeyboardEnd: *key = "End"; *code = "End"; return;
    case UIKeyboardHIDUsageKeyboardPageUp: *key = "PageUp"; *code = "PageUp"; return;
    case UIKeyboardHIDUsageKeyboardPageDown: *key = "PageDown"; *code = "PageDown"; return;
    case UIKeyboardHIDUsageKeyboardLeftShift: *key = "Shift"; *code = "ShiftLeft"; return;
    case UIKeyboardHIDUsageKeyboardRightShift: *key = "Shift"; *code = "ShiftRight"; return;
    case UIKeyboardHIDUsageKeyboardLeftControl: *key = "Control"; *code = "ControlLeft"; return;
    case UIKeyboardHIDUsageKeyboardRightControl: *key = "Control"; *code = "ControlRight"; return;
    case UIKeyboardHIDUsageKeyboardLeftAlt: *key = "Alt"; *code = "AltLeft"; return;
    case UIKeyboardHIDUsageKeyboardRightAlt: *key = "Alt"; *code = "AltRight"; return;
    case UIKeyboardHIDUsageKeyboardLeftGUI: *key = "Meta"; *code = "MetaLeft"; return;
    case UIKeyboardHIDUsageKeyboardRightGUI: *key = "Meta"; *code = "MetaRight"; return;
    default: break;
  }
  long c = long(k.keyCode);
  if (c >= UIKeyboardHIDUsageKeyboardA && c <= UIKeyboardHIDUsageKeyboardZ) {
    *code = std::string("Key") + char('A' + (c - UIKeyboardHIDUsageKeyboardA));
  } else if (c >= UIKeyboardHIDUsageKeyboard1 && c <= UIKeyboardHIDUsageKeyboard9) {
    *code = std::string("Digit") + char('1' + (c - UIKeyboardHIDUsageKeyboard1));
  } else if (c == UIKeyboardHIDUsageKeyboard0) {
    *code = "Digit0";
  } else if (c >= UIKeyboardHIDUsageKeyboardF1 && c <= UIKeyboardHIDUsageKeyboardF12) {
    *code = "F" + std::to_string(1 + (c - UIKeyboardHIDUsageKeyboardF1));
    *key = *code;
    return;
  }
  const char* chars = k.characters.UTF8String;
  *key = chars && *chars ? chars : "Unidentified";
}

API_AVAILABLE(ios(13.4))
static int domMods(UIKey* k) {
  UIKeyModifierFlags f = k.modifierFlags;
  return ((f & UIKeyModifierShift) ? JSUI_MOD_SHIFT : 0) | ((f & UIKeyModifierControl) ? JSUI_MOD_CTRL : 0) |
         ((f & UIKeyModifierAlternate) ? JSUI_MOD_ALT : 0) | ((f & UIKeyModifierCommand) ? JSUI_MOD_META : 0);
}

// ------------------------------------------------------------------ gamepads

// Last state sent per pad, so only changes reach the runtime.
struct PadState {
  float buttons[17] = {};
  float axes[4] = {};
};

@interface JsuiViewController : UIViewController <MTKViewDelegate>
@end

@implementation JsuiViewController {
  MTKView* _view;
  id<MTLDevice> _device;
  id<MTLCommandQueue> _queue;
  JsuiRuntime* _rt;
  JsuiMetalContext* _mtl;
  ViewerVideos* _videos;
  BOOL _cpu;
  BOOL _presented;  // the drawable on screen shows the latest frame
  CADisplayLink* _link;
  double _frameTime;  // ms, the display link's vsync time for this frame
  // CPU fallback: RGBA frame -> BGRA staging buffer -> blit into the drawable.
  std::vector<uint8_t> _pixels;
  id<MTLBuffer> _staging;
  id<MTLCommandBuffer> _lastCpuFrame;
  int _pixelW, _pixelH;
  float _logicalW, _logicalH, _scale;
  // Keyed by address: std::hash can't take an ARC object pointer. UIKit
  // keeps a UITouch alive (same object) for the whole touch sequence.
  std::unordered_map<const void*, int> _touchIds;
  int _nextTouchId;
  NSMutableArray<GCController*>* _pads;
  std::vector<PadState> _padStates;
}

- (void)loadView {
  _device = MTLCreateSystemDefaultDevice();
  _view = [[MTKView alloc] initWithFrame:UIScreen.mainScreen.bounds device:_device];
  _view.delegate = self;
  _view.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
  _view.framebufferOnly = NO;  // Ganesh may read the target back (dst-read blends); the CPU path blits into it
  _view.preferredFramesPerSecond = UIScreen.mainScreen.maximumFramesPerSecond;
  _view.clearColor = MTLClearColorMake(0, 0, 0, 1);
  _view.multipleTouchEnabled = YES;
  // Drawn from our own CADisplayLink (displayFrame:), which knows the vsync time.
  _view.paused = YES;
  _view.enableSetNeedsDisplay = NO;
  _view.backgroundColor = UIColor.blackColor;
  self.view = _view;
}

- (void)viewDidLoad {
  [super viewDidLoad];
  _queue = [_device newCommandQueue];
  NSUserDefaults* args = NSUserDefaults.standardUserDefaults;
  _cpu = [[args stringForKey:@"JsuiRenderer"] isEqualToString:@"cpu"] || !_device;
  if (!_cpu) {
    _mtl = jsui_metal_create((__bridge void*)_device, (__bridge void*)_queue);
    if (!_mtl) {
      NSLog(@"[jsui] Metal backend unavailable, using the CPU renderer");
      _cpu = YES;
    }
  }
  NSLog(@"[jsui] renderer: %s", _cpu ? "cpu" : "metal");

  _scale = float(_view.contentScaleFactor);
  _logicalW = float(_view.bounds.size.width);
  _logicalH = float(_view.bounds.size.height);
  NSString* url = [args stringForKey:@"JsuiURL"];
  JsuiConfig cfg;
  jsui_config_init(&cfg);
  cfg.width = int(std::lround(_logicalW));
  cfg.height = int(std::lround(_logicalH));
  cfg.pixel_ratio = _scale;
  cfg.log = jsuiLog;
  cfg.dev_mode = url.length > 0;
  NSString* jsThread = [args stringForKey:@"JsuiJsThread"];
  cfg.js_thread = [jsThread isEqualToString:@"off"]        ? JSUI_JS_THREAD_OFF
                  : [jsThread isEqualToString:@"lockstep"] ? JSUI_JS_THREAD_LOCKSTEP
                                                           : JSUI_JS_THREAD_ON;
  _videos = _cpu ? viewer_videos_create(nullptr, nullptr)
                 : viewer_videos_create((__bridge void*)_device, (__bridge void*)_queue);
  cfg.on_video = viewer_videos_command;
  cfg.video_user = _videos;
  _rt = jsui_create(&cfg);
  viewer_videos_attach(_videos, _rt);
  [self publishSafeArea];
  [self applyBundledState];
  jsui_set_clipboard_callbacks(
      _rt,
      [](void*) -> const char* {
        static std::string text;
        NSString* s = UIPasteboard.generalPasteboard.string;
        text = s ? s.UTF8String : "";
        return text.c_str();
      },
      [](void*, const char* utf8) { UIPasteboard.generalPasteboard.string = [NSString stringWithUTF8String:utf8]; },
      nullptr);
  if (url.length == 0) {
    NSString* path = [NSBundle.mainBundle pathForResource:@"bundle" ofType:@"js" inDirectory:@"ui"];
    url = path ? [NSURL fileURLWithPath:path].absoluteString : nil;
  }
  if (url) {
    NSLog(@"[jsui] loading %@", url);
    jsui_load_bundle_url(_rt, url.UTF8String);
  } else {
    NSLog(@"[jsui] no ui/bundle.js in the app and no -JsuiURL");
  }

  _pads = [NSMutableArray array];
  for (GCController* c in GCController.controllers) [self padConnected:c];
  [NSNotificationCenter.defaultCenter addObserver:self
                                         selector:@selector(padNotification:)
                                             name:GCControllerDidConnectNotification
                                           object:nil];
  [NSNotificationCenter.defaultCenter addObserver:self
                                         selector:@selector(padNotification:)
                                             name:GCControllerDidDisconnectNotification
                                           object:nil];
  [NSNotificationCenter.defaultCenter addObserver:self
                                         selector:@selector(memoryWarning)
                                             name:UIApplicationDidReceiveMemoryWarningNotification
                                           object:nil];

  _link = [CADisplayLink displayLinkWithTarget:self selector:@selector(displayFrame:)];
  float maxFps = float(UIScreen.mainScreen.maximumFramesPerSecond);
  _link.preferredFrameRateRange = CAFrameRateRangeMake(std::min(60.f, maxFps), maxFps, maxFps);
  [_link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
}

// ui/state.json: {"key": value, ...}, each value set as shared state.
- (void)applyBundledState {
  NSString* path = [NSBundle.mainBundle pathForResource:@"state" ofType:@"json" inDirectory:@"ui"];
  NSData* data = path ? [NSData dataWithContentsOfFile:path] : nil;
  id state = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
  if (![state isKindOfClass:NSDictionary.class]) return;
  [(NSDictionary*)state enumerateKeysAndObjectsUsingBlock:^(NSString* key, id value, BOOL*) {
    NSData* json = [NSJSONSerialization dataWithJSONObject:value options:NSJSONWritingFragmentsAllowed error:nil];
    if (!json) return;
    std::string s(static_cast<const char*>(json.bytes), json.length);
    jsui_set_state(_rt, key.UTF8String, s.c_str());
  }];
}

- (void)dealloc {
  [_link invalidate];
  [NSNotificationCenter.defaultCenter removeObserver:self];
  if (_rt) jsui_destroy(_rt);
  if (_videos) viewer_videos_destroy(_videos);
  if (_mtl) jsui_metal_destroy(_mtl);
}

- (BOOL)prefersStatusBarHidden {
  return YES;
}
- (BOOL)prefersHomeIndicatorAutoHidden {
  return YES;
}
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures {
  return UIRectEdgeAll;
}

// Notch / Dynamic Island / home indicator: published as shared state, which
// SafeAreaView and useSafeAreaInsets() read.
- (void)publishSafeArea {
  if (!_rt) return;
  UIEdgeInsets i = _view.safeAreaInsets;
  char json[160];
  snprintf(json, sizeof json, "{\"top\":%g,\"right\":%g,\"bottom\":%g,\"left\":%g}", double(i.top),
           double(i.right), double(i.bottom), double(i.left));
  jsui_set_state(_rt, "jsui.safeAreaInsets", json);
}

- (void)viewSafeAreaInsetsDidChange {
  [super viewSafeAreaInsetsDidChange];
  [self publishSafeArea];
}

- (void)memoryWarning {
  if (_mtl) jsui_metal_purge(_mtl, 0);
}

// ------------------------------------------------------------------ frame

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size {
  _scale = float(view.contentScaleFactor);
  _logicalW = float(view.bounds.size.width);
  _logicalH = float(view.bounds.size.height);
  if (_rt) jsui_set_viewport(_rt, int(std::lround(_logicalW)), int(std::lround(_logicalH)), _scale);
  _presented = NO;
}

- (void)displayFrame:(CADisplayLink*)link {
  _frameTime = link.timestamp * 1000.0;  // same clock as CACurrentMediaTime and UITouch timestamps
  [_view draw];
}

- (void)drawInMTKView:(MTKView*)view {
  if (!_rt) return;
  [self pollPads];
  jsui_tick(_rt, _frameTime > 0 ? _frameTime : nowMs());
  viewer_videos_tick(_videos);  // new video frames mark the runtime for a redraw
  if (_presented && !jsui_needs_redraw(_rt)) return;  // the screen already shows this frame
  id<CAMetalDrawable> drawable = view.currentDrawable;
  if (!drawable) return;
  int w = int(drawable.texture.width), h = int(drawable.texture.height);
  id<MTLCommandBuffer> cb;
  if (!_cpu) {
    if (jsui_render_metal(_rt, _mtl, (__bridge void*)drawable.texture, MTLPixelFormatBGRA8Unorm, w, h, 0) != 0) {
      NSLog(@"[jsui] Metal render failed, switching to the CPU renderer");
      _cpu = YES;
    }
  }
  cb = [_queue commandBuffer];  // same queue as JSUI's work: ordered after it
  if (_cpu) [self renderCpu:drawable.texture width:w height:h commandBuffer:cb];
  [cb presentDrawable:drawable];
  [cb commit];
  _presented = YES;
  if (const char* err = jsui_last_error(_rt)) NSLog(@"[jsui] %s", err);
}

- (void)renderCpu:(id<MTLTexture>)target width:(int)w height:(int)h commandBuffer:(id<MTLCommandBuffer>)cb {
  size_t rowBytes = size_t(w) * 4;
  if (w != _pixelW || h != _pixelH || !_staging) {
    _pixelW = w;
    _pixelH = h;
    _pixels.assign(rowBytes * size_t(h), 0);
    _staging = [_device newBufferWithLength:rowBytes * size_t(h) options:MTLResourceStorageModeShared];
  }
  // The staging buffer may still be read by the previous frame's blit.
  [_lastCpuFrame waitUntilCompleted];
  _lastCpuFrame = cb;
  jsui_render_rgba(_rt, _pixels.data(), w, h, rowBytes);
  // RGBA (premultiplied) -> BGRA, the drawable's format.
  const uint8_t* src = _pixels.data();
  uint8_t* dst = static_cast<uint8_t*>(_staging.contents);
  for (size_t i = 0, n = rowBytes * size_t(h); i < n; i += 4) {
    dst[i] = src[i + 2];
    dst[i + 1] = src[i + 1];
    dst[i + 2] = src[i];
    dst[i + 3] = src[i + 3];
  }
  id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
  [blit copyFromBuffer:_staging
             sourceOffset:0
        sourceBytesPerRow:rowBytes
      sourceBytesPerImage:rowBytes * size_t(h)
               sourceSize:MTLSizeMake(NSUInteger(w), NSUInteger(h), 1)
                toTexture:target
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
  [blit endEncoding];
}

// ------------------------------------------------------------------ touch

- (void)sendTouches:(NSSet<UITouch*>*)touches event:(UIEvent*)event phase:(int)phase {
  for (UITouch* t in touches) {
    int touchId;
    const void* key = (__bridge const void*)t;
    auto it = _touchIds.find(key);
    if (it == _touchIds.end()) {
      if (phase != 0) continue;
      touchId = _nextTouchId++;
      _touchIds[key] = touchId;
    } else {
      touchId = it->second;
    }
    // Coalesced touches carry the samples between two frames (120 Hz+ on
    // ProMotion / Pencil), which gesture velocities want.
    NSArray<UITouch*>* samples = phase == 1 ? [event coalescedTouchesForTouch:t] : nil;
    if (samples.count == 0) samples = @[ t ];
    for (UITouch* s in samples) {
      CGPoint p = [s locationInView:_view];
      jsui_set_event_time(_rt, s.timestamp * 1000.0);  // same clock as CACurrentMediaTime
      jsui_send_touch(_rt, touchId, phase, float(p.x), float(p.y));
    }
    if (phase >= 2) _touchIds.erase(key);
  }
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  [self sendTouches:touches event:event phase:0];
}
- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  [self sendTouches:touches event:event phase:1];
}
- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  [self sendTouches:touches event:event phase:2];
}
- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  [self sendTouches:touches event:event phase:3];
}

// --------------------------------------------------------------- keyboard

- (BOOL)canBecomeFirstResponder {
  return YES;
}
- (void)viewDidAppear:(BOOL)animated {
  [super viewDidAppear:animated];
  [self becomeFirstResponder];
}

- (BOOL)sendPresses:(NSSet<UIPress*>*)presses down:(BOOL)down {
  BOOL handled = NO;
  if (@available(iOS 13.4, *)) {
    for (UIPress* p in presses) {
      if (!p.key) continue;
      std::string key, code;
      domKey(p.key, &key, &code);
      int mods = domMods(p.key);
      handled |= jsui_send_key(_rt, key.c_str(), code.empty() ? nullptr : code.c_str(), down, 0, mods) != 0;
      // Printable characters also arrive as text (no Ctrl / Cmd chords).
      if (down && !(mods & (JSUI_MOD_CTRL | JSUI_MOD_META)) && key.size() >= 1 &&
          (key.size() == 1 ? (unsigned char)key[0] >= 0x20 : (unsigned char)key[0] >= 0x80))
        handled |= jsui_send_text(_rt, key.c_str()) != 0;
    }
  }
  return handled;
}

- (void)pressesBegan:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event {
  if (![self sendPresses:presses down:YES]) [super pressesBegan:presses withEvent:event];
}
- (void)pressesEnded:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event {
  if (![self sendPresses:presses down:NO]) [super pressesEnded:presses withEvent:event];
}
- (void)pressesCancelled:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event {
  if (![self sendPresses:presses down:NO]) [super pressesCancelled:presses withEvent:event];
}

// --------------------------------------------------------------- gamepads

- (void)padNotification:(NSNotification*)note {
  GCController* c = note.object;
  if ([note.name isEqualToString:GCControllerDidConnectNotification]) {
    [self padConnected:c];
  } else {
    NSUInteger i = [_pads indexOfObjectIdenticalTo:c];
    if (i == NSNotFound) return;
    jsui_set_gamepad_connected(_rt, int(i), 0, nullptr);
    _pads[i] = (GCController*)[NSNull null];
  }
}

- (void)padConnected:(GCController*)c {
  if (!c.extendedGamepad || [_pads indexOfObjectIdenticalTo:c] != NSNotFound) return;
  NSUInteger slot = [_pads indexOfObjectIdenticalTo:(GCController*)[NSNull null]];
  if (slot == NSNotFound) {
    slot = _pads.count;
    [_pads addObject:c];
    _padStates.emplace_back();
  } else {
    _pads[slot] = c;
    _padStates[slot] = PadState{};
  }
  NSString* name = c.vendorName ?: @"Game Controller";
  jsui_set_gamepad_connected(_rt, int(slot), 1, name.UTF8String);
}

// Polled once per frame: buttons and axes in the W3C standard mapping.
- (void)pollPads {
  for (NSUInteger i = 0; i < _pads.count; i++) {
    GCController* c = _pads[i];
    if ((id)c == [NSNull null]) continue;
    GCExtendedGamepad* g = c.extendedGamepad;
    if (!g) continue;
    float b[17] = {
        g.buttonA.value,
        g.buttonB.value,
        g.buttonX.value,
        g.buttonY.value,
        g.leftShoulder.value,
        g.rightShoulder.value,
        g.leftTrigger.value,
        g.rightTrigger.value,
        g.buttonOptions ? g.buttonOptions.value : 0.f,
        g.buttonMenu.value,
        g.leftThumbstickButton ? g.leftThumbstickButton.value : 0.f,
        g.rightThumbstickButton ? g.rightThumbstickButton.value : 0.f,
        g.dpad.up.value,
        g.dpad.down.value,
        g.dpad.left.value,
        g.dpad.right.value,
        g.buttonHome ? g.buttonHome.value : 0.f,
    };
    // GameController's Y axes point up; the standard mapping's point down.
    float a[4] = {g.leftThumbstick.xAxis.value, -g.leftThumbstick.yAxis.value, g.rightThumbstick.xAxis.value,
                  -g.rightThumbstick.yAxis.value};
    PadState& s = _padStates[i];
    for (int k = 0; k < 17; k++) {
      bool was = s.buttons[k] > 0.5f, is = b[k] > 0.5f;
      if (was != is || (k == 6 || k == 7 ? std::fabs(b[k] - s.buttons[k]) > 0.01f : false))
        jsui_send_gamepad_button(_rt, int(i), k, is, b[k]);
      s.buttons[k] = b[k];
    }
    for (int k = 0; k < 4; k++) {
      if (std::fabs(a[k] - s.axes[k]) > 0.001f) jsui_send_gamepad_axis(_rt, int(i), k, a[k]);
      s.axes[k] = a[k];
    }
  }
}

@end

// ------------------------------------------------------------------- app

@interface JsuiAppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow* window;
@end

@implementation JsuiAppDelegate
- (BOOL)application:(UIApplication*)app didFinishLaunchingWithOptions:(NSDictionary*)options {
  self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
  self.window.rootViewController = [JsuiViewController new];
  [self.window makeKeyAndVisible];
  return YES;
}
@end

int main(int argc, char* argv[]) {
  @autoreleasepool {
    return UIApplicationMain(argc, argv, nil, NSStringFromClass([JsuiAppDelegate class]));
  }
}
