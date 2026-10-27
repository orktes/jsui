# @orktes/jsui-web

Runs the JSUI game-UI runtime (QuickJS + Yoga + Skia, compiled to
WebAssembly) on a `<canvas>`, with the app's JS on the page's own engine
(the default) or on QuickJS (`js: 'quickjs'`). Pair it with `@orktes/jsui-cli`, which bundles your
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
jsui.audio.setVolume(0.8);                                   // Sound.play / useMusic / <UISounds> play through Web Audio
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
  Polling uploads when the decoded frame count or `currentTime` moved.
  While playing, a count that ticks frame by frame (Chrome for a visible
  video; a tiny hidden one jumps a few frames at a time, and WebKit's H.264
  count ~45 frames every 1.5 s) decides alone: one upload per video frame,
  not per display frame.
  Playback (`play()`, `pause()`, `muted`, `currentTime`) stays on the element.
  Playwright WebKit 26 on macOS loses every WebGL context of the page right
  after a frame callback on a playing video (with the runtime in a worker it
  can first present nothing for about 6 s), so WebKit (`navigator.vendor`:
  Safari and every iOS browser) always polls. Elsewhere, if one of the
  runtime's WebGL contexts is lost within a second of a bound video starting
  to play, every bound video of the page switches to polling, and later binds
  start there (logged once).
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
  A source the browser refuses (`SecurityError`) is logged once and not tried
  again until its URL changes; a texture uploaded before it (a poster) stays.
  Browsers only autoplay muted video; unmute after a click.

## Sharing the game's WebGPU device (Three.js, Babylon, your own)

A WebGPU game can hand JSUI its own `GPUDevice`. UI and game then trade GPU
textures directly, in both directions, with no copies and no second canvas:

```js
import * as THREE from 'three/webgpu';
const renderer = new THREE.WebGPURenderer();
await renderer.init();

const jsui = await createJsui({ device: renderer.backend.device, bundleUrl: './ui.js' });

// 1. The UI inside the game: a surface rendered into a GPUTexture.
const terminal = jsui.createTextureSurface({ component: 'Terminal', width: 800, height: 500, pixelRatio: 2 });
// Three.js' WebGPU renderer neither flips nor sRGB-decodes an rgba8unorm
// ExternalTexture: the color node does both (alpha is premultiplied).
const { sRGBTransferEOTF, texture, uv, vec4 } = THREE.TSL;
const map = texture(new THREE.ExternalTexture(terminal.texture), uv().flipY());
const color = vec4(sRGBTransferEOTF(map.rgb.div(map.a.max(1e-4))).mul(map.a), map.a);
screenMesh.material = new THREE.MeshBasicNodeMaterial({ colorNode: color });
terminal.on('texture', (t) => { map.value = new THREE.ExternalTexture(t); });  // after setSize()

// input: raycast the screen, forward the event at the hit's UV
canvas.addEventListener('pointermove', (e) => {
  const hit = raycaster.intersectObject(screenMesh)[0];
  if (hit) terminal.dispatchEvent(e, hit.uv.x * terminal.width, (1 - hit.uv.y) * terminal.height);
  else terminal.pointerLeave();
});

// 2. The game inside the UI: a render target shown with <HostTexture name="minimap" />.
const minimap = new THREE.RenderTarget(256, 256);
renderer.setRenderTarget(minimap); renderer.render(scene, topCamera); renderer.setRenderTarget(null);
jsui.setTexture('minimap', renderer.backend.get(minimap.texture).texture);   // each time it was redrawn
```

- `createJsui({ device })` draws with that device instead of requesting one
  (it needs `dist-gpu/`; the main canvas, if any, uses it too). Not in worker
  mode: a `GPUDevice` cannot move to a worker, so `worker: true` with a
  device runs on the main thread (one warning).
