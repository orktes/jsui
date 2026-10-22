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

**Showcase: [orktes.github.io/jsui](https://orktes.github.io/jsui/)**, a site
rendered entirely by the JSUI WebAssembly build into one canvas: interactive
game screens, input, motion, a live-code playground and embedding guides. Its
source is ordinary React Native in [site/](site/) (`node site/scripts/build.mjs`,
then `node site/scripts/serve.mjs`).

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
import { Host, useHostState } from '@jsui/react';

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
