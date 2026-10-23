# @orktes/jsui-web

Runs the JSUI game-UI runtime (QuickJS + Yoga + Skia, compiled to
WebAssembly) on a `<canvas>`. Pair it with `@orktes/jsui-cli`, which bundles your
React Native–style app into the single script this host loads.

```sh
npm install @orktes/jsui-web
npx @orktes/jsui-cli build app.jsx -o public/bundle.js
```

```js
import { createJsui } from '@orktes/jsui-web';

const jsui = await createJsui({ canvas: document.querySelector('canvas'), bundleUrl: './bundle.js' });
jsui.on('message', (name, data) => console.log(name, data)); // Host.postMessage(...) from the UI
jsui.postMessage('hud', { hp: 42 });                         // Host.handle('hud', fn) in the UI
jsui.setState('gold', 1250);                                 // useHostState('gold') in the UI
jsui.setBuffer('enemies', new Float32Array(positions));      // every frame, no JSON: useHostBuffer / useHostBufferValue
const view = jsui.mapBuffer('enemies', Float32Array, 90);    // or write in place (zero copy) ...
jsui.commitBuffer('enemies');                                // ... then publish
jsui.bindVideo('trailer', videoElement);                     // <HostTexture name="trailer" /> in the UI
```

## Several root views (surfaces)

Like React Native's root views, one runtime (one bundle, JS context, shared
state, fonts and images) can show several components, each on its own canvas
with its own props, size, input and focus. Register them in the bundle and
place them anywhere in the page:

```js
// app.jsx
AppRegistry.registerComponent('Clock', () => Clock);
AppRegistry.registerComponent('Cart', () => Cart);
```

```js
const jsui = await createJsui({ bundleUrl: './bundle.js' });  // no canvas: only the widgets below
const clock = jsui.createSurface(document.querySelector('#clock'), {
  component: 'Clock',
  initialProps: { tz: 'UTC' },
  autoSize: true,          // the canvas takes the component's laid-out size
});
const cart = jsui.createSurface(document.querySelector('#cart'), { component: 'Cart' });
clock.setProps({ tz: 'CET' });  // re-renders, keeping its state
cart.destroy();
```

- `createSurface(target, options)` takes a `<canvas>` or an element (it gets a
  canvas filling it). Options: `component` (the AppRegistry key),
  `initialProps`, `width` / `height` (CSS px), `autoSize` (`true`, `'width'`
  or `'height'`), `renderer`, `pixelRatio`, `autoFocus` (default false),
  `trapTab` (default false, so Tab moves on through the page).
- The surface object has `setProps(props)`, `run(component, props)`,
  `destroy()`, `rootTag`, `canvas` and `stats`. In the UI, `RootTagContext`
  and `useWindowDimensions()` give the surface's root tag and size.
- `createJsui({ component, initialProps })` picks the main canvas' component
  (default: the bundle's first registered one); `jsui.run()` / `jsui.setProps()`
  change it later. Without `canvas` the main surface is not shown.
- The runtime ticks once per frame and each canvas repaints only what changed.
  Surfaces have a transparent background. Keyboard input goes to the surface
  whose canvas was clicked last; gamepads to the one that has keyboard focus.
- Surfaces are GPU-drawn by default and share the runtime's GPU context, so a
  page can hold any number of them without running into the browser's cap of
  about 16 live WebGL contexts. On WebGPU one Skia Graphite context on the
  page's device draws every surface, and each canvas keeps only its own
  `GPUCanvasContext`. On WebGL one offscreen WebGL context draws each surface
  and the frame is copied into that canvas' 2D context (`renderer` then reads
  `gpu-webgl2-shared`). Each surface keeps its own retained frame, so small
  changes still repaint only their damage.
- `renderer: 'webgl'` gives a surface a WebGL context of its own (which counts
  against the cap); `'cpu'` and `'2d'` use the CPU raster.

## Host textures (video, canvases, render targets)

JSUI does not play video. It shows pictures the page makes itself: a
`<video>`, WebCodecs `VideoFrame`s, a `<canvas>` your 3D scene draws into, a
webcam, an image. Name a texture, keep it current, and show it in the UI by
that name:

