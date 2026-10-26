// Scroll input on macOS, read from AppKit instead of SDL: SDL2 forwards
// NSEvent.deltaX/Y (coarse, accelerated line deltas), browsers use
// scrollingDeltaX/Y (pixels on trackpads and Magic Mice, momentum phase
// included). Plain C entry point so main.cpp stays C++.
#import <AppKit/AppKit.h>

extern "C" {

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
