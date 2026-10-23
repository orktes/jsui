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