- `createTextureSurface({ component, initialProps, width, height, pixelRatio,
  texture, format, autoRender })`: a surface of the runtime (like
  `createSurface`) whose frames go into `surface.texture` (`rgba8unorm`
  unless `format` says `bgra8unorm` / `rgba16float`), made by the surface and
  replaced on `setSize()` (`'texture'` event). Or pass `texture`, a
  `GPUTexture` of the game's (`RENDER_ATTACHMENT | TEXTURE_BINDING`), or call
  `renderTo(texture)` yourself with `autoRender: false` (from the game loop,
  after the runtime's `'frame'`). The texture keeps the last frame, so a
  frame repaints only what changed, straight into it (no retained copy); a
  frame where nothing changed renders nothing. `'render'` fires after each
  new frame, `surface.version` counts them. The texture holds premultiplied,
  sRGB-encoded colors, top row first. Renderer-wide tone mapping (Three.js
  applies it to the whole frame in its output pass) shifts the UI's colors
  too: see site/three/main.js for a scene without it.
- Input comes from the page: `dispatchEvent(e, x, y)` takes pointer, mouse,
  wheel and keyboard events (a printable `keydown` also types its text), or
  call `pointerDown / pointerMove / pointerUp / pointerLeave / wheel / key /
  text` with logical surface coordinates (`u * width`, `(1 - v) * height` for
  a hit's UV). Each returns whether the UI consumed it. `surface.cursor` and
  the `'cursor'` event give the CSS cursor the UI wants. `focus()` points
  gamepads at the surface.
- `setTexture(name, gpuTexture)`: a texture of the shared device is sampled
  in place (`rgba8unorm`, `bgra8unorm`, `rgba8unorm-srgb` or `rgba16float`,
  `TEXTURE_BINDING` usage; the sRGB and half-float ones are read as linear
  colors, as a Three.js render target in `SRGBColorSpace` or `HalfFloatType`
  holds them). Call it again after the game redrew it: that repaints just the
  views showing it. Queue order keeps it consistent: what the game submitted
  before JSUI's frame is what the UI shows.
- Device loss: JSUI never replaces a device it was given. `'devicelost'`
  fires once; make a new device (and your textures on it), then
  `jsui.setDevice(device)`: views, texture surfaces (their own textures are
  made again: `'texture'` event) and copied host textures move over;
  `setTexture` the game's `GPUTexture`s and `renderTo` its targets again.
- `jsui.device` is the runtime's device (the page's, or the one JSUI
  requested), so a page can also use JSUI's own device for its WebGPU work.

## Audio (Web Audio)

The UI's sounds (`Sound.play`, `useSound`, `useMusic`, `<UISounds>`,
Pressable `sounds`; see [docs/AUDIO.md](../docs/AUDIO.md)) play through Web
Audio (`jsui-audio.js`): one `AudioContext` for the page, a gain per bus,
decoded sounds cached by URL, long files (over 2 MiB) streamed through an
`<audio>` element.

```js
jsui.audio.unlocked;                          // false until the first click / tap / key press
jsui.on('audiounlock', () => hideSoundHint());
jsui.audio.setVolume(0.5, { fade: 300 });     // the runtime's master volume
jsui.audio.muted = true;
jsui.audio.unlock();                          // from your own gesture handler ("Press any key")
jsui.audio.context;                           // the AudioContext (null before the first gesture)

createJsui({ canvas, bundleUrl, audio: false });                       // no sound
createJsui({ canvas, bundleUrl, audio: { context: myAudioContext } }); // into your own context
createJsui({ canvas, bundleUrl, audio: (cmd, audio) => {               // your own player
  // cmd: { op, voice, sound, bus, volume, pitch, pan, fadeMs, offset, loop, muted } (AudioOp)
  // every PLAY ends with audio.voiceEnded(cmd.voice, AudioStatus.FINISHED | STOPPED | FAILED);
  // every PRELOAD with audio.soundLoaded(cmd.sound, ok, durationSeconds)
} });
```

- **Autoplay**: browsers keep audio silent until a user gesture. The host
  creates / resumes the context on the first `pointerdown`, `pointerup`,
  `mousedown`, `keydown`, `touchend` or `click` anywhere on the page, and
  again when the page becomes visible. Until then one-shot sounds are dropped
  (their `ended` resolves `{ reason: 'failed' }`; those the unlocking click
  itself triggers, such as its press sound, still play), looping ones (music) wait
  and start with their fade-in once audio unlocks. **Gamepad buttons are not
  a user activation** in any browser: show a "press any key / click to
  enable sound" hint for controller players.
- Formats are whatever the browser's `decodeAudioData` takes (WAV, MP3, Ogg
  Vorbis / Opus except older Safari, AAC). Cross-origin sounds need CORS.
- In Node (`@orktes/jsui-cli` headless) the runtime is made with audio off:
  sounds end `'failed'`, preloads reject, nothing throws.

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

## Clipboard

