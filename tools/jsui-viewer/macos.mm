// macOS window chrome and input. --titlebar-overlay: the UI fills the window
// up to the top edge, the title bar is transparent and the traffic lights sit
// centred in the UI's own header (like Docker Desktop or Chrome). Scroll is
// read from AppKit instead of SDL: SDL2 forwards
// NSEvent.deltaX/Y (coarse, accelerated line deltas), browsers use
// scrollingDeltaX/Y (pixels on trackpads and Magic Mice, momentum phase
// included). Plain C entry points so main.cpp stays C++.
#import <AppKit/AppKit.h>

#include <algorithm>
#include <cmath>

extern "C" {

// Before SDL_Init: SDL registers AppleMomentumScrollSupported = NO
// (SDLApplication registerUserDefaults), which stops AppKit delivering the
// trackpad's momentum phase, so flick scrolling stops dead. The argument
// domain outranks registered defaults.
void viewer_mac_init(void) {
  NSUserDefaults* d = [NSUserDefaults standardUserDefaults];
  NSMutableDictionary* args = [[d volatileDomainForName:NSArgumentDomain] mutableCopy];
  args[@"AppleMomentumScrollSupported"] = @YES;
  [d setVolatileDomain:args forName:NSArgumentDomain];
}

// Calls `cb(user, dx, dy, precise)` with DOM wheel deltas (px, positive dy scrolls
// down; the system's scroll direction setting already applied) for every
// scroll event of the app, on the main thread while SDL pumps events.
void viewer_mac_watch_wheel(void (*cb)(void* user, float dx, float dy, int precise), void* user) {
  [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskScrollWheel
                                        handler:^NSEvent*(NSEvent* e) {
                                          // Lines (mouse wheel steps, accelerated: a slow notch is
                                          // 0.1 line) at 40 px, like Chrome.
                                          bool precise = e.hasPreciseScrollingDeltas;
                                          CGFloat k = precise ? 1 : 40;
                                          cb(user, float(-e.scrollingDeltaX * k), float(-e.scrollingDeltaY * k), precise);
                                          return e;
                                        }];
}

// --titlebar-overlay: places the window buttons centred in a `header` px tall
// strip (again after every resize and full screen change, AppKit lays them out
// anew). Writes their box (logical px from the top left) to `controls`
// (x, y, width, height).
void viewer_mac_titlebar_overlay(void* nswindow, float header, float* controls) {
  NSWindow* w = (__bridge NSWindow*)nswindow;
  // SDL sets the style mask anew when the window leaves full screen (and on
  // other mode changes), dropping the full size content view: applied again
  // whenever it is missing.
  void (^style)(void) = ^{
    if (!(w.styleMask & NSWindowStyleMaskFullSizeContentView) && !(w.styleMask & NSWindowStyleMaskFullScreen))
      w.styleMask |= NSWindowStyleMaskFullSizeContentView;
    w.titlebarAppearsTransparent = YES;
    w.titleVisibility = NSWindowTitleHidden;
  };
  style();
  NSButton* close = [w standardWindowButton:NSWindowCloseButton];
  if (!close) return;
  NSSize b = close.frame.size;
  CGFloat inset = std::floor((header - b.height) / 2), spacing = 20;
  CGFloat left = std::max<CGFloat>(12, inset);
  // AppKit lays the title bar out again whenever it likes (showing the
  // window, key changes, resizes, full screen), so the buttons are placed
  // again after every frame change of the title bar or a button.
  __block bool placing = false;
  NSView* container = close.superview.superview;  // NSTitlebarContainerView
  void (^place)(void) = ^{
    if (placing || !container || (w.styleMask & NSWindowStyleMaskFullScreen)) return;
    placing = true;
    style();
    // Only as wide as the buttons: the rest of the header is the UI's (its
    // clicks, and drags through the hit test in main.cpp).
    NSRect r = NSMakeRect(0, w.frame.size.height - header, left + 2 * spacing + b.width + left, header);
    if (!NSEqualRects(container.frame, r)) container.frame = r;
    // The title bar's decoration (its separator and edge shading) would
    // outline the button box over the UI.
    for (NSView* v in container.subviews)
      if ([NSStringFromClass([v class]) containsString:@"Decoration"]) v.hidden = YES;
    NSView* bar = close.superview;  // NSTitlebarView
    NSRect br = NSMakeRect(0, 0, r.size.width, header);
    if (!NSEqualRects(bar.frame, br)) bar.frame = br;
    NSWindowButton kinds[] = {NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton};
    for (int i = 0; i < 3; i++) {
      NSButton* button = [w standardWindowButton:kinds[i]];
      NSPoint o = NSMakePoint(left + i * spacing, inset);
      if (!NSEqualPoints(button.frame.origin, o)) [button setFrameOrigin:o];
    }
    placing = false;
  };
  place();
  NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
  for (NSNotificationName n in @[ NSWindowDidResizeNotification, NSWindowDidExitFullScreenNotification,
                                  NSWindowDidBecomeKeyNotification, NSWindowDidResignKeyNotification ])
    [nc addObserverForName:n
                    object:w
                     queue:nil
                usingBlock:^(NSNotification*) { dispatch_async(dispatch_get_main_queue(), place); }];
  NSMutableArray<NSView*>* watched = [NSMutableArray arrayWithObjects:container, close.superview, nil];
  for (NSWindowButton k : {NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton})
    [watched addObject:[w standardWindowButton:k]];
  for (NSView* v in watched) {
    v.postsFrameChangedNotifications = YES;
    [nc addObserverForName:NSViewFrameDidChangeNotification
                    object:v
                     queue:nil
                usingBlock:^(NSNotification*) { dispatch_async(dispatch_get_main_queue(), place); }];
  }
  dispatch_async(dispatch_get_main_queue(), place);
  // SDL gives the window back a frame without the title bar's height after
  // full screen (it sizes the content area as if the title bar took space).
  __block NSRect windowed = NSZeroRect;
  [nc addObserverForName:NSWindowWillEnterFullScreenNotification
                  object:w
                   queue:nil
              usingBlock:^(NSNotification*) {
                // SDL has already dropped the full size content view by now (the
                // frame grew by the title bar): the content rect is our frame.
                windowed = (w.styleMask & NSWindowStyleMaskFullSizeContentView) ? w.frame
                                                                                 : [w contentRectForFrameRect:w.frame];
              }];
  [nc addObserverForName:NSWindowDidExitFullScreenNotification
                  object:w
                   queue:nil
              usingBlock:^(NSNotification*) {
                // After SDL's own handling of the change (it may resize again).
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 4), dispatch_get_main_queue(), ^{
                  style();
                  if (!NSIsEmptyRect(windowed) && !NSEqualRects(w.frame, windowed)) [w setFrame:windowed display:YES];
                });
              }];
  controls[0] = float(left);
  controls[1] = float(inset);
  controls[2] = float(2 * spacing + b.width);
  controls[3] = float(b.height);
}

}  // extern "C"
