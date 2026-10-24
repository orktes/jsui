# JSUI @VERSION@ (@PLATFORM@)

Prebuilt JSUI: the C API as a shared library, plus the desktop viewer.
Source, docs and examples: https://github.com/orktes/jsui

```
include/jsui/jsui.h          the C API (the only header)
lib/libjsui.so | .dylib      the shared library (Windows: lib/jsui.lib import library)
bin/jsui.dll, icudtl.dat     Windows: the DLL and the ICU data it loads from its own folder
lib/cmake/jsui/              CMake package: find_package(jsui) -> jsui::jsui
bin/jsui-viewer              runs a bundle in a window: jsui-viewer [--dev] bundle.js|url
```

The library exports the `jsui_*` functions of `jsui.h` and nothing else.
Skia, QuickJS-ng, Yoga, HarfBuzz, ICU and FreeType are linked in and kept
private, as is the C++ runtime on Linux (static libstdc++), so it loads into
hosts built with any compiler or C++ runtime.

## Use it from CMake

```cmake
list(APPEND CMAKE_PREFIX_PATH /path/to/jsui-@VERSION@-@PLATFORM@)
find_package(jsui CONFIG REQUIRED)
target_link_libraries(game PRIVATE jsui::jsui)
```

Or by hand: add `include/` to the include path and link `lib/libjsui.so`
(`-ljsui`), `lib/libjsui.dylib`, or `lib/jsui.lib` on Windows, where you
also define `JSUI_SHARED` (the functions are then `dllimport`). Ship
`bin/jsui.dll` and `bin/icudtl.dat` next to the game executable.

## Build a bundle

The UI is React Native code bundled with the CLI:

```sh
npx @orktes/jsui-cli@@VERSION@ build app.jsx -o ui/bundle.js
bin/jsui-viewer ui/bundle.js
```

See the repository's `docs/ARCHITECTURE.md` and `integrations/` for hosts
(raylib, SDL3, sokol, Godot, Unity) that render on the GPU through this API.

## Platform notes

- macOS: one universal (arm64 + x86_64) binary, macOS 13 or newer. Ad-hoc
  signed, not notarized: if Gatekeeper blocks it, run
  `xattr -dr com.apple.quarantine <folder>`. `install_name` is
  `@rpath/libjsui.dylib`. GPU: Skia Graphite on Metal (`jsui_gpu_create_metal`),
  Ganesh on OpenGL / Metal, Graphite on a host's Dawn.
- Linux: built on Ubuntu 24.04 (glibc 2.39 or newer). Needs
  `libfontconfig1` at run time. GPU: Ganesh on OpenGL / EGL, Graphite on
  Vulkan (`jsui_gpu_create_vulkan`) and on a host's Dawn.
- Windows: x64, static C runtime inside the DLL. GPU: Ganesh on OpenGL. Uses
  Skia m124 (the other platforms use m157), which has no Graphite yet.

Licenses: JSUI is MIT (`LICENSE`); the bundled third-party code is listed in
`THIRD_PARTY_NOTICES.md`; the embedded Inter font is OFL (`Inter-LICENSE.txt`).
