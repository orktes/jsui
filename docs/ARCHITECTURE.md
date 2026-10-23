# JSUI architecture

JSUI is a small, embeddable UI runtime for games: React (JSX) components
written exactly like React Native code, executed by QuickJS, laid out by Yoga
and painted by Skia. It is a "browser-shaped" stack (JS + layout + 2D paint +
resource loading + input) without a browser: the whole runtime is a static
library with a C API, and the same core compiles to WebAssembly.

```
 ┌───────────────────────── app bundle (JS) ──────────────────────────┐
 │  import { View, Text, Image, StyleSheet } from 'react-native'       │
 │  react + react-reconciler + @jsui/react (host config, components,   │
 │  event system, StyleSheet)  — bundled by @jsui/cli (esbuild)        │
 └───────────────┬───────────────────────────────▲─────────────────────┘
   __jsui_native.* (create/setProps/append…)     │ __jsui_dispatchEvent(ev)
 ┌───────────────▼───────────────────────────────┴─────────────────────┐
 │ JsEngine (QuickJS-ng): globals, timers, rAF, fetch, fonts, messages │
 ├──────────────────────────────────────────────────────────────────────┤
 │ Document / Node tree ── Yoga layout ── Text (skparagraph) ── Images  │
 │ InputManager: hit test, pointer/keyboard/gamepad, focus navigation   │
 │ Painter (Skia): backgrounds, gradients, borders, radii, shadows,     │
 │                 transforms, opacity, clipping, images, text          │
 ├──────────────────────────────────────────────────────────────────────┤
 │ ResourceLoader: file, http (dev server), embedder callback, browser  │
 └───────────────┬──────────────────────────────────────────────────────┘
          C API  │ include/jsui/jsui.h
   ┌─────────────┼───────────────┬─────────────────────┬──────────────┐
   │ jsui-render │ jsui-viewer   │ web/ (WASM + canvas)│ game engine  │
   │ (headless)  │ (window, dev) │                     │ (Godot, ...) │
   └─────────────┴───────────────┴─────────────────────┴──────────────┘
```

## Why these choices

* **C++20 core.** Skia and Yoga are C++, QuickJS is C. One language, no FFI
  layer, a single static library, and Emscripten builds it for the browser.
  Game engines (Godot GDExtension, Unreal, Unity native plugins) consume C/C++
  directly. The public surface is a C header so any language can bind it.
