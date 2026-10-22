# JSUI on iOS

`JsuiDemo` is a minimal UIKit host: one `MTKView` that renders a JSUI bundle
with Skia Ganesh on **Metal** (`jsui_render_metal`, straight into the
drawable), falling back to the CPU rasteriser plus a blit. It forwards
touches (with coalesced samples and timestamps for gesture velocities),
hardware keyboards and game controllers (GameController.framework, mapped to
the W3C standard gamepad layout). Landscape on phones, any orientation on iPad.

## Build and run (macOS + Xcode)

```sh
brew install ninja cmake
RUN=1 scripts/build-ios.sh          # Skia slice, UI bundle, app, simulator, screenshot
```

* `scripts/build-skia-ios.sh` builds Skia (m157, Ganesh + Metal, CoreText
  fonts, bundled ICU / HarfBuzz) into `third_party/skia-ios/out/<sdk>-<cpu>`
  (`iphoneos-arm64`, `iphonesimulator-arm64`, `iphonesimulator-x64`).
* `scripts/build-ios.sh` bundles `APP` (default: the Godot demo UI) with
  `jsui build`, configures CMake with `CMAKE_SYSTEM_NAME=iOS` and builds
  `build-ios-sim/integrations/ios/JsuiDemo.app`. `SDK=iphoneos` builds for
  devices (sign with `-DJSUI_IOS_TEAM=<team id> -G Xcode` to install).

## Live reload

```sh
npx jsui dev integrations/godot/demo/ui/app.jsx --host 0.0.0.0
xcrun simctl launch booted com.orktes.jsui.demo -JsuiURL http://localhost:5173/bundle.js
```

`-JsuiRenderer cpu` forces the CPU path.

## Embedding

Link `libjsui.a` (built by this CMake project for iOS) and the Skia
libraries, then per frame:

```objc
jsui_tick(rt, CACurrentMediaTime() * 1000);
if (jsui_needs_redraw(rt)) {
  id<CAMetalDrawable> d = layer.nextDrawable;
  jsui_render_metal(rt, mtl, (__bridge void*)d.texture, MTLPixelFormatBGRA8Unorm, w, h, 0);
  id<MTLCommandBuffer> cb = [queue commandBuffer];  // same queue JSUI submits on
  [cb presentDrawable:d];
  [cb commit];
}
```

Not yet: the on-screen keyboard for `TextInput` (hardware keyboards work).
