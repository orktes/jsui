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
active license (Personal is fine). macOS, Linux (x86_64) and Windows (x64).

```sh
npm install
npx jsui build integrations/shared/ui/app.jsx -o integrations/shared/ui/dist/bundle.js
# the native plugin -> integrations/unity/package/Plugins/<platform>/
# (JSUI_SKIA_DIR: Skia m157 from scripts/build-skia-desktop.sh, for the GPU
# path; with the m124 prebuilt the plugin is CPU-only)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DJSUI_BUILD_UNITY=ON \
      -DJSUI_SKIA_DIR=third_party/skia-desktop
cmake --build build --target jsui_unity
# Windows (Git Bash, as Skia is built: clang-cl, static CRT):
#   cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -T ClangCL -DJSUI_BUILD_UNITY=ON \
#         -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DJSUI_SKIA_DIR=third_party/skia-desktop
#   cmake --build build --config Release --target jsui_unity
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
`--frames N --screenshot out.png`, `--bundle path-or-url`, `--cpu` (the CPU
renderer even where the GPU path works) and `--dev` (with
`npx jsui dev integrations/shared/ui/app.jsx` and `--bundle
http://127.0.0.1:5173/bundle.js` for hot reload). `--selftest` is the same
pass-through check as the other examples (see
[../README.md](../README.md#build-and-run)) and logs `selftest: ok`.

## How it works

* **Native plugin** ([native/jsui_unity.cpp](native/jsui_unity.cpp)): the
  static jsui library linked into a loadable module (`jsui_unity.bundle`,
  `libjsui_unity.so`, `jsui_unity.dll`), so every `jsui_*` function is
  exported and the C# side P/Invokes the C API directly
  ([JsuiNative.cs](package/Runtime/JsuiNative.cs)).
  Its few `jsuiu_*` helpers cover what P/Invoke does badly: JS messages and log
  lines are queued inside `jsui_tick` and drained by C# afterwards (no native
  to managed callbacks, which IL2CPP restricts), and the CPU frame is kept
  between frames so `jsui_render_rgba_incremental` repaints only what changed.
  It is also a Unity native rendering plugin (`UnityPluginLoad`,
  [native/jsui_unity_plugin.cpp](native/jsui_unity_plugin.cpp)) with a GPU
  backend per graphics API ([native/jsui_unity_gpu.h](native/jsui_unity_gpu.h)),
  picked when Unity's device initialises. The few plugin API declarations it
  uses are in [native/unity_api/jsui_unity_api.h](native/unity_api/jsui_unity_api.h),
  so no Unity install is needed to build it; `-DJSUI_UNITY_PLUGIN_API_DIR=<an
  editor's PluginAPI folder>` (`Unity.app/Contents/PluginAPI`,
  `Editor/Data/PluginAPI`) builds against Unity's own headers instead and
  compiles a check that the two agree. On Windows the DLL needs Skia's ICU
  data (`icudtl.dat`) beside it; the build puts it there, and
  [Editor/JsuiBuildPostprocess.cs](package/Editor/JsuiBuildPostprocess.cs)
  copies it into Windows players.
* **Rendering** ([JsuiView.cs](package/Runtime/JsuiView.cs)). The GPU path
  is the default: Skia draws the UI straight into a `RenderTexture` on
  Unity's own graphics device, repainting only what changed
  (`JSUI_GPU_INCREMENTAL`). After `jsui_tick` in `LateUpdate` the view queues
  a render job as two `GL.IssuePluginEvent` calls (prepare, render), which
  run before the frame draws:

  | Unity's graphics API | Platforms | Drawn by | How it is ordered with Unity's frame |
  | --- | --- | --- | --- |
  | Metal | macOS | Skia Graphite on Unity's `MTLDevice` / `MTLCommandQueue` ([jsui_unity_metal.mm](native/jsui_unity_metal.mm)) | commits what Unity recorded so far, then Skia's command buffer, on the same queue |
  | Vulkan | Linux, Windows | Skia Graphite on Unity's `VkDevice` / graphics queue ([jsui_unity_vulkan.cpp](native/jsui_unity_vulkan.cpp)) | `AccessTexture` records the barrier into Unity's command buffer (and Unity tracks the layout), then `AccessQueue(flush)` renders once Unity's work so far is submitted and the queue is free |
  | Direct3D 11 (Windows default) | Windows | Skia Ganesh on a D3D12 device on the same adapter, copied into the texture on Unity's immediate context ([jsui_unity_d3d.cpp](native/jsui_unity_d3d.cpp)) | the copy is queued on the render thread like Unity's own commands |
  | Direct3D 12 | Windows (Unity 6.3+: `IUnityGraphicsD3D12v8`) | Skia Ganesh on Unity's `ID3D12Device` / command queue ([jsui_unity_d3d.cpp](native/jsui_unity_d3d.cpp)) | `RequestResourceState` in Unity's command list, then a render event configured for queue access after Unity flushed |

  The job uses the runtime, so the main thread waits for it
  (`jsuiu_wait_render`, usually already done) before its next runtime call;
  `JsuiView.Runtime` does that for game code. The CPU path is the fallback
  (OpenGL Core / ES, a CPU-only plugin, `forceCpu` / `--cpu`, D3D12 before
  Unity 6.3, or a failed GPU render, which logs why): the UI is rasterised
  on the CPU and, on frames where it changed, uploaded into a `Texture2D`. Either texture is premultiplied, holds sRGB-encoded values in a
  non-sRGB format and is shown by a `RawImage` on a Screen Space - Overlay
  canvas with [a premultiplied-alpha shader](package/Runtime/Resources/JsuiPremultiplied.shader)
  (which decodes to linear in Linear projects), flipped (the rows are
  top-down), so both paths look the same. In the example on a Mac Studio (Apple silicon),
  uncapped: about 2700 fps on the GPU path, 1100 on the CPU path. The Vulkan
  backend is tested without Unity in CI:
  [native/test/unity_vulkan_test.cpp](native/test/unity_vulkan_test.cpp)
  loads the built plugin, plays Unity's part over a lavapipe device (the
  plugin API, a frame recorded into command buffers submitted at its end,
  queue-access callbacks run synchronously and from a submission thread)
  under synchronization validation, and compares the frames with the CPU
  raster.
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

Add the package with the native plugin prebuilt. In Package Manager, use
**+ > Install package from git URL** with
`https://github.com/orktes/jsui.git#upm` (or `#upm/vX.Y.Z` for a fixed
version), or **Install package from tarball** with
`com.orktes.jsui-X.Y.Z.tgz` from the
[GitHub Releases](https://github.com/orktes/jsui/releases). The prebuilt plugin
covers macOS (universal; GPU path on Metal), Linux x86_64 (Vulkan) and
Windows x64 (Vulkan, Direct3D 11, Direct3D 12). To work on the package itself, use
`"com.orktes.jsui": "file:<path>/integrations/unity/package"` in
`Packages/manifest.json` and build the plugin as above. Then put your bundle in
`Assets/StreamingAssets/jsui/bundle.js` and add a `JsuiView`:

```csharp
var ui = gameObject.AddComponent<Jsui.JsuiView>();   // bundle: StreamingAssets/jsui/bundle.js
ui.OnMessage += (name, json) => { if (name == "game:spawn") Spawn(); };
ui.PassedMouseDown += (button, pos) => SelectUnitAt(pos);   // only clicks the UI did not take
ui.SetState("game", "{\"hp\":100}");
```