```js
const video = Object.assign(document.createElement('video'), {
  src: 'https://example.com/trailer.webm', crossOrigin: 'anonymous', muted: true, loop: true, playsInline: true,
});
const unbind = jsui.bindVideo('trailer', video);   // one upload per presented frame
video.play();

jsui.setTexture('minimap', minimapCanvas);         // whenever its picture changes
jsui.setTexture('cam', frame); frame.close();      // VideoFrame: yours to close
jsui.removeTexture('minimap');
```

```jsx
import { HostTexture, useHostTexture } from 'react-native';

const size = useHostTexture('trailer');            // { width, height } once set, else null
<HostTexture name="trailer" resizeMode="cover" style={{ width: 640, aspectRatio: 16 / 9, borderRadius: 16 }} />
```

- `setTexture(name, source, { opaque })`: `source` is a `VideoFrame`,
  `HTMLVideoElement`, `HTMLCanvasElement`, `OffscreenCanvas`, `ImageBitmap`,
  `HTMLImageElement`, `ImageData` or raw pixels
  (`{ data: Uint8Array | Uint8ClampedArray, width, height }`, straight RGBA);
  its current picture is uploaded. Calling
  it again at the same size refills the same texture and repaints only the
  views showing it (no layout, no JS); a new size re-lays them out and
  updates `useHostTexture`. Returns `false` while the source has no picture
  (a video before its first frame) or when the upload failed (logged).
  `opaque` (default: `true` for video) ignores alpha.
- `bindVideo(name, video, options)` keeps a texture current from a video
  element (`requestVideoFrameCallback`, else polled per display frame,
  paused frames and seeks included) and returns a function that stops it.
  Playback (`play()`, `pause()`, `muted`, `currentTime`) stays on the element.
- `removeTexture(name)`; `textureBackend` says where textures go now.
- Textures belong to the runtime: every surface can show them, and they
  survive bundle reloads.
- Uploads use the renderer in use: on WebGPU a `GPUTexture` per name on the
  page's device (`copyExternalImageToTexture`, sampled in place by Skia); on
  WebGL a texture in the views' context; on the CPU raster, or when views
  draw with different contexts (a main canvas with its own WebGL context next
  to surfaces on the shared one), a CPU copy. Renderer fallbacks, lost
  contexts and new devices re-upload from the last source by themselves
  (except `VideoFrame`s, which are not kept: the next `setTexture` brings
  them back).
- GPU uploads of a cross-origin video or image need CORS: set
  `crossOrigin = 'anonymous'` and serve it with `Access-Control-Allow-Origin`.
  Browsers only autoplay muted video; unmute after a click.

## Input pass-through (HUD over a game)

