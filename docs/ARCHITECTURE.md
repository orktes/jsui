# JSUI architecture

JSUI is a small, embeddable UI runtime for games: React (JSX) components
written exactly like React Native code, executed by QuickJS (or, in the
browser and Node, by the host's own JS engine), laid out by Yoga and painted
by Skia, on the GPU wherever the host has one. It is a "browser-shaped" stack
(JS + layout + 2D paint + resource loading + input + audio requests) without
a browser: the whole runtime is a static (or shared) library with a C API,
and the same core compiles to WebAssembly.

```
 ┌───────────────────────── app bundle (JS) ────────────────────────────────┐
 │  import { View, Text, Image, StyleSheet } from 'react-native'            │
 │  react + react-reconciler + @orktes/jsui-react (host config, components, │
 │  events, Animated, host data hooks, LottieView, ShaderView, Sound)       │
 │  bundled by @orktes/jsui-cli (esbuild; QuickJS bytecode by default)      │
 └───────────────┬────────────────────────────────▲─────────────────────────┘
   __jsui_native.* (create/updateProps/append…)   │ __jsui_dispatchEvent(ev), __jsui_on*(…)
 ┌───────────────▼────────────────────────────────┴─────────────────────────┐
 │ JsEngine: QuickJS-ng (native, WASM) or the host's JS engine (WASM only)  │
 │   globals, timers, rAF, fetch, fonts, host data bridge, dev session      │
 ├──────────────────────────────────────────────────────────────────────────┤
 │ Surfaces ── Document / Node tree ── Yoga layout ── Text (skparagraph)    │
 │ InputManager: hit test, pointer/keyboard/gamepad, focus, scrolling       │
 │ Native Animated, LayoutAnimation, Lottie (Skottie), ShaderView (SkSL)    │
 │ Host data: shared state, frame buffers, calls, messages, host textures   │
 │ Painter (Skia) with damage tracking, sprite / shadow / layer caches      │
 │ Audio: requests to the host's handler, or built-in miniaudio             │
 ├──────────────────────────────────────────────────────────────────────────┤
 │ Renderers: CPU raster │ Ganesh GL / GLES / WebGL / Metal / Direct3D 12   │
 │                       │ Graphite Vulkan / Metal / Dawn / browser WebGPU  │
 ├──────────────────────────────────────────────────────────────────────────┤
 │ ResourceLoader: file, http (dev server), embedder callback, browser      │
 └───────────────┬──────────────────────────────────────────────────────────┘
          C API  │ include/jsui/jsui.h
   ┌─────────────┼─────────────┬───────────────┬────────────────┬──────────┐
   │ jsui-viewer │ web/ (WASM, │ jsui CLI      │ game engines:  │ bindings │
   │ jsui-render │ main thread │ (headless in  │ Godot, Unity,  │ Rust, Go │
   │ jsui-record │ or worker)  │ Node; agents) │ Unreal, raylib,│          │
   │ jsui-bench  │             │               │ SDL3, sokol,   │          │
   │             │             │               │ Android, iOS   │          │
   └─────────────┴─────────────┴───────────────┴────────────────┴──────────┘
```

## Why these choices

* **C++20 core.** Skia and Yoga are C++, QuickJS is C. One language, no FFI
  layer, one library, and Emscripten builds it for the browser. Game
  engines (Godot GDExtension, Unreal, Unity native plugins) consume C/C++
  directly. The public surface is a C header so any language can bind it
  (see [Hosts and integrations](#hosts-and-integrations)).
* **QuickJS-ng** — small, fast startup, ES2023, embeddable, compiles to WASM.
  Bundles ship as precompiled QuickJS bytecode by default (see
  [Bundles](#bundles-precompiled-bytecode)), and QuickJS itself is patched
  and built with PGO in release builds (see [Performance](#performance)).
  The engine is isolated behind the `JsEngine` interface: the WASM build can
  run the bundle on the browser's (or Node's) own JIT-compiled engine
  instead (see [Host JS engine](#host-js-engine)). Small binary size and
  memory matter more here than peak JS speed, which a UI layer rarely needs.
* **Yoga** — the layout engine React Native uses, so flexbox behaves exactly
  like RN (column default, `flexShrink: 0`, `position: relative`, etc.).
* **Skia** — what Chrome and Flutter paint with; gives us anti-aliased rounded
  rects, blurs for shadows, gradients, text shaping (HarfBuzz + ICU via
  skparagraph), image codecs, Skottie (Lottie) and SkSL runtime effects.
  Two milestones are in use: the default native build links the aseprite
  prebuilt static libraries (m124: CPU raster + Ganesh GL), and the plain /
  WebGL WASM modules use m124 built by `scripts/build-skia-wasm.sh`. Every
  GPU path beyond GL (Graphite on Vulkan / Metal / Dawn / WebGPU, Ganesh on
  Metal and Direct3D), Android, iOS and the release binaries use Skia m157
  built from source by `scripts/build-skia-{desktop,wasm-graphite,android,ios}.sh`. The
  JSUI sources build against both (`__has_include` checks for the APIs that
  changed).
* **GPU by default, CPU raster as the fallback.** Every interactive host
  draws with Skia on the host's own GPU device (the viewer, the browser,
  Godot, Unity, Unreal, the engine examples, Android and iOS); see
  [GPU backend](#gpu-backend) for which backend each uses. The CPU raster
  (`jsui_render_rgba*`, an RGBA buffer the host uploads) runs the same
  `paintDocument()` and is used when no GPU context can be made, in CI and
  reference tests (deterministic pixels), and by the headless tools
  (`jsui-render`, `jsui-record`, the CLI's `screenshot` / `inspect` / `run`).
* **React Native semantics, written as `react-native` imports.** App code
  imports from `'react-native'`; the JSUI bundler aliases that to
  `@orktes/jsui-react`, and the Chrome reference build aliases it to
  `react-native-web`. Code an LLM (or person) writes for RN therefore runs
  unchanged in JSUI and in the browser reference.

## Repository layout

| Path | What |
| --- | --- |
| `include/jsui/jsui.h` | Public C API (embedding, input, resources, rendering, host data, textures, audio, fonts, surfaces) |
| `src/core/` | `Value`, `Node`, `Document`, style parsing (`style.cpp`), JSON, scrolling geometry, LayoutAnimation, the overlay layer (`overlay.*`), perf counters |
| `src/render/` | Skia painter, image cache, host textures, Lottie, ShaderView, pipeline cache, GPU backends (`gpu.cpp` Ganesh GL, `gpu_metal.cpp` Ganesh Metal, `gpu_d3d.cpp` Ganesh D3D12 / D3D11, `gpu_graphite.cpp` Graphite Vulkan / Metal / Dawn, `gpu_webgpu.cpp` Graphite on browser WebGPU) |
| `src/text/` | Font manager and decoding (WOFF / WOFF2 / TTC), paragraph building and Yoga measure for `Text`, TextInput editing |
| `src/js/` | `JsEngine`: the QuickJS host and bridge globals (`bridge*.cpp`), the mutation command buffer (`command_buffer.*`), bytecode bundles (`bytecode.cpp`), the host JS engine's native half (`engine_hostjs.cpp`), and the worklet runtimes (`worklets*.cpp`, see "Worklets") |
| `src/animated/` | Native Animated driver |
| `src/input/` | Hit testing, event routing, focus and gamepad navigation, scrolling, text editing keys |
| `src/audio/` | UI audio: requests to the host's audio handler or the built-in miniaudio backend (see [AUDIO.md](AUDIO.md)) |
| `src/platform/` | Resource loader backends (file, http, emscripten) |
| `src/net/` | Minimal HTTP/1.1 client for dev servers (native) |
| `src/host_state.*`, `src/host_buffers.*` | Shared state and frame data buffers (see "Game data") |
| `src/surface.*`, `src/runtime.*`, `src/capi.cpp` | Surfaces, the runtime object and the C API wrappers |
| `src/dev_session.*`, `src/dev_overlay.cpp` | Dev mode: the `jsui dev` shared session client and the error overlay |
| `tools/jsui-render` | Headless renderer: bundle → PNG (tests, CI) |
| `tools/jsui-record` | Headless recorder: bundle + JSON input timeline → 60 fps frames / H.264 (ffmpeg), with cursor, caption and input-legend overlay |
| `tools/jsui-viewer` | Windowed host (SDL2) with dev mode (hot updates / live reload), GPU by default |
| `tools/jsui-bench` | Frame benchmark: per-phase time (JS, commit, Animated, style, layout, input, paint), work counters and allocations per frame (see "Performance") |
| `tools/jsui-paint-bench` | Paint benchmark: paint time, pixels repainted, draw calls and GL calls per frame on the CPU raster and Ganesh GL (see "Paint performance") |
| `tools/dawn-shim` | Generates the `wgpu*` trampolines of the Dawn native backend (see "Dawn native") |
| `integrations/` | Host examples and engine plugins: Godot, Unity, Unreal, raylib, SDL3, sokol, Android, iOS (see [Hosts and integrations](#hosts-and-integrations)) |
| `bindings/` | Rust and Go bindings over the C API, with a quest-log example app |
| `web/` | Browser host for the WASM build (`@orktes/jsui-web`): main-thread and worker modes, WebGPU / WebGL / CPU, Web Audio, host textures |
| `packages/react` | `@orktes/jsui-react`: reconciler, components, StyleSheet, events, Animated, host data hooks |
| `packages/cli` | `@orktes/jsui-cli`: `jsui build` / `jsui dev` (esbuild, HMR, shared session), headless `screenshot` / `inspect` / `run`, the agent skill |
| `site/` | The showcase site (GitHub Pages) |
| `scripts/` | Skia / WASM / Android / iOS builds, PGO, release packaging (`scripts/dist/`), version bumps |
| `examples/` | Realistic game UI examples (shared by reference tests) |
| `tests/native/` | C++ unit tests (`jsui-tests`) |
| `tests/reference/` | Pixel comparison against react-native-web in headless Chrome |
| `assets/fonts/` | Bundled default font (Inter, OFL) |

## Frame loop

The embedder owns the loop (see `jsui.h`):

1. Forward input with `jsui_send_*`. The InputManager hit-tests against the
   last layout and posts the event to `__jsui_dispatchEvent` (see "Inbound
   messages"; delivered at once). Each call returns whether the UI consumed
   the event, so the game gets the rest (see "Input routing details").
2. `jsui_tick(now)`: deliver finished resource loads, run due timers, run
   `requestAnimationFrame` callbacks, drain promise jobs, advance scroll
   physics, evaluate native-driven Animated animations (then deliver their
   end callbacks / listener updates to JS), run Yoga if dirty, advance
   LayoutAnimations, emit `onLayout` events.
   `jsui_tick_native(now)` does only the native part (native-driven Animated,
   LayoutAnimations, Lottie playback, layout) without running any JS, for
   hosts that skip JS on a frame (see "Animated").
3. If `jsui_needs_redraw()`, render with the backend the host set up:
   `jsui_render_gl()` / `jsui_render_gl_target()`, `jsui_render_metal()`,
   `jsui_render_gpu_target()` (Vulkan, Metal, Dawn, Direct3D),
   `jsui_render_webgpu()`, or the CPU raster (`jsui_render_rgba()` /
   `jsui_render_rgba_incremental()`).

Hosts that sleep when idle check `jsui_wants_frame()`: it stays set while
something moves on its own (rAF callbacks, due timers, in-flight loads,
scroll flings / smooth scrolling / snapping, scroll indicator fades,
LayoutAnimations, native-driven Animated animations and their pending end
callbacks, held gamepad sticks or D-pad) and the host should keep ticking at
display rate. JS-driven Animated animations run on `requestAnimationFrame`,
so an endless `Animated.loop` keeps it set without any input either way.
While it is clear, `jsui_next_timer()` says when the next JS timer is due,
so the host can sleep until then (or until input). The web host does this
by default (`idleSleep`): once a frame leaves nothing to tick or paint, it
cancels its `requestAnimationFrame` loop and resumes on a timeout at the
next timer, on any wasm export that changes the runtime (input, state,
messages, sizes; `wakeOnCalls` in web/jsui-web.js), and when the host JS
engine queues work outside a tick (a native promise's continuation). An
idle page then runs no rAF tasks at all instead of one per display frame.

Everything is single-threaded except resource I/O, which completes onto a
queue drained in `jsui_tick`, Graphite's pipeline precompiles on Vulkan and
Metal (a worker thread, see "Pipeline cache") and the built-in audio
backend's device callback. In the browser's worker mode the whole runtime
runs in a Worker (see "Builds").

## Surfaces (several root views)

One runtime can show several independent root views, like React Native's
`RCTRootView` / `ReactSurface`: a HUD, a nameplate per character, a widget
dropped into an existing (non-JSUI) UI, all from one bundle and one JS
context. The bundle registers components with
`AppRegistry.registerComponent(key, () => Component)`; the host calls
`jsui_surface_create(rt, key, propsJson, w, h, pixelRatio)` for each place it
wants one.

* `Surface` (`src/surface.h`) owns what is per view: a `Document` (root
  container, viewport, layout, paint records, focus hints), an
  `InputManager` (hover, capture, focus, scrolling, gamepad repeat), the
  cursor and the render-target bookkeeping (incremental / damage-tracked
  repaints, dev-mode frozen frame and overlay). The `Runtime` keeps what is
  shared: the JS engine, resource loader, image cache, fonts, host state and
  calls, native Animated. `Runtime::main()` is the surface of the runtime
  handle (root tag 1).
* The C handle of a surface is a `JsuiRuntime*`: per-surface calls
  (viewport, `jsui_render_*`, `jsui_send_*`, `jsui_needs_redraw`, cursor)
  act on the handle's surface, runtime-wide calls on the shared runtime. A
  host ticks once per frame and renders each surface that needs it.
* Node ids are unique across the surfaces (one counter), so JS addresses any
  node by id alone. The root tag of a surface is its root container's id.
  `createNode(type, rootTag)` creates the node in that surface's document;
  `appendChild` / `insertBefore` across documents are refused with a
  warning (a portal into another surface).
* JS protocol: after the bundle evaluates, the runtime calls
  `__jsui_runSurfaces([[rootTag, appKey, propsJson], ...])` with every
  surface. Surfaces created or given new props (`jsui_surface_run` /
  `jsui_surface_set_props`) are delivered at the start of the next tick, all
  in one call, which commits them in one React pass; the same props again
  cost nothing (hosts may push their state every frame). `jsui_surface_destroy` calls `__jsui_stopSurface(rootTag)`
  first, then frees the surface (at the start of the next tick when JS is on
  the stack). `@orktes/jsui-react`'s AppRegistry renders each app into its
  own subtree; a key registered later starts the surfaces waiting for it.
  The main surface's app has a React root of its own; the other surfaces'
  apps share one more root, each portaled into its surface's root container
  under an error boundary (an app that throws unmounts alone and is
  reported like an uncaught error). New props re-render only that surface
  (`forceUpdate` of its `SurfaceRoot`, under `flushSync`), so one frame's
  updates of many surfaces are one React render and commit.
  `__jsui_native.surfaceApp(rootTag)` tells AppRegistry whether the host
  picked the main surface's app (`jsui_surface_run` on the runtime handle),
  in which case the first registered component does not auto-start.
* `RootTagContext` gives a component its surface; `useWindowDimensions()`
  is the surface's size (`viewport(rootTag)`), `Dimensions.get('window')`
  the main surface's; `resize` events carry `rootTag` and wake only that
  surface's `useWindowDimensions` subscribers (`Dimensions` change
  listeners hear about the main surface only). A surface's (not the main
  one's) resize reaches JS at the start of the next tick, together with the
  other surfaces' resizes and props, in one microtask drain
  (`JsEngine::beginBatch`). `Modal` portals into
  the root of the surface it is rendered in.
* A viewport width or height <= 0 sizes that axis to the content: the root
  is laid out unconstrained (Yoga ignores the app wrapper's 0 flex basis
  without a definite size) and `jsui_surface_get_size` reports the result.
* Surfaces survive `jsui_reload` and start their apps again with the new
  bundle. Their roots render on transparent.

## Nested runtimes (JSUIContainer)

Surfaces share one JS context. `<JSUIContainer>` runs a *separate* runtime
(its own JS context, React tree, document, input, fonts, image cache) inside
a node of the parent: an app that hosts apps built with JSUI
(`src/render/container_view.*`, `packages/react/src/container.js`).

```jsx
<JSUIContainer
  bundleSrc="apps/solitaire.js"      // or bundleData={source}
  appKey="Solitaire" initialProps={{ seed: 7 }}
  hostState={{ theme: 'dark' }}
  onMessage={(name, payload) => {}} onCall={async (name, args) => answer}
  onStateChange={(key, value) => {}} onError={(message) => {}}
  ref={ref}                          // postMessage / call / setState / patchState / reload
  style={{ width: 640, height: 360 }} />
```

The container node (`NodeType::Container`, "JSUIContainer") is the child's
embedder and does what a host does with the C API:

* **Source.** `bundleSrc` is resolved against the parent's base URL and the
  child's base URL becomes it; the child's every load (bundle, images,
  fonts, `fetch`) goes through the parent's `ResourceLoader`, so the
  embedder's `load` callback decides what a URL means. Inline source reaches
  native once per change as a `load` command (`bundleData` as a prop works
  too, but the bridge would copy it on every update).
* **Frames.** `Containers::tick` runs in the parent's `Runtime::tick` after
  layout: it gives each child its node's content box (layout size less
  padding and border) as viewport at the parent's pixel ratio, creates /
  replaces the child when the source changed, runs queued commands and
  `jsui_tick`s it. A child that has something new to paint marks its node
  paint-dirty, so the parent's damage covers the node and the host repaints.
  The parent's `jsui_wants_frame` / `jsui_next_timer` include its children's.
  The child's JS runs only there, never nested in the parent's JS.
* **Painting.** `container::paint` draws the child's frame into the parent's
  canvas at the content box, clipped to the node's inner rounded rect, under
  the node's transforms and opacity (`Surface::renderCanvas` with the canvas
  scaled back to device px, inside a `NestedPaintScope` that hides the
  parent's LayerCache). No texture: the same on every backend, also inside
  deferred GPU frames.
* **Input** (`InputManager`, "JSUIContainers"). Pointer, touch and wheel
  input over the node goes to the child first, in its coordinates. A press
  routes its pointer's moves and release to the same child; a press the
  child consumed is the child's alone, otherwise the parent handles it as
  well. The container is focusable (a press the child takes focuses it;
  gamepad / Tab navigation can move onto it); keys, text and gamepad input
  go to the focused container's child first and what it does not consume (a
  D-pad press at its edge) moves on in the parent. Focus leaving the
  container blurs the child's focus. `jsui_hit_test` asks the child;
  `jsui_hit_regions` reports the whole container as capturing.
* **Host API both ways.** The child's `Host.postMessage`, `Host.call`,
  `HostState.set/patch` and error logs become `containerevent` events on
  the node (`kind`: `message`, `call`, `callCancel`, `state`, `error`,
  `callResult`); the component turns them into `onMessage`, `onCall` (its
  return value or promise answers the call: `resolve` / `reject` commands),
  `onCallCancel`, `onStateChange` and `onError`. The parent's ref methods
  and `hostState` prop are `containerCommand(id, {op, ...})` bridge calls
  (`post`, `call` → `jsui_call_ui` with a `callResult` event back, `state`,
  `reload`, `load`), queued on the node and run by the next tick.
* **Isolation and limits.** Nothing is shared but the loader, the clock and
  the log. Fonts registered on the parent are not the child's; the child's
  sounds use the built-in audio backend and its `<Video>` has no handler
  (both handlers answer the embedder's own runtime); host textures set on
  the parent are not visible in the child. The child runs JS on the owner
  thread (no JS thread of its own). Containers nest.

## GPU backend

The runtime draws with Skia on the host's own GPU device and queue: it
never creates a device of its own (D3D11 hosts excepted, see below) and
never owns the swapchain. Every backend runs the same painter and damage
tracking as the CPU raster; what differs is how a frame reaches the
target. Which one a host uses:

| Backend | Skia | C API | CMake option | Used by |
| --- | --- | --- | --- | --- |
| OpenGL / GLES / WebGL | Ganesh | `jsui_gpu_create_gl`, `jsui_render_gl[_target]` | `JSUI_GPU` | jsui-viewer (off macOS, or `--gl`), raylib, sokol off macOS, SDL3 on GL, Rust / Go examples, Android without usable Vulkan (GLES 3), the browser's WebGL fallback |
| Browser WebGPU | Graphite (Dawn backend on Emdawnwebgpu) | `jsui_gpu_create_webgpu`, `jsui_render_webgpu[_target]` | `JSUI_WEBGPU` | the web host's first choice |
| Vulkan | Graphite | `jsui_gpu_create_vulkan`, `jsui_render_gpu_target` | `JSUI_VULKAN` | Godot, Unity, Unreal and SDL3 on Linux, and on Windows when the engine runs on Vulkan; Android |
| Metal (desktop) | Graphite | `jsui_gpu_create_metal`, `jsui_render_gpu_target` | `JSUI_GRAPHITE_METAL` | Godot, Unity, Unreal, SDL3, sokol and jsui-viewer on macOS |
| Metal (iOS) | Ganesh | `jsui_metal_create`, `jsui_render_metal` | `JSUI_METAL` | the iOS example |
| Direct3D 12 / 11 | Ganesh on D3D12 | `jsui_gpu_create_d3d12` / `_d3d11`, `jsui_render_gpu_target` | `JSUI_D3D` | Godot, Unity and Unreal on Windows |
| Dawn native | Graphite | `jsui_gpu_create_dawn_procs`, `jsui_render_gpu_target` | `JSUI_GRAPHITE_DAWN` | Godot's `webgpu` driver on the godot-aw3 fork |

Host textures (`jsui_set_texture`) are sampled in place on each of these,
and every Graphite context can record and precompile its pipelines (see
"Pipeline cache"). Each host's README
in `integrations/` says how it orders JSUI's work with its own.

### OpenGL / GLES / WebGL (Skia Ganesh)

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
  target. A texture target the host keeps between frames
  (`JSUI_GL_INCREMENTAL`, e.g. raylib's `RenderTexture2D`) is the retained
  frame itself: only the damage is repainted into it and nothing is copied.
  Each frame ends with one `flushAndSubmit` and no CPU wait. A later
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
  texture. On macOS with a `JSUI_GRAPHITE_METAL` library it uses Skia
  Graphite on Metal instead (falling back to GL): the frame is painted
  incrementally into a retained texture that is blitted into the window's
  CAMetalLayer; `--gl` picks GL. `--cpu` forces the CPU path, `--gpu` is accepted as a no-op
  alias. `jsui-render` / `jsui-record` stay CPU (deterministic output).
  On macOS it asks SDL for a 3.2 core, forward-compatible
  profile (`--gl-core`, default there): the default legacy 2.1 context only
  has GLSL 1.20, which Skia's shaders cannot use (`gl_VertexID`). Ganesh
  detects core profiles (`GL_CONTEXT_PROFILE_MASK`) and binds its own vertex
  array object, since core profiles have no default one. In the browser,
  when it gets no WebGPU device (WebGPU comes first, see "WebGPU (Skia
  Graphite)"), `createJsui({ canvas })` (renderer `'auto'`) loads
  `web/dist-webgl/jsui.js` when WebGL is available (else, or when that is not deployed,
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
  the copy, as tasks of the same Graphite recording (one command encoder and
  one submit a frame). That is one pass, like a direct frame, plus a
  full-frame read of the copy. Targets without `CopySrc`, and `JSUI_WEBGPU_NO_CLEAR`, use a
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
  2-3 maps (~30 draws, ~2 bind groups, ~22 pipeline binds and 1 map with
  the atlases, masks and raster cache below); `webgpu: steady animation
  frames` in the browser tests guards it.
* **No uploads in steady frames.** Graphite evicts its text, path and clip
  atlases after every `snap()` unless every Recording is inserted in the
  order it was snapped, which the contexts promise
  (`fRequireOrderedRecordings`, here and in `gpu_graphite.cpp`): a glyph is
  rasterized and uploaded once, not in every frame that draws it (~134
  glyphs a frame of the showcase's steady animation, ~560 per full
  repaint). Shadow masks (blurred on the CPU, uploaded as textures) were
  made anew whenever a shadow moved by a sub-pixel or changed size. On GPU
  canvases a mask is drawn at its fractional position with bilinear
  filtering instead of being keyed by it, and the shadow of a node whose
  scale or size animates is drawn from the mask of the nearest of 8 sizes
  per octave, stretched to it, and repainted exactly once it stops
  (`quantizedScale`, `NodeRecord::approximate` in `painter.cpp`; the CPU
  raster does the same while a node keeps moving, see Paint performance:
  Shadows). Such approximate
  draws ask for an exact repaint (`Document::markExactRepaint`, per
  surface), which `jsui_needs_redraw()` reports once nothing animates any
  more: while animations, scroll physics or JS frame callbacks drive frames
  the next one repaints it anyway, and the surface still goes idle after
  each frame (`whenIdle`, a perpetually scaling loading screen). A steady showcase
  frame creates no textures and uploads nothing (before: ~1 new texture a
  frame for the deploy button's moving shadow, the tab underline, the
  pulsing live dot and the progress bar's glow); the entrance and
  scrolling upload only what is drawn for the first time (glyphs, item
  icons and card art with their mips).
* **Composited layers.** Like Chrome's compositor layers for opacity and
  transform animations, a layer (a node whose opacity needs one: children
  or overlapping draws) is rendered into a texture of its own once and
  then drawn again at each frame's opacity, instead of being rendered
  offscreen every time it is painted (`LayerCache` in `painter.h`, owned by
  the GPU context and set with `LayerCacheScope` around a frame:
  `GpuLayerCaches` / `GpuLayerFrame` in `gpu_layers.h`, which the Graphite
  backends share, browser WebGPU and native Vulkan / Metal / Dawn alike). The
  texture is reused while the subtree paints the same relative to the node:
  `updateRecords` gives each node a stamp that changes whenever anything
  under it changes, but not when only the node's own opacity, transform or
  zIndex change (`Node::contentVersion`, which `patchStyle` leaves alone for
  those groups). It holds the whole layer (up to 4096 px a side), so a layer
  moving into view needs no new render. A whole-pixel move draws the same
  pixels as a layer; while the node moves by sub-pixels (an Animated
  translation) the texture is drawn at the nearest whole pixel, as a
  compositor would, and when it comes to rest the next frame repaints it
  exactly (`layerInexact`). A subtree that changed in the last frame (a
  progress bar inside a fading panel) takes a plain layer clipped to the
  damage, as before, with its static parts drawn from raster textures (see
  Raster cache). Each surface drawing through the context has its own
  cache (dropped after 600 frames of the context without one of its own);
  textures are freed after 120 of its frames unused or over the budget
  (LRU): 3/8 of the context's (`jsui_gpu_purge`'s `cache_bytes`, 96 of
  the default 256 MB), shared by its surfaces. `jsui_gpu_purge`, a lost
  device and the context's destruction free them all. On native Graphite
  the cache works the same for host-owned targets, retained
  (`JSUI_GPU_INCREMENTAL`, damage rects over the static backdrop) or not;
  a frame recorded off the context's thread (`jsui_record_gpu_frame`) is
  painted without it. `JSUI_LAYER_CACHE=off` in the environment turns it
  off on every GPU context (for A/B runs; `setGpuLayerCaches(false)` in
  tests), `JSUI_WEBGPU_NO_LAYER_CACHE` per WebGPU render. On
  `interactions.jsx` the entrance (staggered menu buttons and panels fading
  and sliding in) went from ~5.9 render passes and ~140 draws per frame to
  ~3.4 and ~90 at 1280x720; what is left are two panels whose content
  animates during the fade. `webgpu: fading and moving layers` in the
  browser tests checks no offscreen passes and pixels equal to plain
  layers.
* **Raster cache.** Damage under or over a subtree that does not change
  repaints it draw by draw: on `interactions.jsx` the glow drifting behind
  the top bar repainted the top bar, the menu, the carousel and the top of
  the armory panel (~75 nodes) every frame. Like Flutter's raster cache, a
  subtree (4+ nodes, no blend mode reaching what is below it) that damage
  reached in two consecutive damaged frames, its stamp unchanged in the
  second (the frame that changed it counts: a list row entering the
  viewport or hovered is drawn from its texture from the next frame on), is
  drawn from a texture of it (the same `LayerCache`
  entries and stamps as composited layers, at opacity 1), until its stamp
  changes, it scales, or no damage reached it for 120 damaged frames
  (`updateRasterCache` in `painter.cpp`). A texture that would be mostly
  hidden (a scroll view's whole content behind its viewport) is not made,
  unless it is small (at most an eighth of the screen: a row cut by the
  viewport's edge). A subtree too large for one texture (over 4096 px a
  side, or half the cache's budget) gets a texture of its part on the
  screen, used only while the subtree stays where it is: at 2x the armory
  list's content (48 rows of 60 px, 5760 device px) passed as mostly
  visible by that part, and moving in every scrolled frame it was rendered
  into a new 704x2160 texture in 3 frames of 5, all its rows painted into
  it (~120 draws and ~136 KB of staged data per frame at 1080p@2x, to ~52
  and ~59 KB now, as at 1280x720).
  The texture is the subtree's draws composited as a group: equal to
  painting them directly up to rounding where it is translucent. So that a
  retained frame still equals a full repaint, the state follows the damage
  only (never what a frame happened to paint), and the whole subtree is
  repainted whenever it switches. On `interactions.jsx` at 1280x720 a
  steady frame went from ~70 draws to ~30 (main thread ~2.7 to ~2.2 ms per
  awaited frame, SwiftShader); scrolling the armory list draws each row
  from its texture (~136 to ~59 draws). Inside a layer that is painted again
  every frame (a panel fading in while its content changes) the static
  subtrees are drawn from their textures too, in a plain layer and when the
  layer is rendered into its cached texture alike (not inside another
  subtree's raster texture, which would only be rendered with it): in the
  entrance the armory panel's rows, ~63 to ~38 draws per frame.
  `JSUI_WEBGPU_NO_RASTER_CACHE` turns it off; `webgpu: raster cache` in the
  browser tests compares it with direct painting, in steady frames and with
  the list scrolled at 2x.
* **Motion layers.** Both of the above reuse a texture only while the
  node's device matrix differs from the one it was rendered at by a
  translation: a scale or rotation animation (a button springing, a card
  popping in) rendered a cached layer into a new texture every frame, and
  a raster-cached subtree was painted node by node while it scaled. Like a
  compositor animating a layer, a cached layer or raster-cached subtree
  that keeps scaling or rotating (it moved in this frame and the one
  before, `NodeRecord::changing`, so not a one-off jump), while nothing in
  it changes, is drawn from the texture it was last rendered into, mapped
  by the change of matrix since, with bilinear sampling (`motionTransform`
  in `painter.cpp`). "Nothing changes" is a second stamp
  (`NodeRecord::motionStamp`) that follows the layout instead of the
  pixel-snapped boxes, which snap differently at every scale. The texture
  must hold the whole layer (not cut at the canvas edge), and the change
  must scale by 1/2 to 5/4. Beyond that a fresh texture is rendered at the
  current scale, so a shrinking layer does not alias and a growing one
  blurs at most by a quarter: a 0.3 to 1 pop-in renders 5 textures in 24
  frames. Text is resampled with the rest, soft by that much while the
  motion lasts; like a sub-pixel move (`layerInexact`), the node is
  repainted exactly the frame after it comes to rest.
  `PaintStats::motionDraws` counts the draws; `setMotionLayers(false)` or
  `JSUI_MOTION_LAYERS=off` turns it off. `tests/native/motion_layers_test.cpp`
  runs the LayerCache on a raster canvas (it works there too; only GPU
  contexts install one; `vulkan_graphite_layer_cache_matches_cpu` and
  `metal_graphite_layer_cache_matches_cpu` run it on the GPU). At rest the frame equals, to the bit, what the same
  scene paints when it was at rest there all along. A texture rendered
  while nodes in it were in motion (shadows from the phase-0 mask, scaling
  text, see PaintMotion) is marked approximate and never drawn once they
  rest. Before, a cached opacity layer that scaled kept such a texture at
  rest, a level off on ~1200 pixels. While a 140x72 card
  springs from 1 to 0.86 to 1.08, at most ~85 pixels (edges and glyphs) are
  over 48 levels off a direct paint. It paints 52-56 nodes in 20 frames
  instead of 120, with 1 texture render instead of 18 for an opacity layer.
* **Static backdrop.** Each damage rect is cleared and repainted from the
  root, so it also repaints whatever lies under everything: on
  `interactions.jsx` the background colour, the full-screen image and the
  scrim's two gradients, 4 draws (plus the clear) in each of a frame's 3-4
  rects. What the document paints first, in paint order up to the first
  node that is changing, is kept in a texture of the canvas' size
  (`Backdrop`, `updateBackdrop` in `painter.cpp`, in the same `LayerCache`),
  and each rect is drawn from it (`kSrc`, in place of the clear) before the
  rest is painted. Down from the root, each node it ends inside of paints
  its own decorations and its first children into it: such a node must be
  unchanged itself (`selfStill`) and paint without a layer; a child in it
  whole must be quiet, have fewer than 4 nodes and no layers (never a
  raster-cache candidate, so nothing in it depends on texture state). A
  signature of what each one paints and where (with the font and image
  generations and the canvas size) must stay the same for 2 updates before
  a texture is made, and a new one renders it again. Painted over nothing,
  the texture holds exactly the pixels painting the backdrop directly gives
  (the canvas' format, at the origin, so dithering matches too), so
  repaints over it equal full ones bit for bit. It costs one texture of the
  canvas' size. On `interactions.jsx` at 1280x720 Skia draws per frame went
  from ~73 to ~58 scrolling the armory list and from ~48 to ~31 in steady
  frames. `JSUI_WEBGPU_NO_BACKDROP` turns it off; `webgpu: static
  backdrop` in the browser tests checks the pixels against painting it,
  also after the backdrop changes.
* **What each pass is** (`interactions.jsx`, `frame-bench.mjs` lists them
  with GPU times). Steady (the looping glow, deploy pulse, live dot and
  progress shimmer): one pass, the canvas (cleared, the last frame drawn
  from the copy-back texture, the damage repainted in up to 4 scissored
  rects), then up to 4 `copyTextureToTexture` back. No offscreen passes:
  the animated boxes are childless, so their opacity folds into their
  draws; outer and inset shadows are cached sprites; paths are rasterized
  into Graphite's path atlas on the CPU and text into its glyph atlas, both
  uploaded with `copyBufferToTexture` when first drawn, not rendered. The
  frame's vertex, index and uniform data (~35-80 KB) goes through one
  mapped staging buffer and 4-6 `copyBufferToBuffer` (browser WebGPU
  cannot map vertex or uniform buffers). Scrolling the armory list: the
  same single pass (no scroll blit on the GPU path; the list's damage is
  repainted), 4 copies, and the raster textures of the row coming into
  view and of the two whose hover state changes under the pointer (352x61,
  ~0.6 a frame). The entrance (first ~2.5 s): plain layers
  only for the armory panel and the season card, which fade while their
  content animates (the list fills in, the progress bar grows): 1024x1024
  and 512x128 approx-fit passes per damage rect at 1280x720, 1536x1024 /
  1536x512 / 1536x256 / 1024x256 at 1080p@2x, with the armory's rows drawn
  from their raster textures. The menu buttons (512x64 each) and the centre
  column (512x1024) fade and slide as composited layers, rendered once.
* **Compared with Chrome.** `frame-bench --renderers chrome` renders the
  same app through react-native-web. Chrome's structural advantages: the
  page's main thread only records paint ops; rasterization happens in the
  GPU process (or raster workers) into 256 px tiles that are kept, so only
  invalidated tiles are re-rastered; compositor layers (animated opacity /
  transform, will-change) are moved and faded by the compositor without
  re-rasterizing; and the display compositor redraws only the damaged
  part of the screen (partial swap). JSUI records with Skia on the page's
  main thread (wasm) and, on WebGPU, must fill the whole canvas every frame
  (`getCurrentTexture()` is a fresh texture), which the copy-back texture
  makes one full-screen textured draw; the composited layers above are the
  part of Chrome's model JSUI now has, and the raster cache stands in for
  its kept tiles. Headless (SwiftShader) at 1280x720, steady: Chrome's main
  thread is busy ~1.6 ms and its GPU main thread ~2.8 ms per frame; JSUI on
  WebGPU ~4.6 ms (~2.5 ms in `jsui.frame()` per awaited frame; ~7.3 and
  ~3.8 ms before the atlas, mask, raster cache and record-skip changes) and
  ~88 ms (SwiftShader drawing the full canvas), on WebGL ~3.1 ms per
  awaited frame and ~50 ms. Of a profiled steady WebGPU frame
  (`web/test/profile-frame.mjs`, ~1.9 ms of `jsui.frame()` with the
  profiler on; ~3.2 ms before the changes above) the painter's canvas calls
  take ~0.3 ms, the damage walk ~0.13 ms and native Animated ~0.09 ms;
  Graphite's snap, command encoding and submit and the canvas texture take
  ~0.85 ms (Graphite's `queue.submit` ~0.2 ms of it), and the copy-back's
  own encoder and submit, which carried one copy, ~0.13 ms (since folded
  into Graphite's recording), with the JS / wasm crossings most of the rest.
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
  `examples/showcase/interactions.jsx` per renderer, after its entrance
  animations (`--settle`, 3 s): the rAF-to-rAF interval of the host's own
  loop (what the user feels) with the main-thread time per frame and, on
  WebGPU, when the GPU finished each frame (observed, never awaited); the
  busy time per frame of the page's main thread, compositor, raster
  workers, GPU main thread and viz from a Chrome trace; then the old
  "finish" metric (frames stepped by hand, each awaited until the GPU is
  done: WebGPU `onSubmittedWorkDone()`, WebGL a 1-pixel `readPixels`, which
  add the round trip to the GPU process that the real loop never waits for);
  then the WebGPU calls per frame (`web/test/gpu-count.js`, with the bytes
  copied and mapped); then GPU time per frame from timestamps around every
  render pass and every run of back-to-back copies (`web/test/gpu-timing.js`,
  the `timestamp-query` feature; WebGL: timer queries, in a loop of their
  own), listed by pass target and copy kind. WebGPU only writes timestamps
  at pass boundaries, so the marks are empty compute passes: around each
  single copy they added a pass and two encoder switches to what was
  measured (on an M1, Graphite's few small staging copies read as ~0.4 ms a
  frame), and a run's time still includes one such switch.
  `--scenario entrance` measures from mount (the instrumented pages step
  from mount time, so it plays the same on a slow software GPU) and
  `--scenario scroll` turns the wheel over a list. `--renderers chrome`
  renders the same app with react-native-web (the reference tests'
  bundler) in Chrome itself, the bar to compare with (rAF interval and
  thread times). Headless it runs on SwiftShader; on a Mac with Chrome
  installed, `node web/test/frame-bench.mjs --real-gpu --renderers
  webgpu,webgl,chrome` uses the real GPU (Metal) in a visible window.

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
  Metal; both: Graphite + Dawn, header-only) into `third_party/skia-desktop`; `-DJSUI_SKIA_DIR=third_party/skia-desktop`
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

### Metal (Skia Ganesh, iOS)

`jsui_metal_create(device, queue)` / `jsui_render_metal(rt, mtl, texture,
format, w, h, flags)` (`src/render/gpu_metal.cpp`, CMake option
`JSUI_METAL`, on by default in iOS builds) draw with Ganesh on Metal
straight into an `id<MTLTexture>`, typically a `CAMetalLayer` drawable. JSUI
submits on the host's queue, so whatever the host encodes on it afterwards
(present, compositing) runs after the UI. Like the GL path it repaints the
whole target each frame (`JSUI_GL_NO_CLEAR` draws over it). Desktop Metal
hosts use Graphite instead (`jsui_gpu_create_metal`, previous section). See
`integrations/ios/README.md`.

### Direct3D 12 / 11 (Skia Ganesh, Windows)

`jsui_gpu_create_d3d12(device, queue)` / `jsui_gpu_create_d3d11(device)`
with `jsui_render_gpu_target` (`src/render/gpu_d3d.cpp`, CMake option
`JSUI_D3D`) draw with Skia **Ganesh** on Direct3D 12 (Graphite has no
Direct3D backend) for D3D hosts: Unreal and Godot on D3D12, Unity on D3D11
or D3D12. `JsuiGpuTarget.texture` is an `ID3D12Resource*` /
`ID3D11Texture2D*`, `format` a `DXGI_FORMAT` (0: the resource's own),
`layout` / `final_layout` D3D12 resource states.

* **D3D12: the host's device and queue.** Ganesh runs on the host's
  `ID3D12Device` and DIRECT queue (adapter found by the device's LUID).
  The target is wrapped per frame in its entry state; Ganesh records and
  tracks its own transitions but cannot end in a state of the caller's
  choosing (its flush ignores a new `MutableTextureState` on D3D), so after
  Skia's submit the render executes one command list of its own on the
  queue with the transition from the state Skia left it in to
  `final_layout`, and those returning sampled host textures to their home
  state (`JsuiTexture.usage`, 0: `PIXEL_SHADER_RESOURCE`; Godot's mirror
  textures live in `COPY_DEST`). No CPU wait; its lists are recycled on a
  fence of its own.
* **Frames.** With `JSUI_GPU_INCREMENTAL` the target is the retained frame
  (as on Graphite); without it small damage goes into a retained frame of
  the context's, drawn into the target whole, as in the GL backend.
* **D3D11: a D3D12 device beside the host's.** The context creates a D3D12
  device on the D3D11 device's adapter and its own queue. Each runtime
  draws into a shared texture (D3D12 `HEAP_FLAG_SHARED` +
  `ALLOW_SIMULTANEOUS_ACCESS`, opened with `OpenSharedResource1`), which is
  also its retained frame, ordered by one shared fence (D3D11 signal ->
  D3D12 wait, Skia's work, after which the texture decays to `COMMON`, D3D12 signal
  -> D3D11 wait) and copied into the host's texture on the immediate
  context (`CopySubresourceRegion`; just the damage with
  `JSUI_GPU_INCREMENTAL`). The host texture needs no special flags.
* **Build.** `scripts/build-skia-desktop.sh` on Windows builds m157 with
  `skia_use_direct3d`; `JSUI_D3D` then defaults on and links `d3d12`,
  `d3d11`, `dxgi`, `dxguid`.
* **Tests.** `tests/native/d3d_test.cpp` uses WARP (no GPU needed): D3D12
  incremental and retained-frame frames compared with the CPU raster, a
  host texture kept in `COPY_DEST`, and the D3D11 path on a WARP D3D11
  device (incremental, full, `JSUI_GPU_NO_CLEAR`). With the D3D12 debug
  layer installed it also fails on debug-layer errors.
  `JSUI_D3D_TEST_REQUIRE=1` (the Windows CI job) turns skips into failures.

### Dawn native (Skia Graphite on a host's Dawn)

`jsui_gpu_create_dawn_procs(wgpu_get_proc_address, instance, device, queue)`
(and the older `jsui_gpu_create_dawn(instance, device, queue)`) with
`jsui_render_gpu_target` (`src/render/gpu_graphite.cpp`, CMake option
`JSUI_GRAPHITE_DAWN`) draw with Graphite's Dawn backend on a host's native
Dawn device into a `WGPUTexture` it owns. The godot-aw3 fork's `webgpu`
rendering driver is the host this exists for (integrations/godot/README.md,
"WebGPU (Dawn) on the godot-aw3 fork", which also has the `WebGPUInterop`
singleton contract).

* **One binary, no Dawn linked.** `scripts/build-skia-desktop.sh` always
  builds Graphite's Dawn backend next to Vulkan / Metal. It is header-only:
  `webgpu.h` / `webgpu_cpp.h` are generated from Skia's own Dawn checkout at
  the revision m157 pins (`DAWN_REVISION`,
  `scripts/patches/skia-m157-dawn-header-only.patch`) and installed into
  `third_party/skia-desktop/dawn/`. `tools/dawn-shim/gen_dawn_shim.py` turns
  every function of that `webgpu.h` into a hidden trampoline that calls
  through one slot of a process-wide table. Nothing defines a real `wgpu*`
  function (the build script checks the archives), so the same library loads
  in hosts without Dawn and chooses Vulkan, Metal or Dawn at run time.
* **Filling the table.** `jsui_gpu_create_dawn_procs` asks the host's
  `wgpuGetProcAddress` for each name (explicit-length `WGPUStringView`);
  `jsui_gpu_create_dawn` uses `dlsym(RTLD_DEFAULT)`, which needs a host
  binary that exports every `wgpu*` symbol and does not work on Windows. The
  table is replaced only when every entry point resolved. Otherwise the call
  returns NULL before any `wgpu*` call. The table is process-wide, so all
  Dawn contexts of a process share the host's Dawn.
* **Same revision or nothing.** `jsui_dawn_revision()` returns the Dawn commit
  the headers came from (`<skia>/dawn/REVISION`, compiled into the shim).
  `webgpu.h`'s structs and enums change between Dawn revisions, so a host
  must link exactly that Dawn. The Godot extension compares it with the
  fork's `WebGPUInterop.get_dawn_revision()` and uses the CPU renderer on a
  mismatch.
* **Ordering.** Everything is submitted to the host's `WGPUQueue`, so queue
  order sequences it with the host's work and Dawn tracks the texture's
  usage. Graphite processes events on the host's `WGPUInstance` while it
  waits (context teardown).
* **Tests.** `tests/native/dawn_procs_test.cpp` fills the table from a fake
  `wgpuGetProcAddress`: every entry point is looked up by name and a
  trampoline calls through. A missing entry point gives NULL and leaves the
  table unchanged, and `jsui_dawn_revision()` is checked. Rendering on a real
  Dawn device is verified on the fork only (no Dawn runtime in CI).

### Pipeline cache (Graphite)

Graphite creates a GPU pipeline the first time a frame needs one, inside
that frame's submit: on a cold driver a list's first scroll or a panel's
first entrance stalls for tens of milliseconds. Every Graphite context
(Vulkan, Metal, Dawn native, browser WebGPU) has a
`pipeline_cache::Precompiler` (`src/render/pipeline_cache.*`;
`jsui_gpu_set_pipeline_cache` / `jsui_gpu_precompile_pipelines` in jsui.h):

* **Recording.** `ContextOptions::fPipelineCachingCallback` reports each
  pipeline created on demand with its serialized key; new keys (de-duplicated
  by a 64-bit hash) are appended to the cache file as one record each, in a
  single `write` (`O_APPEND`), so a crash leaves at most a torn last record.
* **Loading.** The file's header names the format version, the Skia build
  and the backend; another one makes the file stale and it starts afresh. A
  torn or corrupt record ends the file there. Each case logs one line, and
  the file is rewritten (temp file + rename) without the bad part. A file
  without the magic is not touched.
* **Precompiling off the frame.** Queued keys go to
  `PrecompileContext::precompile`: on a worker thread on Vulkan and Metal
  (thread-safe devices; the worker only creates pipelines, Graphite locks its
  shared caches) and on Dawn devices with `ImplicitDeviceSynchronization`
  (Dawn locks the device). Other Dawn devices and the browser are not thread
  safe, so they precompile for ~2 ms after a render (at most one slice per
  12 ms: once per frame, however many surfaces share the context), or when
  the host calls `jsui_gpu_precompile_step`. A frame that needs a pipeline
  still queued compiles it as usual.
* **Keys from other GPUs.** Graphite decodes a key with range checks only,
  and on Vulkan a render pass whose attachment format the device lacks is
  undefined behaviour. So each key is decoded first and fitted to the
  device (`fitKey`): depth/stencil attachments get the format this device's
  Graphite picks for the same aspects (D24_S8 on lavapipe, D32F_S8 on AMD),
  a dst read strategy the device does not use becomes its own, and a key with
  an attachment the device cannot render at its sample count is skipped.
  That is what makes a list shipped with an app (the Godot demo's,
  recorded on lavapipe by `integrations/godot/record-pipelines.sh`) safe on
  other GPUs.
* **Which pipelines depends on the frame rate.** The raster cache and
  opacity layers keep a subtree's texture, or not, by frames (painter.cpp,
  updateRasterCache), so the same UI draws other things at another frame
  rate: at 60 fps the Godot demo's entrance and first scroll render a
  subtree that holds a path into an MSAA texture of its own (a fresh render
  pass, where at lavapipe's ~8 fps it is drawn into the retained frame) and
  draw a raster-cached row under the list's rounded clip: nine pipelines
  that the slower run never needs. A list is therefore recorded on fixed UI
  clocks (JsuiView `fixed_fps`: each frame advances the runtime's clock by
  1/N s, the frames a machine at N fps draws, on any machine), at several
  rates. `JSUI_LOG_PIPELINES=compiled` logs each pipeline a frame compiles,
  `new` (not in the cache) or `known` (still queued for the precompile).
* **Tests.** `tests/native/pipeline_cache_test.cpp` covers the format and
  file repair. `vulkan_test.cpp` records a context's pipelines, then checks
  that the next context precompiles them (worker thread, and sliced) and
  that the same frames compile nothing, also from a list whose depth formats
  were swapped as another vendor's would be. The Godot job's Vulkan run
  records a cache, a run of the list's first scroll on a 60 fps UI clock
  adds to it, and the same run again must compile no pipeline while
  scrolling.

## Game data

Four channels connect the game and the UI (`include/jsui/jsui.h`):

| | Game → UI | UI → game | Use for |
| --- | --- | --- | --- |
| **Shared state** | `jsui_set_state` / `jsui_patch_state` → `useHostState(key)` | `HostState.set` / `patch` → `on_state` | anything the UI shows: currency, inventory, quests, settings |
| **Frame data** | `jsui_set_buffer` / `jsui_map_buffer` → `useHostBuffer*`, `Animated.hostBuffer` | | numbers that change every frame: enemy positions for nameplates and markers, minimap blips, cooldowns |
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
import { Host, useHostState } from '@orktes/jsui-react';

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
  `__jsui_onCall(id, name, json)`; `@orktes/jsui-react` runs the `Host.handle(name, fn)`
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

### Frame data

Shared state is JSON: the game writes text, the runtime parses it, JS parses
it again, and React reconciles what changed. That is fine for data that
changes now and then, but a list of 30 enemy positions pushed every frame
pays all of it every frame. Frame data buffers (`src/host_buffers.*`,
`packages/react/src/hostBuffer.js`) are named typed arrays instead:

* The game copies numbers in (`jsui_set_buffer(rt, key, JSUI_BUFFER_F32,
  data, count)`, a no-op when the bytes are unchanged) or writes them in
  place (`jsui_map_buffer` + `jsui_commit_buffer`). Element types: f32, f64,
  i32, u32, i16, u16, i8, u8. The layout (e.g. x, y, hp per enemy) is a
  convention between game and UI. Buffers belong to the runtime like shared
  state (set any time, survive reloads, shared by all surfaces).
* JS sees a buffer as a typed array over the runtime's storage: an
  ArrayBuffer created with QuickJS's `JS_NewArrayBuffer` on the host memory
  (no copy, no free function), one per key and reused while the storage
  stays put. Storage grows geometrically and only moves when a buffer
  outgrows its capacity or is removed; the ArrayBuffer is detached first
  (`HostBuffers::onRelease` → `JsEngine::releaseBuffer`), so a kept view
  reads as empty rather than freed memory. `HostBuffer.get(key)` caches the
  typed array by version.
* Delivery: changed keys reach JS once per `jsui_tick` (right after shared
  state) as `__jsui_onHostBuffer(keys)`, notifying `HostBuffer.subscribe`
  callbacks and hooks in one discrete-priority batch. `useHostBuffer(key)`
  re-renders on every change; `useHostBufferSelector(key, selector)` only
  when the selected value changes (the number of enemies), and
  `useHostBufferListener(key, cb)` never.
* `Animated.hostBuffer(key, index)` (`useHostBufferValue`) is an Animated
  node that is native from the start: a `hostBuffer` node in
  `src/animated/native_animated.cpp` reads element `index` (0 when out of
  range or unset) and is marked dirty by `HostBuffers::onChange`, so the
  views using it (transforms, opacity, colors through `interpolate()`, math
  nodes) are written by the runtime in the next `jsui_tick` or
  `jsui_tick_native`, with no JS and no React render. A nameplate per enemy
  re-renders only when the enemy count changes. Without the native module it
  follows `HostBuffer.subscribe` on the JS driver.
* Hosts: the Godot `JsuiView.set_buffer(key, packed_array)` takes
  `PackedFloat32Array` / `PackedFloat64Array` / `PackedInt32Array` /
  `PackedByteArray` and `PackedVector2Array` / `PackedVector3Array` /
  `PackedColorArray` (their components); the web host has
  `setBuffer(key, typedArray)` and zero-copy `mapBuffer(key, Float32Array,
  count)` + `commitBuffer(key)`.

`tools/jsui-bench` compares the three ways at 30, 100 and 1000 moving
markers (`markers-json-*`, `markers-buffer-*`, `markers-native-*`, see
"Performance"). Per frame on an M1 Ultra (Release, painting off, mean µs):

| markers | JSON state + React | buffer + React | buffer + `Animated.hostBuffer` |
| --- | --- | --- | --- |
| 30 | 911 | 861 | 54 |
| 100 | 2965 | 2815 | 192 |
| 1000 | 29527 | 27601 | 2709 |

Dropping JSON alone saves the serialization and parsing (~5%); the rest is
React rendering every marker and the bridge's setProps each frame. Driving
the views natively removes both (JS ~15 µs at 1000 markers); what is left is
the native Animated evaluation and style writes (~2.4 µs per marker with two
animated transforms).

### Host textures

Pictures the host makes itself are shown inside the UI by name: a video the
engine decodes (Godot's `VideoStreamPlayer` texture, WebCodecs
`VideoFrame`s, a `<video>` element), a 3D render target (a character
preview, a minimap camera), a webcam. JSUI decodes and plays nothing; it
samples textures (`src/render/host_textures.*`,
`packages/react/src/hostTexture.js`):

```jsx
<HostTexture name="trailer" resizeMode="contain" style={{ width: 640, aspectRatio: 16 / 9, borderRadius: 12 }} />
const size = useHostTexture('trailer');   // { width, height } once set, else null
```

* `HostTexture` is an `Image` host node with a `texture` prop, so it lays out
  and paints like an image: intrinsic size = the texture's px / `scale`,
  `resizeMode`, rounded clipping, `tintColor`, opacity and transforms. It
  is never raster-cached as a sprite and is sampled without mipmaps (new
  contents every frame).
* A texture is CPU pixels (`jsui_set_texture_pixels`: copied into a raster
  `SkImage`, so every renderer shows it, the CPU raster included; GPU
  backends upload it like any image) or a GPU texture of the device the
  runtime renders with (`jsui_set_texture` with a `JsuiTexture`: a GL
  texture name, `WGPUTexture`, `VkImage`, `id<MTLTexture>` or
  `ID3D12Resource*`), sampled in
  place with no copy. The description stays the host's: each GPU backend
  wraps it as an `SkImage` of its own device inside its render
  (`TextureWrapper`, made current by a thread-local `TextureScope` around
  the frame, like `LayerCacheScope`): `SkImages::BorrowTextureFrom` on
  Ganesh GL / Metal / D3D12, `SkImages::WrapTexture` on Graphite (Vulkan in
  `SHADER_READ_ONLY_OPTIMAL`, Metal, Dawn / browser WebGPU). The painter
  only asks `HostTextures::image()`, so it stays backend independent. A
  canvas that cannot sample a kind (the CPU raster for a GPU texture, a GL
  texture on Graphite) draws nothing and logs one warning per texture.
  Wraps are cached per texture until its contents change.
* Repaint: Image nodes showing a texture register with the registry while
  their `NodeCache` lives. `jsui_texture_updated` (the host drew the next
  video frame into the same texture) and `jsui_set_texture_pixels` call
  `markPaintDirty()` on just those nodes: the damage is their rects, no
  layout, no JS. A new size also re-measures them, and textures set,
  resized or removed reach JS once per `jsui_tick` as
  `__jsui_onHostTexture(keys)` (`useHostTexture`, `HostTextures.subscribe`).
  Setting an identical description is a no-op.
* Origins: GPU textures with `JSUI_TEXTURE_BOTTOM_LEFT_ORIGIN` (GL render
  targets) are flipped where they are drawn; CPU pixels are flipped when
  copied.
* Hosts: the web host uploads `VideoFrame` / `<video>` / canvas /
  `ImageBitmap` sources with the renderer in use (WebGPU
  `copyExternalImageToTexture`, WebGL `texImage2D`, the CPU raster through
  `getImageData`); the Godot `JsuiView.set_texture(name, texture, live)`
  passes the RenderingDevice texture behind any `Texture2D` (or its pixels
  on the CPU path).


### Video

`<Video>` (`packages/react/src/video.js`, react-native-video's API; the CLI
aliases `react-native-video` to it) is a host texture with a player behind
it. JSUI still decodes nothing: the runtime (`src/video/video.*`,
`src/js/bridge_video.cpp`) hands the view's requests to the host's video
handler, `JsuiConfig.on_video`, which plays the video into the view's
texture (`textureName`, default one per view) like any host texture:

* Commands (`JsuiVideoCommand`): `OPEN` (video id, the `source`
  alternatives as absolute URLs with their MIME types, the texture key, and
  the whole playback state: paused, muted, volume, loop, rate), `SET` (the
  playback state again whenever a prop changes), `SEEK`, `CLOSE` (unmount,
  a new source, and every open video when the JS context goes away; the
  runtime then removes the texture). The host plays the first source it can
  (the browser: `canPlayType`; AVFoundation: `isPlayableExtendedMIMEType`).
* Reports: `jsui_video_status(state, duration, position, buffered, size,
  error)` whenever something changed, as often as every frame. JS hears state
  changes at once and the position at the view's `progressUpdateInterval`
  (`onLoad`, `onProgress`, `onBuffer`, `onPlaybackStateChanged`, `onEnd`,
  `onError`, `onReadyForDisplay`); every report also goes to host buffer
  `<texture>.time` (`[position / duration, buffered / duration, position]`)
  for `Animated.hostBuffer` progress bars that run without JS.
* Hosts: the browser host (`web/jsui-textures.js` `JsuiVideo`: a `<video>`
  per video, `bindVideo` to its texture; in worker mode the page plays it
  for the worker's runtime), jsui-viewer on macOS (`tools/jsui-viewer/video.mm`:
  AVFoundation, frames blitted into a Metal texture on the Graphite queue, or
  uploaded as pixels on the other renderers). Without a handler every video
  fails (`onError`). Other `HostTexture` views may show the same texture by
  name (the showcase's cinematic screen samples one video five times).

### Lottie (LottieView)

Lottie (Bodymovin JSON) animations play natively with Skia's Skottie
module (`src/render/lottie.*`, `packages/react/src/lottie.js`). The API is
lottie-react-native's; the CLI aliases `lottie-react-native` to it:

```jsx
import LottieView from 'lottie-react-native';
<LottieView source={require('./victory.json')} autoPlay loop style={{ width: 200, height: 200 }} />
<LottieView source={{ uri: 'https://example.com/a.json' }} progress={animatedValue} colorFilters={[{ keypath: '**.Fill 1', color: '#f00' }]} />
ref.current.play(30, 60); ref.current.pause(); ref.current.resume(); ref.current.reset();
```

* `LottieView` is a host node (`NodeType::Lottie`, "LottieView"). Sources:
  `{uri}` / URI strings load through the `ResourceLoader` (files, http, data:
  URIs, the embedder's loader); inline compositions (`require('./a.json')`,
  an object or JSON text) go to native once with `lottieData(json)`, which
  returns a content-hash key the view uses as its source. `LottieCache` (one
  per runtime) parses a composition once per source and colorFilters set and
  shares it between views (each paint seeks it, then renders).
* Playback runs on the runtime clock: `LottieCache::tick()` (in `jsui_tick`
  and `jsui_tick_native`, before layout) advances playing views and calls
  `markPaintDirty()` on just those nodes, so a playing animation repaints its
  own rect and runs no JS. `autoPlay` starts it once the source loads;
  `loop` (default true), `speed` (negative plays backwards) and segments
  (`play(startFrame, endFrame)`, end < start plays down) follow
  lottie-react-native. Calls made before the load apply when it loads.
  `wantsFrame` is true only while a view plays.
* `progress` (0–1) shows that frame instead of playing. It travels as the
  paint-only style key `lottieProgress` (its own style group, no relayout),
  so an `Animated` value drives it like any animated style, on the JS or the
  native driver; `createAnimatedComponent(LottieView)` maps an animated
  `progress` prop to it (`Component.__animatedStyleProps`).
* Without a size the view takes the composition's size (keeping its aspect
  ratio when one side is set); `resizeMode` contain (default), cover, center
  or stretch place it in the padding box, clipped to rounded corners.
* `colorFilters`: `[{keypath, color}]` override solid fills and strokes
  (Skottie `PropertyObserver` color handles, re-applied after every seek).
  A keypath's dot-separated names ("*" one, "**" any) must match, in order,
  names along a property's path (layers, groups, the fill / stroke itself);
  names in between may be skipped, so `"Layer"` recolors a whole layer.
  Gradients are not color properties.
* Events: `load` (onAnimationLoaded), `error` (onAnimationFailure, also for
  JSON that is not a composition) and `animationfinish` (onAnimationFinish,
  `isCancelled` when `play` / `reset` interrupt a non-looping segment).
* Embedded images (data: URIs) and fonts registered with the runtime work;
  external image assets, expressions and dotLottie (.lottie) archives do not.
* Build: `JSUI_LOTTIE` (default on when the Skia build has `libskottie`;
  every `scripts/build-skia-*.sh` and the aseprite prebuilt do). It adds
  about 360 KB to `jsui.wasm` (~90 KB brotli; ~136 KB brotli on the m157
  WebGPU build). Off, sources fail with an error and nothing is linked.

### Shaders (ShaderView)

Your own SkSL (Skia runtime effects, https://skia.org/docs/user/sksl/)
paints a view, on every renderer (the CPU raster runs SkSL too):

```jsx
<ShaderView source={sksl`...`} uniforms={{ glow: anim, tint: '#7cf', center: [x, y] }}
            images={{ image: require('./card.png'), video: { texture: 'trailer', fit: 'contain' } }}
            paused={false} onError={(e) => console.warn(e.nativeEvent.error)} style={...}>
  {children}
</ShaderView>
```

* `ShaderView` is a host node (`NodeType::Shader`, render/shader_view.h).
  `ShaderEffects` (on the Runtime, `Document::shaders`) compiles each
  distinct source once (`SkRuntimeEffect::MakeForShader`) and shares it.
* Uniforms, per frame: style key `uniform:<name>` (whole value), then the
  `uniforms` prop, then the built-ins `time` (float, seconds since the view
  mounted, frozen while `paused`), `resolution` (float2, px) and
  `pixelRatio`; `uniform:<name>.<i>` style keys override single components.
  Values flatten numbers / arrays / booleans / colors (unpremultiplied
  r, g, b, a in 0..1; `layout(color)` uniforms are converted to the working
  color space by Skia); int uniforms round. The `uniform:` keys are a
  paint-only style group (`kGroupShader`), so Animated (JS or native
  driver, `Animated.hostBuffer`) writes them through patchStyle with no
  relayout; @orktes/jsui-react's ShaderView turns Animated uniforms into
  them (one key per animated component of an array).
* Clock: a view whose shader declares `uniform float time` and does not
  get it from the app is repainted by `ShaderEffects::tick` every tick
  (`wantsFrame`), like a playing LottieView; only its own rect is damaged.
* Children (`uniform shader name;`): `images[name]` (`{uri}`, `{texture}`
  for a host texture, `fit`: cover / contain / stretch / center / none /
  repeat, `tile`: clamp / repeat / mirror / decal), mapped onto the view
  like an Image so `name.eval(p)` samples it at view position `p`. Image
  loads repaint through `ImageCache::generation`; host textures register
  the view as a user (new frames repaint it). Missing children are
  transparent.
* Fill (default): drawn into the padding box after the background and
  borders, before children; coordinates start at the padding box's
  top-left. Content: a shader declaring `uniform shader content` (not given
  in `images`) runs as an `SkImageFilters::RuntimeShader` on the view's
  layer, so its background and children are its input (coordinates from the
  border box, clipped to its rounded border box, sample radius = the whole
  view). A change anywhere in that subtree damages the whole view.
* Errors: a source that fails to compile paints nothing; the view gets one
  `error` event (`onError`, `nativeEvent.error` = Skia's message).
  `__jsui_native.shaderInfo(src)` compiles synchronously and returns
  `{error}` or `{uniforms: [{name, type, count, color}], children}`.
* Preloading: a GPU compiles a shader's pipelines the first time a frame
  draws it (cold: 130-250 ms per runtime-effect pipeline on WebGPU / WebGL
  on a Mac Studio). `ShaderEffects::preload` (JS `precompileShaders()` /
  `__jsui_native.shaderPreload(src, images?)`, C `jsui_precompile_shader`)
  compiles the source and queues warm-up draws: one per pipeline-key
  variant a view may produce (aligned / AA rect, rect / rounded own clip,
  with / without a parent's rounded clip (AnalyticClip), and each
  combination of loaded / not-yet-loaded image children; `content` shaders
  as the layer's image filter). Frames painted on a GPU
  (`Surface::finishCanvasTracked` / `renderCanvas`) draw one queued draw
  each into a small off-screen surface of the target's format, so each
  frame builds at most one new pipeline. Views place the shader by
  translating the canvas, never with a local matrix (part of the key, which
  the warm-up's draws would not match). A queued warm-up keeps
  `needsRedraw()` true; a raster frame drops the queue (the CPU has nothing
  to warm). `__jsui_native.shadersWarming()` counts the queued draws.
  vulkan_test checks that mounting preloaded views compiles no pipeline;
  site/test/first-visit.mjs measures a screen's first-visit hitch (Rift,
  cold Metal cache: WebGPU 801 → 150 ms, WebGL 517 → 100 ms; what remains
  is ordinary image / clip pipelines).
* Cost: SkSL compilation and SkRuntimeEffect were already linked (Skottie
  and the GPU backends use them), so ShaderView adds almost nothing to
  `jsui.wasm`.

### Vector drawing (Svg, Canvas)

`react-native-svg` and `@shopify/react-native-skia` work unchanged (the CLI
aliases both, plus `react-native-svg/css`, to `@orktes/jsui-react/svg` and
`@orktes/jsui-react/skia`). Both are one host node, `VectorView`
(`NodeType::Vector`, render/vector_view.h), per `<Svg>` / `<Canvas>`:

* **JS side** (packages/react/src/vector/host.js): inside a VectorView the
  reconciler switches to a drawing host context. `<Circle>`, `<G>`,
  `<LinearGradient>`, `<Blur>`... create `DrawNode`s (strings: `DrawText`)
  that never reach the bridge; custom components, hooks, keys and
  conditional rendering inside work as anywhere else. A commit that touched
  any of them marks the owning `VectorHost` dirty and `resetAfterCommit`
  serializes its subtree once (`registerSerializer('svg' | 'skia')`, in
  svg.js / skia.js) into the `commands` prop: one `setProps` per changed
  view per commit. SVG references (`url(#id)` gradients with `href`
  inheritance, `clipPath`, `mask`, `<Use>` / `<Symbol>`, percentages
  against the viewBox, string / array / prop transforms) are resolved there;
  Skia's effect children (shaders, mask / image / color filters, path
  effects, extra `<Paint>`s, `layer`, `<Mask>`) are folded into their
  parent's paint.
* **Native side**: `vector::propsChanged` parses `commands` into an element
  tree (paths from SVG strings via `SkParsePath`) cached in the node's
  `imageCache`, re-parsed only when the value differs. `vector::paint`
  walks it into the content box (clipped like `overflow: hidden`) with
  core Skia only (`SkPath`, `SkPaint`, `SkShaders`, `SkImageFilters`, path
  effects incl. `SkTrimPathEffect`; no SVG / Paragraph module), so every
  backend and the CPU raster draw it the same. Text runs use the font
  collection (`drawSimpleText`, `textAnchor`, `letterSpacing`), images the
  `ImageCache`, `<Shader>` the `ShaderEffects` sources of ShaderView. It
  builds against the aseprite m124 prebuilt and m157 (gradient / dash APIs
  switch on `__has_include(SkGradient.h)`).
* **Semantics** follow the mode: `svg` inherits fill / stroke / font
  props, `opacity` on a group is one layer, rotations are degrees,
  `currentColor` comes from `color`; `skia` inherits the paint (`color`,
  `style`, `strokeWidth`, `blendMode`...), `opacity` multiplies, rotations
  are radians, `origin` pivots transforms.
* **Animated**: a drawing element given Animated values (directly, in its
  `transform`, or via `Animated.createAnimatedComponent(Circle)`) gets a key
  `k`; the values are style keys `draw:<k>.<prop>` on the VectorView, a
  paint-only style group (`kGroupDraw`). The JS driver writes them with
  `patchStyle`, the native driver connects its props node to the
  VectorView, so a running animation neither renders React nor re-parses
  commands; only the view's rect is damaged. Animated values nested deeper
  (uniform objects, gradient colors) re-render the element (JS driver).
* **Not supported** (accepted, not drawn; most warn once): SVG filters
  (`<Filter>`, `fe*`), `<Pattern>`, `<Marker>`, `<ForeignObject>`,
  per-shape touch events (`<TextPath>` draws its text without the path);
  Skia `Paragraph`, `Glyphs`, `TextPath`, `TextBlob`, `Picture`, `Atlas`,
  `Vertices`, `Patch`, `Box` / `BoxShadow`, backdrop filters, path ops,
  `makeImageSnapshot`, Reanimated shared values.
  `Skia.Font` measures approximately (the real font is matched natively).
* Cost: about +38 KB of native code (`jsui-render` text, Release).
  Tests: tests/native/vector_test.cpp, react_vector_e2e_test.cpp,
  packages/react/test/vector.test.js, reference case
  examples/features/svg.jsx (Chromium parity).

### Audio

The UI asks for sounds; the host plays them (`src/audio/`,
`packages/react/src/sound.js`, full contract in [AUDIO.md](AUDIO.md)).
`Sound.play` / `useSound` / `useMusic` / `UISounds` (interaction sounds on
hover, focus moves and presses) turn into `__jsui_native.audioPlay` /
`audioVoice` / `audioBus` / `audioStopAll` / `audioPreload`, which the
runtime hands to `JsuiConfig.on_audio` as `JsuiAudioCommand`s, so UI
sounds go through the game's own mixer and buses. Hosts without a handler
get a default: the built-in miniaudio backend (`JSUI_AUDIO`, on by default on
desktop), Web Audio in the browser (`web/jsui-audio.js`), or
`AudioStreamPlayer`s on Godot's buses. Hosts report voice ends and loads
back with `jsui_audio_voice_ended` / `jsui_audio_sound_loaded`, delivered to
JS as `__jsui_onAudio`. `JsuiConfig.audio_disabled` turns it off (headless tools, tests).

## JS bridge contract

Native globals (installed by `JsEngine`):

| Global | Meaning |
| --- | --- |
| `__jsui_native.rootId` | id of the root container node |
| `__jsui_native.devMode` | `true` when the runtime runs with `cfg.dev_mode` |
| `createNode(type, rootTag?) → id` | `type` ∈ `View`, `Text`, `RawText`, `Image`, `ScrollView`, `TextInput`, `LottieView`, `ShaderView`, `VectorView`, `JSUIContainer`; created in the document of surface `rootTag` (default: the main one; see "Surfaces"). The id is taken at once; the node exists natively when the command is applied (see "Command buffer") |
| `viewport(rootTag?) → {width, height, scale}` | logical size of a surface (default: the main one) |
| `surfaceApp(rootTag) → {appKey, initialProps} \| null` | the app the host picked for a surface |
| `updateProps(id, reactProps, hidden, keepStyle?)` / `getProps(id)` | the host config's prop path, done natively: builds what native sees from the React props (event handlers → `__events`, the responder hints `__blocksNative` / `__keepsGesture` and `__passive` (see "Input routing details"), flattened style, `pointerEvents` / hidden into it) and diffs it in `Node::setProps` (see "Performance: Props"); `getProps` returns what native holds (the inspector reads it). `keepStyle` (`setNativeProps` of non-style props; `updatePropsKeepsStyle: true` says the runtime has it): `reactProps.style` is ignored and the node keeps the style it has when the command is applied, `patchStyle` and native Animated writes included. The host JS engine has neither (`@orktes/jsui-react` then diffs in JS and calls `setProps`) |
| `setProps(id, props)` | replaces all props; `props.style` is a **flattened** style object; functions are dropped; `props.__events` lists event names the element listens to (e.g. `["press","pointerMove","layout"]`) |
| `patchStyle(id, partialStyle)` | animation fast path: merges keys into the node's current `props.style` (a `null`/`undefined` value removes the key) and keeps all other props (`Node::patchStyle`). When only paint keys such as `transform`, `opacity`, colors changed, the style is updated in place and only those keys' style groups are re-resolved, with no relayout; a layout key takes the `setProps` path. Use it per frame from `requestAnimationFrame` instead of re-rendering through React. |
| `setText(id, string)` | content of a `RawText` |
| `appendChild(p, c)`, `insertBefore(p, c, before)`, `removeChild(p, c)` | tree edits (React semantics; unknown ids and cycles are ignored). While a LayoutAnimation with `delete` is armed, `removeChild` keeps the child as a fading ghost (see "LayoutAnimation") |
| `animated.*` | the native Animated driver (React Native's NativeAnimatedModule): `createNode(tag, config)`, `connect` / `disconnect(parent, child)`, `connectToView` / `disconnectFromView(propsTag, id)`, `dropNode`, `setValue` / `setOffset` / `flattenOffset` / `extractOffset` / `getValue(tag)`, `startAnimation(id, tag, config)`, `stopAnimation(id) → {velocity} \| null`, `addEventMapping(id, eventType, path, tag)` / `removeEventMapping`, `startListening` / `stopListening(tag)`. See "Animated" |
| `configureLayoutAnimation(config)` | arms a LayoutAnimation (`{duration, create, update, delete}`) for the layout pass after the commit in progress; `@orktes/jsui-react` calls it from `prepareForCommit` after `LayoutAnimation.configureNext` |
| `destroyNode(id)` | free a detached subtree |
| `measure(id) → {x, y, width, height, pageX, pageY}` | layout of a node (synchronous; runs a pending layout first). `null` for unknown ids. In JS-thread mode: the last frame's layout (see "Shared state") |
| `measureAsync(id, callback)` / `asyncMeasure` | React Native's measure: `callback({x, y, width, height, pageX, pageY} \| null)` after the layout that follows the commit in progress (a `Measure` command, answered by `Runtime::answerMeasures` with the onLayout events). `asyncMeasure` is `true` in JS-thread mode, where `HostInstance.measure*` and `UIManager.measure*` use it |
| `focus(id)`, `blur(id)` | `InputManager::focus` / `blur` |
| `scrollTo(id, x, y, animated)` | `InputManager::scrollTo` (clamped, dispatches `scroll`). `animated`: eases there on the runtime clock; otherwise immediate (and stops a running fling) |
| `scrollToEnd(id, contentId, horizontal, animated)` | scrolls to the content container's laid-out size minus the ScrollView's on the scroll axis (`contentId` 0: the ScrollView itself), measured when the command is applied; `HostInstance.scrollToEnd` uses it when present |
| `flashScrollIndicators(id)` | shows a ScrollView's overlay indicators for ~1 s |
| `lottieData(json) → key` | hands an inline Lottie composition to native once; LottieViews use the key (content hash) as their `source`. `lottie`: `true` when the build plays Lottie |
| `lottieCommand(id, cmd, a, b) → bool` | LottieView `play` (frames `a`–`b`, undefined: the whole composition) / `pause` / `resume` / `reset` |
| `containerCommand(id, cmd) → bool` | a JSUIContainer's commands to its child runtime: `{op: 'load', data}`, `post`, `call`, `resolve`, `reject`, `state`, `reload` (see "Nested runtimes") |
| `shaderInfo(sksl) → {error} \| {uniforms, children}` | compiles a ShaderView source (shared with the views using it) |
| `shaderPreload(sksl, images?) → like shaderInfo` / `shadersWarming() → n` | compiles it and warms its GPU pipelines in the next frame (see "Shaders") / warm-ups still queued |
| `postMessage(name, json)` | to the embedder's `on_message`. A string is passed through as the JSON text; any other value is `JSON.stringify`-ed |
| `getState(key) → json \| undefined`, `stateVersion(key) → number`, `stateKeys() → [key]` | shared host state (see "Game data"); version 0 = unset |
| `statePatches(key, sinceVersion) → [json] \| undefined` | the merge patches (JSON texts) that took `key` from `sinceVersion` to its current version, oldest first; `undefined` when the bounded log (64 patches / 64 KB since the last `set`) cannot cover the gap. `@orktes/jsui-react` applies them to its cached value instead of re-parsing the whole state after each `jsui_patch_state` |
| `setState(key, json \| undefined) → version`, `patchState(key, json) → version` | UI-owned writes (`undefined` removes); report to `on_state`; invalid JSON throws `SyntaxError` |
| `hostCall(name, json, settle) → id` | `on_call`; `settle(ok, payload)` runs on `jsui_resolve_call` (JSON text) / `jsui_reject_call` (message). Returns 0 without `on_call` |
| `cancelCall(id) → bool`, `callResult(id, ok, payload)` | drop an unanswered call (`on_call_cancel`); the result of a `__jsui_onCall` |
| `buffer(key) → [ArrayBuffer, type, count, version]`, `bufferVersion(key)`, `bufferKeys()` | frame data buffers (see "Frame data") |
| `texture(key) → [width, height, version]`, `textureKeys()` | host texture sizes for layout (see "Host textures"); pixels never reach JS |
| `audioPlay(url, bus, volume, pitch, pan, loop, fadeMs, offsetS) → voice`, `audioVoice(op, voice, …)`, `audioBus(bus, volume, muted, fadeMs)`, `audioStopAll(bus, fadeMs)`, `audioPreload` / `audioUnload` / `audioResolve(url)` | UI audio requests (see "Audio") |
| `loadFont(family, url, weight, italic) → Promise<true>` | load via the resource loader (URL relative to the bundle), register with the FontManager, relayout text; rejects with an `Error` on 404 / bad font data. `''` family = the file's; weight `0` / italic `-1` = the file's (see "Fonts") |
| `loadFontData(family, bytes, weight, italic) → true` | the same from an ArrayBuffer / typed array, synchronously; throws on bad data |
| `unloadFont(family) → bool`, `getFontFamilies() → [family]` | drop a family; the registered families (built-in Inter first) |
| `setFontFallbacks([family])`, `setDefaultFontFamily(family)` | glyph fallback list; the family of text whose style names none |
| `reload()` | reload the bundle at the start of the next tick (safe from inside JS) |
| `evalScript(source, name)`, `devError(text \| null)` | dev mode: run a hot update in the global scope; show / clear an error in the overlay (see "Dev mode") |
| `textEditing` | `true`: TextInput text is edited natively (see "TextInput editing") |
| `setTextInputSelection(id, start, end)` | TextInput selection (UTF-16 indices, clamped; no event) |
| `getClipboard() → string`, `setClipboard(text)` | the embedder's clipboard (`jsui_set_clipboard_callbacks`) or the runtime's in-process one |
| `listenGlobal(type, on, keys?)` | the bundle has (no longer has) global listeners for events of `type` that may call `preventDefault()` (`addGlobalListener`: modals, `BackHandler`, gamepad hooks; passive ones are not reported); `keys` (key names / codes, gamepad button or axis indices) narrows it to those events, `[]`: none. Input asks it when JS answers an event late (see "Input routing details") |

### Command buffer

The QuickJS bridge does not change the native side while JS runs. Every
mutation call (`createNode`, `setProps` / `updateProps`, `patchStyle`,
`setText`, the tree edits, `destroyNode`, `configureLayoutAnimation`,
`focus` / `blur`, `scrollTo` / `scrollToEnd`, `flashScrollIndicators`,
`setTextInputSelection`, `lottieCommand`, `listenGlobal` and the whole
`animated.*` graph API) converts its arguments and records a command in
`JsEngine::Impl::commands` (`src/js/command_buffer.h`); `CommandBuffer::apply`
(`command_buffer.cpp`) does the native work later, in call order. With JS
on its own thread (see "JS thread") the owner thread applies what the JS
thread published; single-threaded (the default) everything behaves exactly
as when each call applied at once:

* **Publication.** A JS task (one native → JS entry with its microtask
  drain, `Impl::drain`) publishes what it recorded as one batch
  (`Impl::publish`), so a React commit is applied whole, never in parts.
  Single-threaded, the engine also applies at every **sync point**
  (`Impl::syncPoint`): whenever JS returns to native code (`call()` inside a
  batch, a microtask round, a script's evaluation, an `onLayout` handler),
  so native never looks at a tree behind JS (e.g. `InputManager` re-hit
  testing after a handler). Commands recorded during a time-sliced render
  (`createInstance` → `createNode` + `updateProps`) create detached nodes;
  only the commit's task attaches them.
* **Reads and host callbacks** apply what is recorded first: `measure`,
  `getProps` and `animated.getValue` answer from the tree JS built, and
  `postMessage`, `setState` / `patchState`, `hostCall` and the font calls
  (which re-measure text) see it too. In JS-thread mode these reads come
  from the frame snapshot instead and the callbacks are queued (see
  "Shared state").
* **Ops that answer or call back** (`CommandBuffer::appliesAtOnce`) are
  applied as soon as they are recorded, with everything before them:
  focus / blur / scroll / selection run JS event handlers inside the call,
  `lottieCommand` returns a bool, `animated.createNode` / `startAnimation`
  throw on a bad config and `stopAnimation` returns the velocity. On a JS
  thread they go with the task's batch like any other command: the focus
  events arrive as events, `lottieCommand` answers whether the command name
  is known, and Animated config errors are logged when applied.
* **Ids.** `createNode` takes the id from the runtime's atomic node counter
  (`Runtime::allocNodeId`, the counter every surface numbers its nodes with)
  and records `Create(id, type, rootTag)` (`Document::createNode(type, id)`
  when applied), so later commands of the batch can name the node first.
  Ids stay in creation order across JS and native creations (surface
  roots). Unknown ids are ignored when applied, as before.
* **Payloads.** Props, styles, text and configs are written straight from
  the JS values into one flat byte arena (`CommandBuffer::Writer`: tagged
  scalars, length-prefixed strings and keys, counted arrays, objects up to
  an end tag) instead of `Value`s; `apply` builds each command's `Value`
  right before the node takes it. Recording allocates nothing once the
  arrays have grown (they keep their capacity across batches), and the
  allocator reuses the memory each node's previous props free: building all
  `Value`s of a 2000-node commit first and freeing the old props after made
  commits 10-25% slower, while the arena makes them ~20% faster than
  applying each call at once (`markers-*-1000` in jsui-bench). A JS getter
  that calls the bridge while a payload is being read records its own
  command first (payloads are staged per nesting level).
* **Perf.** Recording and applying both count as the `Commit` phase;
  `bridgeCalls` counts recorded mutation calls as before.

### Inbound messages

The other direction: every call native makes into the bundle is an
`Inbound::Message` (`src/js/inbound.h`) posted to `Runtime::inbound`, in
the order the calls were made: input and view events (`__jsui_dispatchEvent`,
from `InputManager`, surface resizes, image / Lottie / shader events),
`jsui_post_message`, `onLayout`, the frame's timers + rAF + jobs (`Frame`),
host state / buffer / texture changes, Animated value updates and ends,
audio reports, `Host.call` settlements, `jsui_call_ui`, surface starts and
stops, dev-mode hot updates, loader completions that run JS (`fetch`,
`loadFont`: a `Task`), the bundle's evaluation and the batches that share
one microtask drain (`BeginBatch` / `EndBatch`). Single-threaded,
`post()` delivers at once, so everything behaves as when native called the
engine itself. What native needs back is a **reply**, never a return value
read in place, so the same code works once JS answers from its own thread:

* An event posted with `reply` gets a sequence number; JS's answer (the
  `DispatchFlags` of `__jsui_dispatchEvent`) is stored under it until the
  poster `take`s it (or `forget`s it once its deadline passed). Input
  decides default actions with it (see "Input routing details").
* A `Call` names what a missing global means (`Inbound::Missing`): warn
  (no `AppRegistry`), fail the `jsui_call_ui` call ("no UI handler") or
  reload (no hot-update hook).
* `Eval` replies whether the script ran without an exception.

`Inbound::replyDelayMs` is a test hook: event replies become visible that
long after they were posted, on the runtime clock, as from a busy JS thread
(`tests/native/input_test.cpp` "late JS answers", `text_input_test.cpp`).
Not routed: `releaseBuffer` (host buffers; a JS thread reads published
copies) and the host JS engine's own loader completions (it has no JS
thread). With a JS thread the inspector is polled there.

### Shared state

The third step towards a JS thread: everything JS shares with the owner
thread (the thread calling `jsui_tick`) is made safe to share, and a
**JS-thread mode** (`Runtime::setJsThreadMode`, before the first bundle
load; off by default, the C API comes with the thread itself) makes JS
reach the runtime only the ways a JS thread will (since "JS thread",
JS also runs on a thread of its own in that mode); the default mode
behaves exactly as before.

* **Locks.** `HostState` takes a (recursive) lock in every member; members
  that hand out pointers into the store (`json`, `value`, `patchesSince`)
  are used under `HostState::lock()` (the bridge's `getState`, native
  Animated `hostState` nodes). `jsui_get_state` returns a copy
  (`Runtime::stateText`). The host texture **size table**
  (`HostTextures::size` / `keys`) has its own lock; owner-side reads of the
  entries stay unlocked. `ShaderEffects` locks its program cache and warm-up
  queue (sources compile outside the lock); `LottieCache::addData` keeps
  inline JSON aside under a lock until the owner thread's `find` makes the
  entry. Loader request ids, Audio voice ids and call ids come from atomic
  counters (`ResourceLoader::reserveId`, `Audio::reserveVoice`), so JS names
  a request before the owner thread starts it.
* **Host buffers: copy-on-publish.** A JS thread cannot view storage the
  owner thread rewrites. In JS-thread mode `buffer(key)` subscribes the key
  (`HostBuffers::subscribe`) and reads the copy `HostBuffers::publish`
  made: at the start of each tick (before `__jsui_onHostBuffer`) for keys
  whose version changed, plus a proxy call the first time a key is read.
  `bufferVersion` / `bufferKeys` read the version table of the same
  publish. The ArrayBuffer holds a reference to its copy, so a view JS keeps
  keeps its frame instead of being detached when the storage moves; a copy
  no reader has taken is rewritten in place (one handed out never
  changes). Native Animated `hostBuffer` nodes keep reading the live
  storage (no lag).
* **Frame snapshot** (`src/frame_snapshot.h`). Taken on the owner thread
  after the tick's layout (and at the start of a tick whose surface table
  changed, after `jsui_tick_native`, and before a bundle evaluates) and
  swapped under a lock (`SnapshotStore`): per-surface viewport, app key and
  props, every attached node's layout (local and page rect), every Animated
  value node's raw value and every running animation's velocity, and the
  number of command batches applied before it. In JS-thread mode
  `measure`, `viewport`, `surfaceApp`, `animated.getValue` and
  `stopAnimation`'s velocity read it. A value JS set (`setValue`) reads back
  as set until a snapshot includes the batch that carried it; an animation
  JS started after the snapshot stops with `{velocity: 0}`;
  `flattenOffset` / `extractOffset` fall back to the snapshot. These answers
  are one frame old at most. Taking one allocates nothing while nothing
  changed: each table (surfaces, layouts, values, animation velocities) is
  collected into a reused buffer, compared with what was collected for the
  last snapshot and shared with it when equal; a tick where nothing changed
  publishes no snapshot, one where only native Animated moved copies the
  value table alone. Layouts are collected only once JS has made a sync
  `measure` (`Runtime::snapshotLayouts`; React's measure is async in this
  mode): that first read is a proxy read of the owner thread's tree.
* **Host-callback queue** (`src/host_queue.h`). In JS-thread mode the
  embedder's callbacks JS makes are posted (`Runtime::toHost`, `jsLog`,
  `jsReportError`) and run in order on the owner thread: at the start of
  the tick, after the tick's JS (so JS host-state writes reach native
  Animated `hostState` bindings, `HostState::deferUiChanges`, before they
  tick), after layout (before `jsui_call_ui` results), after a bundle
  evaluates, and before a reload's cancellations. That covers `on_message`,
  `on_state` (with the text JS wrote), `on_call` / `on_call_cancel`,
  Host.handle results, audio requests (`on_audio`), `log` / errors /
  `devError`, `reload`, resource loads (`fetch`, `loadFont`; a custom
  `JsuiLoadFn` runs on the owner thread) and cancels, and the clipboard
  setter. Reads only the owner thread can answer are **proxy calls**
  (`HostQueue::call`): `getClipboard`, `getProps` (the native tree as of
  the last applied batch) and a buffer's first copy. From another thread
  the caller waits until the owner thread ran it (`kProxyTimeoutMs`); on
  the owner thread it drains the queue first, so the order stays FIFO.
* **Async measure.** `measureAsync(id, cb)` records a `Measure` command; the
  owner thread answers after the next layout, in the same batch as the
  onLayout events (`JsEngine::measured`). In JS-thread mode
  `HostInstance.measure` / `measureInWindow` / `measureLayout` and
  `UIManager.measure*` use it (`native.asyncMeasure`): React Native's
  semantics, one frame later than the synchronous form.
* **JS perf block.** Counters JS bumps (`bridgeCalls`, `valueConversions`,
  `jsCalls`, `jsJobs`) go through `Impl::counters`: the main document's, or
  in JS-thread mode a block of the engine's own, merged into the document's
  on the owner thread (`JsEngine::mergePerf`: in the tick and in
  `jsui_get_perf`). Phase times stay on the document's `Perf`.

Single-threaded these stay direct, behind the command buffer's sync
points: font registration (`loadFontData`, `unloadFont`, fallbacks, the
default family) and the calls that apply at once
(`CommandBuffer::appliesAtOnce`); on a JS thread the font calls are proxy
calls (`Impl::onOwner`). Shader preloads and the Lottie data hand-off are
thread-safe either way.

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
  handler ran, `2` `preventDefault()` was called, `8` a JS responder holds the
  gesture, `4` and it blocks native gestures.
* `__jsui_onHostMessage(name, json)` — from `jsui_post_message`.
* `__jsui_onHostState(keys)` — shared-state keys the embedder changed, once per tick.
* `__jsui_onHostBuffer(keys)` / `__jsui_onHostTexture(keys)` — frame data
  buffers changed; host textures set, resized or removed (once per tick).
* `__jsui_runSurfaces([[rootTag, appKey, propsJson], …])` /
  `__jsui_stopSurface(rootTag)` — surfaces to start or re-prop, and to stop
  (see "Surfaces").
* `__jsui_onAudio(reports)` — voice status and load results from the audio
  host (see "Audio").
* `__jsui_onDevUpdate(token)` — the dev server has a new build (hot update;
  absent: the runtime reloads).
* `__jsui_onCall(id, name, json)` — `jsui_call_ui`; answer with `callResult`.
* `__jsui_onLayout(id, {x, y, width, height})` — for `onLayout` listeners.
* `__jsui_onAnimatedEnd(id, finished, value)` / `__jsui_onAnimatedValueUpdate(tag, value)`
  — native Animated ends and value-listener updates, after each tick.

### JS thread

As in React Native, the bundle can run on a thread of its own
(`src/js/js_thread.h`): `JsuiConfig.js_thread` or `jsui_set_js_thread`
(before the first bundle load) with `JSUI_JS_THREAD_ON`. Off in
`jsui_config_init` (so tests, tools and the libraries' own defaults keep
single-threaded frames); the native hosts turn it on (see "Hosts" below);
the host JS engine (WebAssembly) has none. The **owner thread** (the one
calling `jsui_tick`, every `jsui_*` call) keeps the documents, Yoga,
input, native Animated, scrolling, fonts and paint; the **JS thread**
creates, runs and frees QuickJS (its stack is `js_stack_size_kb` plus
headroom for the C++ around the engine). The owner never runs JS and never
waits for it, except in a sync the host asks for.

* **Owner → JS.** `JsThread` is the runtime's `JsEngine`: `Inbound::post`
  queues each message to it, stamped with the runtime clock it was posted
  at (`Message::now`: what `performance.now()` and new timers see while it
  runs). The thread delivers them in order, one task each (its microtask
  drain included); a thread that fell behind runs the latest `Frame` only.
  A `BeginBatch` goes out with the batch's first message, and an empty
  batch is dropped (it would wake JS for nothing).
* **JS → owner.** Each task publishes one `JsBatch`: the commands it
  recorded, the replies to what it answered and the host callbacks it made
  (`Runtime::toHost`). The tick applies the published batches at its start
  (`Runtime::applyJs`), whole and in order, so a React commit is never
  seen in parts; a proxied read (`Runtime::proxy`) first publishes the
  task's replies and callbacks so far, and the owner applies what came
  before it, keeping FIFO order.
* **Its own work.** Between messages the thread runs the timers due at the
  last frame's time (`Runtime::publishedNow`, set before the tick posts its
  `Frame`; at most 64 passes per frame), so React's scheduler goes on
  between frames, and collects garbage while it has nothing else to do.
  `jsui_set_js_frame_budget` is the slice after which React yields
  (`JsThread::shouldYield`, from the start of the task).
* **What the owner sees.** `jsui_wants_frame` / `hasPendingWork` read what
  the thread published (`nextTimer`, `wantsFrame`, a running task, batches
  waiting). `Runtime::jsBusy` is true while JS runs a task. `JsuiPerf`
  adds `js_busy_ms`, `js_batches_applied`, `js_apply_ms`, `js_lag_frames`
  and `js_sync_ms`.
* **Syncs.** `jsui_sync(rt, timeout_ms)` waits for JS to go idle (serving
  proxied reads), applies, lays out and delivers the onLayout events and
  measure callbacks that leads to, up to 8 rounds; 0 when the time ran out
  (what finished is applied, commits whole). `jsui_tick_ex(...,
  JSUI_TICK_SYNC_JS)` does that inside the tick, after posting the frame
  and after layout (`js_sync_timeout_ms`, default 250): state the host set
  before the tick shows on that frame. A default action JS may veto (Tab,
  Enter, wheel) waits for JS's answer up to `js_event_deadline_ms`, unless
  the tree declares the answer (see "Input routing details": declared
  default actions).
* **Lockstep** (`JSUI_JS_THREAD_LOCKSTEP`: tests and tools). Every
  top-level post waits for JS (`syncJs`), and the thread runs no timers of
  its own, which gives single-threaded frames: the React e2e suites run a
  second time in lockstep (`JSUI_TEST_JS_THREAD`).
* **Lifetime.** `JsThread::stop` (reload, destroy, a mode change) makes
  the interrupt handler end running JS, serves proxied reads until the
  thread is out, frees the engine there and joins. Reloads and surface
  destruction asked for by a host callback wait until the batch is applied
  (`JsEngine::inCall`). `jsui_resolve_call` checks the call against the
  set of calls JS has not settled (`JsThread::takeCall`).

**Hosts.** JS on its own thread is React Native's model, and the native
hosts default to it; the defaults are per host, not in the C API:

| Host | Default | Switch | Wake-up while idle |
| --- | --- | --- | --- |
| Viewer (`tools/jsui-viewer`) | on | `--js-thread on\|off\|lockstep`, F6 cycles (restarts the runtime), F7 stalls JS 200 ms | `on_js_ready` pushes an SDL user event |
| raylib, SDL3, sokol examples | on | `--js-thread ...` | none needed (they tick every vsync) |
| Godot `JsuiView` | on | `js_thread` property (`JS_THREAD_*`, applies on `start()`) | none needed (`_process` every frame) |
| Unity `JsuiView` | on | `jsThread` field (applied in `Start`) | none needed |
| Unreal `UJsuiWidget` | on | `JsThread` property (`EJsuiJsThread`) | none needed |
| Android, iOS examples | on | `--es jsui_js_thread off` / `-JsuiJsThread off` | none needed (vsync / MTKView) |
| Rust, Go examples | on (libraries: off) | `--js-thread ...`; `Config::js_thread` / `Config.JSThread` | `on_js_ready` → `glfwPostEmptyEvent` |
| Browser (`web/`), CLI, `jsui-tests`, bench, ctests | off | `jsui-bench --js-thread`, `JSUI_TEST_JS_THREAD` | n/a |

The engine plugins add the same small surface: a sync for this frame
(`SyncJsNextFrame` / `sync_js_next_frame()`, a `JSUI_TICK_SYNC_JS` tick)
and right away (`SyncJs` / `sync_js()`, `jsui_sync` plus the host's
callback delivery), and `DebugStallJs` / `debug_stall_js()`
(`jsui_debug_stall_js`: a busy loop queued on JS, which without a thread
blocks the call instead). Their smoke tests stall JS once: the shared
example SelfTest (raylib, SDL3, sokol, Unreal, Unity) stalls 300 ms and,
with the thread, requires frames to go on (none longer than half the
stall) and a sync to finish after; with it off, that the stall held the
frame. The Godot demo checks `PASS js stall` (400 ms), the Unity plugin's
ctypes smoke runs on, off and lockstep. CI runs each with the thread on
(the default) and once off. Things the host sets that must show on the same
frame (a scene cut, a screenshot) go through the sync tick; everything else
shows a tick or more later, as in React Native.

Not done yet: a default action an undeclared, non-passive listener may
veto still waits for the event's generic reply (up to
`js_event_deadline_ms`); a typed reply channel per event kind (answering
only `defaultPrevented` early, before the listener's task ends) would
shorten that wait.

`tests/native/react_js_thread_e2e_test.cpp` (bundle `stall.jsx`) holds it
to that: 32 host-buffer markers (an interpolation and an `Animated.derive`
among them) equal each frame's buffer while JS is stalled for 80 ms twice
and collects garbage, a fling keeps going, and the owner's tick + render
stays under 4 ms (p95) and 8 ms (max); a sync tick shows state set before
it, and `jsui_sync` gives up on busy JS without splitting a commit.

### Event objects

Native dispatches plain objects; `@orktes/jsui-react` turns them into React-style
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
was `preventDefault()`-ed (or declared so, see "Input routing details").

### Input routing details (`src/input/`)

* **Default actions** (Tab focus moves, Enter/Space/A → `click`, D-pad/stick/arrow
  navigation, wheel scrolling) are skipped when JS calls `preventDefault()`.
  `__jsui_dispatchEvent` returns a bitmask: `1` = a handler ran, `2` =
  default prevented (a boolean `true` is treated as `3` for compatibility).
  A JS Pressable that handles Enter/Space/A itself prevents the default so no
  extra `click` follows. Exception: focus-on-press always happens.
* **Declared default actions.** Props on the committed tree say what JS
  would answer, so native decides without its answer (single-threaded the
  same way: a declaration wins over the answer):
  * `preventDefaultKeys` (`key` or `code` names) / `preventDefaultButtons`
    (gamepad button indices) on the focused node or an ancestor (the active
    focus scope's path when nothing is focused): that keydown / button
    press is prevented at once, consumed, and JS still gets the event. A
    focused TextInput only honours its own (its JS handler stops the keys it
    handles). Modifiers are not part of the match.
  * `__passive` (the `__events` tokens `keyDown`, `textInput`,
    `gamepadButton`, `gamepadAxis`, `wheel`): that node's handlers of those
    events never call `preventDefault()`. `updateProps` derives it from
    handlers marked `fn.__passive = true` (`passive(fn)` in
    `@orktes/jsui-react`); a token goes there only when every handler for it
    (bubble and capture) is passive.
  * A global listener's `listenGlobal(type, true, keys)`: it may veto only
    those keys / buttons / axes.

  `@orktes/jsui-react` declares its own: Pressables (and pressable Text)
  `preventDefaultKeys` Enter / Space / NumpadEnter with a passive keyDown
  (unless the element's own `onKeyDown` is not passive), TextInput a passive
  keyDown / textInput (native editing; unless the owner's handlers are not)
  and Enter when it submits, Modal its close keys and B (with
  `onRequestClose`), `BackHandler` / Modal global listeners only for those
  keys / B, the gamepad state store passive, `useGamepad({ passive })` /
  `vetoes`.
* **Touch drags vs. ScrollView** follow React Native's host responder rules.
  A drag past the slop inside a ScrollView starts scrolling (the target gets
  `pointercancel`) unless a JS responder holds the touch and either blocks
  native gestures (its `onResponderGrant` returned `true`; PanResponder's
  `onShouldBlockNativeResponder`, default `true`) or refuses the
  `responderterminationrequest` native sends first
  (`onResponderTerminationRequest` → `false`). `preventDefault()` on
  `pointermove` keeps the touch too, like `touch-action: none`.
* **Late answers.** Native never reads JS's answer to an event in place:
  it posts the event (see "Inbound messages") and acts on the reply.
  Single-threaded the reply is there as soon as the event is posted, so
  nothing below changes what happens; it matters once JS answers later (a
  JS thread; in tests, `Inbound::replyDelayMs`):
  * A default action JS could veto (Tab, Enter / Space / A → `click`,
    arrows / D-pad / stick navigation, keyboard scrolling, TextInput
    editing, wheel scrolling) waits for the reply only when a listener could
    `preventDefault()` it and nothing declares the answer (see "Declared
    default actions"): a `keyDown` on the focused path that is not passive,
    or a global `keydown` listener that may veto that key, for keys;
    `textInput` for text; `gamepadButton` / `gamepadAxis` (or global ones
    for that button / axis) for the gamepad; a non-passive `wheel` on the
    path for the wheel (global listeners are reported with `listenGlobal`).
    A declared `preventDefaultKeys` / `preventDefaultButtons` key never
    waits. The action then waits in `InputManager::deferred_`, in event
    order (an event with nothing to wait for queues behind a waiting one),
    and runs when the reply comes without `preventDefault()`, or when
    `Runtime::eventDeadlineMs` (100) passes, with a warning. With the
    `@orktes/jsui-react` components, focus navigation, Pressable activation,
    typing in a TextInput, modal close keys and gamepad navigation (with or
    without `useGamepad`'s store) do not wait; an app's own non-passive
    `onKeyDown` / `onGamepadButton` / `onWheel` / global listener does.
    Meanwhile `jsui_send_*` reports a key or press with a pending action as
    consumed, a pointer by the hit test alone. State styles (`pressedStyle`
    by Enter / A) do not wait.
  * A touch dragged past the slop while a reply of its gesture is out and
    a node on the path can be a JS responder (`responder` / `pointerMove`
    in `__events`) is **held**: the ScrollView does not scroll yet, the
    drag start stays put, and it decides when the replies are in (the
    `responderterminationrequest` is asked the same way) or at the
    deadline. Then the static hints decide: `__blocksNative` (an
    `onResponderGrant` marked `__blocksNative = true`: a PanResponder
    without `onShouldBlockNativeResponder`) keeps the touch for JS, as does
    `__keepsGesture` for an unanswered termination request (an
    `onResponderTerminationRequest` not marked `__keepsGesture = false`:
    PanResponder marks it so without `onPanResponderTerminationRequest`).
    Without hints the ScrollView takes it. A touch with no possible JS
    responder on its path scrolls at once.
  * The `click` after `pointerup` hits the tree as it is when it is sent,
    without JS's changes from a late `pointerup` handler (the DOM order for
    asynchronous handlers).
* **`__events` drives native decisions.** Names are matched case-insensitively
  by substring: `press`/`click` = pressable (focusable by default, `pointer`
  cursor); `move`/`responder`/`pan`/`drag` = wants `pointermove` (moves are not
  sent to JS unless a node on the path lists one); `hover`/`enter`/`leave`/`over`
  = wants `pointerenter`/`pointerleave` (sent only to such nodes); `wheel` =
  wants `wheel` (sent only when a node on the path lists it); `scroll` = a
  ScrollView's `scroll` events reach JS (see Scrolling); `"*"` = everything.
  Native `Animated.event` mappings see `wheel` and `scroll` events whether or
  not JS listens. Any pointer-ish name makes `jsui_send_*` report "consumed".
  The parsed mask is cached on the node (`Node::listenMask`) until its props
  change.
* **Consumed vs. pass-through.** Every `jsui_send_*` returns whether the UI
  captured the event, so the game handles everything else (a click on an empty
  or merely decorative part of a HUD). Pointer: a node on the hit path with a
  pointer-ish `__events` name, a TextInput, JS `preventDefault()`, or (touch
  only) a ScrollView that can scroll; `pointerEvents` `none` / `box-none`
  views are skipped by the hit test, so full-screen containers don't swallow
  input. The press decides the gesture: its moves and release report what the
  press did (`PointerState::owned`), so the game sees whole drags. Keys and
  gamepad buttons: JS `preventDefault()` or a default action that ran (focus
  moved, activation, scrolling, editing); the repeats and release of a
  consumed press report consumed too (`consumedKeys_`, `Pad::consumed`).
  Sticks: focus moved or a ScrollView scrolls. `jsui_hit_test(x, y)`
  (`InputManager::capturesAt`) answers the pointer question without sending
  anything, for hosts that route by hit testing: Godot's `JsuiView._has_point`
  (`input_pass_through`), the web host's `passThrough` overlay mode.
  `jsui_hit_regions` returns the same answer for the whole view as rects
  (clipped bounding boxes in paint order, with their `JSUI_HIT_*` flags), for
  hosts that hit-test away from the runtime (the web host's worker mode).
* **Pointer:** mouse `pointerId` 0, touch `id + 1`. Implicit capture while a
  button / touch is down; `click` (primary button only) targets the nearest
  common ancestor of the down and up nodes. Wheel `deltaY > 0` scrolls down
  (DOM convention); over a `horizontal` ScrollView a vertical wheel scrolls
  sideways. A touch dragged > 8 px inside a scrollable ScrollView sends
  `pointercancel` and scrolls it.
* **Scrolling** (`src/input/scroll.cpp`; geometry in `src/core/scroll.*`).
  All animations are analytic curves on the runtime clock, advanced in
  `jsui_tick`.
  * Touch drag: `scrollbegindrag`, `scroll`…, `scrollenddrag`. The content
    starts moving from where the drag was recognized (no jump by the
    slop). With host event times (`jsui_set_event_time`) the content
    follows the finger resampled at the tick's time (interpolated between
    the touch samples a few ms back, by as much as the newest sample's age
    at recent frames, 5-25 ms, drifting slowly; predicted at most 8 ms past
    the newest), so touch samples that don't line up with frames (120 Hz
    digitizer, 60 Hz rAF, a worker getting events late) still give even
    per-frame motion; without them it moves as events arrive. Release
    velocity: least-squares quadratic over the last 100 ms, its slope at
    the newest sample (a flick that speeds up keeps its final speed); a
    finger that rested 50 ms has none. Then a
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
  * `scroll` events go to JS only when the ScrollView lists `scroll` in
    `__events`, at the rate its `scrollEventThrottle` prop allows (RN): 0 /
    unset sends every offset change; up to 16 ms at most one per frame; more
    at most one per interval. An offset change inside the interval is held
    back and sent from `jsui_tick` once it is due, with the offset then and
    the time of the change, so JS always gets the final position (`trailing`
    event; `ScrollState::scrollOwed`). Other scroll events (drag, momentum)
    are never throttled.
  * Content scrolling under a still mouse does not update hover per row:
    enter / leave and the cursor catch up once it has stopped scrolling under
    the mouse for 100 ms (`kHoverAfterScrollMs`), like browsers; a mouse move
    updates them at once.
  * Overlay indicators (3 px thumbs, `indicatorStyle` `default`/`black`/
    `white`, hidden by `showsVertical/HorizontalScrollIndicator: false`) show
    while scrolling and fade out after 500 ms. Scrolling and fading repaint
    only the ScrollView's rect / the thumbs' rects.
  * `@orktes/jsui-react` adds `contentOffset` (applied on mount),
    `scrollEventThrottle` (passed to native, which throttles as above, so
    held-back offsets never reach JS; with sticky headers, which follow every
    offset, `onScroll` is throttled in JS the same way; `VirtualizedList` /
    `FlatList` / `SectionList` default it to 50 like RN), `stickyHeaderIndices` (vertical; headers wrapped
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

## Host JS engine

In the WASM build the bundle (React and the app's code) can run on the JS
engine of whatever loaded `jsui.wasm`, i.e. V8, JavaScriptCore or
SpiderMonkey in a page or worker and V8 in Node, instead of QuickJS
compiled to WASM. On the web it is `createJsui`'s default (`js: 'host'`;
`js: 'quickjs'` opts out); a thread that may not eval (a
Content-Security-Policy without `'unsafe-eval'`) runs QuickJS instead. In
the CLI it is opt-in, so headless screenshots match native runtimes:
`jsui screenshot|inspect|run|dev --headless --js host` and
`launch({ js: 'host' })` (`JSUI_JS=host` makes it the default there). A
precompiled bundle (`jsui build`'s default `combo` format) runs its
embedded source on the host engine; a `bytecode`-only one cannot run there,
and a `split` one loads its source file. Everything else stays in WASM: the document, Yoga, Skia, input,
native Animated, host data, fonts, resources and the renderers. Native
builds keep QuickJS. `jsui.jsEngine` / `app.hostJs` say which engine runs.
A module built without it (`-DJSUI_HOST_JS=OFF`; on by default in every
WASM variant) falls back to QuickJS, with a warning. JIT-compiled React is
several times faster than interpreted QuickJS in WASM. See
[Performance](#performance).

* **One module, two engines.** `JsEngine` (`src/js/engine.h`) is an
  interface; `JsEngine::create(rt, kind)` makes the QuickJS engine
  (`engine.cpp`) or the host one (`engine_hostjs.cpp`), per runtime
  (`jsui_web_create`'s `js` argument sets `JsEngine::nextKind` around
  `jsui_create`). Runtimes on both engines can share a page.
* **Native → JS.** `engine_hostjs.cpp` calls into `web/native/jsui_hostjs.js`
  (linked into `jsui.js` with `--pre-js`) through `EM_JS`: eval, the timer /
  rAF / microtask runs, events, host messages, layout events, settle
  functions, load and font callbacks. Each call is timed as the `js` phase.
* **JS → native.** `__jsui_native` is implemented in JS over
  `jsui_hjs_*` exports (`EMSCRIPTEN_KEEPALIVE`, called raw from
  `wasmExports`). Numbers and ids go as numbers; strings as (pointer,
  length) into a scratch arena the JS side writes (ASCII without
  `TextEncoder`); props, style patches, events, Animated configs and call
  arguments as JSON texts (`Value::parseJson` / `toJson`). Strings back are
  a pointer with the length (and number results) in `jsui_hjs_scratch`.
  The host engine has no `updateProps` / `getProps`, so `@orktes/jsui-react`
  diffs props in JS (`toNativeProps` / `nativePropsChanged`) and sends
  `setProps` only for changed elements, as it does on older runtimes.
* **The runtime's globals, not the host's.** The prelude (`prelude.cpp`'s
  sources, the same QuickJS runs) and the bundle are evaluated as
  `(function (<names>) { … })` (indirect eval, with `//# sourceURL`, the
  wrapper on the first line so line numbers match). The parameters are the
  runtime's global object `G`'s properties: the ECMAScript built-ins, the
  shim's `setTimeout` / `setInterval` / `requestAnimationFrame` /
  `queueMicrotask` / `performance` on the runtime clock (`jsui_hjs_now`,
  driven by `jsui_tick`), the prelude's `fetch`, `console`, `URL`,
  `TextEncoder`, `MessageChannel`, `process`, … and `globalThis` / `window`
  / `self` = `G`. Every other global of the host (`document`,
  `WebAssembly`, Node's `setImmediate`, …) is a parameter too, `undefined`,
  so feature checks (React's scheduler) see what QuickJS has. Globals a
  script defines later and others read as bare identifiers
  (`__REACT_DEVTOOLS_GLOBAL_HOOK__`, `__jsui_hmr`) are live bindings to
  `G`'s property. `Intl` is the host's (QuickJS has none).
* **Batching.** As in `JsEngine::Impl`: every native → JS call increments a
  depth; when the outermost returns, the runtime's microtask queue drains,
  then queued `onLayout` events, up to 64 rounds. `beginBatch` / `endBatch`
  hold the drain across a native batch, and a `queueMicrotask` outside any
  call flushes on a promise job. Native promises (`async` functions,
  `Promise.then`) are the host's and settle at the host's own microtask
  checkpoint, after the task that ran the frame, not inside the call that
  resolved them. `@orktes/jsui-react` uses the runtime's `queueMicrotask`
  where the order matters (`AppRegistry`'s auto start, `Host.handle` plain
  results, FlatList's deferred scroll); headless (`jsui screenshot` etc.)
  yields to Node's event loop after every frame on the host engine.
* **Errors.** Exceptions of native → JS calls are reported like QuickJS's
  (`jsui_last_error`, the dev overlay); unhandled rejections come from the
  host's `unhandledrejection` event (page, worker; `preventDefault`-ed) or
  `process.on('unhandledRejection')` (Node, where other listeners see them
  too), attributed to the runtime whose script the stack names.
* **Host data.** `getBuffer` returns a copy of the elements per buffer
  version (an `ArrayBuffer` cannot alias part of WASM memory without
  copying the whole heap), with the same `[ArrayBuffer, type, count,
  version]` shape; state, textures, calls, audio, Lottie and shader events
  cross as above.
* **Not on the host engine:** the watchdog (`js_timeout_ms`), the memory and
  stack limits (`js_memory_limit_mb`, `js_stack_size_kb`) and
  `memoryUsage()`: a runaway script blocks the page like any other script.
  String code passed to `setTimeout` is ignored. The page needs `eval`
  (a CSP with `'unsafe-eval'`). The sampling JS profiler
  (`JSUI_JS_PROFILE`) samples QuickJS only; use the host's own profiler
  (Chrome DevTools, `node --cpu-prof`).

Tests: `packages/cli/test/hostjs.test.js` renders `examples/showcase` and
the jsui-bench scenarios on both engines and compares the pixels and view
trees, and checks events, `Host.call`, host data, timers, native Animated,
errors and hot updates. `JSUI_JS=host` runs `web/test/node-smoke.mjs`,
`web/test/browser.test.mjs` (`npm run test:web:host`, main thread and
worker) and the CLI tests on the host engine. `web/test/js-bench.mjs`
measures both.

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

`@orktes/jsui-react`'s `Animated` (`packages/react/src/animated/`) has React
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
  * React Native's rules, mostly: a native value stays native and a
    JS-driven animation on it throws; animated non-style props throw.
    Unlike React Native, every style key can be driven: paint-only keys
    (`opacity`, `transform`, colors, border radii, ...) only repaint, and
    layout keys (`width`, `top`, `margin`, `flex`, ...) take
    `Node::patchStyle`'s general path and relay out in the same tick
    (also under `jsui_tick_native`). `Animated.ValueXY` is native through
    its channels.
  * Composites: `sequence` / `parallel` / `stagger` / `loop` (and `delay`)
    whose steps are all native-driven animations run as one native `group`
    animation (`__nativeSpec()` in animations.js; `NativeAnimated::runStep`):
    leaf, wait, sequence, parallel (stopTogether) and loop steps (resetting
    the values to their starting values before each iteration). A step
    starts on the frame the previous one ended, as the JS composites' end
    callbacks would, so frames match the JS-orchestrated version; only the
    group's end reaches JS. Any step that needs JS (a JS-driven animation,
    `toValue` tracking) keeps the JS composite. A value's `stopAnimation` or
    a new animation on it stops the whole group.
  * Text: `<Animated.Text text={node} />` sets the Text's content from an
    Animated node (`Animated.format(node, {decimals, grouping, prefix,
    suffix, minDigits, time})` for numbers; `time: 'h:mm:ss'` formats
    whole seconds as clock fields): natively a `format` node feeds the
    props node's `text` input, which sets the view's first RawText child,
    and React keeps the children it rendered when the view went native, so
    it never writes stale text. On the JS driver the component re-renders.
  * Writes go through `Node::patchStyle` (paint-only, so no relayout; the
    incremental renderer damages just the node's old and new rects). Each
    `style` node keeps its keys with their paint groups, resolved once when
    it is created, and hands the view its inputs' outputs directly
    (`Node::StyleWrite`): no partial style object is built. The view's
    `props.style` is updated in place with `Value::assignReusing`, which
    compares and copies in one walk and reuses the existing transform
    arrays, objects and strings, so later `setProps` diffs see the live
    values; only the written groups are re-resolved (transform, opacity and
    backgroundColor without indexing the whole style). Transform nodes
    update their output array in place and color / string outputs reuse
    their buffers, and the flush bookkeeping uses per-node flags instead of
    hash sets, so a steady frame allocates nothing
    (`perf_native_animated_writes_allocate_nothing`). When React re-props
    a connected view, the live values are overlaid again in the same tick;
    views are only re-checked for that after some `setProps` /
    `patchStyle` ran (`Perf::propsEpoch`), not every frame. Unchanged
    frames write nothing.
  * Native nodes are freed when their derived JS node detaches, when the
    component unmounts, and (values) when the JS object is garbage collected
    (`FinalizationRegistry`). A reload clears the native graph.
  * Without `__jsui_native.animated` (Node tests, other hosts)
    `useNativeDriver` falls back to the JS driver.
  * Derived values: `Animated.derive(inputs, fn)` (and `Animated.expr(fn)`
    to define one up front) compiles `fn`'s body, a pure-math subset of JS
    (`packages/react/src/animated/expression.js`), into a stack program
    (`expression` node: constants, inputs, locals, arithmetic, comparisons,
    `&&` / `||` / `?:` as eager selects, `Math.*` and GLSL-style helpers).
    The native node validates the program once (ops, operand ranges, stack
    balance) and runs it without allocating; the JS driver runs the same
    program, so both agree bit for bit
    (`tests/native/react_derive_e2e_test.cpp`). Bytecode bundles have no
    function source, so `jsui build` (`packages/cli/src/derive.js`) rewrites
    inline `derive(..., fn)` / `.expr(fn)` calls to pass `{source, scope}`:
    a text pass (strings, comments and template text masked) that uses the
    runtime's own parser to find the function's end and its free names.
    Free names are captured at call time: numbers become constants,
    Animated nodes extra inputs. Functions outside the subset are build
    warnings and run only on the JS driver (making them native throws).
  * `Animated.clock()`: a `clock` node, the runtime clock in seconds. Clock
    nodes with dependents are marked dirty in every `NativeAnimated::tick`
    and count as animating (`jsui_wants_frame`).
  * `Animated.clock({start, speed, paused, running})`
    (`AnimatedClockValue` in nodes.js): a value node plus, while it runs, a
    `clock` animation on it (`{type: 'clock', speed, running}`):
    `value = from + (t - start) / 1000 * speed`, never ending. So the
    controls are the existing commands: `pause()` is `stopAnimation` (the
    clock first counts up to the runtime time the stop was issued at, so
    pausing and `setSpeed` lose no time between frames), `resume()` a new
    clock animation from where the value is, `reset(t)` stop + `setValue` +
    start, `setSpeed(x)` stop + start. `running` is a node tag read every
    tick (`gen_` is bumped first, so it is this frame's output): while it is
    0 the clock is held at the time of the frame that saw it, and goes on
    from there when it is not (it is checked at start too). A held clock,
    and a clock animation on a value nothing depends on or listens to, do
    not count in `NativeAnimated::animating()`, so a paused clock does not
    keep `jsui_wants_frame` on (an unread one is computed from the time
    whenever a frame comes). The JS driver mirrors it: a
    `requestAnimationFrame` loop while something reads the clock, the value
    computed from `now()` when read, `running` followed through its change
    notifications (`packages/react/test/reveal-clock.test.js`).
  * Text reveal (typewriter): `<Text reveal revealMode revealFade
    revealFraction>` becomes the `textReveal` / `textRevealMode` /
    `textRevealFade` / `textRevealFraction` style keys (Text's
    `__animatedStyleProps` maps an animated `reveal` to `textReveal`, as
    LottieView's `progress`), a paint-only style group (`kGroupReveal`), so
    an Animated value drives it through `patchStyle` (JS driver) or a
    `style` node (native driver) with no reshaping and no relayout. The
    paragraph root lays out and shapes its whole text; `text::paintText`
    then paints only its first N units (`paintRevealed` in
    `src/text/text_layout.cpp`): units are ICU grapheme clusters
    (`SkUnicode::computeCodeUnitFlags`) or words (whitespace-delimited, ICU
    dictionary words inside unspaced scripts), counted over the text as
    shaped (all spans), as UTF-16 starts cached in the `TextCache`. The
    fully revealed prefix is drawn under a clip: whole lines before the last
    line it reaches (so overhang, shadows and decorations show) and, on
    that line, `getRectsForRange` boxes up to the reveal, open towards the
    line's start. With `revealFade` = f, unit k's alpha is
    `clamp((reveal - k) / f, 0, 1)` (in units, not time: stateless, so it
    scrubs and reverses); the few fading units are painted again under
    their own boxes with the painter's alpha multiplier. `revealFraction`
    scales 0..1 to `n` units (`n - 1 + f` with a fade, so 1 is fully
    shown). Fully revealed paints exactly what no reveal paints
    (`tests/native/text_reveal_test.cpp`, `react_reveal_e2e_test.cpp`).
  * `Animated.interaction()` / `useInteraction()`: `interaction` nodes
    (`{key, state}`) read the eased 0..1 weight of one state of the element
    whose `interaction` prop carries `key` (see "State styles"); marked dirty
    when the weight moved.
  * `Animated.hostState(key, path)` / `useHostStateBind`: `hostState` nodes
    read a field of shared host state (`HostState::value`; numbers, booleans
    as 1 / 0, strings as text). `HostState::onChange` marks them dirty on
    every change, by the embedder or by JS, so the next native tick (also
    `jsui_tick_native`) shows it without JS.
  * Props nodes take `visible` (Visibility's `visible={node}`):
    `Node::setHidden(value == 0)`, see "State styles".
  * Not native (yet): `toValue` bound to another Animated node (RN's
    `tracking` node) restarts native animations from JS when the target
    changes; scroll physics (and so `Animated.event` from flings) only
    advance in `jsui_tick`, not in `jsui_tick_native`.

## Worklets

`react-native-worklets` (Software Mansion, MIT; API of 0.13) runs on JSUI
unchanged: the CLI aliases the package to `@orktes/jsui-react`'s
implementation (`packages/react/src/worklets.js`, shim
`src/shims/react-native-worklets.js`, also `@orktes/jsui-react/worklets`).

**Build time.** A worklet must carry its source, and bytecode bundles keep
none, so as in React Native the build transforms it.
`packages/cli/src/worklets.js` runs Software Mansion's Babel plugin
(vendored with its license in `src/vendor/worklets-babel-plugin.cjs`, on
Babel 7) on every file that mentions `worklet`, in `jsui build`, `jsui dev`
and its HMR updates, before React Compiler and React Refresh (TypeScript is
stripped first by esbuild). Functions with a `'worklet'` directive and the
callbacks passed straight to `runOnUI` / `scheduleOnUI` / `runOnRuntime` /
... become functions carrying `__workletHash`, `__closure` (the values they
capture) and `__initData = { code, location, line }`; production builds
leave out `location` / `line` and the dev stack data. Worklet classes are
off (`disableWorkletClasses`).

**Runtimes.** `src/js/worklets.{h,cpp}`: `Worklets` (`Runtime::worklets`)
owns the worklet runtimes, each its own QuickJS `JSRuntime` + context (own
heap and GC, memory limit, 5 s watchdog, unhandled rejections reported). The
UI runtime (id 2, `RuntimeKind.UI`) is created on the first job, so an app
that never uses worklets pays nothing (no runtime, no tick phase);
`createWorkletRuntime` adds more (ids from 3, `RuntimeKind.Worker`). All of
them live on the owner thread (the thread calling `jsui_tick`, JSUI's UI
thread) and never touch the bundle's heap. The tick's worklet phase runs
after the frame's JS has been applied and before native Animated, in
`jsui_tick` and in `jsui_tick_native` alike: queued jobs (batched per
runtime), then each runtime's due timers (`setTimeout` / `setInterval` /
`setImmediate`) and `requestAnimationFrame` callbacks with the frame time
(`performance.now()` is the runtime clock). A runtime with nothing due is
not entered. `jsui_wants_frame` / `jsui_next_timer` include them. After
running something, a runtime that goes idle gets one cycle collection, so
closures holding remote functions (a self-scheduling rAF loop) are freed and
the bundle gets them back. Each runtime starts with the console and the JS
half of the protocol (`src/js/worklets_prelude.cpp`: timers, rAF, the wire
codec, the worklet cache and the react-native-worklets functions a worklet
can call, each also callable when captured from the bundle).

**Bridge** (`src/js/bridge_worklets.cpp`, `__jsui_native.worklets`):
`schedule(runtimeId, job)` queues a job through `Runtime::toHost` (owner
thread; with JS on its own thread it is applied with the frame's commands);
`runSync(runtimeId, job)` runs it on the owner thread now (single-threaded),
or waits for the owner (`Impl::onOwner`, at most `kProxyTimeoutMs`) with JS
on its own thread, and returns `{"ok", "v" | "e"}`; `createRuntime(name)`;
`sync*` for synchronizables. Worklet runtimes send to the bundle through
`Worklets::toJs`, delivered once per `jsui_tick` as one
`Inbound::call("__jsui_onWorklets", [ops])` (kept for the next `jsui_tick`
by `jsui_tick_native`): `['c', fn, args]` call a remote function,
`['r', id, n]` / `['a', id]` release / retain one, `['p', id, ok, value]`
settle a `runOn*Async` promise.

**Wire format.** A job is JSON `[worklet, args, promiseId]`. Values follow
react-native-worklets' rules: JSON values as is; tagged objects
`{ "$": tag }` for `undefined` (`u`), non-finite numbers and `-0` (`n`),
BigInt (`bi`), objects (`o`), worklets (`w`: hash, init data the first time a
runtime sees that hash, closure, own props), remote functions (`f`: any
other function, reference counted by the bundle), synchronizables (`s`),
Animated values (`av`), react-native-worklets builtins (`b`), Errors (`e`),
`ArrayBuffer` / typed arrays / `DataView` (`ab`, `tv`, base64 copies), `Map`,
`Set`, `RegExp`, worklet runtimes (`rt`) and `registerCustomSerializable`
values (`cu`). The receiving side evaluates a worklet's code once per hash
and binds it to a holder with its closure (react-native-worklets'
`valueUnpacker`). Remote functions throw when called directly in a worklet;
`runOnJS(fn)(...)` / `scheduleOnRN(fn, ...)` send the call back. Cycles
throw; class instances other than the above are warned about and become
`undefined`.

**Synchronizables** live natively (`Worklets::Sync`: the value as wire JSON,
a recursive timed mutex for `lock()` / `setBlocking`, 1 s timeout, reference
counted by every runtime holding one) and are read and written from any
runtime and from the JS thread.

**Animated values.** An `Animated.Value` that reaches a worklet is made
native (as `useNativeDriver` does) and arrives as a handle on its node:
`getValue()`, `setValue(v)`, `setOffset(v)` and a `value` property call
`NativeAnimated::nodeOutput` / `setValue` / `setOffset` directly. Its views
change in the same tick with no command batch and no JS: a UI-runtime rAF
loop driving one keeps moving while JS is stalled or not ticked (tested in
`tests/native/react_worklets_e2e_test.cpp`). JS-side `AnimatedValue` gained
`getValue()` and `value` for the same code to run on either side. Anything
an Animated node feeds (`derive`, interpolations, `ShaderView`'s
`uniform:<name>` style keys and other animatable style keys) follows; this
is how worklets can drive drawing later (a Canvas / Svg node whose
animatable props are style keys, or WebGPU / Skia drawing called from the UI
runtime as react-native-webgpu-worklets does) without another mechanism.

**Errors.** A throwing worklet is reported through `Runtime::reportError`
(`jsui_last_error`, the dev overlay) as ``[Worklets] in worklet `name`
(file:line): Error: ...`` with its stack; `runOnUISync` / `runOnUIAsync`
rethrow / reject in the bundle instead.

**Without the native module** (the host JS engine on the web, Node tests)
everything runs on the bundle's runtime like react-native-worklets on the
web: `runOnUI` on the next animation frame, `runOnJS` in a microtask,
`runOnUISync` at once, synchronizables as plain objects.

**Costs.** None until the first worklet. A runtime takes about 240 KB of
heap when started (the UI runtime about 260 KB after the e2e test's jobs)
and about 3 ms to start, once (parsing the preludes; printed by the e2e
test). Precompiling the prelude to bytecode would cut the start time if it
matters. The native code is about 70 KB (Release
objects: `worklets.cpp` 44 KB, the prelude 16 KB, the bridge 8 KB).

**Differences from React Native.**
* Worker runtimes (`createWorkletRuntime`) run on the UI thread's tick,
  not on threads of their own: separate heaps and queues, but a long worker
  job delays the frame. `useDefaultQueue` / custom queues are ignored.
* `runOnUISync` with JS on its own thread waits for the owner thread
  (at most `kProxyTimeoutMs`, then throws); it sees the native views as
  of the last applied commit.
* `createSerializable` / `makeShareable*` return the value itself (values
  are copied when a job is sent); `createShareable` and Bundle Mode do not
  exist; worklet classes are not transformed.
* `react-native-worklets-core` (Margelo) is not aliased.
* Browser (WASM) builds have the runtimes (same C++), but this has not been
  run there yet; the host JS engine uses the fallback above.

## State styles

Hover / press / focus feedback without JS (`src/core/state_style.*`,
`src/input/state_styles.cpp`). Host props, on any element:
`hoveredStyle`, `pressedStyle`, `focusedStyle`, `focusVisibleStyle`,
`disabledStyle`; `group` (`true` or a name) and, on descendants,
`groupHoveredStyle` / `groupPressedStyle` / `groupFocusedStyle` /
`groupFocusVisibleStyle` (+ `groupName`); `transition` (ms or `{duration,
easing, delay, <state>: ...}`); `interaction` (`{key}`, from
`useInteraction`).

* `Node::setProps` keeps the React props (`StateStyles::raw`, what it diffs
  against) and their style as the base; `props()["style"]` is the base with
  the overlays at their current weights, so re-renders keep the active
  states.
* `InputManager::tickStates`, every `jsui_tick` (after input and JS) and
  `jsui_tick_native`, before native Animated: for each element in
  `Document::stateNodes`, which states hold (hovered: id on the mouse's
  hover chain; pressed: a primary pointer press that began on it or a
  descendant while the pointer is inside its visual rect and not scrolling,
  an activation key / gamepad A held on it while focused, or a press since
  the last frame; focused / focusVisible: `focused_`, with keyboard modality;
  group: the same for the nearest `group` ancestor-or-self, focus within;
  disabled: the `disabled` prop, which turns hovered / pressed off). Each
  state's weight eases toward 0 / 1 over its transition (starting a frame
  back: the input came during the last frame), and changed weights write the
  overlay keys through `patchStyle` (paint keys re-resolve their groups,
  layout keys relayout).
* Blend: per overlay key, from the base value through the states in order
  (group hovered / focused / focusVisible / pressed, hovered, focused,
  focusVisible, pressed, disabled), mixing numbers, colors (alpha-weighted,
  a missing base is the target color transparent), transforms of the same
  functions (angles in degrees, a missing base is identity) and numeric
  objects; anything else switches at 0.5. A key the base lacks returns to
  absent at weight 0.
* The first frame an element is seen takes its states without a
  transition; `disabledStyle` already applies at mount.
* Visibility: `visibility` (`{mode, effect, scale}`) makes an element's
  hidden state the last slot. Its overlay is the effect (`opacity: 0`, plus
  a `scale` transform); once the weight reaches 1 the end keys apply
  (`display: 'none'` for `remove`, `pointerEvents: 'none'` (and opacity 0)
  for `hide`). `visible` comes from the props (a boolean) or from native
  Animated (`Node::setHidden`, which steps the weights at once, starting a
  frame back like input does).
* Group context (`packages/react/src/components/group.js`): `<Group>` and
  `<Pressable group>` give their element an `interaction` key and provide
  it through `GroupContext`; `useGroupState(name?)` returns its
  `interaction` Animated nodes (`instant: true` reads the state's target
  instead of its eased weight).

## Overlay layer

Popouts, tooltips and dropdowns that escape the clips (`overflow`,
ScrollView) and the z-order of their ancestors while staying attached to an
anchor (`src/core/overlay.*`; React: `<Overlay>` / `<OverlayArrow>` in
`packages/react/src/components/overlay.js`). Host props: `overlay`
(`{anchor, placement, offset, crossOffset, flip, collision,
collisionPadding, boundary, matchWidth, hideWhenClipped}`; `anchor` and
`boundary` are node ids, -1 for a ref not attached yet) on a View, and
`overlayArrow` on a view inside one.

* Two trees over the same nodes. The logical tree is React's:
  `Node::parent()` / `logicalChildren()`; the reconciler's inserts and
  removes, destruction, hover / press chains (`chainBuild_`), group state,
  focus order (`collectFocusable`, modal scopes) and event bubbling follow
  it (`measure` gives an overlay's `x` / `y` in its host, `pageX` / `pageY`
  in the viewport). The render tree is what layout,
  painting, damage and hit testing walk: `children()` / `renderParent()`,
  and `markPaintTree` / `markBoundsDirty` / `absoluteRect` / the input's
  `localToViewport` climb it. They are the same vectors except around
  overlays (`Node::render_` exists only on a node whose render list
  differs: a parent of an overlay, the root, the layer, a host), so
  nothing else pays for the split and the painter and hit testing have no
  overlay cases.
* `Document` creates, with the first overlay, the overlay layer: a native
  View (`nativeOnly`, no React instance) appended to the root's render list
  after its React children (`setRenderExtras`), absolutely filling the
  viewport, `pointerEvents: box-none`, its `zIndex` raised above the root's
  other children (a `Modal` is one, at 10000). Each overlay node gets a
  host in the layer (also native, `box-none`): the overlay leaves its
  parent's render list and Yoga children and becomes the host's only
  render child; the hosts are kept in logical tree order (re-sorted when
  `Document::treeVersion` moved), so a later overlay, a nested one
  included, paints and hits above an earlier one. A host is an absolutely
  positioned box in the viewport-sized layer, so the overlay in it sizes
  to its content (up to the viewport's width).
* `Document::layout()`: before Yoga, `syncOverlays` sets each host
  `display: none` while a logical ancestor of its overlay is undisplayed or
  it is detached (a `<Visibility mode="remove">` around a popout's anchor
  hides the popout), takes the product of the logical ancestors' opacity
  (and LayoutAnimation fades), and raises the layer's zIndex. After Yoga,
  on every call (not only when layout was dirty: a scroll or an Animated
  transform moves an anchor without relayout), `placeOverlays` runs in
  tree order (an outer overlay is placed before one nested in it). An
  overlay with nothing to show (an empty box whose children are all
  `display: none`, as a hidden `<Visibility>` popout) is skipped: the
  layout pass that shows its content places it. Each placed one gets:
  - the logical parent's full matrix to the viewport (`viewMatrix`: layout,
    ancestors' scroll offsets, transforms and LayoutAnimation scales, as
    the painter places them), the anchor's box (the parent, or the
    `anchor` node mapped into the parent's space) and the boundary (the
    viewport or the `boundary` node, inset by `collisionPadding`) in it;
  - the side: `placement`'s, or the opposite one when it does not fit and
    the opposite fits or has more room (`flip`); `auto` takes the first of
    bottom, top, right, left with room. The box goes `offset` away from the
    anchor on that side, aligned (`-start` / centre / `-end`) plus
    `crossOffset` along it, and is clamped along the anchor's edge into
    the boundary (`collision: 'shift'`);
  - the host: the parent's matrix times that position, relative to the
    layer: its translation is `Node::offsetX/Y`, which `layoutRect()` adds
    to Yoga's position (no relayout; pixel snapping as for any box), and
    its linear part, when not the identity, a `transform: [{matrix}]`
    about the top-left corner. So the popout of a slot on a stage scaled
    to the window is scaled with it, and rotates with a rotated parent;
  - `hideWhenClipped`: when the anchor's box lies outside the clips of its
    render ancestors (or the viewport), the host gets `opacity: 0` and
    `pointerEvents: none` until it is back; otherwise the host's opacity
    is the ancestors' product;
  - `matchWidth` writes the anchor's width as the host's `width` (or
    `minWidth`) and lays out once more;
  - `overlayArrow` views whose nearest logical overlay ancestor is this
    one get an offset that puts their centre on the edge facing the
    anchor, at the anchor's centre clamped to the box (transforms between
    the overlay and the arrow are not followed).
  Values are written only when they change, so a still popout costs a
  matrix walk per frame and no repaint.
* Hit testing finds the topmost node in the render tree as usual (the
  layer last); the hover chain built from the hit node climbs the logical
  tree, so the pointer on a popout rendered inside a `group` keeps that
  group (and every logical ancestor) hovered: an interactive popout stays
  open while the pointer moves onto it (without a gap between them). Wheel
  and scroll chains also climb the logical tree (a wheel over a popout
  scrolls the list its anchor is in); `scrollIntoView` climbs the render
  tree (an overlay is not inside its anchor's ScrollViews).
* Everything runs on the thread that owns the document, in single-threaded
  and JS-thread modes alike, after the tick's input, JS commands and native
  Animated; the CPU raster and GPU paths, incremental damage, the scroll
  blit and layer caches see the hosts as ordinary absolutely positioned,
  maybe transformed Views. A LayoutAnimation does not animate host frames;
  an overlay removed while a delete animation is armed is not ghosted.
* `Modal` stays a portal to the surface's root with zIndex 10000; the layer
  goes above it, so popouts inside a modal show over it.

## LayoutAnimation

`LayoutAnimation.configureNext(config, onEnd)` (and the `easeInEaseOut` /
`linear` / `spring` presets, `LayoutAnimation.create`) works like React
Native: `@orktes/jsui-react` arms the config natively at the start of the next
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
* Text: `color`, `fontFamily` (a CSS list), `fontSize`, `fontWeight`, `fontStyle`,
  `fontVariant`, `fontVariationSettings`, `fontFeatureSettings`, `lineHeight`, `letterSpacing`, `textAlign`, `textTransform`,
  `textDecorationLine`, `textDecorationColor`, `textShadow*`; props
  `numberOfLines`, `ellipsizeMode`.
* Image: `resizeMode` (style or prop), `tintColor`; `source={{uri}}` or
  `source={require('./a.png')}` (the bundler emits a URL).

Colors are CSS colors (`#rgb[a]`, `#rrggbb[aa]`, `rgb[a]()`, `hsl[a]()`, names).
The default font is Inter (bundled); `fontFamily` falls back to it.

### Fonts

`FontManager` (src/text/font_manager.cpp) owns a registry of faces that is
skparagraph's asset font manager and Skottie's. Fonts are runtime-wide.

* Sources: app code (`Fonts.load` / `useFonts` / the `expo-font` shim, from
  `require('./x.ttf')`, a URL or bytes), hosts (`jsui_register_font`, web
  `registerFont` / `loadFont`, Godot `JsuiView.register_font` / `fonts`).
* Containers: TTF, OTF, TTC / OTC (every face), WOFF (zlib) and WOFF2
  (google/woff2 + brotli), unpacked to sfnt in src/text/font_decode.cpp
  because not every platform font manager reads web fonts (cmake/WebFonts.cmake;
  `-DJSUI_WOFF2=OFF` drops WOFF2). A Skia build that ships libbrotli provides
  the brotli symbols.
* Matching works like CSS @font-face: a face is registered under a family
  name (case-insensitive) with a weight and slant, taken from the file unless
  the caller overrides them; `fontWeight` / `fontStyle` pick the closest
  face (CSS3 matching). skparagraph synthesizes bold / oblique when the
  picked face is 200+ lighter / upright. Re-registering a family + style
  replaces that face.
* Variable fonts: a 'wght' axis without a weight override is registered once
  per 100 step of its range (instances share the data), so `fontWeight` works;
  `fontVariationSettings` (`"'wght' 650, 'wdth' 80"` or `{wght: 650}`) sets
  axes directly, with 'wght' following `fontWeight` unless named.
  `fontFeatureSettings` (`"'ss01', 'liga' 0"` or `{ss01: 1}`) adds OpenType
  features.
* Glyph fallback, per character: the style's family list, the default
  family, `Fonts.setFallbacks` / `jsui_set_font_fallbacks` (in order), then
  any registered font that has the glyph, then system fonts on platforms that
  have them (none on the web). Ship emoji / CJK fonts and list them as
  fallbacks for deterministic results.
* Any change bumps `FontManager::generation` and `Runtime::fontsChanged()`
  re-measures text on every surface.

## Builds

* Native: `cmake -S . -B build -G Ninja && cmake --build build`. Dependencies
  are fetched at configure time (QuickJS-ng, Yoga via git; Skia prebuilt zip).
  `tools/jsui-viewer` downloads SDL 2.30.12 (SHA-256 checked) and links it
  statically; `-DJSUI_BUILD_VIEWER=OFF` skips it, `-DJSUI_SDL_DIR=` uses a
  local SDL2 source tree, and a failed download only skips the viewer.
  `jsui-viewer [--dev] [--cpu | --gl | --gl-core] [--width W --height H] [--js-thread on|off|lockstep] [--state key=json] <bundle path or URL>`
  (`--state` sets host state before the bundle loads; on macOS `video` host messages
  play videos with AVFoundation into host textures, the protocol of the showcase site's
  trailer: tools/jsui-viewer/video.mm, the viewer as an example host; `--titlebar-overlay H`, macOS, draws the UI
  under a transparent title bar with the window buttons centred in its top H px, publishes
  their box as HostState `jsui.windowControls` and drags the window from empty spots there); F5
  reloads, F6 cycles the JS thread mode (on by default), F7 stalls JS for 200 ms; `--frames N --screenshot out.png` runs headless (GPU by default,
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
  `-DJSUI_HOST_JS=OFF` leaves out the host JS engine (`createJsui({ js: 'host' })`,
  see [Host JS engine](#host-js-engine)), which links `web/native/jsui_hostjs.js` as a `--pre-js`.
  Tests: `node web/test/node-smoke.mjs`, `node web/test/browser.test.mjs` (Playwright);
  `JSUI_JS=host` runs both on the host JS engine.
* Web worker mode (`createJsui({ worker: true })`, `web/jsui-worker.js`): the
  runtime, frame loop and renderers run in a dedicated Worker on
  OffscreenCanvases (`transferControlToOffscreen`), with the same WebGPU →
  WebGL → CPU chain and the same GPU-loss handling. The worker's entry is
  `jsui-web.js` itself, so both threads load the same versioned files. The
  page's thread only does what needs the DOM. It forwards input with
  timestamps (moves and wheels coalesced per frame), canvas size and DPR,
  gamepad snapshots and `bindVideo` frames (`new VideoFrame(video)`,
  transferred). Both directions batch messages, one `postMessage` per task.
  The page answers synchronous questions from copies the worker posts:
  `hitTest` and pass-through routing from a hit map (`jsui_hit_regions`,
  posted when it changes, so it is about a frame old), keys from the
  runtime's last answer, the cursor, `renderer` and stats. Calls that need a
  runtime answer (`getState`, `getPerf`, `readPixels`, ...) return Promises;
  `mapBuffer` is a page-side array that `commitBuffer` copies over (no
  SharedArrayBuffer). The worker's rAF drives frames; the page's rAF takes
  over where a worker has none. Missing APIs or a failed start fall back to
  main-thread mode with one warning. See `web/README.md` ("Worker mode").
* Desktop Skia m157 (Graphite on Vulkan / Metal and header-only Dawn,
  Ganesh on GL, plus Metal on macOS and Direct3D 12 on Windows):
  `scripts/build-skia-desktop.sh` (Linux / macOS / Windows, about 25 min on
  4 cores, cached in CI on the hash of the script and its Dawn patch) into
  `third_party/skia-desktop`, then configure with
  `-DJSUI_SKIA_DIR=third_party/skia-desktop`. Every GPU host beyond GL
  (Godot, Unity, Unreal, SDL3 on Metal / Vulkan, anything on Direct3D)
  and the release binaries use it. See "Vulkan and Metal (Skia Graphite, native)", "Direct3D 12 /
  11" and "Dawn native".
* Shared library: `-DJSUI_BUILD_SHARED=ON` builds `jsui_shared`
  (`libjsui.so` / `.dylib` / `jsui.dll`), the static library linked whole
  into a shared one that exports only the `jsui_*` C API. Unreal, the Rust /
  Go bindings and the release archives use it.
* Hosts: `-DJSUI_BUILD_GODOT=ON`, `-DJSUI_BUILD_UNITY=ON`,
  `-DJSUI_BUILD_RAYLIB=ON`, `-DJSUI_BUILD_SDL3=ON`, `-DJSUI_BUILD_SOKOL=ON`
  (all off by default); Android and iOS through `scripts/build-android.sh`
  / `scripts/build-ios.sh` (Skia m157 from `scripts/build-skia-android.sh`
  / `build-skia-ios.sh`). Each README in `integrations/` has the details.
* Release binaries: the CI `dist-*` jobs build the shared library, the
  viewer, the Godot addon, the Unity package, the Unreal plugin and the iOS
  xcframework, and `.github/workflows/release.yml` attaches them to the
  GitHub Release of each `vX.Y.Z` tag next to the npm packages. See
  [RELEASING.md](RELEASING.md).
* JS: `npm install` at the repo root (npm workspaces), `npx jsui build app.jsx`
  (QuickJS bytecode with the source inside by default, see
  [Bundles](#bundles-precompiled-bytecode)) or `npx jsui dev app.jsx` (serves
  `bundle.js`, hot updates and the shared session, see "Dev mode").

## Bundles (precompiled bytecode)

`jsui build` compiles the bundle to QuickJS bytecode by default, so a
runtime skips parsing and keeps no function source in memory
(`src/js/bytecode.*`, `packages/cli/src/bytecode.js`). The file is a small
container: an 8-byte magic (`"\0JSUIBC\1"`, never valid JS), the key of the
QuickJS that compiled it, the bytecode (`JS_WriteObject`), an optional
source URL and an optional source. The CLI compiles with the CPU WASM
runtime, which has the same patched QuickJS as every native runtime of that
JSUI version.

Bytecode is only valid for the exact QuickJS it came from (its version plus
a hash of JSUI's patched `quickjs.c` / `.h` / opcode list,
`jsui_bytecode_key()`), so `Runtime::loadSource` runs the bytecode only
when the key matches and the engine runs bytecode (not the host JS engine);
otherwise it runs the embedded source, or loads the source URL. `--format`
picks what goes in:

| Format | Contents | Size (vs JS) | Loads in |
| --- | --- | --- | --- |
| `combo` (default) | bytecode + the source as the fallback | ~2.2x | every runtime |
| `bytecode` | bytecode only | ~1.25x | this JSUI version's QuickJS only |
| `source` | plain JS | 1x | everything (browser JS engines, other tools) |
| `split` | bytecode, source next to it in `<name>.source.js`, read only as the fallback | ~1.25x + the source file | every runtime |

`jsui dev` and the headless commands serve and run source.

## Hosts and integrations

Every host drives the same C API (`include/jsui/jsui.h`): it forwards input
and uses what the UI did not consume, ticks once per frame, renders into its
own target and exchanges game data (see "Game data"). The engine examples
share one HUD (`integrations/shared/ui/app.jsx`) and a `--selftest` that
clicks through each host's own input path; [integrations/README.md](../integrations/README.md)
walks through the pattern.

| Host | Where | Rendering |
| --- | --- | --- |
| Native viewer | `tools/jsui-viewer` (SDL2) | Graphite Metal on macOS (with `JSUI_GRAPHITE_METAL`), Ganesh GL, CPU fallback |
| Browser | `web/` (`@orktes/jsui-web`), [web/README.md](../web/README.md) | WebGPU → WebGL → CPU; on the main thread or in a worker |
| CLI / Node | `packages/cli` (`@orktes/jsui-cli`), [README](../packages/cli/README.md) | CPU raster of the WASM module (deterministic) |
| Godot 4 | [integrations/godot](../integrations/godot/README.md) (GDExtension, `JsuiView`) | Graphite on Godot's Vulkan / Metal device, Ganesh D3D12, Dawn on the godot-aw3 fork |
| Unity 6 | [integrations/unity](../integrations/unity/README.md) (`com.orktes.jsui`) | Graphite Metal / Vulkan, Ganesh D3D11 / D3D12 |
| Unreal Engine 5 | [integrations/unreal](../integrations/unreal/README.md) (`UJsuiWidget` / `SJsuiView`) | Graphite Metal / Vulkan, Ganesh D3D12 / D3D11 |
| raylib, SDL3, sokol | [integrations/raylib](../integrations/raylib/README.md), [sdl3](../integrations/sdl3/README.md), [sokol](../integrations/sokol/README.md) | Ganesh GL (SDL3: Graphite on Metal / Vulkan; sokol: Graphite on Metal on macOS) |
| Android | [integrations/android](../integrations/android/README.md) (NDK NativeActivity) | Graphite on Vulkan, Ganesh on GLES 3 as the fallback |
| iOS | [integrations/ios](../integrations/ios/README.md) (UIKit) | Ganesh on Metal |
| Rust, Go | [bindings/](../bindings/README.md) over `libjsui` | Ganesh GL (GLFW window) |

The native hosts run the bundle's JS on a thread of its own by default, as
React Native does; the browser, the CLI and the tests keep it on the
ticking thread (see "JS thread" for the table and each host's switch).

## Agent tools and the shared session

The CLI runs a bundle headless in Node (`packages/cli/src/headless.js`: the
CPU WASM module on a simulated clock, so the same bundle and steps give the
same pixels): `jsui screenshot`, `jsui inspect` (the view tree, `--find`
selectors) and `jsui run` (scripted steps: clicks by `testID`, keys,
gamepad buttons, waits on the UI clock, state writes), plus the `launch()`
API for scripts and tests. `jsui skill install` installs the JSUI agent
skill (`packages/cli/skills/jsui`) for coding agents.

`jsui dev` hosts a shared session (`packages/cli/src/shared.js`) that a
person and agents use together. Every dev-mode runtime with an `http://`
bundle (the native viewer, a Godot `JsuiView`, the web host, a
`--headless` runtime inside the server) joins it when the server offers one
(`X-Jsui-Session` on the bundle and version responses): `src/dev_session.*`
long-polls `POST /__jsui/session/poll` for commands and pushes results and
events (the person's input, UI messages, calls, state writes, logs, errors)
to `POST /__jsui/session/push`. Commands run at the end of a tick through
the C API (pointer, keys, text, gamepad, state, buffers, messages, UI calls,
hit tests, screenshots, reload). Agents attach with
`jsui screenshot|inspect|run --attach <url>`, `jsui clients` and `jsui log`
and drive the live runtime (the last to join, or `--client`); in a
browser, `/shared` shows the live app and the session's activity feed. `jsui_set_dev_session`
(web: `devName` / `devSession`) names a runtime or keeps it out.

## Performance

What a frame costs before painting, and how it stays proportional to what
changed rather than to the tree size:

* **Frame budget for JS** (`jsui_set_js_frame_budget`, default 5 ms; web
  `jsFrameBudget`, Godot `js_frame_budget_ms`; 0 turns it off). The frame
  clock (`performance.now()`) stands still within a tick, so React's
  scheduler, which times its slices with it, would never yield. The bundled
  scheduler instead asks `__jsui_native.shouldYield()`, which turns true once
  this tick's JS has run for the budget (`packages/cli/src/specialize.js`,
  `patchScheduler`). Updates in `startTransition` therefore render in slices
  across frames; the site's router navigates in a transition, so a click
  does no rendering in its own event, and the new page commits a few frames
  later (the showcase on one cloud CPU core, native QuickJS: from a 45-97 ms
  click handler and a frame of up to 62 ms after it, to ~5 ms slices and a
  15-24 ms commit frame).
  Zero-delay timers queued after the budget is spent (the scheduler's next
  slice, passive effects) wait for the next tick.
  Three things keep the commit frame small:
  * The `onLayout` events of a tick's layout passes are delivered in one
    batch (one microtask drain): state set by several handlers renders once.
  * Once the frame's budget is spent, state set in `onLayout` renders as a
    transition from the next frames, not synchronously: a page whose
    components size themselves from `onLayout`, one nesting level per render,
    no longer runs those renders back to back in the frame that commits it
    (react-native-web renders ResizeObserver updates in a later task too).
    Within budget, `onLayout` state still renders in the same frame.
  * Text nodes created by a sliced render are shaped in the spare time of
    the frames before the commit (`Runtime::prepareText`, up to twice the
    budget per tick): the same paragraph build and first layout the measure
    would do, so pixels are unchanged.
  Tearing down the old page is spread out the same way. The bundled
  reconciler queues React's per-fiber detach of a deleted subtree
  (`detachFiberAfterEffects`) and runs it at idle priority in budget-sized
  slices. Each field it nulls frees what it held, which took 4-8 ms of the
  frame after a navigation commit. The runtime takes destroyed nodes out of
  the document at once, but frees them in each tick's spare time
  (`Runtime::freeDestroyedNodes`, at least 256 per tick).
  Offline tools on simulated time (jsui-render, jsui-record, jsui-bench, the
  CLI's headless runtime) turn the budget off, so a frame's content does not
  depend on the machine's speed.

* **Style.** `setProps` diffs the old and new style objects; when every
  changed key is a known paint key, only the paint groups those keys feed
  (opacity, transform, colors, radii, shadows, gradients, filters, text, ...)
  are re-resolved and Yoga is not touched (`applyPaintStyleChanges`,
  `src/core/style.cpp`). `patchStyle` (bridge and native Animated) does the
  same in place, without copying the props; native Animated's writes
  allocate nothing in steady state (see "Animated"): `markers-native-1000`
  (2000 view writes per frame) went from ~31k allocations and ~19.7M
  instructions per frame to 6 and ~6.6M.
* **Layout.** Yoga only reruns when something layout-affecting changed; the
  post-layout walk (`Document::afterLayout`) visits only nodes Yoga flagged
  (`hasNewLayout`), and not the children of a node whose layout Yoga took
  from its cache (it reports which through its `NodeLayout` event: a 2000-row
  list next to a changing counter is one visit, not 2000). Only a node whose
  frame, padding or border changed is marked for the painter
  (`PerfCounters::layoutVisits` counts the visits). Text measurement reuses
  the paragraph's intrinsic layout.
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

`Document::perf` (one per runtime: surfaces report into the main
surface's) counts the work (`src/core/perf.h`: bridge calls, setProps,
style resolves, layout passes, text measures / shaping, Animated writes,
events, hit-test node visits, image decodes and their time, GPU pipelines
compiled by the runtime's renders) and, when `profiling` is set, the
exclusive time per phase (JS, commit, Animated, style, layout, input, paint,
and Graphite's submit after the paint, where pipelines compile).
`tests/native/perf_counters_test.cpp` asserts on these counters, so
regressions fail deterministically, without timing. Hosts read the same
numbers through `jsui_set_profiling` / `jsui_get_perf` (JsuiView
`profiling` / `get_perf()` in Godot).

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
every frame), `--patch-state KEY,SLOTS` (one `jsui_patch_state` per frame),
`--surfaces N[,changing|same|one|resize]` (N `Plate` surfaces of 120x40 and
what the host does to them every frame), `--wheel X,Y,DY[,FRAMES]` (a wheel
delta at X,Y every frame, reversing every FRAMES), `--react-stats` (installs
a React DevTools hook and adds `reactCommits` / `reactRenders`: commits and
function / memo / forwardRef components rendered per frame), `--dump
out.pam` (the last frame, fully repainted, as an RGBA PAM image, to diff two
builds of a bundle). Besides the
phase times, the JS work is counted: `events` (events sent to JS), `jsCalls`
(native → JS calls: events, timers, frame callbacks, layout callbacks,
messages), `jsJobs` (promise jobs) and `hostMessages` (`postMessage` to the
host). Scenarios: `interactions`
(examples), `interactions-wheel` (the same, wheel-scrolling the armory
list under a still mouse), `list` (2000 rows, one counter), `menu` (native spring on
hover), `inventory` (500 slots through `useHostState` +
`jsui_patch_state`), `idle`, `mount` (the showcase screen of
`examples/showcase/interactions.jsx` unmounted and mounted again every frame:
React's mount path, the host config's createInstance / appendChild and
Animated / Pressable setup), and 30 nameplate surfaces: `surfaces-static`,
`surfaces-props` (every surface's props change every frame),
`surfaces-same-props` (pushed unchanged every frame), `surfaces-one-props`
(one surface's per frame), `surfaces-resize` (every surface resized every
frame), and `markers-{json,buffer,native}-{30,100,1000}` (N enemy markers
moving every frame, fed as JSON shared state, as a frame data buffer rendered
by React, or as a buffer driving `Animated.hostBuffer` natively; painting
off; `host` is the host's own share, writing the JSON text, and counts in
`total`). `--markers N,json|buffer|native` runs one. Under valgrind, run with
`--tool=callgrind --instr-atstart=no`: the tool switches instrumentation on
for the measured frames only.

Where the JS time goes: with `JSUI_JS_PROFILE=out.txt` in the environment the
engine samples the JS stack from QuickJS's interrupt poll (every ~10000
bytecode ops) while the runtime profiles its frames (jsui-bench's measured
frames), weights each sample by the time since the previous poll, and writes
self and inclusive shares per function at exit (`JSUI_JS_PROFILE_LINES=1`:
per source line). Bundles built without `--minify` keep React's function
names.

* **Props.** `__jsui_native.updateProps(id, reactProps, hidden)` builds what
  native sees from the React props (event handlers become `__events`
  tokens, the style is flattened, `pointerEvents` / hidden go into it) and
  `Node::setProps` diffs it, so the host config does neither in JS; it only
  skips renders whose React props are identical (`mayDiffer`). A style
  array rebuilt with equal contents therefore reaches native (a skipped
  `setProps`), which is cheaper than diffing every changed style in JS.
  `mayDiffer` answers from `style` first (the prop that changes most) before
  its for-in over the props, and keeps the props' key count on the instance
  (`propCount`, -1 when unknown) so the old props are not enumerated again
  to detect a removed key: for-in is one of QuickJS's dearer operations.
* **React in the bundle.** Production `jsui build` bundles are tuned for
  QuickJS, where a JS function call costs far more than in a JIT
  (`JS_CallInternal` is about half of native self time on React-heavy
  frames), so the aim is fewer calls and property reads per fiber:
  * `minifySyntax` is on for production builds (constant folding, dead
    branches, shorter conditionals): 1-4% fewer instructions per frame and
    ~3% per commit. Identifier and whitespace minification add nothing
    measurable, so `--minify` stays opt-in (it also drops React's function
    names from `JSUI_JS_PROFILE`).
  * `packages/cli/src/specialize.js` loads react-reconciler's production
    build for the renderer and patches it at build time against the host
    config in `packages/react/src/renderer.js` (`hostConfig`): the
    `supportsMutation` / `supportsHydration` / `supportsPersistence` /
    `supportsMicrotasks` / `supportsResources` / `supportsSingletons` /
    `supportsTestSelectors` / `isPrimaryRenderer` reads become constants
    (esbuild then drops the dead branches), `shouldSetTextContent` is
    `false`, `getPublicInstance(x)` is `x`, and the fragment-instance calls
    (our `createFragmentInstance` returns null) are gone. One patch changes
    behaviour cost, not results: lazy context propagation
    (`propagateParentContextChanges`) walks a fiber's siblings when a
    context changes, until it meets one flagged `DidPropagateContext`;
    React flags only fibers that bailed out, so a freshly mounted fiber
    under a bailing parent made it re-scan every memo sibling on every
    frame. The patch also flags the parent when a newly mounted fiber
    propagates, which is what the walk would conclude anyway (inventory:
    -8% per frame). Every patch is anchored on the expected number of
    matches and the specializer is all-or-nothing: if any anchor misses (a
    new React), the bundle uses the published build unchanged and the build
    logs why. `JSUI_REACT_PATCHES=0` turns it off.
    `packages/cli/test/specialize.test.js` checks that every patch applies,
    that the folded constants match `hostConfig`, and runs a context app
    (300 memo rows that bail out, deep consumers, a nested provider, commits
    that keep the value) under both builds and compares every render.
  * ESM imports of `react` and `react/jsx-runtime` go through a generated
    facade of plain `export var` bindings (`config.js`, production only)
    instead of esbuild's `__toESM` copy, whose every export is a getter: a
    getter call per hook and per JSX element otherwise. `require()` still
    gets `module.exports`, and names React does not export still build
    (`export *`). About 1-1.5% per frame and per commit.
  * In the React layer: `Easing` functions are memoized and marked pure, so
    the native Animated driver samples each curve once (`sampleCurve`
    cache: -6.6% on `mount`); animated props and styles are resolved in one
    walk and the element is built with `jsx()`; `StyleSheet.flatten`
    allocates no closure; `Text` / `Pressable` check press props directly;
    host-state patches (`hostData.js` `patchShared`) copy only along the
    patched members.

  Ir per measured frame (callgrind, `--paint none`, QuickJS GC excluded:
  where a collection falls in the measured window moves a scenario by
  several percent) before → after these changes, Linux x86-64:

  | scenario | Ir / frame |
  | --- | --- |
  | `mount` | 123.6M → 109.2M (−11.6%) |
  | `inventory` | 30.7M → 26.9M (−12.4%) |
  | `markers-buffer-100` | 25.0M → 22.8M (−9.1%) |
  | `markers-json-100` | 27.4M → 25.1M (−8.2%) |
  | `surfaces-props` | 26.9M → 24.9M (−7.5%) |
  | `interactions-wheel` | 2.99M → 2.72M (−9%) |
  | `list` | 2.45M → 2.32M (−5%) |

  `commit-bench.jsx` (Ir per commit, sections run one by one in jsui-bench):
  mount −5%, plain re-renders −9%, memo re-renders −7%, Animated ±1%.
  Layout and allocator effects move such numbers by about ±1%, so compare
  several scenarios before trusting a small change. Pixels are identical
  (reference suite and `jsui-bench --dump` on every scenario).
* **QuickJS engine patches.** `cmake/Deps.cmake` applies two patch series
  (`git apply`, in file name order) to a copy of the QuickJS-ng sources in
  the build tree. `third_party/quickjs-rnq` is react-native-quickjs's engine
  work (MIT; rebased from their quickjs-ng base onto v0.17.0, what each patch
  does: `UPSTREAM-README.md` there): inline caches for property reads and
  stores, object literal shape templates, fused opcodes for prop-diff code,
  dense arrays with holes, string caches, faster JSON and RegExp, deferred
  GC and the debugger interface. On an M1 Ultra they take 8-24% off the JS
  time of every jsui-bench scenario (mount 24%, React commits about 11%) for
  about 100 KB more code (56 KB gzipped), with the same memory.
  `third_party/quickjs-jsui` holds JSUI's own, on top of those:
  * *Cycle collection between frames.* QuickJS frees by reference counting
    and collects cycles with a stop-the-world pass over every GC object
    (about 1 ms per MB of heap), which landed 15-35 ms pauses in the middle
    of React commits (a screen mount). Collections that come due while JS
    runs are deferred (react-native-quickjs's 0002, with a dynamic ceiling),
    and `Runtime::tick` runs them at the end of a tick
    (`JsEngine::collectIdle`): on a light tick (under 4 ms), else after 3
    heavy ones. That took the JS p95 of the mount scenario from 24 to 8.4 ms
    on an M1 Ultra. The patch also has a generational mode (young
    collections over the objects allocated since the last one, in time-boxed
    slices, survivors kept as roots); it is off, because on showcase
    navigation and mounts it cost 3-5x the GC time and 5 MB more for no
    shorter pause: a new screen outlives the next few collections anyway.
    `jsui_get_perf` reports collections and pauses (`gc_*`).
  * *Platform fixes* for the rnq series: the holey-array union on 32-bit
    targets, SSE4.2 detection under clang-cl, and the inline-cache hit paths
    under switch dispatch (Emscripten, MSVC), which ran the opcode twice.
* **QuickJS.** On top of the patch series, `cmake/Deps.cmake` patches quickjs.c further:
  `JS_FreeValue` / `JS_DupValue` are inlined (quickjs-ng exports them out of
  line, a call per value the interpreter pops) and `===` / `!==` decide
  objects, symbols, booleans, null, undefined, mismatched types and string
  pairs inline. Further named patches (each a whole-text replace that is
  left out with a warning if quickjs.c changed under it, and that
  `-DJSUI_QUICKJS_PATCHES_OFF="name;..."` leaves out for bisecting):
  `object-keys` (`Object.keys` of an ordinary object builds its array in one
  pass over the shape), `own-names` (the same for the string keys of
  `JS_GetOwnPropertyNames` on non-exotic objects), `for-in` (the iterator
  keeps its key table instead of building a dictionary object, and skips
  prototypes with no enumerable string keys without listing them),
  and `define-field` (object literal fields go straight to
  `add_property`). Together with the former `shape-transitions` patch
  (now react-native-quickjs's 0051) they took 5-11% of the instructions out
  of the React scenarios in `jsui-bench`, for 3 KB of code; none changes
  the bytecode. `JSUI_QUICKJS_FLAGS`
  adds compile flags for QuickJS alone.
* **QuickJS PGO.** Release binaries compile QuickJS (and only QuickJS) with
  profile-guided optimization: 6-9% less JS time on React-heavy scenarios
  (also ones outside the training set) and 2-3% on light ones on an M1 Ultra
  with Apple clang; -O2 vs -O3 and frame-pointer flags made no difference.
  On Linux x86-64 (shared cloud cores, so timing is noisy: medians of 10
  interleaved suite runs) JS time dropped 2-3% with clang 18 and GCC 13
  alike, and callgrind counts 3-4% (clang) / 8-9% (GCC) fewer instructions
  per whole frame on `inventory`, `markers-json-100` and `surfaces-props`
  (`menu`, with almost no JS, is unchanged).
  `scripts/pgo-quickjs.sh BUILD_DIR [cmake args]` does both stages in one
  build directory: it configures `-DJSUI_QUICKJS_PGO=generate`, builds an
  instrumented jsui-bench and jsui-render, trains on jsui-bench `markers`
  (100, buffer and JSON), `inventory`, `surfaces-props`,
  `interactions-wheel`, `list` and `packages/react/bench/commit-bench.jsx`
  (jsui-render), merges the profile (`llvm-profdata merge`, `xcrun
  llvm-profdata` with Apple clang; GCC keeps its `.gcda` directory) and
  reconfigures with `-DJSUI_QUICKJS_PGO=use -DJSUI_QUICKJS_PROFILE=...`.
  Only QuickJS's four C files change flags, so the real build afterwards
  reuses every other object and the stage costs an instrumented QuickJS, two
  tool links and about a minute of training. `cmake/QuickJSPGO.cmake`
  makes every failure soft: a missing profile, one the compiler cannot read
  (checked with a test compile; another LLVM's format) or no PGO support
  (MSVC / clang-cl) is a configure warning and a build without PGO; stale
  or unprofiled functions are not diagnosed (`-Wno-profile-instr-*`; GCC:
  `-Wno-missing-profile`, `-Wno-error=coverage-mismatch`,
  `-fprofile-partial-training`, and `-fprofile-prefix-path` so the
  `.gcda` names do not depend on the build directory). The script itself
  falls back to a normal configuration with a warning when training or
  merging fails (`PGO_STRICT=1` makes that an error). No profile is checked
  in: profiles are tied to the compiler version and the patched quickjs.c,
  so they are generated where they are used. Dev and test builds leave
  `JSUI_QUICKJS_PGO` empty. Which release builds use it: docs/RELEASING.md.
* **React Compiler** (opt-in: `jsui build|dev --react-compiler`,
  `packages/cli/src/react-compiler.js`). An esbuild `onLoad` plugin runs
  `babel-plugin-react-compiler` 1.0.0 (target 19, `react/compiler-runtime`
  resolved to the singleton React) over the app's own files, not
  `node_modules` or `@orktes/jsui-react/src`, on up to 4 worker threads. It
  runs on Babel 7 (`babel-core-7`, an npm alias of `@babel/core` 7): under
  Babel 8 the compiler skips every function with a defaulted destructured
  parameter (`function App({ size = 1 })`). Functions it cannot compile
  (Rules of React violations) are left as written and listed by
  `--compiler-report` (`build()` returns them as `reactCompiler.skipped`).
  Instructions per measured frame (callgrind, `--paint none`, 120 frames),
  uncompiled → compiled:

  | scenario | React renders / frame | Ir / frame |
  | --- | --- | --- |
  | `interactions` | 5.1 → 1.3 | 2.61M → 1.23M (−53%) |
  | `interactions-wheel` | 6.1 → 2.3 | 3.93M → 2.71M (−31%) |
  | `surfaces-props` | 120 → 60 | 27.1M → 19.8M (−27%) |
  | `inventory` | 4 → 3 | 31.3M → 31.1M (−0.5%) |
  | `list`, `menu`, `idle`, `surfaces-same-props`, `spring-wiggle` | same | ±0.3% |
  | `markers-{json,buffer}-{30,100}` | same | +10-11% |

  The gains come from elements the compiler caches: a parent's re-render
  hands React the same child elements, so their subtrees bail out. The
  markers lose because every `React.memo` marker gets new props each frame,
  so the memo cache is pure overhead, and React 19.3 copies each compiled
  component's cache on every render (`useMemoCache` without
  `enableNoCloningMemoCache`). `packages/react/bench/commit-bench.jsx`
  (500 cells, only `Cell` compiles): an unchanged re-render of plain cells
  sends 0 instead of 500 bridge calls, a re-render where all cells change
  takes ~50% longer. It stays off by default for that, for build time (4-25x:
  the site 0.3 s → 7 s; the compiler needs ~25 ms per KB of source on one
  thread) and because memoized code can keep a stale value read during
  render outside props, state and hooks (`Font.isLoaded()`, a mutable
  object). `useHostBuffer` returns a new view object per version so it
  memoizes correctly. Bundles grow 2-9%. Compiled and uncompiled bundles
  render identical pixels for every reference case and bench scenario
  (`JSUI_REF_REACT_COMPILER=1`, `jsui-bench --dump`).

### JS engines compared

`node web/test/js-bench.mjs` runs jsui-bench scenarios on QuickJS (in
`jsui.wasm`) and on the host JS engine ([Host JS engine](#host-js-engine)),
in Node (the CLI's headless runtime, no paint) and in headless Chromium (the
web host's CPU renderer, painting every frame); `--real-gpu --renderer
webgpu` runs the Chromium half in the installed Chrome on the machine's GPU.
Per frame (mean of 300 after 60 warm-up frames, median of 3 runs) on an M1
Ultra with Node 26, headless Chromium 141 and Chrome 155 (Metal). `js` is
the profiler's `js` phase, `core` adds commit, Animated, style and layout,
`wall` is the frame's wall time (Chromium: with the paint). Mount is
`examples/showcase/interactions.jsx` loaded into a running runtime: the
bundle's evaluation and its first 10 frames in one total, each run compiling
source the engine has not seen.

| Scenario | Where | js QuickJS → host | core QuickJS → host | wall QuickJS → host |
| --- | --- | --- | --- | --- |
| markers, 100, buffer feed | Node | 1.74 → 0.23 ms | 2.19 → 0.71 ms | 2.20 → 0.72 ms |
| | Chromium, CPU paint | 1.84 → 0.28 ms | 2.41 → 0.84 ms | 3.64 → 2.12 ms |
| | Chrome, WebGPU | 1.74 → 0.25 ms | 2.28 → 0.79 ms | 2.94 → 1.42 ms |
| inventory (one slot patched) | Node | 2.18 → 0.09 ms | 2.39 → 0.30 ms | 2.41 → 0.31 ms |
| | Chromium, CPU paint | 2.28 → 0.10 ms | 2.50 → 0.32 ms | 2.72 → 0.46 ms |
| | Chrome, WebGPU | 2.19 → 0.11 ms | 2.42 → 0.33 ms | 2.78 → 0.59 ms |
| surfaces (30, props every frame) | Node | 2.48 → 0.14 ms | 2.68 → 0.33 ms | 2.75 → 0.37 ms |
| | Chromium, CPU paint | 2.47 → 0.13 ms | 2.70 → 0.34 ms | 2.79 → 0.39 ms |
| | Chrome, WebGPU | 2.43 → 0.12 ms | 2.64 → 0.32 ms | 2.74 → 0.38 ms |
| mount interactions.jsx | Node | 44 → 37 ms | 54 → 47 ms | 57 → 49 ms |
| | Chromium, CPU paint | 54 → 52 ms | 122 → 89 ms | 187 → 152 ms |
| | Chrome, WebGPU | 55 → 52 ms | 138 → 93 ms | 182 → 135 ms |

Steady-state frames spend 7 to 20 times less time in JS once V8 has
compiled the hot paths. The core figure also counts the boundary on the
host engine: JSON props and events, and JS-side prop diffing in place of
QuickJS's native `updateProps`. A mount gains much less: it is mostly
code that runs once (compiling the bundle, React's first render), which a
JIT runs about as fast as an interpreter, plus GC.

## Paint performance

What painting costs is proportional to the damage, and within the damage
the expensive per-pixel work is done once:

* **Damage.** `updateDamage` (`src/render/painter.cpp`) compares each node's
  paint record (version, size, device matrix, children, scroll indicators)
  with the last frame and damages the old and new bounds of what changed;
  unchanged subtrees outside the damage are culled. Unchanged subtrees are
  not walked either: every change that can affect painting (props, text,
  children, layout, scroll offsets and indicators, LayoutAnimation frames,
  the caret) marks the node and its ancestors (`Node::markPaintTree`,
  stopping at the first marked one, like the hit-test bounds flag), and a
  clean child that was not changing in the last frame and is placed with
  the same matrices and clip keeps its records, its curved outlines and
  raster candidates carried over. A clean child out of view (scrolled out
  of its scroll view) is skipped as long as a translation keeps it out;
  its records keep the old placement until it comes back. When a skipped
  subtree is walked again (one of its rows changed), its clean children are
  skipped again: their entries in the lists moved with the copies the
  skipped parent made (`NodeRecord::walkedPass`), so the walk does not
  fall back to every node below. A steady
  showcase frame walks the 19 nodes on the paths to what animates instead
  of the whole tree, and jsui-bench `list` (20k nodes, one counter) the 5
  nodes on the counter's path. `PaintStats::nodesUpdated` counts the walked nodes;
  `setRecordSkipping(false)` walks them all, which the tests compare with
  (also for two surfaces of one runtime: each document has its own
  records and passes). Skia does not
  rasterize an anti-aliased outline cut by the clip exactly like the whole
  outline, so damage crossing a curved node's edge grows to the whole node
  (damage inside its padding box does not, unless an edge of the node's own
  may cross it: a sharp outer shadow that is offset or shrunk, an outline
  with a negative offset; a ring around the box, `0 0 0 1px` or an outline,
  does not), and the repaint stays exact. Before, any sharp shadow or
  outline counted: the site's screen stage (radius 18 with a `0 0 0 1px`
  ring) repainted all of its 1280x720 screen for the inventory's two
  pulsing legendary slots, every frame.
  On Graphite (WebGPU) only outlines whose pixels depend on the clip grow
  (dashed or dotted edges, outlines over 1024 device px): it draws solid
  rrects analytically and, patched, applies a whole-pixel damage rect as a
  scissor (see WebGPU).
* **Occlusion.** What a later opaque node covers is not painted
  (`Occlusion`). `updateRecords` keeps in each record the 4 largest opaque
  rects of its subtree (`OccCandidate`): the device pixels that a node with
  a solid background colour at full alpha, opacity 1, no blend mode, filter
  or content shader (its own or an ancestor's: a parent with any of them
  drops its children's) and an axis-aligned device matrix (scale,
  translation, quarter turns) is sure to cover, its border box inside the
  rounded corners' arcs, cut by the clips in the subtree (inside their
  rounded corners too), rounded inwards to whole pixels. Only large ones
  count (at least 65536 device pixels, or an eighth of a smaller canvas;
  `tMinOccluderArea`): a small opaque button or badge cannot save more
  than the checks it costs every node painted before it, so screens made of
  them pay nothing. One that a later one contains is dropped, and one with
  nothing painted before it overlapping it (a screen's own opaque
  background) is marked so and never used. Records keep a list only when
  they have one (`OccList`), and a node works out its own rect only when
  its background is opaque and large enough, its children's only when one
  has some. Unchanged subtrees keep theirs with their records, so it costs
  nothing beyond the nodes the update walks anyway, and the root's list is
  the frame's. A paint (each damage rect's pass) takes those of the root's
  that reach its damage and are at least a tenth of its area; with none it
  does no per-node work (`PaintStats::occluders`), and once all are reached
  it stops checking. Walking in paint order it marks
  each one reached when it gets to it or skips the subtree holding it (culled,
  covered, drawn from a texture, in the static backdrop), and `paintNode`
  skips a subtree whose `subtree` bounds lie inside one not reached and not
  inside the subtree, or only a node's own drawing (background, borders,
  shadows, text, image) when its `own` bounds lie inside one not reached;
  covered pixels are painted over anyway, so the frame is the same, and a
  modal card or a full-screen menu over a live HUD records nothing below
  it. Not culled: what is inside a filter or content-shader layer (it
  spreads pixels past the cover; the layer node itself is culled whole, its
  bounds include the spread), and nothing while the textures of the
  LayerCache (opacity layers, raster-cached subtrees, the static backdrop)
  are rendered, since they are drawn again under other covers; an occluder
  inside a raster-cached subtree that moves (drawn at the nearest whole
  pixel) is not used. A paint after a change that `updateDamage` has not
  seen (the root is marked), or on a canvas that maps the document
  elsewhere, culls nothing. The damage is computed as before (covered nodes
  keep their records), so a cover moving away repaints what was under it.
  `PaintStats::nodesOccluded` counts the skips; `setOcclusionCulling(false)`
  or `JSUI_OCCLUSION=off` paints everything, for A/B runs
  (`tests/native/occlusion_test.cpp` compares the two to the bit). Nothing
  in the painter reads what was painted below it (no backdrop filters);
  such an effect would have to end the occluders' reach at itself.
* **Scroll blit.** When a ScrollView's content only scrolled by whole device
  pixels over a uniform backdrop, the CPU path moves the viewport's pixels
  and repaints only what the scroll exposed, what is painted above the
  content and curved outlines at the viewport's edge (`ScrollBlit`).
* **Sprites.** On the CPU raster, gradient backgrounds and Image nodes of
  128x128 to 2.2M device pixels are rendered once per content, device scale
  and sub-pixel phase and blitted at whole-pixel offsets afterwards
  (`drawSprite`, 48 MB per document), so static art at a fractional
  position is cached too. While a node keeps moving by sub-pixels or
  scaling (it changed in this frame and the one before, `PaintMotion`) a
  sprite per phase would be a new sprite every frame: a gradient moving by sub-pixels (the showcase's drifting glow) is
  then resampled from its sprite of phase 0 (`resampleMask`, within ~5
  levels of an exact paint, at its anti-aliased edge), images and scaling
  content are drawn directly, and the node is repainted from the sprite a
  frame after it stops (`NodeRecord::approximate`,
  `Document::markExactRepaint`): static frames and frames at rest paint
  exactly the pixels of a full repaint.
* **Blits.** Sprite and shadow-mask blits on a raster target bypass Skia's
  drawImage (`blitSprite`): an SSE2 src-over of premultiplied 8888 rows
  over the clip's pixel rects (a region; anti-aliased clips are left to
  Skia), with transparent and opaque runs skipped or copied. It uses the
  arithmetic of Skia's 8-bit raster pipeline and is only used after a check
  at first use that it paints Skia's pixels on this build and CPU
  (`fastSpriteBlits()`; elsewhere, NEON included, Skia draws). Skia draws
  such a blit through its generic pipeline at ~20 instructions a pixel,
  and a game UI repaints its full-screen backdrop and art sprites under
  whatever animates (~45% of the CPU frame of the interactions showcase).
* **Shadows.** Blurred shadows and inset shadows are cached masks, blurred
  once per device size, sigma, sub-pixel phase, radii and colour
  (`ShadowCache`) and blitted at whole-pixel offsets; only the ring around
  the box is drawn (`MaskSkip`: the box minus its corners, as a cross of
  two rects on the CPU, one rect on GPU canvases). A node at rest gets its
  exact mask. While it keeps moving or scaling (a spring, a drag) the CPU
  raster approximates as GPU canvases always do: the mask of phase 0 and of
  the nearest of 8 scales per octave (`quantizedScale`) is resampled to the
  fractional position and size with a separable bilinear filter
  (`resampleMask`, SSE2 / NEON) into a scratch buffer blitted as a sprite,
  and the box's hole is cut out after resampling (its anti-aliased
  coverage), so the edge stays sharp. The shadow then stays within 4 levels
  of the exact one, the moving frames blur no mask after the first two
  (the menu's springing buttons blurred one per button per frame, ~21% of
  the frame), and the node is repainted exactly once it stops.
* **Layers.** A node whose draws cannot overlap applies its opacity to them
  instead of a `saveLayer` (`opacityFoldable`): one draw (a background
  colour or one gradient, nothing else), a childless box with at most one
  fill and one outer shadow (the shadow is then not drawn under the box), or
  a childless box that only draws a solid border ring of one colour. A
  layer at zero opacity is skipped. On GPU backends each layer is an
  offscreen pass, per damage rect, every time it is painted; on Graphite
  (WebGPU, Vulkan, Metal, Dawn) the layer's texture is cached and
  composited instead while its subtree is unchanged (`LayerCache`, see
  WebGPU: Composited layers).
* **Text.** Paragraphs keep one layout: painting and the painter's overflow
  bounds lay out at the same content width.

`PaintStats` (`lastPaintStats()`) reports per paint the nodes painted,
culled and occluded, the pixels and rects repainted, sprite draws and renders, shadow
mask draws and renders (blurs), blitted pixels and, with
`setPaintCounting()`, the canvas draw calls, text / image draws, layers and
clips. `tests/native/render_test.cpp` and `gpu_test.cpp` assert on these
(incremental frames equal full ones; perf guards on pixels repainted,
passes, sprite re-renders, also while a gradient moves, shadow blurs while
a shadow moves, layers).

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
panels with a ticking clock, a ticker and a pulsing badge) and `menu`
(jsui-bench's main menu, shadowed buttons springing in translation and
scale under a mouse sweep). Options:
`--frames`, `--warmup`, `--settle-ms`, `--width` / `--height` / `--scale`,
`--backends`, `--json`, `--no-count`. Under valgrind (callgrind
instruction counts are steadier than wall time on a shared machine), pass
`--no-count` and `--toggle-collect='jsui::Surface::renderIncremental(*'`
(`'jsui::gpu::render(*'` for `gl`; the warmup frames are counted too).
Wall times on llvmpipe say little about a hardware GPU (shader JIT and
rasterization run on the CPU); the GL call counts carry over.

## Dev mode

`jsui dev app.jsx` rebuilds on change and serves `http://localhost:5173/bundle.js`.
Point any host at it (`jsui-viewer --dev http://localhost:5173/bundle.js`, or
`cfg.dev_mode = 1` + `jsui_load_bundle_url`). A dev-mode runtime also joins the
server's shared session, where agents can drive it (see "Agent tools and
the shared session").

* **Version endpoint.** When the bundle came from `http://host:port/...`, the
  runtime GETs `http://host:port/__jsui/version?hmr=1` every ~500 ms (2 s
  while the server is unreachable). The response body is an opaque version
  token (whitespace trimmed); any change means "the bundle changed" and calls
  `Runtime::devUpdate()` (C API `jsui_dev_update`, for hosts that poll
  themselves, like the web host on https). If the bundle defined
  `globalThis.__jsui_onDevUpdate`, that handles it (hot update); otherwise the
  runtime does a `reload()`. The server should answer immediately (no
  long-poll needed); a bundle response may carry `X-Jsui-Version: <token>` so
  a rebuild between the bundle download and the first poll is not missed.
  With `hmr=1`, `jsui dev` also changes the token when a build fails, so the
  hot-update client can show the error.
* **Hot updates (React Fast Refresh).** `jsui dev` (unless `--no-hmr`) serves
  a module-per-function bundle (`packages/cli/src/hmr.js`): the vendor part
  (node_modules, `@orktes/jsui-react`, assets) bundled once by esbuild, then
  each app file run through React Compiler (with `--react-compiler`; its
  per-file source hash resets a component's memo cache when the file is
  hot-updated), react-refresh's Babel plugin and esbuild's
  CommonJS transform, wrapped in `__jsui_hmr.define(id, deps, factory)`, and
  the client (`hmr-runtime.js`). On a change the client fetches
  `/__jsui/hmr?from=<its version>`, a script with the changed modules and
  `__jsui_hmr.apply(ids, version)`, which it runs with
  `__jsui_native.evalScript(source, name)`. `apply` re-executes the changed
  modules and their importers up to the nearest refresh boundaries (modules
  that export only React components) and calls React Refresh: edited
  components re-render and keep their state (hooks that changed remount
  that component). An edit that reaches a module without importers (the
  entry), a changed dependency or asset, or an unknown version reloads the
  whole bundle instead. Build errors, and errors thrown while re-running a
  module, go to `__jsui_native.devError(text)` (the overlay); the next good
  update clears it with `devError(null)` and retries what failed. A render
  error unmounts the root as usual; the fix mounts it again. `module.hot`
  offers `accept()`, `dispose(cb)`, `data`, `decline()` and `invalidate()`.
* **JS debugger.** Dev-mode runtimes built with `JSUI_DEBUGGER` (the native
  default) debug from Chrome DevTools and VS Code through the dev server:
  `X-Jsui-Inspect: 1` on the bundle makes the runtime attach a CDP agent
  (`third_party/quickjs-cdp`, on the engine's debug traps) to each JS
  context before the bundle is parsed, and tunnel CDP over HTTP long-polls
  to `/__jsui/inspect/*` on threads of its own (`src/js/inspector.cpp`), so
  it answers while JS is paused. `jsui dev` lists the runtimes in
  `/json/list` and relays each frontend's WebSocket. See
  [DEBUGGING.md](DEBUGGING.md).
* **Reload** tears down the JS context, the Document, in-flight loads and the
  image cache, then reloads the bundle URL. The last good frame (recorded as
  an `SkPicture` before teardown) stays on screen until the new bundle renders,
  and keeps showing if the new bundle throws.
* **Error overlay.** While `jsui_last_error()` is set, a red-tinted panel with
  the message and stack (monospace) is painted over the frame. Escape
  (`jsui_send_key`) dismisses it; the next successful load (or hot update)
  clears it. A bundle URL that answers with an error status shows the
  response body too (`jsui dev` sends the build error with its 503).

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

Images decode in `ImageCache` (`src/render/image_cache.*`). The codec reads
the header first, and each tick has an inline decode budget of 1 ms of
measured decode time (`inlineBudgetUs`): an image decodes on the runtime
thread if its estimated cost (pixels times `decodeNsPerPixel`, starting at
the ~17 ns/px measured for PNG / JPEG card art on an M-series Mac and
learned from the inline decodes since) still fits, where the hand-over would
only cost a frame of pop-in, and the time it took is spent. The rest go to a
pool of 1-2 decode threads (a quarter of the cores, started on the first
such image). So a full-screen background (over the budget on its own) or a
screen of card art (31 images, 6.3 ms on one cold frame of the showcase's
inventory at first, still 4.3-4.6 ms with a 256k px budget) costs a frame
about 1 ms of decoding. A pool image's
entry stays `Loading` until `jsui_tick` publishes the raster image (after
the loader's completions), then `generation` is bumped and Images sized by
it are re-measured, as for any load. `data:` URIs follow the same rule.
`jsui_pending_loads()` and `jsui_wants_frame()` count the decodes in flight,
so `jsui-render`, `jsui-record` and the reference tests wait for them.
On the web the estimate starts at 60 ns/px (the codec in wasm is a few
times slower) and the browser decodes the rest instead of a pool: the wasm glue
(`jsui_js_image_decode` in `image_cache.cpp`, so page and worker mode alike)
posts the bytes to one or two decode Workers made from a Blob URL, which run
`createImageBitmap`, read the pixels back through an `OffscreenCanvas` 2D
context and premultiply them; the runtime's thread only copies the RGBA into
the `SkData` a raster `SkImage` wraps (`jsui_emscripten_image_begin` /
`_end`), and the next tick publishes it like a pool decode. Without Workers
(or a CSP that refuses blob: workers) the same runs on the runtime's thread
between ticks, the decode itself still off it; when the browser cannot
decode an image, Skia's codec does it at publish time; without
`createImageBitmap` (Node: the headless CLI) images decode inline as before.
The result is always a raster image, so the CPU raster, layout, ShaderView
and recorded frames see it like any other; on WebGPU Graphite uploads it once
through its image provider. `image_decodes` / `image_decode_us` count these
decodes on the tick that publishes them (the time the pool thread or the
browser took).

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

## @orktes/jsui-react bridge usage (JS side)

Details of how the JS package uses the bridge, for native implementers:

* `__events` tokens sent by `@orktes/jsui-react` (matched by substring natively):
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
  `updatePropsKeepsStyle` (`setNativeProps` of other props passes
  `keepStyle`; otherwise it reads the style back with `getProps`),
  `animated` (native Animated driver; otherwise `useNativeDriver` uses the JS driver),
  `viewport()` (native: returns `{width, height, scale}`), `focus`, `blur`,
  `scrollTo`, `scrollToEnd` (otherwise measured in JS), `flashScrollIndicators`,
  `measure`, `postMessage`, `loadFont`, `loadFontData`,
  `unloadFont`, `getFontFamilies`, `setFontFallbacks`, `setDefaultFontFamily`, `getImageSize(uri)`,
  `prefetchImage(uri)`, `reload`.
* Events beyond input: `{type: 'resize', width, height, scale}` (sent by
  `jsui_set_viewport` when the size changes; drives Dimensions);
  `{type: 'load', target, source: {uri, width, height}}` (resolved URL, image
  pixels) / `{type: 'error', target, error}` / `{type: 'progress', target,
  loaded, total}` on `Image` nodes (`onLoad` / `onError` / `onProgress`;
  `@orktes/jsui-react` adds `onLoadStart` when the source changes and `onLoadEnd`
  after load or error). Sent once per source, always asynchronously like
  React Native: on a tick after the one that set the source (after its
  timers and passive effects), also for data URIs and images already cached
  at mount; `progress` only for images that had to be loaded.
* `globalThis.__jsui_reportError(error)` (else the standard `reportError`): uncaught
  React render errors are passed to it; otherwise they are rethrown from a
  `setTimeout` so the engine reports them.
* `globalThis.__jsui_assets[url] = {width, height, scale}` is filled by
  bundled image modules (`@orktes/jsui-cli`); `Image` uses it for intrinsic size.
* Host-only props sent with nodes: `Modal` content carries
  `accessibilityViewIsModal: true` (focus scope); disabled pressables carry
  `disabled: true` and `focusable: false`; `ScrollView` carries `horizontal`.
* Dev server endpoints besides `/bundle.js` and `/__jsui/version`:
  `/__jsui/version?since=<token>` long-polls until the token changes;
  `/__jsui/error` returns the current build error text; `/__jsui/hmr?from=<version>`
  serves hot updates; `/__jsui/session/*` and `/__jsui/shared/*` (with the
  `/shared` page) are the shared session (see "Agent tools and the shared
  session"); `/bundle.js` carries an `X-Jsui-Version` header.
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
* `tests/native/react_e2e_test.cpp` drives a real `@orktes/jsui-react` bundle through
  the C API (mouse, keys, text, gamepad, viewport, host messages, Animated);
  `npm test -w @orktes/jsui-cli` builds the bundle and runs it.
