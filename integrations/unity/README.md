# JSUI in Unity

Two parts:

* [package/](package) is a Unity package (`com.orktes.jsui`): a native plugin
  over [the C API](../../include/jsui/jsui.h) plus a `JsuiView` component that
  draws a JSUI runtime over the game as a full-screen uGUI layer.
* [project/](project) is a Unity 6 project with the engine examples' game
  ([../shared/ui/app.jsx](../shared/ui/app.jsx) over a small 3D scene), the
  same game as the [raylib](../raylib), [SDL3](../sdl3) and [sokol](../sokol)
  examples.

## Build and run

Needs Unity 6 (tested with 6000.3.27f1, 6.3 LTS) with a signed-in Hub and an
active license (Personal is fine). macOS and Linux; Windows is not wired up
yet (the plugin's exports need a `JSUI_SHARED` build of the library).

```sh
npm install
npx jsui build integrations/shared/ui/app.jsx -o integrations/shared/ui/dist/bundle.js
# the native plugin -> integrations/unity/package/Plugins/<platform>/
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DJSUI_BUILD_UNITY=ON
cmake --build build --target jsui_unity
# the player -> build/unity/JSUI Example.app (macOS)
Unity -batchmode -nographics -projectPath integrations/unity/project \
      -executeMethod BuildExample.Build -logFile -
"build/unity/JSUI Example.app/Contents/MacOS/JSUI Example" -screen-width 1280 -screen-height 720 -screen-fullscreen 0
```

`Unity` is the editor binary, e.g.
`/Applications/Unity/Hub/Editor/6000.3.27f1/Unity.app/Contents/MacOS/Unity`.
The project also opens in the editor as is: **JSUI > Build Example** builds
the player, Play runs `Assets/Example/Example.unity` (after **JSUI > Copy
Bundle + Save Example Scene** has copied the bundle into StreamingAssets).

The player takes the examples' options after Unity's own: `--selftest`,
`--frames N --screenshot out.png`, `--bundle path-or-url` and `--dev` (with
`npx jsui dev integrations/shared/ui/app.jsx` and `--bundle
http://127.0.0.1:5173/bundle.js` for hot reload). `--selftest` is the same
pass-through check as the other examples (see
[../README.md](../README.md#build-and-run)) and logs `selftest: ok`.

## How it works

* **Native plugin** ([native/jsui_unity.cpp](native/jsui_unity.cpp)): the
  static jsui library linked into a loadable module (`jsui_unity.bundle`,
  `libjsui_unity.so`), so every `jsui_*` function is exported and the C#
  side P/Invokes the C API directly ([JsuiNative.cs](package/Runtime/JsuiNative.cs)).
  Its few `jsuiu_*` helpers cover what P/Invoke does badly: JS messages and log
  lines are queued inside `jsui_tick` and drained by C# afterwards (no native
  to managed callbacks, which IL2CPP restricts), and the CPU frame is kept
  between frames so `jsui_render_rgba_incremental` repaints only what changed.
* **Rendering** ([JsuiView.cs](package/Runtime/JsuiView.cs)): the UI is
  rasterised on the CPU and, on frames where it changed, uploaded into a
  `Texture2D` (`LoadRawTextureData`) shown by a `RawImage` on a Screen Space -
  Overlay canvas. The texture is premultiplied and drawn with
  [a premultiplied-alpha shader](package/Runtime/Resources/JsuiPremultiplied.shader),
  flipped (the rows are top-down). It works on every graphics API Unity runs
  on. A GPU path (Skia on Unity's Metal / Vulkan device through a native
  rendering plugin) would skip the upload; it is not built yet.
* **Input pass-through.** `JsuiView` sends mouse input (polled in its
  `Update`, which runs before other scripts) and keyboard and text input (IMGUI
  events, which carry key repeats and typed characters) to the UI first. What
  the UI did not consume is raised as `PassedMouseDown`, `PassedWheel` and
  `PassedKeyDown`; the game listens to those instead of `Input`. In the
  example: drag empty space to orbit, wheel to zoom, Space spawns, Esc pauses.
  It uses the legacy Input Manager (`Active Input Handling` must be "Input
  Manager (Old)" or "Both"). No gamepad yet.
* **Game <-> UI.** `OnMessage` delivers `Host.postMessage` calls;
  `SetState(key, json)` feeds `useHostState`, `SetBuffer(key, floats, n)` a
  frame buffer read with `useHostBufferValue` (the example writes each unit's
  screen position, from `WorldToUi`, so the nameplates move with no JS per
  frame).

## Using it in your game

Add the package (Package Manager > Install package from disk, or
`"com.orktes.jsui": "file:<path>/integrations/unity/package"` in
`Packages/manifest.json`), build the plugin as above, put your bundle in
`Assets/StreamingAssets/jsui/bundle.js` and add a `JsuiView`:

```csharp
var ui = gameObject.AddComponent<Jsui.JsuiView>();   // bundle: StreamingAssets/jsui/bundle.js
ui.OnMessage += (name, json) => { if (name == "game:spawn") Spawn(); };
ui.PassedMouseDown += (button, pos) => SelectUnitAt(pos);   // only clicks the UI did not take
ui.SetState("game", "{\"hp\":100}");
```
