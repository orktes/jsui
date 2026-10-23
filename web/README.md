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
  `HTMLImageElement` or `ImageData`; its current picture is uploaded. Calling
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
