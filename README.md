# JSUI

Fast development of cross-platform, beautiful, responsive in-game UIs or standalone Apps with React.

## Why?

- **Game engines and editors are built for 3D, as they should be.** Building UI is secondary there, and it feels foreign both to how games are authored and, especially, to how modern UIs are made. Performant UI rendering is also a large problem space of its own.
- **Many games end up embedding a browser or webview.** That usually means bloat and awkward, non-optimal integrations between the game's data and its UI.
- **LLMs already know how to build great-looking UIs on the web**, thanks to the massive amount of training data around web technologies.
- **Webviews and regular React Native behave a bit differently on each platform** and render outside the game's own context.

JSUI answers all four: you write plain React Native code that LLMs and web developers already know, and a small embeddable runtime renders it on the GPU inside your game, the same way on every platform.

**Using a coding agent?** Add the JSUI skill to your project with one command:

```sh
npx @orktes/jsui-cli skill install            # -> .claude/skills/jsui/ (--global: ~/.claude/skills/)
```

* Write components exactly like React Native (`import { View, Text } from 'react-native'`).
* Runs in **QuickJS**, lays out with **Yoga**, paints with **Skia**.
* Embeds through a small C API (`include/jsui/jsui.h`): feed it input
  (mouse, keyboard, touch, gamepad), tick it, get pixels — or let it draw
  straight into your OpenGL / GLES / WebGL framebuffer or texture with the
  optional Skia Ganesh GPU backend (`jsui_render_gl`).
* Native and **WebAssembly** builds from one codebase.
* Dev mode: point the runtime at `jsui dev` and edits hot-update in place (React Fast Refresh keeps component state), with an error overlay for build and runtime errors.
* JS debugging from Chrome DevTools or VS Code in every native runtime (breakpoints in your JSX, stepping, scopes, console), through `jsui dev`. See [docs/DEBUGGING.md](docs/DEBUGGING.md).
* Reference tests compare every example pixel-by-pixel with the same code
  rendered by react-native-web in headless Chrome.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the design. For keeping per-frame work off JS (native animation, state styles, overlays, host-state binds), see [docs/NATIVE_FIRST.md](docs/NATIVE_FIRST.md). For what JSUI costs a game (frame time, JS per frame, spikes, memory and size), see [docs/PERFORMANCE.md](docs/PERFORMANCE.md).

