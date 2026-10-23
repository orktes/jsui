# @orktes/jsui-cli

Bundles React Native–style apps for the JSUI runtime (esbuild 0.28.2) and
serves them to a dev host with live reload.

```sh
npx jsui build app.jsx -o dist/bundle.js [--minify] [--sourcemap] [--dev]
npx jsui dev app.jsx [--port 5173] [--host 127.0.0.1]
```

## Build output

* One IIFE script (ES2020, UTF-8) suitable for QuickJS: no top-level
  `import`/`export`, no runtime `require`.
* `react-native` (and deep `react-native/...` paths) → `@orktes/jsui-react`.
  `react`, `react/jsx-runtime`, `react-reconciler` and `scheduler` always
  resolve to the copy `@orktes/jsui-react` was built against (one React per bundle).
  `react-native-safe-area-context`, `expo-status-bar`, `expo-linear-gradient`
  and `react-native-linear-gradient` resolve to JSUI shims.
* JSX (automatic runtime) in `.js`, `.jsx`, `.tsx`; TypeScript is stripped.
  Platform extensions resolve first: `.jsui.js`, then `.native.js`, then `.js`.
* `process.env.NODE_ENV` is `"production"` (`--dev`: `"development"`),
  `__DEV__` matches, `global` is `globalThis`.
* Assets: `png jpg jpeg gif webp bmp svg ttf otf woff woff2 mp3 wav ogg mp4
  bin` are emitted to `assets/<name>-<hash>.<ext>` next to the bundle and the
  import evaluates to that URL relative to the bundle (so
  `<Image source={require('./a.png')} />` works). Images also register their
  intrinsic size (`name@2x.png` halves it) so unsized `<Image>`s size
  themselves like React Native. JSON is inlined.

## Dev server

| Path | |
| --- | --- |
| `/bundle.js`, `/bundle.js.map` | last successful build (errors keep the previous one) |
| `/assets/*` | emitted assets |
| `/__jsui/version` | plain-text opaque token; changes on every successful rebuild. `?since=<token>` long-polls (~25 s) until it differs |
| `/__jsui/error` | current build error text (empty when OK) |
| `/` | index page (links the WASM web host if `web/` is built) |
| anything else | static files from the entry's directory |

Point a host at it: `jsui-viewer --dev http://127.0.0.1:5173/bundle.js`.
Build errors are printed with file/line and the last good bundle stays live.

## Programmatic use

```js
import { build, dev } from '@orktes/jsui-cli';
await build({ entry: 'app.jsx', outfile: 'dist/bundle.js', minify: true });
const server = await dev({ entry: 'app.jsx', port: 0 }); // server.port, server.close()
```

Tests: `npm test -w @orktes/jsui-cli`.
