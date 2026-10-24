# JSUI for Unreal Engine @VERSION@

The JSUI plugin for Unreal Engine 5. It draws a React UI over your game,
with input pass-through and game <-> UI messages, state and per-frame data.

1. Unzip `Jsui/` into your project's `Plugins/` folder.
2. Add `"Jsui"` to the module dependencies in your game's `*.Build.cs`.
3. Regenerate the project files and build. Unreal compiles the plugin's
   module; the JSUI library itself is prebuilt in
   `Source/ThirdParty/JsuiLibrary` (macOS universal, Win64, Linux x86_64).

The UI is drawn on the GPU, on Unreal's own device: Skia Graphite on the
Metal RHI (macOS) and the Vulkan RHI (Linux, Windows with `-vulkan`), Skia
Ganesh on the D3D12 and D3D11 RHIs (Windows). Only the Metal path has been
run so far. On other RHIs, with `bForceCpu`, or when the GPU path fails, the
UI is rasterised on the CPU.

Usage (`UJsuiWidget` in UMG, `SJsuiView` in Slate), the bundle setup and
the example game: https://github.com/orktes/jsui/tree/main/integrations/unreal
