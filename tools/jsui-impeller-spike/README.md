# Impeller spike

Can Flutter's Impeller renderer give JSUI what it promises on paper: no
shader-compile jank on first use and predictable frame times? This tool
answers that by drawing **the same JSUI frames** with JSUI's Skia Graphite
backend and with Impeller, on Metal (macOS), in a window.

## What Impeller claims vs what JSUI already does

| Impeller claim | How Impeller gets it | JSUI on Graphite today |
|---|---|---|
| No runtime shader compilation | A small fixed set of shaders compiled offline by `impellerc` (MSL / SPIR-V in the binary). It still builds the GPU pipeline objects at run time, mostly at context start. | Skia generates and compiles shaders at run time. To avoid first-use stalls: the compile pool (#113), the common-pipeline Precompile at context start (#116, #117 `jsui_gpu_prepare_target`), the pipeline cache file for later launches (#113), the Vulkan driver cache (#131) and Ganesh's shader cache (#119). Web: pipeline lists + `createRenderPipelineAsync` warm-up (#169). |
| Predictable frame times | Few pipeline variants, uber-shaders, everything known up front. | Many specialised Graphite pipelines. Unseen combinations still compile on demand, on a pool thread, so the frame does not wait (except on the web, #166). Shadows use cached nine-patch masks (#111). Text uses a glyph atlas in both. |
| Smaller and simpler | Core renderer under 200 KB compressed; text adds about 2.5 MB (the README's numbers). | Skia in libjsui is about 4.7 MB per arch (Graphite about 0.8 MB of it). |

Limits of Impeller for JSUI, from the SDK's header (`impeller.h`, version
1.4.0 as of Flutter 3.47.9):

- **The context owns the device.** `ImpellerContextCreateMetalNew` takes no
  device or queue, and its Vulkan context creates its own device. A host
  engine (Godot, Unity, Unreal) cannot hand its GPU device in, and JSUI's
  model is rendering into the host's device. Only OpenGL can wrap the host's
  FBO.
- **Metal renders only into a `CAMetalDrawable`**, not into an arbitrary
  texture, so it cannot render into a host's texture or JSUI's UI-in-texture
  targets.
- **No web backend.** Flutter web still draws with Skia (CanvasKit /
  Skwasm), and JSUI's top platform is the browser on WebGPU.
- **No API or ABI stability** promised yet. Prebuilt binaries are
  published per Flutter engine commit.
- JSUI exposes `SkCanvas` (native components, VectorView / RN Skia,
  ShaderView SkSL, Lottie via Skottie). Impeller could only ever be a second
  renderer next to Skia, not a replacement, so binary size goes up.

## What the tool does

`jsui-impeller-spike` loads each bundle (a game screen from
`examples/screens`) and renders every frame from t = 0, entrance animations
included, at simulated 60 Hz. Each frame is recorded once by JSUI's painter
for a GPU replay (`Surface::recordFrame`, the JS-thread path native hosts use
by default), then:

- **graphite** replays the recording through JSUI's Graphite Metal context
  (the production path, with all its warm-up machinery) into the window's
  drawable;
- **impeller** replays it into `ImpellerCanvas`, an `SkCanvas` subclass that
  issues the equivalent `ImpellerDisplayListBuilder` calls, and has Impeller
  draw and present the display list.

The window's `CAMetalLayer` has three drawables and no vsync wait, so the
present-to-present interval is the real frame cost (CPU or GPU bound) for
both. Context creation time, time to first present, memory
(`phys_footprint` and its peak) and what the translator could not map are
also reported.

The translation is faithful for what the painter emits for these screens
(rects, rounded rects, paths, clips, images, nine-patch shadow masks,
gradients, blur mask filters, colour filters, opacity / blend layers). Text
is the approximation: JSUI draws shaped glyph runs, and Impeller's API only
takes strings it shapes itself. So glyphs are mapped back to text and drawn
as cached Impeller paragraphs at the run's origin. That is the cost profile
Impeller would have for the same text, with slightly different glyph
positions. Compare `graphite-*.png` and `impeller-*.png` to check fidelity.

## Running it (Mac)

```sh
# Impeller SDK for the Flutter engine commit of your choice (here: Flutter 3.47.9 stable)
sha=89c39edfdfae2949f6806e4a315c0c08522238b7
curl -LO https://storage.googleapis.com/flutter_infra_release/flutter/$sha/darwin-arm64/impeller_sdk.zip
unzip impeller_sdk.zip -d ~/Development/impeller-sdk-$sha
cmake -B build -DJSUI_IMPELLER_SDK=$HOME/Development/impeller-sdk-$sha
cmake --build build --target jsui-impeller-spike
tools/jsui-impeller-spike/run.sh build --runs 3
```

`run.sh` takes the shared bench lock, runs each setup in a fresh process,
moves the macOS Metal shader cache away for first-launch runs (and restores
it at the end), and prints `report.mjs`'s tables:

| setup | meaning |
|---|---|
| graphite-bare | no pipeline cache file, no common precompile: about what Skia gives without warm-up |
| graphite-first | as shipped, first launch |
| graphite-second | as shipped, second launch (cache file present) |
| impeller-first | first launch |
| impeller-second | second launch (Metal's OS cache warm) |
| *-1440p | warm, 2560x1440 at 2x with a mouse sweep: steady-state GPU load |