**Showcase: [orktes.github.io/jsui](https://orktes.github.io/jsui/)**, a site
rendered entirely by the JSUI WebAssembly build into one canvas: interactive
game screens, input, motion, a live-code playground and embedding guides. Its
source is ordinary React Native in [site/](site/) (`node site/scripts/build.mjs`,
then `node site/scripts/serve.mjs`). The same bundle runs natively in the viewer:
`jsui-viewer --titlebar-overlay 64 --state 'intro={"play":true,"warmup":false}' --state 'audio={"native":true}'
site/dist/app/bundle.js` (the states stand in for the page: the intro's go-ahead and the UI
owning the sound switch; the nav bar holds the window buttons). Its trailer's `<Video>` plays
there too, with AVFoundation on macOS.

## Game data

The game shares state with the UI by key; the UI asks the game with calls
that return promises. Details: [docs/ARCHITECTURE.md#game-data](docs/ARCHITECTURE.md#game-data).

```c
jsui_set_state(rt, "gold", "1250");                       // any time, even before the bundle loads
jsui_set_state(rt, "inventory", "{\"items\":{\"potion\":{\"name\":\"Potion\",\"count\":3}}}");
jsui_patch_state(rt, "inventory", "{\"items\":{\"potion\":{\"count\":2}}}");  // RFC 7386 merge patch

cfg.on_call = on_call;  // Host.call() from JS: answer now or on a later frame
static void on_call(void* user, uint64_t id, const char* name, const char* args) {
    if (strcmp(name, "shop.buy") == 0) jsui_resolve_call(rt, id, "{\"gold\":1100}");
    else jsui_reject_call(rt, id, "unknown call");
}
```

```jsx
import { Host, useHostState } from '@orktes/jsui-react';

function Inventory() {
  const gold = useHostState('gold', 0);                    // re-renders only when "gold" changes
  const { items } = useHostState('inventory', { items: {} });
  const buy = () => Host.call('shop.buy', { item: 'potion' }).catch((e) => console.warn(e.message));
  return (
    <View>
      <Text>{gold} gold</Text>
      {Object.entries(items).map(([id, it]) => <Text key={id}>{it.name} ×{it.count}</Text>)}
      <Button title="Buy potion" onPress={buy} />
    </View>
  );
}
```

State lives in the runtime, so it survives hot reloads. The UI can own keys
too (`HostState.set` → `JsuiConfig.on_state`), and the game can call handlers
the UI registers with `Host.handle(name, fn)` (`jsui_call_ui`).

Data that changes every frame (enemy positions for nameplates and markers)
skips JSON: the game pushes typed arrays, the UI reads them in place, and
`Animated` views can follow them natively without any JS per frame:

```c
float enemies[30 * 3];                                      // x, y, hp per enemy
jsui_set_buffer(rt, "enemies", JSUI_BUFFER_F32, enemies, 30 * 3);  // every frame
```

```jsx
const count = useHostBufferSelector('enemies', (b) => b.length / 3, 0);  // re-renders when the count changes
function Marker({ i }) {
  const x = useHostBufferValue('enemies', i * 3), y = useHostBufferValue('enemies', i * 3 + 1);
  return <Animated.View style={{ transform: [{ translateX: x }, { translateY: y }] }} />;  // moved by the runtime
}
```

Pictures the game draws itself (a video it decodes, a render target, a
camera) are shown by name; JSUI samples the engine's own GPU texture in place
and repaints just those views when the game says it has new contents:

```c
JsuiTexture t = {JSUI_TEXTURE_VULKAN, vk_image, 0, 1280, 720, JSUI_TEXTURE_OPAQUE};
jsui_set_texture(rt, "trailer", &t);   // once (or when it changes)
jsui_texture_updated(rt, "trailer");   // after each new video frame
```

```jsx
<HostTexture name="trailer" resizeMode="contain" style={{ width: 640, aspectRatio: 16 / 9, borderRadius: 12 }} />
```

Lottie animations play natively (Skia's Skottie, no JS per frame) with
lottie-react-native's API:

```jsx
import LottieView from 'lottie-react-native';
<LottieView source={require('./victory.json')} autoPlay loop style={{ width: 200, height: 200 }} />
```

Video with react-native-video's API: the host plays it (a `<video>` element in the
browser, AVFoundation in the macOS viewer, the engine's own player in a game) into a host
texture the view samples, so frames cost no JS. Hosts implement `JsuiConfig.on_video`:

```jsx
import Video from 'react-native-video';
<Video source={{ uri: 'https://example.com/trailer.m3u8' }} paused={!playing} muted repeat
       resizeMode="cover" style={{ width: 640, aspectRatio: 16 / 9 }} onEnd={next} />
```

Custom shaders: your own SkSL (Skia runtime effects) paint a view on every
renderer. Uniforms can be Animated values (also on the native driver), a
shader that reads `time` animates on the runtime clock, child shaders are
images or host textures, and `uniform shader content` post-processes the
view's own children:

```jsx
<ShaderView
  source={sksl`uniform float2 resolution; uniform float time; layout(color) uniform half4 tint;
    half4 main(float2 p) { return tint * half(0.5 + 0.5 * sin(p.x / resolution.x * 6.28 + time)); }`}
  uniforms={{ tint: animatedColor }}
  style={{ width: 300, height: 120, borderRadius: 16 }}
/>
```

## npm packages

`@orktes/jsui-react` (the React Native API), `@orktes/jsui-cli` (`jsui build` / `jsui dev`,
headless `jsui screenshot` / `inspect` / `run`, and the agent skill) and `@orktes/jsui-web`
(the browser host with the WASM modules) are published to npm as 0.x releases. See
[docs/RELEASING.md](docs/RELEASING.md).

## Coding agents and headless checks

`npx @orktes/jsui-cli skill install` adds the JSUI [agent skill](packages/cli/skills/jsui/SKILL.md)
to a project (`.claude/skills/jsui/`; nothing else to install first), or install it in Claude Code as a plugin with
`/plugin marketplace add orktes/jsui` and `/plugin install jsui@jsui`. It teaches the
supported React Native subset, gamepad focus, animation, game data, Lottie, shaders, host
textures and embedding, and how to verify a UI without a browser (with
`@orktes/jsui-cli` in the project's devDependencies):

```sh
npx jsui screenshot app.jsx -o shot.png       # the CPU WASM runtime in Node, deterministic
npx jsui inspect app.jsx                      # view tree: types, testIDs, text, focus, layout
npx jsui run app.jsx steps.json               # scripted clicks / keys / gamepad / state + expectations
```

Every runtime that loads the bundle from `jsui dev` in dev mode (the game, the native
viewer, the browser at `http://127.0.0.1:5173/shared`, or `jsui dev --headless` inside the
server) joins its shared session: an agent drives and screenshots the live one with
`jsui screenshot|inspect|run --attach http://127.0.0.1:5173` while you play it, and reads
what you did with `jsui log --attach`.

## Prebuilt binaries

Every release on [GitHub Releases](https://github.com/orktes/jsui/releases) has the C API
as a shared library (`libjsui.so` for Linux x64 / arm64, a universal `libjsui.dylib`,
`jsui.dll`) with `jsui.h`, a CMake package (`find_package(jsui)` → `jsui::jsui`) and the
viewer; `JSUI.xcframework` for iOS; and the Godot addon (`addons/jsui`, every desktop
platform in one zip). The library exports only the `jsui_*` functions, so it drops into
any engine or toolchain. See [docs/RELEASING.md](docs/RELEASING.md#native-binaries).

## Build (native)

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/jsui-tests
./build/jsui-render bundle.js -o frame.png --width 1280 --height 720
./build/tools/jsui-viewer/jsui-viewer bundle.js   # window, rendered on the GPU (--cpu: CPU raster, --gl: Ganesh on GL)
./build/jsui-record bundle.js --timeline examples/showcase/session.json --ffmpeg out.mp4  # scripted demo video
```

WebAssembly: `GPU=1 scripts/build-wasm.sh` (Skia m157 with Graphite on WebGPU, Ganesh on
WebGL2 and the CPU raster in one module, `web/dist-gpu/`: preferred when the browser has
WebGPU), `WEBGL=1 scripts/build-wasm.sh` (Skia on WebGL2, `web/dist-webgl/`) and
`scripts/build-wasm.sh` (CPU raster only, `web/dist/`: the fallback module, and what CI's
CPU browser tests use). `web/jsui-web.js` picks WebGPU, then WebGL, then the CPU raster.
GPU rendering is the default everywhere a GL / WebGL context is available; the CPU
raster is the fallback (`jsui-viewer --cpu`, `createJsui({ renderer: 'cpu' })`) and
what the deterministic tools (`jsui-render`, `jsui-record`) and reference tests use.

## Godot 4

`integrations/godot` is a GDExtension with a `JsuiView` Control and a demo
project. Build it with `-DJSUI_BUILD_GODOT=ON` (it is off by default). See
[integrations/godot/README.md](integrations/godot/README.md).

## iOS and Android

[integrations/ios](integrations/ios/README.md) is a UIKit app rendering with
Skia on Metal (`RUN=1 scripts/build-ios.sh` builds it and runs it in the
simulator). [integrations/android](integrations/android/README.md) is an NDK
NativeActivity app rendering with Skia on OpenGL ES (`RUN=1
scripts/build-android.sh` builds the APK and runs it on a device or emulator).
Both forward touch, hardware keys and gamepads and publish the safe area.

## Rust, Go and other languages

JSUI is a C library, so any language with a C FFI can host a React UI.
[bindings/rust](bindings/rust/README.md) (a crate) and
[bindings/go](bindings/go/README.md) (a cgo package) are thin bindings over the
C API, and each comes with the same desktop app: a quest log whose data and
rules live in Rust or Go while the UI is React, drawn by Skia on the GPU into a
GLFW window. The program publishes shared state, the UI asks for changes with
`Host.call`, and the program answers. See [bindings/README.md](bindings/README.md).

## raylib, SDL3, sokol, Unity and Unreal

`integrations/raylib`, `integrations/sdl3` and `integrations/sokol` run one
shared game HUD in three game loops through the C API: input pass-through
(what the UI does not consume moves the camera), per-frame unit positions
driving nameplates with no JS per frame, and three ways to get the UI on
screen (Skia GL into a raylib `RenderTexture2D`, Skia into SDL's own texture on
its Metal, Vulkan or GL renderer, Skia into an image on sokol_gfx's Metal
device on macOS and straight into sokol's GL swapchain framebuffer elsewhere).
Build them with `-DJSUI_BUILD_RAYLIB=ON -DJSUI_BUILD_SDL3=ON
-DJSUI_BUILD_SOKOL=ON`. See [integrations/README.md](integrations/README.md).

The same game also runs in Unity 6: [integrations/unity](integrations/unity/README.md)
is a Unity package (a native plugin exporting the C API to C#, and a
`JsuiView` component that composites the UI as a uGUI overlay and passes on
the input it did not consume) and an example project built from the command
line. [integrations/unreal](integrations/unreal/README.md) is the same for
Unreal Engine 5: a plugin (`UJsuiWidget` / `SJsuiView`, drawing with Skia
Graphite on the Metal RHI's own queue) over JSUI as a shared library with just
the C API, and a C++ example project packaged headlessly with RunUAT.
