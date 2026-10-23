# @orktes/jsui-cli

Bundles React Native–style apps for the JSUI runtime (esbuild 0.28.2), serves
them to a dev host with live reload, renders and scripts them headlessly, and
ships the JSUI agent skill for coding agents.

```sh
npx jsui build app.jsx -o dist/bundle.js [--minify] [--sourcemap] [--dev]
npx jsui dev app.jsx [--port 5173] [--host 127.0.0.1]
npx jsui screenshot app.jsx -o shot.png [--width 1280 --height 720 --scale 2 --wait 1000]
npx jsui inspect app.jsx [--find '#play'] [--json]
npx jsui run app.jsx steps.json|script.mjs
npx jsui skill [print --all | install [dir] [--global] | path]
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

## Headless: screenshot, inspect, run

`screenshot`, `inspect` and `run` run the app in Node on JSUI's CPU
WebAssembly runtime (`dist/` of `@orktes/jsui-web`, a dependency; or
`JSUI_WASM_DIR`): no browser, GPU or native build, the same pixels on every
OS. The UI clock is simulated (`--fps`, default 60), so animations and timers
advance exactly as asked and runs are deterministic. A file that only
`export default`s a component is registered as `main`.

* `screenshot` waits for images and fonts, advances `--wait` ms (default
  1000) and writes a PNG (`--scale` for HiDPI).
* `inspect` prints the view tree, one element per line (`Text #gold "1250
  gold" [pressable focused] 64,120 180x24`), or `--json`; `--find <selector>`
  prints the matches. The tree comes from `@orktes/jsui-react`'s inspector
  (`jsui_call_ui('__jsui.inspect')`).
* `run` executes steps (clicks, taps, drags, wheel, keys, typing, gamepad
  buttons and sticks, shared state, buffers, messages, `Host.handle` calls,
  screenshots and `expect*` checks) and prints one JSON result per step;
  `jsui run --help` lists them. A `.mjs` script gets the app object instead.
  Exit code 1 on a failed step or an uncaught JS error.

Selectors: `#testID`, `text=Exact`, `Contains`, `label=...`, `type=TextInput`,
`... >> n`, or `[x, y]`.

Options for all three: `--width`, `--height`, `--scale`, `--fps`,
`--component`, `--props <json>`, `--state key=<json>` (repeatable),
`--bundle` (the entry is a built bundle), `--dev`.

```js
import { launch } from '@orktes/jsui-cli';
const app = await launch({ entry: 'app.jsx', width: 1280, height: 720, state: { gold: 10 }, calls: { 'shop.buy': { ok: true } } });
await app.press('Down');                    // gamepad
await app.click('#buy');
await app.screenshot('after.png');
console.log(await app.find('#gold'), app.messages, app.errors);
app.close();
```

## Agent skill

`skills/jsui/` is an [Agent Skill](https://docs.claude.com/en/docs/agents-and-tools/agent-skills)
that teaches coding agents (Claude Code and others) to write JSUI UIs and to
check them with the headless tools. It is versioned with this package:

* `npx jsui skill install` copies it to `./.claude/skills/jsui/`
  (`--global`: `~/.claude/skills/jsui/`); rerun after upgrading.
* `npx jsui skill print --all` prints it (for agents that take instructions
  as text).
* Claude Code plugin: `/plugin marketplace add orktes/jsui`, then
  `/plugin install jsui@jsui`.

## Programmatic use

```js
import { build, dev } from '@orktes/jsui-cli';
await build({ entry: 'app.jsx', outfile: 'dist/bundle.js', minify: true });
const server = await dev({ entry: 'app.jsx', port: 0 }); // server.port, server.close()
```

Tests: `npm test -w @orktes/jsui-cli`.
