# JSUI in Unreal Engine

Two parts:

* [Plugins/Jsui](Plugins/Jsui) is an Unreal plugin: `UJsuiWidget` (UMG) and
  `SJsuiView` (Slate) draw a JSUI runtime over the game, forward input to it
  first and pass on what it did not consume. It calls only
  [the C API](../../include/jsui/jsui.h) of a prebuilt JSUI shared library, so
  the plugin source builds against any UE 5.x and only the C library is
  prebuilt per platform.
* [project](project) is a UE 5 C++ project with the engine examples' game
  ([../shared/ui/app.jsx](../shared/ui/app.jsx) over a small 3D scene), the
  same game as the [raylib](../raylib), [SDL3](../sdl3), [sokol](../sokol)
  and [Unity](../unity) examples. It has no assets: the game mode builds the
  scene in code from engine content (`/Engine/BasicShapes`) in the engine's
  empty `Entry` map.

## Build and run

Needs Unreal Engine 5 (tested with 5.8.4 from the Epic Games Launcher, on
macOS with Xcode 26) and, for the GPU path, a Skia with Graphite
(`scripts/build-skia-desktop.sh`). macOS today; the plugin is wired for Win64
and Linux (the library's release artifacts) but not tested there yet.

```sh
npm install
integrations/unreal/scripts/build-example.sh "/Users/Shared/Epic Games/UE_5.8" third_party/skia-desktop
"build/unreal/Mac/JsuiExample.app/Contents/MacOS/JsuiExample" -windowed -ResX=1280 -ResY=720
```

`build-example.sh` does three things, each of which can be run on its own:

```sh
# 1. the UI bundle, into the project (Content/Jsui is staged as a non-UFS dir)
npx jsui build integrations/shared/ui/app.jsx -o integrations/shared/ui/dist/bundle.js
mkdir -p integrations/unreal/project/Content/Jsui
cp integrations/shared/ui/dist/bundle.js integrations/unreal/project/Content/Jsui/

# 2. the C API as a shared library (libjsui.dylib, Graphite Metal on), staged
#    into Plugins/Jsui/Source/ThirdParty/JsuiLibrary/{include,lib,bin}
cmake -S . -B build-unreal -G Ninja -DCMAKE_BUILD_TYPE=Release -DJSUI_BUILD_SHARED=ON \
      -DJSUI_BUILD_TOOLS=OFF -DJSUI_BUILD_TESTS=OFF -DJSUI_BUILD_VIEWER=OFF \
      -DJSUI_SKIA_DIR=third_party/skia-desktop -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0
cmake --build build-unreal --target jsui_shared
integrations/unreal/scripts/stage-lib.sh build-unreal

# 3. build, cook, stage, package (headless) -> build/unreal/Mac/JsuiExample.app
"/Users/Shared/Epic Games/UE_5.8/Engine/Build/BatchFiles/RunUAT.sh" BuildCookRun \
  -project="$PWD/integrations/unreal/project/JsuiExample.uproject" -platform=Mac \
  -clientconfig=Development -build -cook -stage -pak -iostore -package -archive \
  -archivedirectory="$PWD/build/unreal" -unattended -utf8output -nop4 -nodebuginfo
```

The project also opens in the editor as is (`JsuiExample.uproject`; the editor
builds the game module and the plugin on first open). The library and the
copied bundle are not committed (`.gitignore`): run steps 1 and 2 first.

The game takes the examples' options, with values after `=` (Unreal reads a
bare argument as a map to open): `--selftest`, `--frames=N
--screenshot=out.png`, `--bundle=path-or-url`, `--cpu` (the CPU renderer even
on Metal) and `--dev` (with `npx jsui dev integrations/shared/ui/app.jsx` and
`--bundle=http://127.0.0.1:5173/bundle.js` for hot reload). `--selftest` is
the same pass-through check as the other examples (see
[../README.md](../README.md#build-and-run)): it logs `selftest: ok` and exits
0, or logs what failed and exits 1:

```sh
"build/unreal/Mac/JsuiExample.app/Contents/MacOS/JsuiExample" -windowed -ResX=1280 -ResY=720 --selftest --screenshot=/tmp/unreal.png
```

The example app is not sandboxed (`Build/Mac/Resources/Unsandboxed.entitlements`;
Unreal's default is the App Sandbox), so `--screenshot` and `--bundle` take
any path. Windowed games on macOS are locked to the display's refresh by
Unreal's Metal RHI (`displaySyncEnabled` is only off in exclusive fullscreen):
the HUD shows 60 fps on a 60 Hz display. Uncapped (`'-ExecCmds=r.SetRes
1280x720f'`, exclusive fullscreen) on a Mac Studio (M1 Ultra), about 320 fps on
either path: the engine's frame, not the UI, sets the pace.

## How it works

* **The library** ([Jsui.Build.cs](Plugins/Jsui/Source/Jsui/Jsui.Build.cs)).
  JSUI, Skia and every other dependency are one shared library exporting just
  the C API (`jsui_shared`, `-DJSUI_BUILD_SHARED=ON`): `libjsui.dylib`,
  `libjsui.so`, `jsui.dll` + `jsui.lib`. Unreal's own toolchains (its libc++
  on Linux, `/MD` on Windows) never link JSUI's C++, and none of the
  libraries Unreal bundles too (zlib, libpng, HarfBuzz, ICU) can clash. The
  build file links it and stages it next to the game binary
  (`$(BinaryOutputDir)`); on Windows the DLL is delay-loaded from there.
  [scripts/stage-lib.sh](scripts/stage-lib.sh) copies a local CMake build into
  `Source/ThirdParty/JsuiLibrary` in the layout of the release artifacts:
  `include/jsui/jsui.h`, `lib/libjsui.dylib` (universal), `lib/libjsui.so`,
  `bin/jsui.dll` + `lib/jsui.lib` (+ `bin/icudtl.dat`).
* **Rendering** ([JsuiHost.cpp](Plugins/Jsui/Source/Jsui/Private/JsuiHost.cpp)).
  On the Metal RHI, Skia Graphite draws the UI straight into a
  `UTextureRenderTarget2D` on the RHI's own `MTLDevice` and `MTLCommandQueue`.
  `SJsuiView::Tick` (Slate's tick, after the world's) ticks the runtime and,
  when the UI changed, queues a render command; on the render thread it hands
  a job to `IMetalDynamicRHI::RHIRunOnQueue`, which the RHI runs on its
  submission side with its direct queue, in order with the command buffers of
  the command lists submitted before it. Graphite commits its frame to that
  queue (repainting only what changed, `JSUI_GPU_INCREMENTAL`); this frame's
  Slate draw, which samples the texture, is queued later on the render thread,
  so it is committed after Graphite's work on the same queue and the GPU runs
  them in that order (like the Unity and Godot Metal paths). The job uses the
  runtime, so the game thread waits for it before its next runtime call
  (`SJsuiView::Runtime`, usually already done). The Metal RHI has no native
  queue getter (`RHIGetNativeGraphicsQueue` is null there): `RHIRunOnQueue`
  is the way onto its queue, and the plugin only makes virtual calls into the
  RHI, so nothing of MetalRHI is linked. On any other RHI, with `bForceCpu` /
  `--cpu`, or after a failed GPU render, the UI is rasterised on the CPU and
  the changed rectangle is uploaded into a `UTexture2D`. Either texture is
  premultiplied, holds sRGB-encoded values in a non-sRGB format and is drawn by
  Slate with `ESlateDrawEffect::PreMultipliedAlpha | NoGamma`, so both paths
  look the same. Vulkan and D3D12 would need their own job (Graphite has
  Vulkan; there is no D3D backend).
* **Input pass-through**
  ([SJsuiView.cpp](Plugins/Jsui/Source/Jsui/Private/SJsuiView.cpp)). An
  `IInputProcessor` sees every mouse and key event before Slate routes it and
  sends it to the UI. What the UI consumed stops there (and so does its
  release); the rest goes on to the game as usual and is also raised as
  `PassedMouseDown`, `PassedWheel` and `PassedKeyDown`. Typed text comes from
  `OnKeyChar` while the view has keyboard focus, which it takes when the UI
  consumes a press. In the example: drag empty space to orbit, wheel to zoom,
  Space spawns, Esc pauses. No gamepad yet.
* **Game <-> UI.** `OnMessage` delivers `Host.postMessage` calls;
  `SetState(key, json)` feeds `useHostState`, `SetBuffer(key, floats)` a frame
  buffer read with `useHostBufferValue` (the example projects each unit with
  this frame's camera and writes its screen position, so the nameplates move
  with no JS per frame). Each has a Blueprint version on `UJsuiWidget`.

## Using it in your game

Unzip `jsui-unreal-X.Y.Z.zip` from the
[GitHub Releases](https://github.com/orktes/jsui/releases) into your project's
`Plugins/`. It holds the plugin source plus the prebuilt JSUI library for
macOS (universal), Win64 and Linux x86_64, and Unreal compiles the plugin's
module. To use this checkout instead, copy `Plugins/Jsui` with the library
staged by `scripts/stage-lib.sh`. Add `"Jsui"` to your module's
dependencies, put your bundle in `Content/Jsui/bundle.js` and add
`+DirectoriesToAlwaysStageAsNonUFS=(Path="Jsui")` to `DefaultGame.ini`'s
`[/Script/UnrealEd.ProjectPackagingSettings]`. Then:

```cpp
UJsuiWidget* Ui = NewObject<UJsuiWidget>(this);   // Bundle: Content/Jsui/bundle.js
Ui->OnMessageNative.AddLambda([this](const FString& Name, const FString& Json) {
    if (Name == TEXT("game:spawn")) Spawn();
});
Ui->PassedMouseDownNative.AddLambda([this](const FKey& Button, const FVector2D& Pos) {
    SelectUnitAt(Pos);   // only clicks the UI did not take
});
Ui->AddToViewport();
Ui->SetState(TEXT("game"), TEXT("{\"hp\":100}"));
```

From Blueprints: construct a `JSUI View` widget (or place one in a UMG layout),
bind `On Message` / `Passed Mouse Down`, call `Set State`.
