# JSUI for Godot @VERSION@

`JsuiView`, a Godot `Control` that runs a JSUI (React Native-style) UI, as a
GDExtension for Godot 4.3 and newer on Windows x64, macOS (universal),
Linux x64 and Linux arm64.

1. Copy `addons/jsui` into your project (so this file is at
   `res://addons/jsui/README.md`) and restart the editor.
2. Bundle your UI: `npx @orktes/jsui-cli@@VERSION@ build ui/app.jsx -o ui/dist/bundle.js`
3. Add a `JsuiView` node and set its `bundle_url` to `res://ui/dist/bundle.js`.

On the Forward+ and Mobile renderers Skia draws on the GPU into a texture of
Godot's RenderingDevice (Vulkan on Linux, Metal on macOS); on Compatibility,
on Windows, and headless it renders on the CPU. Full reference (state,
calls, messages, host textures, audio, input):
https://github.com/orktes/jsui/blob/main/integrations/godot/README.md

macOS: the library is ad-hoc signed, not notarized. If Godot cannot load it,
run `xattr -dr com.apple.quarantine addons/jsui`.

Licenses: `LICENSE` (JSUI, MIT), `THIRD_PARTY_NOTICES.md`, `Inter-LICENSE.txt`.
