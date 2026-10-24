# JSUI @VERSION@ for iOS

`JSUI.xcframework`: JSUI as a static library (with QuickJS-ng, Yoga and
Skia on Metal inside) for iOS 15+ devices and the arm64 simulator, with the C
API header `jsui/jsui.h` (also a `JSUI` clang module).

Add the xcframework to your app target (Frameworks, Libraries, and Embedded
Content: "Do Not Embed") and link these system libraries:
`CoreFoundation`, `CoreGraphics`, `CoreText`, `Foundation`, `UIKit`, `Metal`,
`libc++` and `libz`.

Render with `jsui_render_metal` into your `MTKView`'s drawable, or on the CPU
with `jsui_render_rgba`. A complete host (touch, keyboard and controllers) is
the example app in the repository:
https://github.com/orktes/jsui/tree/main/integrations/ios

Licenses: `LICENSE` (JSUI, MIT), `THIRD_PARTY_NOTICES.md`, `Inter-LICENSE.txt`.