* **QuickJS-ng** — small, fast startup, ES2023, embeddable, compiles to WASM.
  The engine is isolated behind `JsEngine`, so a different engine (or the
  browser's own JS when running as WASM) can be slotted in later.
* **Yoga** — the layout engine React Native uses, so flexbox behaves exactly
  like RN (column default, `flexShrink: 0`, `position: relative`, etc.).
* **Skia** — what Chrome and Flutter paint with; gives us anti-aliased rounded
  rects, blurs for shadows, gradients, text shaping (HarfBuzz + ICU via
  skparagraph) and image codecs. Native builds use prebuilt static libraries
  (aseprite/skia m124); WASM uses the same milestone built with
  `scripts/build-skia-wasm.sh`.
* **CPU raster first, GPU optional.** By default the runtime paints into an
  RGBA buffer the embedder uploads as a texture. Deterministic (great for
  reference tests) and works everywhere. The optional GPU backend runs the
  same `paintDocument()` on a Skia Ganesh surface wrapping the embedder's
  OpenGL / GLES / WebGL framebuffer or texture (see [GPU backend](#gpu-backend)).
* **React Native semantics, written as `react-native` imports.** App code
  imports from `'react-native'`; the JSUI bundler aliases that to
  `@jsui/react`, and the Chrome reference build aliases it to
  `react-native-web`. Code an LLM (or person) writes for RN therefore runs
  unchanged in JSUI and in the browser reference.

## Repository layout

| Path | What |
| --- | --- |
| `include/jsui/jsui.h` | Public C API (embedding, input, resources, messages, shared state, calls) |
| `src/core/` | `Value`, `Node`, `Document`, style parsing (`style.cpp`) |
| `src/render/` | Skia painter, image cache, GPU backend (`gpu.cpp`) |
| `src/text/` | Font manager, paragraph building and Yoga measure for `Text` |
| `src/js/` | QuickJS host (`JsEngine`) and bridge globals |
| `src/input/` | Hit testing, event routing, focus and gamepad navigation |
| `src/platform/` | Resource loader backends (file, http, emscripten) |
| `src/net/` | Minimal HTTP/1.1 client for dev servers (native) |
| `src/runtime.*`, `src/capi.cpp` | Runtime object and C API wrappers |
| `tools/jsui-render` | Headless renderer: bundle → PNG (tests, CI) |
| `tools/jsui-record` | Headless recorder: bundle + JSON input timeline → 60 fps frames / H.264 (ffmpeg), with cursor, caption and input-legend overlay |
| `tools/jsui-viewer` | Windowed host with dev mode (live reload) |
| `tools/jsui-bench` | Frame benchmark: per-phase time (JS, commit, Animated, style, layout, input, paint), work counters and allocations per frame (see "Performance") |
| `tools/jsui-paint-bench` | Paint benchmark: paint time, pixels repainted, draw calls and GL calls per frame on the CPU raster and Ganesh GL (see "Paint performance") |
| `integrations/godot` | Godot 4 GDExtension (`JsuiView` Control) + demo project |
| `web/` | Browser host for the WASM build |
| `packages/react` | `@jsui/react`: reconciler, components, StyleSheet, events |
| `packages/cli` | `@jsui/cli`: `jsui build` / `jsui dev` (esbuild + dev server) |
| `examples/` | Realistic game UI examples (shared by reference tests) |
| `tests/native/` | C++ unit tests (`jsui-tests`) |
| `tests/reference/` | Pixel comparison against react-native-web in headless Chrome |
| `assets/fonts/` | Bundled default font (Inter, OFL) |

## Frame loop

The embedder owns the loop (see `jsui.h`):

1. Forward input with `jsui_send_*`. The InputManager hit-tests against the
   last layout and calls `__jsui_dispatchEvent` synchronously.
2. `jsui_tick(now)`: deliver finished resource loads, run due timers, run
   `requestAnimationFrame` callbacks, drain promise jobs, advance scroll
   physics, evaluate native-driven Animated animations (then deliver their
   end callbacks / listener updates to JS), run Yoga if dirty, advance
   LayoutAnimations, emit `onLayout` events.
   `jsui_tick_native(now)` does only the native part (native-driven Animated,
   LayoutAnimations, layout) without running any JS, for hosts that skip JS
   on a frame (see "Animated").
3. If `jsui_needs_redraw()`, `jsui_render_rgba()` paints the tree (or
   `jsui_render_gl()` with the GPU backend).

Hosts that sleep when idle check `jsui_wants_frame()`: it stays set while
something moves on its own (rAF callbacks, due timers, in-flight loads,
scroll flings / smooth scrolling / snapping, scroll indicator fades,
LayoutAnimations, native-driven Animated animations and their pending end
callbacks, held gamepad sticks or D-pad) and the host should keep ticking at
display rate. JS-driven Animated animations run on `requestAnimationFrame`,
so an endless `Animated.loop` keeps it set without any input either way.

Everything is single-threaded except resource I/O, which completes onto a
queue drained in `jsui_tick`.

## GPU backend

`jsui_gpu_create_gl()` / `jsui_render_gl()` / `jsui_render_gl_target()`
(`include/jsui/jsui.h`, `src/render/gpu.cpp`) draw a frame with Skia Ganesh
into the embedder's GL target while its GL context is current:

```c
JsuiGpuContext* gpu = jsui_gpu_create_gl(get_proc, user);  // or NULL: EGL/GLX/WGL/CGL/WebGL default
...
jsui_tick(rt, now);
jsui_render_gl(rt, gpu, 0 /* default framebuffer */, w, h, JSUI_GL_NO_CLEAR);  // HUD over the scene
// or a texture the engine samples:
JsuiGLTarget t = { .texture = tex, .width = w, .height = h, .flags = JSUI_GL_TOP_LEFT_ORIGIN };
jsui_render_gl_target(rt, gpu, &t);
```

* **Same painter, same damage tracking.** `Runtime::renderCanvasTracked()`
  lays out, refreshes the incremental paint records and tells the backend
  how much changed. GPU targets (swapped default framebuffers in particular)
  are not assumed to keep the previous frame, so:
  * when little changed (a spinner, a hover, a caret), only the damage is
    repainted into a retained frame owned by the `JsuiGpuContext` (one per
    runtime), as up to four scissored rect passes covering the damage
    (`paintDocumentRects`; a region clip would cost Ganesh a stencil or mask
    pass), and that frame is drawn into the target with one draw;
  * when at least half the frame changed (a full-screen animation, a
    scrolling list), the whole frame is painted straight into the target.
    The retained frame is repainted fully once the damage stayed small for
    3 frames, and freed after 300 direct frames.
  `JSUI_GL_NO_RETAIN` and MSAA targets always paint the whole frame into the
  target. Each frame ends with one `flushAndSubmit` and no CPU wait. A later
  `jsui_render_rgba_incremental()` repaints fully. `jsui_needs_redraw()`
  still says when a frame is needed.
* **Effects.** Everything the painter uses is plain Skia: blurred shadows
  (the painter's cached shadow masks are CPU images drawn as sprites; Ganesh
  uploads each once), `saveLayer` with opacity / image filters (blur, drop
  shadow, colour matrices) / blend modes, gradients, clips, text (Ganesh glyph
  atlases from the same scaler) and images. Output matches the CPU raster to
  anti-aliasing / blur rounding (see `tests/native/gpu_test.cpp`: mean channel
  difference < 0.25 on the test scene, < 0.6 on the example screens).
* **Images.** Decoded images stay raster `SkImage`s owned by the runtime's
  `ImageCache`; Ganesh uploads each on first draw and keeps the texture in its
  resource cache keyed by the image (freed when the image is). Nothing in a
  runtime is tied to a GPU context, so one `JsuiGpuContext` can serve several
  runtimes, and a runtime can move between contexts or back to the CPU.
* **GL state.** Each render starts with `GrDirectContext::resetContext()`, so
  the embedder may change any GL state between frames. Skia leaves its own
  state bound afterwards (framebuffer, program, textures, blend, scissor,
  viewport, VAO, pixel store); engines with a GL state cache re-apply theirs.
* **Context loss.** `jsui_gpu_context_lost()` abandons all GPU objects
  without GL calls; the next render rebuilds the Ganesh context through the
  same loader from whichever context is current then (the web host does this
  on `webglcontextrestored`).
* **Loader.** `get_proc` (e.g. `SDL_GL_GetProcAddress`) or NULL for the
  platform default. On Linux nothing GL-related is linked: libEGL / libGL are
  dlopen'ed and whichever has a current context is used, and the prebuilt
  Skia's GLX references resolve to hidden forwarders. `egl*` names are only
  answered while an EGL context is current (GLX returns stubs for any name,
  which Skia would call).
* **Targets.** `JsuiGLTarget` takes an FBO (0 = default framebuffer) or a
  `GL_TEXTURE_2D`, the sized format (`GL_RGBA8` default, `GL_BGRA8_EXT`,
  `GL_RGB8`, `GL_SRGB8_ALPHA8` (blends in linear light), `GL_RGB10_A2`,
  `GL_RGBA16F`), MSAA sample count and stencil bits of an FBO, the origin
  (`JSUI_GL_TOP_LEFT_ORIGIN` for textures sampled with v=0 at the top) and
  `JSUI_GL_NO_CLEAR` to composite over the current contents. Wrapped FBOs
  are cached (up to four); textures are wrapped per render (a deleted and
  recreated texture may reuse its name).
* **Builds.** CMake option `JSUI_GPU` (default ON for native builds; the
  aseprite prebuilt Skia includes Ganesh GL (checked for Linux x64); macOS
  also links the OpenGL framework). WASM: `WEBGL=1 scripts/build-wasm.sh`
  builds Skia with Ganesh + WebGL (`third_party/skia-wasm-webgl`, canvaskit's
  WebGL args) and JSUI with `-sMAX_WEBGL_VERSION=2` into
  `web/dist-webgl/jsui.{js,wasm}` (the module the web host loads by
  default); the plain WASM build (`web/dist/`) is CPU-only and its
  `jsui_gpu_*` entry points return failure.
* **Viewer / web: GPU by default.** `jsui-viewer` renders into the SDL GL
  window's default framebuffer (`--screenshot` reads it back, and fails if
  the frame is blank); if the GL window, GL context or Skia GPU context
  cannot be created it prints a warning and uses the CPU raster + SDL
  texture. `--cpu` forces the CPU path, `--gpu` is accepted as a no-op
  alias. `jsui-render` / `jsui-record` stay CPU (deterministic output).
  On macOS it asks SDL for a 3.2 core, forward-compatible
  profile (`--gl-core`, default there): the default legacy 2.1 context only
  has GLSL 1.20, which Skia's shaders cannot use (`gl_VertexID`). Ganesh
  detects core profiles (`GL_CONTEXT_PROFILE_MASK`) and binds its own vertex
  array object, since core profiles have no default one. In the browser,
  `createJsui({ canvas })` (renderer `'auto'`) loads `web/dist-webgl/jsui.js`
  when WebGL is available (else, or when that is not deployed,
  `web/dist/jsui.js`), registers the canvas' WebGL2 (or WebGL 1) context with
  emscripten and draws through Skia, unless WebGL is only available through
  a software rasteriser (`failIfMajorPerformanceCaveat`, e.g. Chrome's
  SwiftShader fallback on a machine without a usable GPU: there Skia's full
  GPU repaints cost more than the incremental CPU raster; `renderer: 'gpu'`
  uses it anyway). It falls back to the CPU raster
  (`jsui.renderer` `'webgl2'` / `'webgl'` / `'2d'`, a `'renderer'` event) when
  the module has no GPU backend, the Skia GPU context cannot be created, a
  GPU frame fails, or the WebGL context is lost and not restored within
  `contextRestoreTimeout` (default 3000 ms; the canvas is then replaced by a
  clone, as a canvas with a dead WebGL context cannot get another context).
  `renderer: 'cpu'` forces the CPU raster. Measuring GPU frames, note that
  `jsui.stats.renderMs` only covers issuing the GL commands; time a frame
  with `gl.finish()` to include the GPU (or SwiftShader) work.
* **CPU path in the browser.** `jsui_render_rgba_incremental()` repaints only
  the damaged pixels of a buffer in the WASM heap, and the host uploads only
  the damaged rect (WebGL2: `UNPACK_ROW_LENGTH`; WebGL 1: the rows it spans;
  Canvas2D: un-premultiplies and puts just that rect); an unchanged frame
  paints and uploads nothing (`jsui.stats.renders` / `blits` / `blitPixels`
  / `lastBlit`, asserted by the browser tests' perf guards). Skia for WASM is
  built with SIMD128 and `-msse4.1` (`scripts/build-skia-wasm.sh`): without
  an SSE level Skia's raster pipeline runs its scalar fallback (one pixel per
  stage call, no 16-bit lowp pipeline), ~10x slower than native on
  gradients, image draws and blending.
* **Tests.** `tests/native/gpu_test.cpp` makes an EGL context (Mesa's
  surfaceless platform / llvmpipe works headless), on desktop GL, a 3.2 core
  profile (as on macOS), GLES 3 and GLES 2, or on macOS a headless CGL 3.2
  core context, renders scenes through the GPU path and compares with the
  CPU output; without a context it prints SKIP (`JSUI_GPU_TEST_REQUIRE=1`, set in CI,
  makes that a failure). `JSUI_WEB_DIST=dist-webgl node web/test/browser.test.mjs`
  runs the browser tests against the GPU module, including GPU-vs-CPU pixel
  comparisons.

### WebGPU (Skia Graphite)

`jsui_gpu_create_webgpu(instance, device, queue)` / `jsui_render_webgpu()` /
`jsui_render_webgpu_target()` (`src/render/gpu_webgpu.cpp`, CMake option
`JSUI_WEBGPU`) draw with Skia Graphite's Dawn backend into a `WGPUTexture`
of the embedder's device: the browser's WebGPU in the WASM build (through
Emdawnwebgpu's `webgpu.h`), and later Dawn native with an engine's device and
texture (the API takes plain `webgpu.h` handles; a non-null `instance` is
used to process Dawn events).

* **Direct or retained, per frame.** `Runtime::renderCanvasTracked` (the
  same damage tracking as the CPU path) tells the context the bounds of the
  changed pixels before anything is painted. When they cover half the frame
  or more (an animated full-screen UI; the area of the damage rects, not
  their bounds) the frame is painted straight into the target: one pass, no
  copy. When less changed, only the damage is repainted, rect by rect
  (`splitDamage`: each a scissor, where a region clip is a complex clip on
  Graphite). Canvas textures start cleared every frame, so the last frame
  comes from a copy the context keeps (copy-back, for targets with
  `CopySrc` usage such as the web canvas): the frame's single pass first
  draws that copy into the target, then the damage over it, and afterwards
  the damage rects are copied (`copyTextureToTexture`) from the target into
  the copy. That is one pass, like a direct frame, plus a full-frame read of
  the copy. Targets without `CopySrc`, and `JSUI_WEBGPU_NO_CLEAR`, use a
  retained Graphite render target instead: the damage pass loads and stores
  all of it and a second pass draws it into the target. On a 4K canvas on
  an Apple GPU those two full-frame passes cost more than a full repaint
  (GPU done 4.5 vs 2.4 ms at 1080p@2x). Damage crossing a curved outline
  does not grow to the whole node here (see Paint performance), so a few
  small animations stay small: on `interactions.jsx` the glow, the pulsing
  ring and the live dot are ~240k of 920k pixels, where growing to the
  cards and buttons under the glow made every frame a full repaint. After
  direct frames the last frame is made valid again once the damage stayed
  small for 3 frames (a full repaint, copied whole). The web host skips frames
  where nothing changed altogether (no `getCurrentTexture()`, so the canvas
  keeps its last frame). `JSUI_WEBGPU_NO_RETAIN` always paints the whole
  frame into the target; `JSUI_WEBGPU_NO_CLEAR` composites over it.
* **No MSAA, few passes.** Browser WebGPU has none of the features Graphite
  needs to make its internal MSAA cheap (ExpandResolveTexture load ops,
  transient attachments, MSAA-render-to-single-sampled), so a single path or
  complex clip used to turn the whole pass into a 4x MSAA pass: a
  full-surface draw to load it, 4x samples stored to memory and a second
  full-surface pass to resolve it, which on a tiler (Apple GPUs) dominated
  the frame. The context sets `fInternalMultisampleCount = 1` in the browser,
  so paths go through Graphite's raster path atlas. Inset shadows (a blurred
  DRRect, which Graphite has no analytic blur for: an offscreen mask plus
  blur passes each) are pre-rendered once per shape and drawn as sprites,
  like outer shadows (`drawCachedInsetShadow`, also on the CPU path). On
  `examples/showcase/interactions.jsx` that took a frame from 53 render passes
  (one MSAA 1536x1024 pass, two full-surface copies, ~45 blur-mask passes) to
  8, and from ~225 draws to ~180 (`node web/test/frame-bench.mjs` prints the
  WebGPU calls per frame). The device asks for `dual-source-blending` when
  the adapter has it (anti-aliased non-src-over Porter-Duff draws then need
  no destination copy).
* **Per-draw cost.** Chrome validates and translates every WebGPU call in
  its GPU process, and `finish` / GPU-done times on a real GPU include that
  work, so the call count is what the backend pays for. Browser WebGPU has
  no push constants, so Graphite's Dawn backend made a new uniform bind
  group for every pipeline or uniform buffer change (~175 per frame); the
  Skia patch caches them in `DawnResourceProvider` by layout, buffers and
  binding sizes (the offsets are dynamic), which leaves ~5 new ones per
  frame. Two more parts of the patch keep damage-only frames exact: a
  whole-pixel device rect clip is applied as a scissor rather than cropping
  each draw's geometry or being merged into other clip shapes
  (`ClipStack.cpp`; cropping shifts the interpolation of local coordinates,
  so retained frames differed from full ones by 1), and paths up to an atlas
  plot (1024 px) are rasterized whole into the path atlas instead of cut to
  the clip (`Device.cpp`), so their masks do not depend on the damage and
  are reused across frames. On `interactions.jsx` (1280x720, SwiftShader) a
  steady frame went from ~2.8 render passes, ~172 draws, ~175 new bind
  groups, 67 pipeline binds and 9 buffer maps to 1 pass (the canvas, with
  the damage copied back), ~65 draws, ~5 bind groups, ~40 pipeline binds and
  2-3 maps; `webgpu: steady animation frames` in the browser tests guards
  it.
* **No CPU waits.** In the browser Graphite runs without a tick function:
  submits never wait, buffer maps are async and finished work is collected by
  `checkAsyncWorkCompletion()` each frame. Raster images are uploaded once per
  image through a caching `ImageProvider` (LRU, half of the GPU budget).
* **Device loss.** Renders return -1 once the device is lost. The host asks
  for a new device and rebuilds the context on the same canvas, or continues
  with Skia on WebGL / the CPU raster on a fresh canvas.
* **Build.** One module with every backend: `GPU=1 scripts/build-wasm.sh`
  runs `scripts/build-skia-wasm-graphite.sh` (Skia chrome/m157, emsdk 6.0.13,
  Emdawnwebgpu v20261021.191805; Graphite + Dawn/WebGPU, Ganesh + WebGL,
  CPU; SIMD128) into `third_party/skia-wasm-gpu` and JSUI into
  `web/dist-gpu/jsui.{js,wasm}`. Skia does not support Emdawnwebgpu upstream
  yet (its emscripten code still targets the removed `-sUSE_WEBGPU`), so
  `scripts/patches/skia-m157-emdawnwebgpu.patch` ports Graphite's Dawn
  backend to it (and carries the bind group cache, scissor clip and whole
  path mask changes above, each marked `jsui:`). The m124 builds (`web/dist`, `web/dist-webgl`) and native
  Skia are unchanged; the JSUI sources build against both milestones
  (`__has_include` checks for the APIs that changed).
* **Browser host.** `renderer: 'auto'` (default) tries WebGPU on a hardware
  adapter, then Skia on hardware WebGL, then the CPU raster (WebGL / 2D blit);
  `'gpu'` takes the best GPU path even on a software adapter; `'webgpu'` /
  `'webgl'` ask for one API (falling down the chain if it fails); `'cpu'` /
  `'2d'` force the CPU raster. The chosen renderer is logged and in
  `jsui.renderer` (`'webgpu'`, `'gpu-webgl2'`, `'webgl2'`, `'2d'`). The
  WebGPU code lives in `web/jsui-webgpu.js` (one shared device per page);
  the host loads `dist-gpu` first when it got a WebGPU device. WebGPU
  readback is asynchronous: `jsui.readPixelsAsync()`.
* **Tests.** `JSUI_WEB_DIST=dist-gpu node web/test/browser.test.mjs` adds the
  `webgpu:` tests (WebGPU vs CPU pixel comparisons on the HUD and effects
  fixtures, incremental repaints, DPR 2 / resize, device loss, several
  runtimes, idle frames, direct vs retained frames, no MSAA targets, the
  steady frames of `interactions.jsx`: passes, bind groups, draws and
  retained vs full-repaint pixels) on
  Chromium's SwiftShader WebGPU adapter (`--enable-unsafe-webgpu`).
* **Benchmark.** `node web/test/frame-bench.mjs` measures
  `examples/showcase/interactions.jsx` per renderer: the rAF-to-rAF interval
  of the host's own loop in the steady state (what the user feels) with the
  main-thread time per frame and, on WebGPU, when the GPU finished each frame
  (observed, never awaited); then the old "finish" metric (frames stepped by
  hand, each awaited until the GPU is done, which adds the round trip to the
  GPU process that the real loop never waits for); then the WebGPU calls per
  frame (`web/test/gpu-count.js`). Headless it runs on SwiftShader; on a Mac
  with Chrome installed, `node web/test/frame-bench.mjs --real-gpu
  --renderers webgpu,webgl` uses the real GPU (Metal) in a visible window.

### Vulkan and Metal (Skia Graphite, native)

`jsui_gpu_create_vulkan(&JsuiVulkanDevice)` / `jsui_gpu_create_metal(device,
queue)` and `jsui_render_gpu_target(rt, gpu, &JsuiGpuTarget, &dirty)` (or
the `jsui_render_vulkan(rt, gpu, image, w, h, flags)` shorthand)
(`src/render/gpu_graphite.cpp`, CMake options `JSUI_VULKAN` /
`JSUI_GRAPHITE_METAL`) draw with Skia Graphite on an engine's own device
into a `VkImage` / `id<MTLTexture>` it owns. The Godot GDExtension uses it
to render into RenderingDevice textures (integrations/godot/README.md,
"Rendering").

* **The engine's device and queue.** The context wraps the host's
  `VkInstance` / `VkPhysicalDevice` / `VkDevice` / `VkQueue` (Vulkan 1.1
  core; optional extension and feature lists) or `MTLDevice` /
  `MTLCommandQueue`. Vulkan functions come from the host's
  `vkGetInstanceProcAddr` or the loader (`dlopen`, nothing is linked);
  memory goes through Skia's bundled VulkanMemoryAllocator.
* **Synchronous submit, explicit layouts.** Each render records one
  Graphite recording and submits it to the host's queue before returning,
  bracketed (Vulkan) by two full pipeline barriers, so it is ordered after
  host work submitted before the call and before host work submitted after
  it, without semaphores. The target is in `layout` on entry (`UNDEFINED`
  discards it) and Graphite leaves it in `final_layout`
  (`InsertRecordingInfo::fTargetTextureState`). The host must not use the
  queue from another thread during the call.
* **Retained target, damage only.** With `JSUI_GPU_INCREMENTAL` the target
  still holds the last frame (keyed by the texture handle), so only the
  damage is repainted, as in the GL retained frame: `CanvasChoice::splitDamage`
  paints scattered damage as up to four scissored rect passes
  (`paintDocumentRects`) instead of their bounding box. A frame with no
  damage does no GPU work at all. `dirty` returns the damage bounds.
* **Targets.** RGBA8 / BGRA8 (and sRGB) images; Graphite's Vulkan backend
  only renders into images that have `COLOR_ATTACHMENT` *and*
  `INPUT_ATTACHMENT` usage (it reads the destination through an input
  attachment for some blends). Raster images are uploaded once through the
  same caching `ImageProvider` as the WebGPU context (`graphite_images.h`).
* **Build.** Needs Skia m157 built by `scripts/build-skia-desktop.sh`
  (pinned commit, shared checkout with the wasm Graphite build; Linux:
  Graphite + Vulkan, Ganesh + GL; macOS: Graphite + Metal, Ganesh + GL +
  Metal) into `third_party/skia-desktop`; `-DJSUI_SKIA_DIR=third_party/skia-desktop`
  then turns the options on from its `args.gn`, and `cmake/Deps.cmake`
  compiles with the defines the build recorded (`defines.txt`) and links
  all its libraries. The prebuilt m124 has neither backend: the functions
  return NULL / -1 and hosts fall back to the CPU raster.
* **Tests.** `tests/native/vulkan_test.cpp` makes its own Vulkan device
  (Mesa lavapipe in CI), renders a bundle with moving, far-apart boxes over
  several incremental frames and compares each with the CPU raster;
  `JSUI_VULKAN_TEST_REQUIRE=1` turns its skip into a failure. The Godot job
  also runs the demo on Vulkan (lavapipe) and compares the GPU frame with
  the CPU raster. Metal compiles in the macOS CI job but is not run there.

## Game data

Three channels connect the game and the UI (`include/jsui/jsui.h`):

| | Game → UI | UI → game | Use for |
| --- | --- | --- | --- |
| **Shared state** | `jsui_set_state` / `jsui_patch_state` → `useHostState(key)` | `HostState.set` / `patch` → `on_state` | anything the UI shows: currency, inventory, quests, settings |
| **Calls** | `jsui_call_ui` → `Host.handle(name, fn)` → `done` | `await Host.call(name, args)` → `on_call` → `jsui_resolve_call` / `jsui_reject_call` | requests with an answer: buy, equip, confirm dialogs |
| **Messages** | `jsui_post_message` → `useHostMessage` | `Host.postMessage` → `on_message` | fire-and-forget events: "level up", "play sound" |

An inventory screen:

```c
// Game side. Any time, even before the bundle loads; survives reloads.
jsui_set_state(rt, "gold", "1250");
jsui_set_state(rt, "inventory",
    "{\"items\":{\"sword\":{\"name\":\"Iron Sword\",\"count\":1},"
    "\"potion\":{\"name\":\"Potion\",\"count\":3}}}");
// Later: only what changed (RFC 7386 merge patch; null deletes).
jsui_patch_state(rt, "inventory", "{\"items\":{\"potion\":{\"count\":2},\"key\":null}}");

// UI -> game requests (JsuiConfig.on_call), answered now or on a later frame.
static void on_call(void* user, uint64_t id, const char* name, const char* args) {
    Game* g = user;
    if (strcmp(name, "shop.buy") == 0) game_queue_purchase(g, id, args);  // answers later
    else jsui_reject_call(g->ui, id, "unknown call");
}
// ... when the purchase went through:
jsui_resolve_call(rt, id, "{\"gold\":1100}");    // or jsui_reject_call(rt, id, "not enough gold")
```

```jsx
import { Host, useHostState } from '@jsui/react';

const Row = memo(({ item }) => <Text>{item.name} ×{item.count}</Text>);

function Inventory() {
  const gold = useHostState('gold', 0);
  const { items } = useHostState('inventory', { items: {} });
  const [error, setError] = useState(null);
  const buy = (item) =>
    Host.call('shop.buy', { item }, { timeout: 5000 }).catch((e) => setError(e.message));
  return (
    <View>
      <Text>{gold} gold</Text>
      {Object.entries(items).map(([id, item]) => <Row key={id} item={item} />)}
      <Button title="Buy potion" onPress={() => buy('potion')} />
      {error && <Text>{error}</Text>}
    </View>
  );
}
```

**Shared state** (`src/host_state.*`, `src/js/bridge_host.cpp`,
`packages/react/src/hostData.js`):

* The runtime owns the store (one JSON value per key), not the JS context, so
  values set before the bundle loads, or before a dev-mode / `jsui_reload`
  reload, are there when the (new) bundle starts. Values are kept in an
  order-preserving JSON document (`src/core/json.*`): members keep the
  embedder's order, numbers their text, and a merge patch appends new members.
* Every change bumps a runtime-wide version (0 = unset). Setting an equal
  value (same text, or text that parses equal; a patch that changes nothing)
  is a no-op: no version bump, no re-render.
* Delivery: keys the embedder changed are handed to JS once per `jsui_tick`
  (right after resource completions, before timers and rAF) in one
  `__jsui_onHostState(keys)` call, which notifies the keys' subscribers in one
  React batch at discrete priority, so they render in that same tick.
  Reads are not delayed: `HostState.get` compares the key's version with
  its cache, so a component that mounts between `jsui_set_state` and the next
  tick (from an input event or a host message) already renders the new value.
* JS parses a key's JSON once per version, however many components read it,
  and only when something reads it. The new value is merged into the previous
  one so that unchanged objects and arrays stay identical (`===`): rows behind
  `React.memo`, `useMemo` deps and `useHostStateSelector` results survive
  updates elsewhere in the value. Development builds deep-freeze values,
  because mutating a shared value would silently diverge from the runtime's
  copy.
* `useHostState(key, fallback)` is `useSyncExternalStore` on that cache (only
  the key's subscribers re-render; `fallback` while unset).
  `useHostStateSelector(key, selector, fallback)` re-renders only when the
  selected result changes (`Object.is`). `HostState.get(key)`,
  `HostState.subscribe(key, cb)` (`'*'` for every key) and
  `HostState.keys()` are the non-hook API.
* UI-owned keys: `HostState.set(key, value)` / `HostState.patch(key, patch)`
  write to the same store, notify JS subscribers synchronously and call
  `JsuiConfig.on_state(key, json)` (full new value; NULL when removed). The
  embedder's own writes are not echoed to `on_state`, and JS writes are not
  re-delivered to JS. The embedder reads any key with `jsui_get_state`.
* Granularity: one key per thing that changes independently ("gold",
  "inventory", "quests"). Keep big collections as objects keyed by id so
  merge patches can address single entries (an array is replaced whole).

**Calls**:

* `Host.call(name, args, {timeout, signal})` serializes `args`, registers the
  call's settle function with the engine, then calls `on_call(id, name, json)`
  (ids are unique for the runtime's lifetime, never 0). The embedder may
  answer inside `on_call` or any number of frames later; the promise resolves
  with the parsed JSON or rejects with an `Error` (`name` `HostCallError`,
  `message` = the embedder's message, `call` = the name). Invalid result JSON
  rejects the call (`jsui_resolve_call` returns -2) rather than leaving it
  pending. Without `on_call` the promise rejects at once.
* A call that ends unanswered is reported to `on_call_cancel(id)`: the JS
  timeout or abort signal fired, or the JS context went away (`jsui_reload`,
  a dev-mode reload, `jsui_destroy`). The engine frees the call's JS
  function with its context; answering a cancelled id returns -1. Nothing
  leaks on either side.
* Reverse direction: `jsui_call_ui(rt, name, args, done, user)` calls
  `__jsui_onCall(id, name, json)`; `@jsui/react` runs the `Host.handle(name, fn)`
  / `useHostHandler(name, fn)` handler (inside a discrete batch, so its state
  updates render in the same tick) and posts its value (or promise result) back
  with `callResult`. `done(user, id, ok, result)` runs exactly once, at the end
  of a `jsui_tick` (never inside `jsui_call_ui`, so the id is always known
  first), with ok = 0 and a message when the handler threw or rejected, no
  handler is registered, the result is not JSON-serializable, or the context
  went away (from `jsui_reload` / `jsui_destroy`).

Hosts: the WASM page API mirrors this (`jsui.setState` / `patchState` /
`getState`, `jsui.handle(name, fn)` for `Host.call`, `jsui.callUi(name, args)
→ Promise`, `'state'` / `'call'` / `'callCancel'` events), and the Godot
`JsuiView` exposes `set_state(key, Variant)` / `patch_state` / `get_state`,
`call_requested` + `resolve_call` / `reject_call` and `call_ui(name, args,
Callable)` (see `integrations/godot/README.md`). `tools/jsui-record`
timelines can set state with `{"state": key, "data": value}` steps.

## JS bridge contract

Native globals (installed by `JsEngine`):

| Global | Meaning |
| --- | --- |
| `__jsui_native.rootId` | id of the root container node |
| `__jsui_native.devMode` | `true` when the runtime runs with `cfg.dev_mode` |
| `createNode(type) → id` | `type` ∈ `View`, `Text`, `RawText`, `Image`, `ScrollView`, `TextInput` |
| `setProps(id, props)` | replaces all props; `props.style` is a **flattened** style object; functions are dropped; `props.__events` lists event names the element listens to (e.g. `["press","pointerMove","layout"]`) |
| `patchStyle(id, partialStyle)` | animation fast path: merges keys into the node's current `props.style` (a `null`/`undefined` value removes the key) and keeps all other props (`Node::patchStyle`). When only paint keys such as `transform`, `opacity`, colors changed, the style is updated in place and only those keys' style groups are re-resolved, with no relayout; a layout key takes the `setProps` path. Use it per frame from `requestAnimationFrame` instead of re-rendering through React. |
| `setText(id, string)` | content of a `RawText` |
| `appendChild(p, c)`, `insertBefore(p, c, before)`, `removeChild(p, c)` | tree edits (React semantics; unknown ids and cycles are ignored). While a LayoutAnimation with `delete` is armed, `removeChild` keeps the child as a fading ghost (see "LayoutAnimation") |
| `animated.*` | the native Animated driver (React Native's NativeAnimatedModule): `createNode(tag, config)`, `connect` / `disconnect(parent, child)`, `connectToView` / `disconnectFromView(propsTag, id)`, `dropNode`, `setValue` / `setOffset` / `flattenOffset` / `extractOffset` / `getValue(tag)`, `startAnimation(id, tag, config)`, `stopAnimation(id) → {velocity} \| null`, `addEventMapping(id, eventType, path, tag)` / `removeEventMapping`, `startListening` / `stopListening(tag)`. See "Animated" |
| `configureLayoutAnimation(config)` | arms a LayoutAnimation (`{duration, create, update, delete}`) for the layout pass after the commit in progress; `@jsui/react` calls it from `prepareForCommit` after `LayoutAnimation.configureNext` |
| `destroyNode(id)` | free a detached subtree |
| `measure(id) → {x, y, width, height, pageX, pageY}` | layout of a node (synchronous; runs a pending layout first). `null` for unknown ids |
| `focus(id)`, `blur(id)` | `InputManager::focus` / `blur` |
| `scrollTo(id, x, y, animated)` | `InputManager::scrollTo` (clamped, dispatches `scroll`). `animated`: eases there on the runtime clock; otherwise immediate (and stops a running fling) |
| `flashScrollIndicators(id)` | shows a ScrollView's overlay indicators for ~1 s |
| `postMessage(name, json)` | to the embedder's `on_message`. A string is passed through as the JSON text; any other value is `JSON.stringify`-ed |
| `getState(key) → json \| undefined`, `stateVersion(key) → number`, `stateKeys() → [key]` | shared host state (see "Game data"); version 0 = unset |
| `statePatches(key, sinceVersion) → [json] \| undefined` | the merge patches (JSON texts) that took `key` from `sinceVersion` to its current version, oldest first; `undefined` when the bounded log (64 patches / 64 KB since the last `set`) cannot cover the gap. `@jsui/react` applies them to its cached value instead of re-parsing the whole state after each `jsui_patch_state` |
| `setState(key, json \| undefined) → version`, `patchState(key, json) → version` | UI-owned writes (`undefined` removes); report to `on_state`; invalid JSON throws `SyntaxError` |
| `hostCall(name, json, settle) → id` | `on_call`; `settle(ok, payload)` runs on `jsui_resolve_call` (JSON text) / `jsui_reject_call` (message). Returns 0 without `on_call` |
| `cancelCall(id) → bool`, `callResult(id, ok, payload)` | drop an unanswered call (`on_call_cancel`); the result of a `__jsui_onCall` |
| `loadFont(family, url, weight, italic) → Promise<true>` | load via the resource loader (URL relative to the bundle), register with the FontManager, relayout text; rejects with an `Error` on 404 / bad font data |
| `reload()` | reload the bundle at the start of the next tick (safe from inside JS) |
| `textEditing` | `true`: TextInput text is edited natively (see "TextInput editing") |
| `setTextInputSelection(id, start, end)` | TextInput selection (UTF-16 indices, clamped; no event) |
| `getClipboard() → string`, `setClipboard(text)` | the embedder's clipboard (`jsui_set_clipboard_callbacks`) or the runtime's in-process one |

Standard globals (`src/js/prelude.cpp` + `host_api.cpp`):

* `console.log/info/debug/warn/error/trace/assert/dir/table/group*/time*/count*`
  with Node-style `%s %d %i %f %o %O %j %c` substitution and object
  inspection (cycles, Map/Set, Errors with stack). Levels map to `JsuiLogFn`.
* `setTimeout`/`setInterval` (+ extra args, clear), `setImmediate`,
  `requestAnimationFrame(ts)`/`cancelAnimationFrame`, `queueMicrotask`,
  `MessageChannel`/`MessagePort` (a posted message is a zero-delay timer, which
  is what React's scheduler uses). Timers run in due-time then FIFO order;
  zero-delay timers queued by callbacks run in later passes of the same tick
  (at most 8 passes, so a self-rescheduling timer cannot starve a frame).
* `performance.now()` is the **runtime clock** (the `now_ms` the embedder
  passes to `jsui_tick`), the same timeline as rAF timestamps and timers, so
  headless renders are deterministic. `Date.now()` stays wall-clock.
* `fetch(input, {method, headers, body, signal, timeout})` → `Response`
  (`ok/status/statusText/url/redirected/headers`, `text()/json()/arrayBuffer()/bytes()`,
  `clone()`), `Headers`, `Request`, `Response.json/error/redirect`. Bodies:
  string, `ArrayBuffer`/views, `URLSearchParams`. Network errors reject with
  `TypeError`; HTTP errors resolve with `ok === false`; aborts reject with
  `AbortError`. URLs resolve against the bundle URL.
* `TextEncoder`/`TextDecoder` (UTF-8 only; `fatal`, `ignoreBOM`, `stream`),
  `URL`/`URLSearchParams` (WHATWG subset), `AbortController`/`AbortSignal`
  (`timeout`, `any`), `Event`/`CustomEvent`/`EventTarget`, `DOMException`,
  `structuredClone`, `atob`/`btoa`, `reportError`, `crypto.getRandomValues`
  / `randomUUID` (not cryptographically secure), `navigator`, `location`
  (bundle URL), `process.env.NODE_ENV`, `globalThis.window = globalThis.self = globalThis`.
  React 19 + react-reconciler run unmodified (dev and prod builds).

Engine limits (`JsuiConfig`): `js_memory_limit_mb` (default 256),
`js_stack_size_kb` (1024) and `js_timeout_ms` (5000; negative disables). The
watchdog interrupts any single native→JS call (eval, a timer, a frame
callback, an event, the microtask drain after it) that runs longer than the
budget and reports "Script timed out"; the engine keeps working afterwards.
Uncaught exceptions and unhandled promise rejections (checked once the
microtask queue drains) are reported via `JsuiLogFn` and `jsui_last_error()`
with the stack. `onLayout` events raised while JS is on the stack (e.g. by
`measure`) are queued and delivered when it unwinds.

Callbacks the bundle installs (all optional):

* `__jsui_dispatchEvent(event) → number` — native → JS event. Bitmask: `1` a
  handler ran, `2` `preventDefault()` was called.
* `__jsui_onHostMessage(name, json)` — from `jsui_post_message`.
* `__jsui_onHostState(keys)` — shared-state keys the embedder changed, once per tick.
* `__jsui_onCall(id, name, json)` — `jsui_call_ui`; answer with `callResult`.
* `__jsui_onLayout(id, {x, y, width, height})` — for `onLayout` listeners.
* `__jsui_onAnimatedEnd(id, finished, value)` / `__jsui_onAnimatedValueUpdate(tag, value)`
  — native Animated ends and value-listener updates, after each tick.

### Event objects

Native dispatches plain objects; `@jsui/react` turns them into React-style
synthetic events, does capture/bubble along its instance tree and maps them to
React Native props (`onPress`, `onPressIn/Out`, `onLongPress`, `onHoverIn/Out`,
`onPointerDown/Move/Up/Enter/Leave`, `onKeyDown/Up`, `onFocus/onBlur`,
`onScroll`, `onScrollBeginDrag/EndDrag`, `onMomentumScrollBegin/End`,
`onWheel`, `onChangeText`).

```
{ type: 'pointerdown'|'pointermove'|'pointerup'|'pointercancel'|'pointerenter'|'pointerleave'
        |'click'|'wheel'|'keydown'|'keyup'|'textinput'|'focus'|'blur'|'scroll'
        |'scrollbegindrag'|'scrollenddrag'|'momentumscrollbegin'|'momentumscrollend'
        |'gamepadbutton'|'gamepadaxis'|'gamepadconnected'|'change'|'selectionchange',
  target: <node id>,              // hit node, focused node for keys
  timestamp: <ms>,                // jsui_set_event_time, else the last tick time
  // pointer
  x, y,                           // viewport coords (pageX/pageY)
  localX, localY,                 // relative to target (locationX/Y)
  pointerId, pointerType: 'mouse'|'touch', button, buttons,
  // wheel
  deltaX, deltaY,
  // keys
  key, code, repeat, shiftKey, ctrlKey, altKey, metaKey,
  // text; TextInput edits ('change': text + eventCount, 'selectionchange')
  text, eventCount, selection: {start, end},
  // gamepad
  gamepad, buttonIndex, value, axis,
  // scroll, scrollbegindrag, scrollenddrag, momentumscroll* (on the ScrollView)
  contentOffset: {x, y}, contentSize: {width, height}, layoutMeasurement: {width, height},
  contentInset: {top, left, bottom, right}, zoomScale: 1,
  velocity: {x, y},               // px/ms, + = offset increasing (scrollenddrag)
  targetContentOffset: {x, y} }   // where the fling / snap will end (scrollenddrag)
```

`timestamp` is on the runtime clock. A host that knows when an event happened
calls `jsui_set_event_time(rt, ms)` before sending it (the web host passes the
DOM `event.timeStamp`); otherwise events carry the last tick's time, so all
events between two frames share one. PanResponder's `vx` / `vy` (px/ms) are
measured over the moves of the last ~100 ms; moves with the same timestamp
count as one sample at their latest position, the release keeps the velocity
of the moves before it (React Native), and a pointer that rested longer than
that before letting go has none. ScrollView drag velocity uses the same clock.

`pointerenter`/`pointerleave` are dispatched per node (no bubbling), like the DOM.
Gamepad D-pad / stick moves focus spatially between `focusable` nodes and A
activates the focused node (dispatched as `click`), unless the gamepad event
was `preventDefault()`-ed.

### Input routing details (`src/input/`)

* **Default actions** (Tab focus moves, Enter/Space/A → `click`, D-pad/stick/arrow
  navigation, wheel scrolling) are skipped when JS calls `preventDefault()`.
  `__jsui_dispatchEvent` returns a bitmask: `1` = a handler ran, `2` =
  default prevented (a boolean `true` is treated as `3` for compatibility).
  A JS Pressable that handles Enter/Space/A itself prevents the default so no
  extra `click` follows. Exception: focus-on-press always happens.
* **`__events` drives native decisions.** Names are matched case-insensitively
  by substring: `press`/`click` = pressable (focusable by default, `pointer`
  cursor); `move`/`responder`/`pan`/`drag` = wants `pointermove` (moves are not
  sent to JS unless a node on the path lists one); `hover`/`enter`/`leave`/`over`
  = wants `pointerenter`/`pointerleave` (sent only to such nodes); `wheel`;
  `"*"` = everything. Any pointer-ish name makes `jsui_send_*` report "consumed".
* **Pointer:** mouse `pointerId` 0, touch `id + 1`. Implicit capture while a
  button / touch is down; `click` (primary button only) targets the nearest
  common ancestor of the down and up nodes. Wheel `deltaY > 0` scrolls down
  (DOM convention); over a `horizontal` ScrollView a vertical wheel scrolls
  sideways. A touch dragged > 8 px inside a scrollable ScrollView sends
  `pointercancel` and scrolls it.
* **Scrolling** (`src/input/scroll.cpp`; geometry in `src/core/scroll.*`).
  All animations are analytic curves on the runtime clock, advanced in
  `jsui_tick`.
  * Touch drag: `scrollbegindrag`, `scroll`…, `scrollenddrag` (release
    velocity over the last 100 ms; a finger that rested has none), then a
    fling (`momentumscrollbegin`, `scroll`…, `momentumscrollend`) decaying
    by `decelerationRate` per ms (`'normal'` 0.998, `'fast'` 0.99, or a
    number) until 0.02 px/ms or the content edge (no overscroll). A touch on
    a moving ScrollView stops it (`momentumscrollend`), is not a press, and
    drags without the slop.
  * Snapping (flings, wheel notches, keys, stick release, trackpad settle):
    `pagingEnabled` moves one page in the velocity direction (else nearest);
    `snapToInterval` (+ `snapToAlignment`, `disableIntervalMomentum`) and
    `snapToOffsets` (+ `snapToStart`/`snapToEnd`) pick the snap point on the
    velocity side of the projected end, like React Native on iOS. The glide to
    the snap point is momentum.
  * Wheel: whole deltas ≥ 40 px (mouse notches) glide over 150 ms and
    accumulate; other deltas (trackpads) apply pixel-precisely and snap after
    150 ms idle. `InputManager::smoothWheel = false` disables gliding.
  * Keyboard (no TextInput focused): PageUp/PageDown (87.5 % of the
    viewport), Home/End, Space / Shift+Space when nothing pressable is
    focused, arrows when spatial navigation finds nothing further. D-pad
    likewise scrolls 40 px past the last focusable item. Target: the focused
    node's ScrollView, else the hovered one, else the first scrollable one
    in the focus scope.
  * Gamepad sticks: the right stick scrolls that target continuously
    (dead zone 0.2, quadratic response up to 1.6 px/ms); the left stick does
    too when the focus scope has nothing focusable. `preventDefault()` on the
    `gamepadaxis` event keeps the stick for the app.
  * Focus moved by keys / gamepad (or by code after them) glides the node
    into view keeping up to 32 px of context; focus from a press never
    scrolls; programmatic focus after pointer input jumps edge-aligned.
  * `scrollEnabled: false` stops user scrolling only (programmatic and focus
    scrolling still work). Offsets are re-clamped when content shrinks.
  * Overlay indicators (3 px thumbs, `indicatorStyle` `default`/`black`/
    `white`, hidden by `showsVertical/HorizontalScrollIndicator: false`) show
    while scrolling and fade out after 500 ms. Scrolling and fading repaint
    only the ScrollView's rect / the thumbs' rects.
  * `@jsui/react` adds `contentOffset` (applied on mount),
    `scrollEventThrottle` (> 16 ms limits `onScroll`, with a trailing event;
    otherwise every event), `stickyHeaderIndices` (vertical; headers wrapped
    in a `zIndex: 10` View translated on scroll, the next header pushes the
    previous one) and wraps paging children like react-native-web.
* **Focusable:** `focusable: true`, pressable nodes (unless `focusable: false`),
  `TextInput`; never `disabled: true` or inside `display: none`. Extra event
  fields: `focus` has `focusVisible` (true when the last input was a key or
  gamepad, like `:focus-visible`) and `relatedTarget`; `blur` has
  `relatedTarget`; synthesized clicks have `pointerType: 'keyboard'|'gamepad'`;
  `gamepadbutton` has `pressed` and `repeat`; `gamepadconnected` has
  `connected` and `id`.
* **Focus scopes:** the last displayed node with `accessibilityViewIsModal` or
  `aria-modal` traps Tab and spatial navigation; focus moves into it when it
  appears (its `autoFocus`/`hasTVPreferredFocus` child or first focusable) and
  is restored to the previously focused node when it is removed or hidden.
  `nextFocusUp/Down/Left/Right` (node ids) override spatial navigation.
* Stick: deadzone 0.5 on the dominant axis; held D-pad / stick repeats after
  400 ms, then every 120 ms. Focus changes scroll the node into view in its
  ScrollView ancestors (see Scrolling).

## TextInput editing

The runtime owns a TextInput's text like a native text field does in React
Native (`src/text/text_edit.*`, `src/input/text_editing.cpp`). Keys and
text reach JS first (`keydown`, `textinput`); unless JS calls
`preventDefault()`, the focused TextInput applies them:

* Typing / IME commits replace the selection (single-line inputs drop line
  breaks; control characters are dropped), honoring `maxLength` (UTF-16
  units, never splitting a surrogate pair) and `editable` / `readOnly`.
* Arrows move by grapheme-ish clusters (code points + combining marks, ZWJ
  sequences, flags); Ctrl/Alt+arrows by word, Cmd+arrows / Home / End to the
  visual line edge, Ctrl+Home/End and PageUp/PageDown to the text edges;
  Up/Down move a visual line keeping the goal column (single line: text
  edges); Shift extends. ArrowLeft/Right are mirrored in RTL text.
* Backspace / Delete delete the selection, or a cluster / word (Ctrl, Alt) /
  to the line edge (Cmd). Enter inserts a newline in multiline inputs (the
  `TextInput` component prevents Enter when it submits).
* Ctrl/Cmd + A selects all, C / X / V copy, cut, paste through the clipboard
  (`jsui_set_clipboard_callbacks`, else in-process; never for
  `secureTextEntry`), Z / Shift+Z / Ctrl+Y undo / redo (typing coalesces).
* Mouse: press places the caret (Shift extends), drag selects, double click
  selects a word, triple click everything; touch taps place the caret. A
  press that focuses a `selectTextOnFocus` input selects everything.

Edits dispatch `change` (`text`, `eventCount`) and `selectionchange`
(`selection: {start, end}`) to nodes listing the `change` /
`selectionChange` tokens. Props: `value` is adopted when it differs from the
native text and `mostRecentEventCount` >= the native edit count (renders that
have not seen the latest edit are stale; without the prop a changed `value`
is adopted), `defaultValue`, `placeholder`, `placeholderTextColor`,
`secureTextEntry` (U+2022 per code point), `multiline`, `maxLength`,
`editable`, `selectTextOnFocus`, `caretHidden`, `cursorColor`,
`selectionColor` (highlight; opaque colors at 30 % alpha, default
`rgba(0,122,255,0.3)`; also the caret color unless `cursorColor`).

Geometry is skparagraph's: the caret is the trailing edge of the previous
grapheme (`getGlyphInfoAtUTF16Offset`) on its line's ascent..descent,
clicks use `getGlyphPositionAtCoordinate`, the highlight `getRectsForRange`.
A value -> display index map keeps indices right under `textTransform` and
bullets. Single-line inputs scroll horizontally to keep the caret visible
(multiline: vertically) and show their start again on blur.

The caret blinks (500 ms on / off, restarted by any edit or move). A blink
or a caret-only move marks the document paint-dirty without bumping the
node's paint version; `updateDamage()` then damages just the caret's old
and new rects (`text::inputCaretDamage`), so `jsui_needs_redraw()` turns
true twice a second while an input is focused and each such frame repaints
a few pixels.

## Animated

`@jsui/react`'s `Animated` (`packages/react/src/animated/`) has React
Native's API and both of its drivers:

* **JS driver** (default). Every running animation shares one
  `requestAnimationFrame` loop; changed values patch the mounted components'
  styles with `patchStyle` once per frame, without React renders.
* **Native driver** (`useNativeDriver: true`), like React Native's
  NativeAnimatedModule. Starting a native animation makes its value native
  (`__makeNative`), which spreads to everything computed from it and to the
  components using it. Each node is serialized once through
  `__jsui_native.animated` (`NativeAnimatedHelper.js`) and evaluated by
  `src/animated/native_animated.cpp` in `Runtime::tick()` on the runtime
  clock, after JS and scroll physics: the runtime writes the connected
  views' styles itself, and JS hears back only for end callbacks and value
  listeners. Native animations therefore keep moving while JS is busy or
  not ticked at all (`jsui_tick_native`).
  * Nodes: `value` (+ offset), `interpolation` (numeric outputs; colors as
    four channel ranges, rounded / clamped like the JS driver; strings with
    numbers such as `'45deg'` as a template; `extrapolate*` extend / clamp /
    identity; a custom `easing` as a 257-point sampled curve), `addition`,
    `subtraction`, `multiplication`, `division`, `modulus`, `diffclamp`
    (continuing the JS node's state), `color` (`Animated.Color`), and per
    component a `transform` (animated and static entries), `style` and
    `props` node connected to the host view.
  * Animations: timing as `frames` (the easing sampled every ~2 ms over the
    duration, linearly interpolated; RN samples at 60 Hz), `spring`
    (tension / friction and bounciness / speed are converted to stiffness /
    damping / mass in JS; a spring started on a value whose spring was just
    stopped continues its velocity), `decay`; `delay`; `iterations`
    (`Animated.loop` of a native animation repeats natively, restarting from
    the start value). The formulas and frame times are the JS driver's:
    springs and decay match it exactly, sampled easings within ~0.006 px of a
    200 px move (`tests/native/react_native_animated_e2e_test.cpp`).
  * Sync: animations start at the runtime time JS started them; ends reach JS
    after the tick as `{finished, value}`. `__getValue()` reads the native
    value synchronously; `setValue` / `setOffset` / `flattenOffset` /
    `extractOffset` forward to it (`setValue` stops the node's animations and
    writes the views at once); `stopAnimation(cb)` stops natively and passes
    the native value; `addListener` on a native value gets per-frame updates.
  * `Animated.event(mapping, {useNativeDriver: true})` on an animated
    component prop (`onScroll`, `onMomentumScrollEnd`, ...): the paths under
    `nativeEvent` map onto native values (`addEventMapping`), applied by
    `InputManager::dispatch` before the event reaches JS (a JS `listener`
    still runs). `Animated.attachNativeEvent` does the same for a ref.
  * React Native's rules: a native value stays native and a JS-driven
    animation on it throws; only paint-only style keys can be driven
    (`opacity`, `transform`, colors, border radii, `shadowOpacity` /
    `shadowRadius`, ...) and transforms; layout keys throw `Style property
    'width' is not supported by native animated module`, animated non-style
    props throw too. `Animated.ValueXY` is native through its channels.
  * Writes go through `Node::setProps` with the merged style (paint-only, so
    no relayout; the incremental renderer damages just the node's old and
    new rects). When React re-props a connected view, the live values are
    overlaid again in the same tick. Unchanged frames write nothing.
  * Native nodes are freed when their derived JS node detaches, when the
    component unmounts, and (values) when the JS object is garbage collected
    (`FinalizationRegistry`). A reload clears the native graph.
  * Without `__jsui_native.animated` (Node tests, other hosts)
    `useNativeDriver` falls back to the JS driver.
  * Not native (yet): `toValue` bound to another Animated node (RN's
    `tracking` node) restarts native animations from JS when the target
    changes; scroll physics (and so `Animated.event` from flings) only
    advance in `jsui_tick`, not in `jsui_tick_native`.

## LayoutAnimation

`LayoutAnimation.configureNext(config, onEnd)` (and the `easeInEaseOut` /
`linear` / `spring` presets, `LayoutAnimation.create`) works like React
Native: `@jsui/react` arms the config natively at the start of the next
commit (`configureLayoutAnimation`), and the layout pass after that commit
animates (`src/core/layout_animation.cpp`); the runtime disarms at the end of
the tick, so the config never leaks into a later commit.

* `update`: every node whose parent-relative frame changed moves / resizes
  from the frame it was drawn at to its new layout. Painting and hit testing
  use the in-between frame (`Node::visualRect()`); `measure()` and
  `onLayout` report the final layout at once, like RN. The target is read
  live, so a relayout during the animation retargets it.
* `create`: the root of each inserted subtree fades in (`opacity`) or grows
  from its center (`scaleX`, `scaleY`, `scaleXY`).
* `delete`: a child removed while armed becomes a ghost of its old parent:
  out of layout and hit testing (siblings reflow at once, animated by
  `update`), painted below the live children at its last frame while it
  fades / shrinks out, then freed (React's `destroyNode` is deferred until
  then; re-inserting it revives it).
* Types: `linear`, `easeInEaseOut`, `easeIn`, `easeOut` (CSS cubic-beziers),
  `keyboard`, `spring` (`springDamping` is the damping ratio; it settles
  within the duration). Per-type `duration` and `delay` override the
  config's `duration`. `onEnd` runs after the longest duration.

## Pressables

`Pressable`, the Touchables and `Text` share one press state machine
(`packages/react/src/components/usePress.js`) that follows RN's
Pressability: `onPressIn` / `onPressOut` / `onPress` / `onLongPress`
(`delayLongPress`), `delayPressIn` (`unstable_pressDelay`), `delayPressOut`
and `minPressDuration` (130 ms: a quick tap or a gamepad A press fires
`onPressIn`, `onPress`, and `onPressOut` once the press lasted 130 ms, so
pressed feedback is always visible), `onHoverIn` / `onHoverOut` from native
`pointerenter` / `pointerleave` with `delayHoverIn` / `delayHoverOut` (a
visit shorter than `delayHoverIn` fires neither), and `onFocus` / `onBlur`.
Style and children functions receive `{pressed, hovered, focused}` (the
react-native-web shape; hover and focus update immediately). Keyboard
Enter / Space and gamepad A activate the focused pressable; `disabled`
pressables are neither focusable nor pressable and ignore hover.

## Styles

Style keys follow React Native, including RN 0.76+ web-style additions:

* Layout: all Yoga props (`flex*`, `align*`, `justifyContent`, `gap`,
  `rowGap`, `columnGap`, `margin*`, `padding*`, `position`, `top/right/bottom/left`,
  `inset*`, `width/height/min*/max*`, `aspectRatio`, `display`, `overflow`,
  `borderWidth*`, `direction`, `flexWrap`, `boxSizing`). Numbers are px,
  strings may be `"50%"` or `"auto"`.
* Paint: `backgroundColor`, `opacity`, `borderColor*`, `borderRadius*`
  (corner and logical variants), `borderStyle`, `boxShadow` (CSS string or
  RN object array, incl. `inset` and `spread`), legacy
  `shadowColor/shadowOffset/shadowOpacity/shadowRadius`,
  `experimental_backgroundImage` / `backgroundImage` (`linear-gradient`,
  `radial-gradient`), `transform` (RN array or CSS string), `transformOrigin`,
  `filter` (`blur`, `brightness`, `contrast`, `saturate`, `grayscale`),
  `zIndex`, `pointerEvents`, `cursor`.
* Text: `color`, `fontFamily`, `fontSize`, `fontWeight`, `fontStyle`,
  `lineHeight`, `letterSpacing`, `textAlign`, `textTransform`,
  `textDecorationLine`, `textDecorationColor`, `textShadow*`; props
  `numberOfLines`, `ellipsizeMode`.
* Image: `resizeMode` (style or prop), `tintColor`; `source={{uri}}` or
  `source={require('./a.png')}` (the bundler emits a URL).

Colors are CSS colors (`#rgb[a]`, `#rrggbb[aa]`, `rgb[a]()`, `hsl[a]()`, names).
The default font is Inter (bundled); `fontFamily` falls back to it.

## Builds

* Native: `cmake -S . -B build -G Ninja && cmake --build build`. Dependencies
  are fetched at configure time (QuickJS-ng, Yoga via git; Skia prebuilt zip).
  `tools/jsui-viewer` downloads SDL 2.30.12 (SHA-256 checked) and links it
  statically; `-DJSUI_BUILD_VIEWER=OFF` skips it, `-DJSUI_SDL_DIR=` uses a
  local SDL2 source tree, and a failed download only skips the viewer.
  `jsui-viewer [--dev] [--cpu | --gl-core] [--width W --height H] <bundle path or URL>`; F5
  reloads; `--frames N --screenshot out.png` runs headless (GPU by default,
  which needs a GL-capable display, e.g. `xvfb-run` with Mesa; `--cpu` with
  `SDL_VIDEODRIVER=dummy`). `-DJSUI_GPU=OFF` builds without the GPU backend.
* WASM: `scripts/build-skia-wasm.sh` (once), then `emcmake cmake -S . -B build-wasm`.
  The `web/` host loads `jsui.wasm`, forwards DOM input, blits frames to a canvas.
  `scripts/build-wasm.sh` does all steps (emsdk 3.1.44 → Skia m124 → `web/dist/jsui.{js,wasm}`);
  `WEBGL=1 scripts/build-wasm.sh` builds the GPU variant into `web/dist-webgl/`
  (deploy it next to `web/dist/`: the host prefers it, see "GPU backend").
  Both use WASM SIMD (`SKIA_WASM_SIMD=0` builds without it).
  `GPU=1 scripts/build-wasm.sh` builds the WebGPU + WebGL + CPU module into
  `web/dist-gpu/` (emsdk 6.0.13 → Skia m157 with Graphite, see "WebGPU (Skia Graphite)").
  In the page: `const jsui = await createJsui({ canvas, bundleUrl })` from
  `web/jsui-web.js`; `jsui.on('message', (name, data) => …)`, `jsui.postMessage(name, data)`.
  The browser loader backend is `fetch()` (`src/platform/loader_emscripten.cpp`).
  Tests: `node web/test/node-smoke.mjs`, `node web/test/browser.test.mjs` (Playwright).
* Desktop Skia m157 (Graphite on Vulkan / Metal, for the Godot GPU renderer):
  `scripts/build-skia-desktop.sh` (Linux / macOS, about 25 min on 4 cores,
  cached in CI on the script's hash) into `third_party/skia-desktop`, then
  configure with `-DJSUI_SKIA_DIR=third_party/skia-desktop`. See
  "Vulkan and Metal (Skia Graphite, native)".
* JS: `npm install` at the repo root (npm workspaces), `npx jsui build app.jsx`
  or `npx jsui dev app.jsx` (serves `bundle.js` and a reload endpoint).

## Performance

What a frame costs before painting, and how it stays proportional to what
changed rather than to the tree size:

* **Style.** `setProps` diffs the old and new style objects; when every
  changed key is a known paint key, only the paint groups those keys feed
  (opacity, transform, colors, radii, shadows, gradients, filters, text, ...)
  are re-resolved and Yoga is not touched (`applyPaintStyleChanges`,
  `src/core/style.cpp`). `patchStyle` (bridge and native Animated) does the
  same in place, without copying the props.
* **Layout.** Yoga only reruns when something layout-affecting changed; the
  post-layout walk (`Document::afterLayout`) visits only nodes Yoga flagged
  (`hasNewLayout`). Text measurement reuses the paragraph's intrinsic layout.
  Yoga's `PixelGrid.cpp` is patched at configure time (`cmake/Deps.cmake`) to
  skip pixel rounding when the point scale factor is 0, and `CalculateLayout.cpp`
  to check for baseline alignment once per node per pass, not once per flex line.
* **Input.** Hit testing skips a subtree when the pointer is outside the
  subtree's root box and every hittable descendant lies inside that box
  (cached per node, invalidated bottom-up by `Node::markBoundsDirty` on frame,
  transform, display, overflow, pointerEvents, children, scroll and
  LayoutAnimation changes). Focus / modal scopes track `autoFocus` /
  `hasTVPreferredFocus` / modal nodes in a list instead of walking the tree.
* **Host state.** `statePatches` lets JS apply a `jsui_patch_state` as a
  merge patch. React still reconciles every child of a component that reads
  the whole key; for large collections, select the slice each child needs
  (`useHostStateSelector`) so unchanged children bail out.

`Document::perf` counts the work (`src/core/perf.h`: bridge calls, setProps,
style resolves, layout passes, text measures / shaping, Animated writes,
events, hit-test node visits) and, when `profiling` is set, the exclusive
time per phase. `tests/native/perf_counters_test.cpp` asserts on these
counters, so regressions fail deterministically, without timing.

`tools/jsui-bench` runs a bundle on a simulated 60 Hz clock and prints
mean / p50 / p95 per phase plus the counters, Yoga layouts / cache hits /
measures (from Yoga's event hooks) and heap allocations per frame (malloc
interposition on glibc, `operator new` elsewhere):

```sh
cmake --build build --target jsui-bench
tools/jsui-bench/run.sh build                   # builds tools/jsui-bench/scenarios + examples/interactions.jsx, runs the suite
tools/jsui-bench/run.sh build --json out.json   # save; later: --compare out.json
build/jsui-bench app.js --frames 600 --paint none --mouse-sweep 200,100,600,240
```

Options: `--frames`, `--warmup`, `--width` / `--height`, `--paint
incremental|full|none`, `--mouse-sweep X,Y0,Y1,FRAMES` (moves the mouse
every frame), `--patch-state KEY,SLOTS` (one `jsui_patch_state` per frame).
Scenarios: `interactions` (examples), `list` (2000 rows, one counter),
`menu` (native spring on hover), `inventory` (500 slots through
`useHostState` + `jsui_patch_state`), `idle`. Under valgrind, run with
`--tool=callgrind --instr-atstart=no`: the tool switches instrumentation on
for the measured frames only.

## Paint performance

What painting costs is proportional to the damage, and within the damage
the expensive per-pixel work is done once:

* **Damage.** `updateDamage` (`src/render/painter.cpp`) compares each node's
  paint record (version, size, device matrix, children, scroll indicators)
  with the last frame and damages the old and new bounds of what changed;
  unchanged subtrees outside the damage are culled. Skia does not
  rasterize an anti-aliased outline cut by the clip exactly like the whole
  outline, so damage crossing a curved node's edge grows to the whole node
  (damage inside its padding box does not), and the repaint stays exact.
  On Graphite (WebGPU) only outlines whose pixels depend on the clip grow
  (dashed or dotted edges, outlines over 1024 device px): it draws solid
  rrects analytically and, patched, applies a whole-pixel damage rect as a
  scissor (see WebGPU).
* **Scroll blit.** When a ScrollView's content only scrolled by whole device
  pixels over a uniform backdrop, the CPU path moves the viewport's pixels
  and repaints only what the scroll exposed, what is painted above the
  content and curved outlines at the viewport's edge (`ScrollBlit`).
* **Sprites.** On the CPU raster, gradient backgrounds and Image nodes of
  128x128 to 2.2M device pixels are rendered once per content and device
  scale and blitted at whole-pixel offsets afterwards (`drawSprite`, 48 MB
  per document); blurred shadows and inset shadows are cached masks drawn
  the same way. Sub-pixel positions draw directly (a moving sprite would be
  re-rendered every frame).
* **Layers.** A node whose draws cannot overlap applies its opacity to them
  instead of a `saveLayer` (`opacityFoldable`): one draw (a background
  colour or one gradient, nothing else), a childless box with at most one
  fill and one outer shadow (the shadow is then not drawn under the box), or
  a childless box that only draws a solid border ring of one colour. A
  layer at zero opacity is skipped. On GPU backends each layer is an
  offscreen pass.
* **Text.** Paragraphs keep one layout: painting and the painter's overflow
  bounds lay out at the same content width.

`PaintStats` (`lastPaintStats()`) reports per paint the nodes painted and
culled, the pixels and rects repainted, sprite draws and renders, blitted
pixels and, with `setPaintCounting()`, the canvas draw calls, text / image
draws, layers and clips. `tests/native/render_test.cpp` and `gpu_test.cpp`
assert on these (incremental frames equal full ones; perf guards on
pixels repainted, passes, sprite re-renders, layers).

`tools/jsui-paint-bench` renders bundles headless on a simulated 60 Hz
clock and times only the paint call, per backend: `cpu`
(`jsui_render_rgba_incremental` into one retained buffer, what game hosts
and the browser's CPU path do), `cpu-full` (`jsui_render_rgba`), `gl`
(`jsui_render_gl` into a texture FBO with Ganesh) and `gl-direct` (the same
with `JSUI_GL_NO_RETAIN`). It reports mean / p95 / max paint ms (`gpu`:
including `glFinish`), pixels repainted, painter draw calls (counted in a
second, untimed pass), text / image draws, layers, nodes, and GL draw calls,
framebuffer binds, readbacks, program links and upload KB through a counting
GL loader. The GL context is headless: EGL on Linux (Mesa's surfaceless
platform, so llvmpipe works without a display or GPU) and CGL on macOS (no
window or Xvfb needed).

```sh
cmake --build build --target jsui-paint-bench
node tools/jsui-paint-bench/run.mjs                        # all scenarios, cpu + gl
node tools/jsui-paint-bench/run.mjs --only cards,text --backends cpu,cpu-full,gl,gl-direct --frames 240 --json out.json
build/jsui-paint-bench --frames 240 --backends cpu app=path/to/bundle.js[@hover]
```

Scenarios (`run.mjs` bundles them into `build/paint-bench/`):
`interactions` (examples/showcase/interactions.jsx idling on its looping
animations), `interactions-hover` (the same with a mouse sweep), `caret`
(a static screen with a blinking caret), `cards` (a grid of shadowed,
rounded, clipped cards scrolling 6 px per frame) and `text` (text-heavy
panels with a ticking clock, a ticker and a pulsing badge). Options:
`--frames`, `--warmup`, `--settle-ms`, `--width` / `--height` / `--scale`,
`--backends`, `--json`, `--no-count`. Under valgrind (callgrind
instruction counts are steadier than wall time on a shared machine), pass
`--no-count` and `--toggle-collect='jsui::Runtime::renderIncremental(*'`.
Wall times on llvmpipe say little about a hardware GPU (shader JIT and
rasterization run on the CPU); the GL call counts carry over.

## Dev mode

`jsui dev app.jsx` rebuilds on change and serves `http://localhost:5173/bundle.js`.
Point any host at it (`jsui-viewer --dev http://localhost:5173/bundle.js`, or
`cfg.dev_mode = 1` + `jsui_load_bundle_url`).

* **Version endpoint.** When the bundle came from `http://host:port/...`, the
  runtime GETs `http://host:port/__jsui/version` every ~500 ms (2 s while the
  server is unreachable). The response body is an opaque version token
  (whitespace trimmed); any change means "the bundle changed" and triggers
  `reload()`. The server should answer immediately (no long-poll needed); a
  bundle response may carry `X-Jsui-Version: <token>` so a rebuild between
  the bundle download and the first poll is not missed.
* **Reload** tears down the JS context, the Document, in-flight loads and the
  image cache, then reloads the bundle URL. The last good frame (recorded as
  an `SkPicture` before teardown) stays on screen until the new bundle renders,
  and keeps showing if the new bundle throws.
* **Error overlay.** While `jsui_last_error()` is set, a red-tinted panel with
  the message and stack (monospace) is painted over the frame. Escape
  (`jsui_send_key`) dismisses it; the next successful load clears it.

## Resource loading

`ResourceLoader` (`src/platform/loader.*`) serves the bundle, `fetch`, images
and fonts. `data:` URIs (base64 and percent-encoded) are decoded inline on
every platform; files and `http://` go to an on-demand pool of up to 4 worker
threads; completions are delivered on the runtime thread in `jsui_tick`.
`src/net/http_client.*` is a minimal HTTP/1.1 client for dev / LAN asset
servers: Content-Length, chunked and read-until-close bodies, up to 5
redirects, a 30 s default timeout, cooperative cancellation, no TLS
(`https://` fails with status 0 and a clear error unless the embedder installs
a `JsuiLoadFn`). An embedder loader only sees the URL (GET semantics).
`jsui_pending_loads()` lets headless hosts wait for in-flight loads.

## Reference tests

`tests/reference` renders each example twice — with JSUI (`jsui-render`) and
with react-native-web in headless Chromium (Playwright), same viewport, same
Inter font files — and compares the PNGs with a per-pixel tolerance. A failing
case writes `expected/actual/diff` images and an HTML report. Cases in
`examples/animated` are rendered frame by frame (the frame index drives the
transform values) and also pass a motion check: text must move rigidly with
its box, and boxes must land where Chrome puts them.

Pixel snapping follows Chrome: border boxes snap to device pixels in layout
space, before the node's own transform, and the transform then applies
unsnapped. Under a fractional translate or scale (springs), a box and its
text therefore move together; text under a transform snaps its baseline in
the same layout space and uses subpixel glyph positions on both axes.

## @jsui/react bridge usage (JS side)

Details of how the JS package uses the bridge, for native implementers:

* `__events` tokens sent by `@jsui/react` (matched by substring natively):
  `click` (onClick — every Pressable/Touchable/Button host has it, so they are
  pressable), `pointerDown`, `pointerUp`, `pointerMove`, `pointerCancel`,
  `hoverIn`/`pointerEnter`, `hoverOut`/`pointerLeave`, `wheel`, `keyDown`,
  `keyUp`, `focus`, `blur`, `scroll`, `textInput`, `change`,
  `selectionChange`, `gamepadButton`,
  `gamepadAxis`, `layout`, `load`, `error`, and `responder` (+`pointerMove`) for
  RN responder / PanResponder views. Plain `onPointerDown` does not make a node
  pressable.
* `__jsui_dispatchEvent` returns a number: bit 1 = a handler ran, bit 2 =
  `preventDefault()` was called (suppresses native default actions).
  Pressables handle Enter/Space themselves and return 3; the click a gamepad A
  synthesizes is the press itself.
* Optional bridge members used when present: `patchStyle(id, partialStyle)`
  (Animated / `setNativeProps`; otherwise `setProps` with the merged style),
  `animated` (native Animated driver; otherwise `useNativeDriver` uses the JS driver),
  `viewport()` (native: returns `{width, height, scale}`), `focus`, `blur`,
  `scrollTo`, `flashScrollIndicators`, `measure`, `postMessage`, `loadFont`, `getImageSize(uri)`,
  `prefetchImage(uri)`, `reload`.
* Events beyond input: `{type: 'resize', width, height, scale}` (sent by
  `jsui_set_viewport` when the size changes; drives Dimensions);
  `{type: 'load', target, source: {uri, width, height}}` (resolved URL, image
  pixels) / `{type: 'error', target, error}` / `{type: 'progress', target,
  loaded, total}` on `Image` nodes (`onLoad` / `onError` / `onProgress`;
  `@jsui/react` adds `onLoadStart` when the source changes and `onLoadEnd`
  after load or error). Sent once per source, always asynchronously like
  React Native: on a tick after the one that set the source (after its
  timers and passive effects), also for data URIs and images already cached
  at mount; `progress` only for images that had to be loaded.
* `globalThis.__jsui_reportError(error)` (else the standard `reportError`): uncaught
  React render errors are passed to it; otherwise they are rethrown from a
  `setTimeout` so the engine reports them.
* `globalThis.__jsui_assets[url] = {width, height, scale}` is filled by
  bundled image modules (`@jsui/cli`); `Image` uses it for intrinsic size.
* Host-only props sent with nodes: `Modal` content carries
  `accessibilityViewIsModal: true` (focus scope); disabled pressables carry
  `disabled: true` and `focusable: false`; `ScrollView` carries `horizontal`.
* Dev server endpoints besides `/bundle.js` and `/__jsui/version`:
  `/__jsui/version?since=<token>` long-polls until the token changes;
  `/__jsui/error` returns the current build error text; `/bundle.js` carries
  an `X-Jsui-Version` header.
* Lists (`packages/react/src/components/lists.js`): `FlatList`,
  `SectionList` and `VirtualizedList` are virtualized like React Native and
  use only the public ScrollView API (`onScroll`, `onLayout`,
  `onContentSizeChange`, `scrollTo`). A window of `windowSize` viewports is
  mounted, grown by `maxToRenderPerBatch` every `updateCellsBatchingPeriod`
  ms (RN's `computeWindowedRenderLimits`). Unmounted ranges become spacer
  Views sized from `getItemLayout` or measured cell `onLayout`s; unmeasured
  cells between two mounted ones share the gap evenly, so spacers never move
  mounted cells, and the first visible cell is kept in place when cells
  above it mount (Chrome's scroll anchoring, which the react-native-web
  reference has). The first `initialNumToRender` cells, the last focused
  cell and its neighbours (a global `focus` listener, since focus does not
  bubble) and the closest sticky header stay mounted, so gamepad / keyboard
  navigation reaches the next off-screen cell and native `scrollIntoView`
  moves the window. Sticky headers are wrapper Views translated with
  `patchStyle` per scroll event. `bridgeStats().nodes` is the live host node
  count.
* `tests/native/react_e2e_test.cpp` drives a real `@jsui/react` bundle through
  the C API (mouse, keys, text, gamepad, viewport, host messages, Animated);
  `npm test -w @jsui/cli` builds the bundle and runs it.