Copies in the UI (a TextInput's Ctrl / Cmd+C or X, a `<Text selectable>`'s
Ctrl / Cmd+C) go to the page's clipboard (`navigator.clipboard.writeText`,
or a copy command where the page is not a secure context). A paste key
waits for the browser's `paste` event and the TextInput pastes the text it
carries; the page's clipboard is never read otherwise. In worker mode the
page's thread writes and reads it for the worker.

## Safe area (notch, Dynamic Island, home indicator)

On a page with `<meta name="viewport" content="..., viewport-fit=cover">` the
canvas can reach under a phone's notch, Dynamic Island, rounded corners or
home indicator. The main view measures the page's `env(safe-area-inset-*)`
where it overlaps the canvas and publishes it as shared state
`jsui.safeAreaInsets` (CSS px, updated on resize and rotation), which
`SafeAreaView`, `useSafeAreaInsets()` and `react-native-safe-area-context`
read: the same API as on iOS and Android. Without `viewport-fit=cover` the
browser keeps the page out of those areas itself and the insets stay zero.
Pass `safeArea: false` to `createJsui` to set the key yourself
(`jsui.setState('jsui.safeAreaInsets', { top, right, bottom, left })`).

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

## The page's own JS engine

By default the bundle runs on the JS engine of the page (or of the worker,
in worker mode): V8, JavaScriptCore or SpiderMonkey, JIT-compiled.
`js: 'quickjs'` runs it on QuickJS instead, compiled into `jsui.wasm` next to
the rest of the runtime, as native builds do. A page whose
Content-Security-Policy blocks `eval` gets QuickJS automatically. In the
jsui-bench scenarios a steady-state frame spends 7 to 20 times less time in
JS (`web/test/js-bench.mjs`); a first mount, mostly cold code, is 1.2 to
1.5x faster.
Layout, painting, input, native Animated and host data stay in WASM, and the
app sees the same API and globals.

```js
const jsui = await createJsui({ canvas, bundleUrl: './bundle.js' });
jsui.jsEngine; // 'host' ('quickjs' when eval is blocked or the module was built without it)
```

`jsFrameBudget` (ms, default 5) is how long the bundle's JS may run in one
frame before React yields: updates in `startTransition` render in slices
across frames instead of in one long frame. `0` turns slicing off.

The bundle still gets the runtime's own globals: `setTimeout`,
`requestAnimationFrame`, `performance.now()` and `queueMicrotask` follow the
runtime's clock (`jsui.frame(now)`), and the page's DOM, `window` and
`WebAssembly` are hidden from it. What differs from QuickJS:

- Promise callbacks (`async` / `await`, `.then`) run at the browser's own
  microtask checkpoint, after the frame that resolved them, rather than
  within it. `queueMicrotask` keeps QuickJS's timing.
- There is no watchdog or JS memory limit: a script stuck in a loop blocks
  its thread like any other script.
- It needs `eval`: under a Content-Security-Policy without
  `'unsafe-eval'` the runtime falls back to QuickJS.
