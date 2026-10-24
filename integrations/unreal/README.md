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
macOS with Xcode 26) and, for the GPU path, a Skia with Graphite / Direct3D
(`scripts/build-skia-desktop.sh`). Tested on macOS; the plugin is wired for
Win64 and Linux (the library's release artifacts, GPU paths included) but has
not been built or run there yet (see [GPU paths](#gpu-paths)).

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
where the GPU path is available) and `--dev` (with `npx jsui dev integrations/shared/ui/app.jsx` and
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
  GPU by default: Skia draws the UI straight into a `UTextureRenderTarget2D`
  on the RHI's own device, ordered with the RHI's own work on its graphics
  queue (per RHI below). `SJsuiView::Tick` (Slate's tick, after the world's)
  ticks the runtime and, when the UI changed, queues a render command; on
  the render thread the RHI's path hands a job to the RHI where it submits
  GPU work (`RHIRunOnQueue` on Metal, Vulkan and D3D12: the RHI's submission
  side, with its graphics queue, after the command lists submitted before;
  on D3D11 a command list lambda, where the immediate context is used). The
  job renders, repainting only what changed (`JSUI_GPU_INCREMENTAL`), and
  Skia submits its work to the queue before it returns; this frame's Slate
  draw, which samples the texture, is recorded later on the render thread,
  so the GPU runs it after Skia's work. The job uses the runtime, so the game
  thread waits for it before its next runtime call (`SJsuiView::Runtime`,
  usually already done). The GPU context is created once, by a job of its
  own on the first paint, shared by every view and freed at engine pre-exit
  (before the RHI's device goes). The plugin only makes virtual calls into
  the RHIs' public interfaces (`IMetalDynamicRHI`, `IVulkanDynamicRHI`,
  `ID3D12DynamicRHI`, `ID3D11DynamicRHI`). On any other RHI (OpenGL, Null),
  with `bForceCpu` / `--cpu`, if the library has no backend for the RHI or
  Skia rejects its device, or after a failed GPU render, the UI is
  rasterised on the CPU and the changed rectangle is uploaded into a
  `UTexture2D`. Either texture is premultiplied, holds sRGB-encoded values in
  a non-sRGB format (BGRA8) and is drawn by Slate with
  `ESlateDrawEffect::PreMultipliedAlpha | NoGamma`, so both paths look the
  same.
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

## GPU paths

| RHI | Platforms | Skia backend | Draws into | Verified |
|---|---|---|---|---|
| Metal | macOS | Graphite, the RHI's `MTLDevice` + queue | the render target | yes (UE 5.8.4, M1 Ultra) |
| D3D12 | Windows (default) | Ganesh, the RHI's `ID3D12Device` + direct queue | the render target | no |
| D3D11 | Windows (`-d3d11`) | Ganesh on a D3D12 device on the same adapter, copied in by the RHI's immediate context | the render target | no |
| Vulkan | Linux (default), Windows (`-vulkan`) | Graphite, the RHI's `VkDevice` + graphics queue | an image of the plugin's, copied into the render target | no |

What each path does to stay in order with Unreal's own GPU work and in step
with the state Unreal tracks for the texture (each file's header has the
details):

* **Metal** ([JsuiGpuMetal.cpp](Plugins/Jsui/Source/Jsui/Private/JsuiGpuMetal.cpp)).
  `IMetalDynamicRHI::RHIRunOnQueue`; Metal's hazard tracking orders Slate's
  sampling after Graphite's command buffer on the same queue. A new render
  target's clear is flushed first (`FlushRHIThread`). The Metal RHI has no
  native queue getter, so `RHIRunOnQueue` is the way onto its queue.
* **D3D12** ([JsuiGpuD3D12.cpp](Plugins/Jsui/Source/Jsui/Private/JsuiGpuD3D12.cpp)).
  The render thread transitions the render target `Unknown -> RTV` (so it is
  in `D3D12_RESOURCE_STATE_RENDER_TARGET`, whatever state Unreal had it in),
  makes it resident and flushes the barriers, then flushes the immediate
  command list to the GPU (`FlushRHIThread`) and queues the job with
  `ID3D12DynamicRHI::RHIRunOnQueue(Graphics)`. Ganesh takes the resource in
  `RENDER_TARGET` and leaves it there, executing its command lists on the
  direct queue during the job; the transition back `RTV -> SRVMask` and
  Slate's draw are recorded after it, so they execute after Ganesh's work in
  queue order.
* **D3D11** ([JsuiGpuD3D11.cpp](Plugins/Jsui/Source/Jsui/Private/JsuiGpuD3D11.cpp)).
  The job is an `EnqueueLambda` on the immediate command list, so it runs on
  the thread that owns the immediate context, in order with the RHI's own
  commands. The library draws on its own D3D12 device and copies the frame
  into the render target with the immediate context behind a shared fence
  (jsui.h); D3D11 has no resource states to keep in step.
* **Vulkan** ([JsuiGpuVulkan.cpp](Plugins/Jsui/Source/Jsui/Private/JsuiGpuVulkan.cpp)).
  Graphite renders only into images with `INPUT_ATTACHMENT` usage, which a
  `UTextureRenderTarget2D`'s image does not have, so it draws into an RHI
  texture of the plugin's (`RenderTargetable | ShaderResource |
  InputAttachmentRead`, created in `CopySrc`). Unreal keeps that image in
  `CopySrc` (`TRANSFER_SRC_OPTIMAL`); Graphite takes it in that layout
  (`UNDEFINED`, a full repaint, when new) and leaves it there. Per render:
  flush the immediate command list to the GPU (`FlushRHIThread`), queue the
  job with `IVulkanDynamicRHI::RHIRunOnQueue(Graphics)` (the graphics queue
  is held during it; Graphite's submission is bracketed by full pipeline
  barriers), then record `Unknown -> CopyDest`, the copy into the render
  target and `CopyDest -> SRVMask`: the render target's layout is only ever
  changed by the RHI. The context gets the RHI's instance, devices, graphics
  queue family, API version, `vkGetInstanceProcAddr` and loaded device
  extensions; the RHI does not expose its enabled instance extensions or
  feature chain, so Skia assumes no optional features.

Not verified yet: none of the Windows / Linux paths has been compiled or run.
They were written against the UE 5.8 RHI sources, which confirm:

* D3D12: `RHIRunOnQueue` queues behind the payloads submitted before it, and
  the submission thread executes the command lists batched so far before the
  callback (`D3D12Submission.cpp`). Legacy resource barriers are the default
  (`r.D3D12.PreferredBarrierImplementation=1`), the same kind Ganesh issues;
  the enhanced-barrier mode is untested. The residency / barrier-flush calls
  need the executing command list (hence `EnqueueLambda`).
* Vulkan: `InputAttachmentRead` on a `RenderTargetable` texture adds
  `INPUT_ATTACHMENT`, and every image gets `TRANSFER_SRC | TRANSFER_DST |
  SAMPLED` (`VulkanTexture.cpp`). `CopySrc` / `CopyDest` are
  `TRANSFER_SRC_OPTIMAL` / `TRANSFER_DST_OPTIMAL` (`VulkanBarriers.cpp`).
  `RHIRunOnQueue` queues behind the payloads submitted before it
  (`VulkanSubmission.cpp`).

Still to check: whether the Vulkan queue submits the payloads batched before
the callback, as D3D12 does (`FVulkanQueue::SubmitQueuedPayloads`), and that
the validation layer reports no layout errors. Each render on Vulkan / D3D12
waits for the RHI thread to catch up (`FlushRHIThread`), which costs the
render thread some parallelism on frames where the UI changed.

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
