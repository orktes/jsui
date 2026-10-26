// macOS input: scroll read from AppKit instead of SDL: SDL2 forwards
// NSEvent.deltaX/Y (coarse, accelerated line deltas), browsers use
// scrollingDeltaX/Y (pixels on trackpads and Magic Mice, momentum phase
// included). Plain C entry points so main.cpp stays C++.
#import <AppKit/AppKit.h>

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

// Calls `cb(user, dx, dy)` with DOM wheel deltas (px, positive dy scrolls
// down; the system's scroll direction setting already applied) for every
// scroll event of the app, on the main thread while SDL pumps events.
void viewer_mac_watch_wheel(void (*cb)(void* user, float dx, float dy), void* user) {
  [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskScrollWheel
                                        handler:^NSEvent*(NSEvent* e) {
                                          // Lines (mouse wheel notches) at ~40 px, like browsers.
                                          CGFloat k = e.hasPreciseScrollingDeltas ? 1 : 40;
                                          cb(user, float(-e.scrollingDeltaX * k), float(-e.scrollingDeltaY * k));
                                          return e;
                                        }];
}

}  // extern "C"
