# @jsui/web

Runs the JSUI game-UI runtime (QuickJS + Yoga + Skia, compiled to
WebAssembly) on a `<canvas>`. Pair it with `@jsui/cli`, which bundles your
React Native–style app into the single script this host loads.

```sh
npm install @jsui/web
npx @jsui/cli build app.jsx -o public/bundle.js
```

```js
import { createJsui } from '@jsui/web';

const jsui = await createJsui({ canvas: document.querySelector('canvas'), bundleUrl: './bundle.js' });
jsui.on('message', (name, data) => console.log(name, data)); // Host.postMessage(...) from the UI
jsui.postMessage('hud', { hp: 42 });                         // Host.handle('hud', fn) in the UI
```

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
`node_modules/@jsui/web/` (or copy `dist*/` next to your built
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