With `passThrough: true` (on `createJsui` or `createSurface`) the canvas is a
layer over the page: input the UI doesn't capture reaches whatever is under it
(the game's canvas or DOM), and input it consumes doesn't. "Captures" means
something in the UI handles it (onPress, pointer / hover / responder handlers,
TextInput, a ScrollView that can scroll), not that the point is painted, so a
click on an inert panel goes through; give a blocking backdrop a handler
(`onStartShouldSetResponder={() => true}`).

```js
const hud = await createJsui({ canvas: hudCanvas, bundleUrl: './hud.js', passThrough: true });
gameCanvas.addEventListener('pointerdown', shoot);   // only presses the HUD didn't take
hud.hitTest(x, y);                                   // HIT_POINTER | HIT_SCROLL, 0: passes through
hud.gamepadConsumed(0, 0);                           // the UI took pad 0's A press
```

- The canvas gets `pointer-events: none` (`auto` while the mouse is over
  something the UI captures, for its cursor). Window capture-phase listeners
  give presses inside the canvas to the UI first; a consumed press, its
  moves, release and click are stopped (`stopImmediatePropagation`) before the
  page sees them, others continue to their target untouched. A drag the page
  started stays the page's over UI. Touch events follow the hit test, keys the
  UI consumed stop at its keyboard sink.
- The keyboard sink is focused only by presses the UI consumed, and
  `autoFocus` / `trapTab` default to false. Gamepads are polled by both: check
  `gamepadConsumed(pad, button)` / `gamepadAxisConsumed(pad, axis)`.
- Overlapping pass-through canvases: the one created first gets first pick.
- Without `passThrough` the canvas receives events like any element; pointer,
  wheel and key events the UI didn't consume are not `preventDefault()`ed and
  bubble on as usual.

## Fonts

The runtime ships Inter. Fonts the page registers are shared by every surface
of the runtime, and text already on screen relayouts with them:

```js
await jsui.loadFont('Display', './fonts/display.woff2');        // fetch + register (URL relative to the page)
await jsui.loadFont(null, './fonts/NotoSansJP.otf');            // family name from the file
jsui.registerFont('Display', bytes, { weight: 700, style: 'italic' }); // ArrayBuffer / typed array
jsui.setFontFallbacks(['Noto Sans JP', 'Noto Color Emoji']);    // for glyphs the styled font lacks
jsui.setDefaultFontFamily('Display');                           // text whose style names no fontFamily
jsui.unregisterFont('Display');
```

- `registerFont(family, bytes, weight?, italic?)` and
  `loadFont(family, url, weight?, italic?)` take TTF, OTF, TTC / OTC (every
  face), WOFF and WOFF2. `family` `''` / `null` uses the family name in the
  file. Weight (1..1000) and italic (`true` / `false`, or `'italic'` /
  `'normal'`) override what the file says; omitted, both come from the file
  (a variable font with a `wght` axis registers its whole weight range).
  The third argument can instead be `{ weight, style }`, and
  `registerFont(bytes, options?)` leaves out the family. Older calls such as
  `registerFont('Inter', bytes, 700, false)` work unchanged.
- `registerFont` returns `true`, or `false` for data that is not a font. The
  bytes are copied. `loadFont` returns a `Promise<boolean>`: `false` (and a
  `'log'` warning) when the fetch fails or the data is not a font. URLs
  resolve against the page, as `bundleUrl` and `loadUrl` do; cross-origin
  fonts need CORS.
- `unregisterFont(family)` removes every face of the family and returns `true`
  if it was registered.
- `setFontFallbacks(families)` takes an array or a comma-separated string,
  tried in order; `null` / `[]` clears it. After the list, every registered
  font is searched for a missing glyph anyway.
- `setDefaultFontFamily(family)` sets the family for text whose style names
  none (`'Inter'` until you change it).
- The UI can also load fonts itself (`Fonts.load(family, url)` in the bundle,
  URLs relative to the bundle); both kinds end up in the same registry.

## Worker mode

`createJsui({ worker: true, ... })` runs the runtime, its frame loop and its
rendering in a dedicated Worker. The canvas is handed over with
`transferControlToOffscreen()`, and the worker draws into it with its own
`requestAnimationFrame`. The page's main thread keeps only the work that needs
the DOM: input listeners, canvas size and `devicePixelRatio`, the cursor,
gamepad polling and video capture. A page whose own main-thread work (a game,
a React app) competes with the UI for frame time gets that time back. The
showcase takes `?worker=1`.

```js
const jsui = await createJsui({ canvas, bundleUrl: './bundle.js', worker: true });
jsui.postMessage('hud', { hp: 42 });             // the same API, posted to the worker
const gold = await jsui.getState('gold');        // answers from the runtime: Promises
const view = jsui.mapBuffer('enemies', Float32Array, 90);
view.set(positions); jsui.commitBuffer('enemies');
```

The worker loads `jsui-web.js` itself (`new Worker(import.meta.url, { type: 'module' })`)
and `jsui-worker.js` next to it, so serve both from one place, as files of
their own. A bundler that inlines `jsui-web.js` into an app bundle would make
the worker load the whole app. The WASM modules are found as in main-thread
mode. Options must be cloneable: function-valued options are dropped.

**Fallback.** Where `Worker`, `OffscreenCanvas` or
`transferControlToOffscreen` is missing, or the worker fails to load or start
the runtime, `createJsui` logs one warning
(`[jsui] worker mode unavailable (...): running on the page's main thread`)
and returns an ordinary main-thread runtime. A canvas whose control already
went to the worker is replaced by a clone in the page. `jsui.worker` is the
`Worker` in worker mode, and is absent otherwise.

**What returns a Promise.** A call that needs an answer from the runtime
cannot block the page's thread, so in worker mode it returns a Promise of the
value it returns in main-thread mode:

| Call | Resolves with | Why it can't stay synchronous |
| ---- | ------------- | ----------------------------- |
| `getState(key)` | the value (`undefined` when unset) | the state lives in the runtime, and the UI can change it (`HostState.set`) |
| `setState`, `patchState` | `true` if the value changed | the comparison happens in the runtime |
| `setBuffer`, `commitBuffer` | `true` if the contents changed | as above; the data is copied to the worker |
| `removeBuffer`, `removeTexture` | `true` if it existed | the runtime owns them |
| `setTexture` | `true` once uploaded | the upload happens in the worker (and `<img>` / canvases are snapshotted with `createImageBitmap`, which is async) |
| `run`, `setProps` (main surface and surfaces) | `false` for invalid props | the runtime parses them |
| `loadSource`, `registerFont` | `true` on success | the runtime evaluates / parses them |
| `unregisterFont` | `true` if the family was registered | the font registry is in the runtime |
| `resolveCall`, `rejectCall` | `0`, or `-1` for an unknown call | the call table is in the runtime |
| `getPerf(reset)` | the perf object | the counters are in the worker |
| `readPixels()` | the pixels | the frame is in the worker (`readPixelsAsync` is the same) |
| `surface.ready` (new) | the surface, once the worker made it | `surface.rootTag` is `undefined` until then |

Already asynchronous in both modes: `callUi`, `whenIdle`, `readPixelsAsync`, `loadFont` (the worker fetches it).
Fire-and-forget calls keep their shape: `postMessage`, `loadUrl`, `reload`, `devUpdate`,
`setFontFallbacks`, `setDefaultFontFamily`,
`start`, `stop`, `frame`, `setProfiling`, `destroy`, `surface.destroy`.

These stay synchronous because the page keeps a copy: `renderer`,
`textureBackend`, `lastError`, `stats` (updated with each `'frame'` event, and
by `whenIdle` / `readPixels`), `width` / `height` / `pixelWidth` /
`pixelHeight` (measured on the page), `hitTest` and `gamepadConsumed` /
`gamepadAxisConsumed` (see below). `jsui.module` is `null`: the WASM module is
in the worker. Event listeners (`on('message' | 'call' | 'state' | 'error' |
'renderer' | 'log' | 'frame', ...)`) work as before. `'log'` and `'frame'` are
only sent while something listens.

**Buffers.** `mapBuffer(key, Type, count)` returns a typed array on the page's
thread. It is the same array for the same key, type and count, and holds what
was written last. `commitBuffer(key)` copies it to the worker as a transferred
buffer, so the page can keep writing into the array straight away. It is a
copy per commit, not shared memory: worker mode does not need
`SharedArrayBuffer` or cross-origin isolation (GitHub Pages has neither).
`setBuffer` copies too.

**Textures.** `setTexture` takes the same sources as before plus raw pixels
`{ data: Uint8Array | Uint8ClampedArray, width, height }` (straight RGBA). A
`VideoFrame` is cloned and an `ImageData` / `ImageBitmap` is structured-cloned.
Raw pixels are copied. With `{ transfer: true }` the source itself is
transferred, and the caller's object is unusable afterwards. A `<video>`
becomes `new VideoFrame(video)`, and `<img>` / canvases become an
`ImageBitmap`. `bindVideo` captures on the page's thread: it uses
`requestVideoFrameCallback`, or polls every display frame where that is
missing. Each presented frame goes to the worker as a transferred `VideoFrame`,
with at most two in flight, so a busy worker skips frames rather than queueing
them. The worker closes each frame after the upload. The same CORS rules apply.

**Input and pass-through.** The page's listeners post pointer, wheel, touch,
key, IME and focus events with their timestamps. Moves and wheels are
coalesced per frame on both sides. The page has to decide immediately whether
to `preventDefault()` an event and, with `passThrough`, whether it reaches the
page below. It decides from a **hit map**: the worker posts the rects of what
the UI captures (`jsui_hit_regions`) whenever they change (after a repaint, a
resize, and at least every 30 frames), and `hitTest(x, y)` answers from the
last one. The map is the one from the last frame that changed what is under
the pointer, delivered one `postMessage` later. It reaches the page about when
that frame's pixels do, and about one frame after the input that changed it.
In headless Chromium at 60 Hz, the page has the new map a median 16 ms after
a wheel event, and 1.5 ms after the start of the frame that made it.
`jsui.hitMapStats` counts the updates, lookups and the times
the runtime disagreed (`misses`), and keeps latency samples. Things to know:

- A wheel over a ScrollView is assumed consumed even at its scroll end, where
  main-thread mode lets it scroll the page.
- A key is assumed consumed if the runtime consumed the same key and
  modifiers last time. Before its first answer, navigation keys (Tab, Enter,
  Escape, Backspace, Delete, arrows, Home / End, Page Up / Down) are assumed
  consumed. Other keys, F-keys and Ctrl / Meta shortcuts are assumed not
  consumed. The prediction decides `preventDefault()` for navigation keys and
  shortcuts and, with `passThrough`, which keys reach the page.
- With `trapTab: false`, the Tab that leaves the UI's focus chain is kept,
  because the previous Tab moved focus inside the UI. The next Tab moves page
  focus on. Focusing the UI again resets this.
- `gamepadConsumed` / `gamepadAxisConsumed` answer for the last frame the
  worker ran, one frame behind main-thread mode.

**Frame loop.** The worker's own `requestAnimationFrame` times frames. If the
worker has none, or its callbacks never come (checked for one second while
the page is visible), the page's rAF posts each frame instead.
`workerLoop: 'main'` forces that. Frame times and event timestamps are on the
page's clock in both cases.

**Renderers.** Worker mode uses the same order as main-thread mode: WebGPU in
the worker, then Skia on WebGL on the OffscreenCanvas, then the CPU raster.
GPU-loss handling also stays the same. A lost WebGL context is restored, or
the view falls back to the CPU on a fresh canvas, which the page clones and
transfers. In both modes, no GL work runs once the context reports itself
lost, even before its `webglcontextlost` event arrives (WebKit lets a frame
run in between). A lost WebGPU device is replaced. The no-GPU warning is logged as
before. Text is measured and drawn by Skia in the WASM module, so it works
the same in the worker.

**Browser support.** Chrome and Edge: everything (OffscreenCanvas with WebGL
and worker rAF since 69, WebGPU in workers since 113). Safari: OffscreenCanvas
since 16.4, WebGL on it in workers since 17. Firefox: OffscreenCanvas since
105. Where a browser lacks WebGL or WebGPU in workers, the runtime falls down
the renderer chain. Where it lacks worker rAF, the page times the loop.

## Profiling

`jsui.setProfiling(true)` times every frame phase, and `jsui.getPerf(reset = true)`
returns what the frames since the last reset cost, the same numbers as
`jsui_get_perf` in the C API:

- exclusive µs per phase: `js_us`, `commit_us`, `animated_us`, `style_us`,
  `layout_us`, `input_us`, `paint_us`, `gpu_submit_us`, `other_us`;
- work counters: `bridge_calls`, `layout_passes`, `text_measures`,
  `paragraph_builds`, `image_decodes` / `image_decode_us`, `pipeline_compiles`.

A runtime's surfaces share its switch and totals. Browsers round
`performance.now()` to about 100 µs on pages that are not cross-origin
isolated, so read phase times as averages over many frames.

## WASM modules

The package ships three builds of the runtime. `createJsui()` loads the best
one the browser can use from the directory next to `jsui-web.js` (override it
with `moduleBase`, or pin one with `moduleUrl`):

| Directory     | Backends                                     | Picked when                 |
| ------------- | -------------------------------------------- | --------------------------- |
| `dist-gpu/`   | Skia Graphite on WebGPU, Ganesh on WebGL, CPU | a WebGPU device is available |
| `dist-webgl/` | Skia Ganesh on WebGL2 / WebGL 1, CPU         | WebGL is available          |
| `dist/`       | CPU raster (blitted with WebGL or Canvas2D)  | fallback                    |

Each directory holds `jsui.js` (the Emscripten loader) and `jsui.wasm`.
Bundlers must copy them as static files rather than inline them: serve
`node_modules/@orktes/jsui-web/` (or copy `dist*/` next to your built
`jsui-web.js`), or set `moduleBase` to wherever you host them. The `.wasm`
files should be served as `application/wasm`.

`renderer` (`'auto'`, `'gpu'`, `'webgpu'`, `'webgl'`, `'cpu'`, `'2d'`) forces
a backend; the chosen one is `jsui.renderer`. All options are documented on
`JsuiOptions` at the top of `jsui-web.js`.

`index.html` and `demo/` are a self-contained demo page
(`?bundle=<url>`, `?dev=1` for live reload with `jsui dev`).

## Status

0.x: the API may change between minor versions. Source, issues and the C
embedding API: <https://github.com/orktes/jsui>.
