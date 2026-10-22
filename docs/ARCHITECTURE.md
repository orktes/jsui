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
| `include/jsui/jsui.h` | Public C API (embedding, input, resources, messages) |
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

* **Same painter.** `Runtime::renderCanvas()` lays out, refreshes the
  incremental paint records and paints the full frame (dev overlay and frozen
  frame included) into the Ganesh surface. The CPU path is untouched and stays
  the default; a later `jsui_render_rgba_incremental()` repaints fully.
* **Full repaints.** The GPU path does not clip to the damage region: GPU
  targets (swapped default framebuffers in particular) are not guaranteed to
  keep the previous frame, and a GPU redraw of a whole HUD costs less than
  a CPU raster + upload. `jsui_needs_redraw()` still says when a frame is
  needed.
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
  `web/dist-webgl/jsui.{js,wasm}`; the default WASM build stays CPU-only and
  its `jsui_gpu_*` entry points return failure.
* **Viewer / web.** `jsui-viewer --gpu` renders into the SDL GL window's
  default framebuffer (`--screenshot` reads it back, and fails if the frame
  is blank). On macOS it asks SDL for a 3.2 core, forward-compatible
  profile (`--gl-core`, default there): the default legacy 2.1 context only
  has GLSL 1.20, which Skia's shaders cannot use (`gl_VertexID`). Ganesh
  detects core profiles (`GL_CONTEXT_PROFILE_MASK`) and binds its own vertex
  array object, since core profiles have no default one. In the browser,
  `createJsui({ canvas, renderer: 'gpu', moduleUrl: '.../dist-webgl/jsui.js' })`
  registers the canvas' WebGL context with emscripten and draws through
  Skia; without a GPU module it falls back to the CPU raster + WebGL blit.
* **Tests.** `tests/native/gpu_test.cpp` makes an EGL context (Mesa's
  surfaceless platform / llvmpipe works headless), on desktop GL, a 3.2 core
  profile (as on macOS), GLES 3 and GLES 2, or on macOS a headless CGL 3.2
  core context, renders scenes through the GPU path and compares with the
  CPU output; without a context it prints SKIP (`JSUI_GPU_TEST_REQUIRE=1`, set in CI,
  makes that a failure). `JSUI_WEB_DIST=dist-webgl node web/test/browser.test.mjs`
  runs the browser tests against the GPU module, including GPU-vs-CPU pixel
  comparisons.

## JS bridge contract

Native globals (installed by `JsEngine`):

| Global | Meaning |
| --- | --- |
| `__jsui_native.rootId` | id of the root container node |
| `__jsui_native.devMode` | `true` when the runtime runs with `cfg.dev_mode` |
| `createNode(type) → id` | `type` ∈ `View`, `Text`, `RawText`, `Image`, `ScrollView`, `TextInput` |
| `setProps(id, props)` | replaces all props; `props.style` is a **flattened** style object; functions are dropped; `props.__events` lists event names the element listens to (e.g. `["press","pointerMove","layout"]`) |
| `patchStyle(id, partialStyle)` | animation fast path: merges keys into the node's current `props.style` (a `null`/`undefined` value removes the key), keeps all other props, and reapplies via `Node::setProps` (which skips relayout when only paint keys such as `transform`, `opacity`, colors changed). Use it per frame from `requestAnimationFrame` instead of re-rendering through React. |
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
  timestamp: <ms>,
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
  `jsui-viewer [--dev] [--gpu [--gl-core]] [--width W --height H] <bundle path or URL>`; F5
  reloads; `--frames N --screenshot out.png` runs headless
  (`SDL_VIDEODRIVER=dummy`; `--gpu` needs a GL-capable display, e.g. `xvfb-run`
  with Mesa). `-DJSUI_GPU=OFF` builds without the GPU backend.
* WASM: `scripts/build-skia-wasm.sh` (once), then `emcmake cmake -S . -B build-wasm`.
  The `web/` host loads `jsui.wasm`, forwards DOM input, blits frames to a canvas.
  `scripts/build-wasm.sh` does all steps (emsdk 3.1.44 → Skia m124 → `web/dist/jsui.{js,wasm}`);
  `WEBGL=1 scripts/build-wasm.sh` builds the GPU variant into `web/dist-webgl/`.
  In the page: `const jsui = await createJsui({ canvas, bundleUrl })` from
  `web/jsui-web.js`; `jsui.on('message', (name, data) => …)`, `jsui.postMessage(name, data)`.
  The browser loader backend is `fetch()` (`src/platform/loader_emscripten.cpp`).
  Tests: `node web/test/node-smoke.mjs`, `node web/test/browser.test.mjs` (Playwright).
* JS: `npm install` at the repo root (npm workspaces), `npx jsui build app.jsx`
  or `npx jsui dev app.jsx` (serves `bundle.js` and a reload endpoint).

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
case writes `expected/actual/diff` images and an HTML report.

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
  `{type: 'load', target, source: {width, height}}` / `{type: 'error', target,
  error}` on `Image` nodes (`onLoad` / `onError`).
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