- Ship bundles as `jsui build --format source` (or `combo`, whose QuickJS
  bytecode the host engine skips; `bytecode`-only bundles can't run there).
- `Intl` is the browser's (QuickJS has none).

The CLI takes the same choice: `jsui screenshot|inspect|run|dev --headless
--js host` (or `JSUI_JS=host`) runs bundles on Node's V8. See "Host JS engine"
in `docs/ARCHITECTURE.md`.

## Worker mode

`createJsui({ worker: true, ... })` runs the runtime, its frame loop and its
rendering in a dedicated Worker. The canvas is handed over with
`transferControlToOffscreen()`, and the worker draws into it with its own
`requestAnimationFrame`. The page's main thread keeps only the work that needs
the DOM: input listeners, canvas size and `devicePixelRatio`, the cursor,
gamepad polling and video capture. A page whose own main-thread work (a game,
a React app) competes with the UI for frame time gets that time back.
`createJsui` stays on the main thread unless you pass `worker: true`. The
showcase (site/) turns worker mode on by default where the worker can draw
with WebGPU (Worker, OffscreenCanvas and `navigator.gpu` in a worker), and
`?worker=0` / `?worker=1` force either mode. On a Mac Studio (Chrome, WebGPU)
that cut the showcase's main-thread time by about 95% and its median
input-to-paint latency, for 8-17% more total CPU (issue #158).

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
| `addPipelines`, `savePipelines`, `pipelineStats`, `precompilePipelines` (WebGPU) | the count / list / stats | the GPU context is in the worker |
| `surface.ready` (new) | the surface, once the worker made it | `surface.rootTag` is `undefined` until then |

Already asynchronous in both modes: `callUi`, `whenIdle`, `whenPipelinesReady`, `readPixelsAsync`, `loadFont` (the worker fetches it).
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
`requestVideoFrameCallback`, or polls every display frame on WebKit, where
that is missing, or after the context loss above (the worker tells the page
about its losses). Each presented frame goes to the worker as a transferred `VideoFrame`,
with at most two in flight, so a busy worker skips frames rather than queueing
them. The worker closes each frame after the upload. The same CORS rules apply,
with the same warning and the same rule: a refused source is not tried again
until its URL changes.

After a GPU loss (a restored WebGL context, a CPU fallback, a new WebGPU
device), the worker uploads its copies again and then tells the page, which
captures its `<img>`, canvas and `<video>` sources again and sends them, plus
the current frame of each bound video. WebKit needs this: an `ImageBitmap`
made before its GPU process was lost uploads blank after the restore,
without an error. `ImageData` and raw pixels stay in the worker's memory and
come back by themselves. An `ImageBitmap` you passed yourself cannot be made
again by the page, so a reset logs once (info) that you should set it again.
As in main mode, a texture set while the context is lost is kept and shown
after the restore.

**Input and pass-through.** The page's listeners post pointer, wheel, touch,
key, IME and focus events with their timestamps. Moves and wheels are
coalesced per frame on both sides. The page has to decide immediately whether
to `preventDefault()` an event and, with `passThrough`, whether it reaches the
page below. It decides from a **hit map**: the worker posts the rects of what
the UI captures (`jsui_hit_regions`) whenever they change (after a repaint, a
resize, and at least every 30 frames), and `hitTest(x, y)` answers from the
last one. The worker computes and posts it only while the page needs it: with
`passThrough`, while the pointer is over the canvas, on touch screens
(`navigator.maxTouchPoints > 0`), and from the first `hitTest()` call on
(that call answers from the last map, or an empty one, until the next
frame's arrives). The map is the one from the last frame that changed what is
under the pointer, delivered one `postMessage` later. It reaches the page about when
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
  worker ran, one frame behind main-thread mode. The worker starts reporting
  them on the first call, so that call answers `false`.

**Frame loop.** The worker's own `requestAnimationFrame` times frames. If the
worker has none, or its callbacks never come (checked for one second while
the page is visible), the page's rAF posts each frame instead.
`workerLoop: 'main'` forces that. Frame times and event timestamps are on the
page's clock in both cases. A frame is two callbacks in a row: the first
applies the queued input, ticks, paints and submits; the second computes the
hit map and posts the frame's events and replies, so the frame's GPU commands
leave the worker before that work runs.

**In traces.** The GPU process can look busier in worker mode: at 1080p @2x on
an M1 Ultra (Chrome, WebGPU), 1.2 ms per frame against 0.54 ms on the main
thread. Most of the difference is
`IOSurfaceImageBacking::WaitForCommandsToBeScheduled` (0.6 ms per frame, 0.003
ms on the main thread): the OffscreenCanvas frame reaches the compositor right
after the worker submitted it, and the compositor waits until Metal has
scheduled those commands before it uses the frame. That time is a blocking
wait on the GPU process' thread, not CPU work.

**Renderers.** Worker mode uses the same order as main-thread mode: WebGPU in
the worker, then Skia on WebGL on the OffscreenCanvas, then the CPU raster.
GPU-loss handling also stays the same. A lost WebGL context is restored, or
the view falls back to the CPU on a fresh canvas, which the page clones and
transfers. In both modes, no GL work runs once the context reports itself
lost, even before its `webglcontextlost` event arrives (WebKit lets a frame
run in between). WebKit can also notice the loss in the middle of a frame,
when a GL call finds its GPU process gone, and WebGL then returns `null`
from `createProgram` / `createShader`. JSUI's GL glue returns 0 for those, as
GL does, so the frame fails quietly and the context is restored as above.
Context checks run at the start of each frame, so a restore repaints in
that frame, textures included. A lost WebGPU device is replaced. The no-GPU
warning is logged as before. Text is measured and drawn by Skia in the WASM module, so it works
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
- JS garbage collection: `gc_collections`, `gc_full_collections`, `gc_us`,
  `gc_max_us` (the longest pause).

A runtime's surfaces share its switch and totals. Browsers round
`performance.now()` to about 100 µs on pages that are not cross-origin
isolated, so read phase times as averages over many frames.

## GPU pipelines (first-visit stutter on WebGPU)

Skia Graphite builds a GPU pipeline (compiled shaders plus render state) the
first time a kind of draw appears: a blurred shadow, a gradient under an
opacity, text in a rounded clip. In the browser that is a
`createRenderPipeline` that Chrome's GPU process compiles (WGSL to Metal, D3D
or Vulkan) before it draws anything else. With the browser's shader cache warm
that is quick; on a first visit, or after a browser update emptied the cache,
each one costs tens of milliseconds and a screen that brings several can drop
a dozen frames. A UI of game screens makes ~90 of them.

The host builds them ahead, while the page is still loading:

```js
const jsui = await createJsui({
  canvas,
  bundleUrl: './bundle.js',
  // Recorded once and shipped with the app (below). `{format}`: the canvas
  // format, bgra8unorm (macOS, Windows) or rgba8unorm (Android, Linux).
  pipelines: './pipelines-{format}.bin',
});
await jsui.whenPipelinesReady();  // e.g. before hiding the splash
```

- **Remembered per browser, no setup:** the pipelines a browser built are
  saved in IndexedDB (per page and canvas format; every few seconds while new
  ones appear, and when the page is hidden) and built again at the start of
  its next visit. `rememberPipelines: false` turns this off, `pipelineKey`
  names the entry (pages with several apps).
- **Shipped list:** `pipelines` (a URL or the bytes) covers first visits.
  Record it with `jsui pipelines <web dir>` (see the CLI), or by calling
  `jsui.savePipelines()` after using the app and saving the bytes.
  A list of another Skia build is skipped (one log line), a missing file
  (404) is ignored, keys the device cannot build are counted as `failed`.
- Lists are queued at startup. Graphite only uses the blocking
  `createRenderPipeline`, which stalls the GPU process (and every frame) for
  the whole compile, so the host first has a throwaway Skia context on the
  same device precompile the list with `createRenderPipelineAsync` (the
  browser compiles those on worker threads, several at once, into the same
  caches), then creates them on the real context a few at a time, each from
  a warm cache. `whenPipelinesReady()` resolves when they are made or after
  `pipelineTimeout` ms (default 5000), with `pipelineStats()`; after a
  timeout the rest are still made in the background as their compiles
  finish. Lists queued later with `addPipelines(list)` are built one per
  frame after frames, unless you call `precompilePipelines()`.
- A frame that needs a pipeline before its turn still builds it itself.
  Pipelines of ShaderView effects are not in lists (no stable key).
- **Warm-up frames:** `jsui.beginWarmup()` sends the frames to a throwaway
  context the same way (nothing is shown or submitted) until
  `await jsui.endWarmup()`, which resolves once the browser compiled what they
  drew. Drive the UI through what is about to play while a splash covers the
  canvas (the showcase steps its intro's timeline through 8 points), and
  every kind of draw in it, ShaderView effects included, is compiled in
  parallel before it plays.

WebGPU only; on WebGL the browser caches compiled shaders itself, and the
calls return `0` / `null`.

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

`npx jsui build app.jsx --web site/` (@orktes/jsui-cli) does all of this:
it writes a deployable folder with every module, versioned. By hand,
deploy `dist-gpu/` too if the UI should render with WebGPU: without it the
runtime falls back to WebGL even in browsers that have WebGPU. With
`moduleVersions`, only the directories listed there are loaded, so list
`dist-gpu` as well (the console warns when WebGPU is available but
`dist-gpu` is left out).

`renderer` (`'auto'`, `'gpu'`, `'webgpu'`, `'webgl'`, `'cpu'`, `'2d'`) forces
a backend; the chosen one is `jsui.renderer`. All options are documented on
`JsuiOptions` at the top of `jsui-web.js`.

`index.html` and `demo/` are a self-contained demo page
(`?bundle=<url>`, `?dev=1` for live reload with `jsui dev`).

## Status

0.x: the API may change between minor versions. Source, issues and the C
embedding API: <https://github.com/orktes/jsui>.
