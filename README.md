# JSUI

Fast development of beautiful, responsive in-game UIs with React.

* Write components exactly like React Native (`import { View, Text } from 'react-native'`).
* Runs in **QuickJS**, lays out with **Yoga**, paints with **Skia**.
* Embeds through a small C API (`include/jsui/jsui.h`): feed it input
  (mouse, keyboard, touch, gamepad), tick it, get pixels — or let it draw
  straight into your OpenGL / GLES / WebGL framebuffer or texture with the
  optional Skia Ganesh GPU backend (`jsui_render_gl`).
* Native and **WebAssembly** builds from one codebase.
* Dev mode: point the runtime at a local server and it live-reloads your bundle.
* Reference tests compare every example pixel-by-pixel with the same code
  rendered by react-native-web in headless Chrome.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the design.

## Build (native)

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/jsui-tests
./build/jsui-render bundle.js -o frame.png --width 1280 --height 720
./build/tools/jsui-viewer/jsui-viewer --gpu bundle.js   # window, rendered on the GPU
./build/jsui-record bundle.js --timeline examples/showcase/session.json --ffmpeg out.mp4  # scripted demo video
```

WebAssembly: `scripts/build-wasm.sh` (CPU raster, `web/dist/`) or
`WEBGL=1 scripts/build-wasm.sh` (adds Skia on WebGL, `web/dist-webgl/`).

## Godot 4

`integrations/godot` is a GDExtension with a `JsuiView` Control and a demo
project. Build it with `-DJSUI_BUILD_GODOT=ON` (it is off by default). See
[integrations/godot/README.md](integrations/godot/README.md).
