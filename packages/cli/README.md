# @orktes/jsui-cli

Bundles React Native–style apps for the JSUI runtime (esbuild 0.28.2), serves
them to a dev host with hot updates (React Fast Refresh), renders and scripts
them headlessly, and ships the JSUI agent skill for coding agents.

Add the agent skill to a project with one command (no install needed first):

```sh
npx @orktes/jsui-cli skill install            # -> .claude/skills/jsui/ (--global: ~/.claude/skills/)
```

In a project with `npm install --save-dev @orktes/jsui-cli`:

```sh
npx jsui build app.jsx -o dist/bundle.js [--minify] [--sourcemap] [--dev] [--format combo|bytecode|source|split]
npx jsui dev app.jsx [--port 5173] [--host 127.0.0.1] [--no-hmr]
npx jsui screenshot app.jsx -o shot.png [--width 1280 --height 720 --scale 2 --wait 1000]
npx jsui inspect app.jsx [--find '#play'] [--json]
npx jsui run app.jsx steps.json|script.mjs
npx jsui run app.jsx press Down click '#play' screenshot after.png   # steps as words
npx jsui skill [print --all | install [dir] [--global] | path]

npx jsui dev app.jsx [--headless]              # + a runtime inside the server
npx jsui clients --attach http://127.0.0.1:5173 # runtimes in the shared session
npx jsui screenshot|inspect|run --attach http://127.0.0.1:5173 [--client sel] ...
npx jsui log --attach http://127.0.0.1:5173 [--since N] [--follow]
```

## Build output

* One IIFE script (ES2020, UTF-8) suitable for QuickJS: no top-level
  `import`/`export`, no runtime `require`.
* Precompiled to QuickJS bytecode by default: the runtime skips parsing
  (loading the shared demo app: 19.8 ms -> 3.7 ms on an M1 Ultra) and keeps no
  function source in memory. Compiling uses the WebAssembly runtime from
  `@orktes/jsui-web`, the same patched QuickJS as the native runtimes of that
  version; without it the bundle stays plain JS. Bytecode only runs on the
  QuickJS that compiled it: a runtime of another JSUI version runs the source
  instead and logs a warning. `--format` picks what goes in:

  | `--format` | writes | size (vs the JS) | other JSUI versions |
  | --- | --- | --- | --- |
  | `combo` (default) | bytecode + the source inside | ~2.2x | run the source |
  | `bytecode` | bytecode only | ~1.25x | fail to load |
  | `split` | bytecode in `bundle.js`, the source in `bundle.source.js` | ~1.25x + the JS file, read only as the fallback | load `bundle.source.js` |
  | `source` | plain JS (browser JS engines, other tools) | 1x | run it |

  Every JSUI host loads all of them (the file is binary: hosts load it by
  path / URL or pass its bytes, never as text). Function source is stripped
  (`Function.prototype.toString()` gives `[native code]`) and stack traces
  name the bundle by its file name (`bundle.js:12:5`). The format is in
  `src/js/bytecode.h`; `readPrecompiled(bytes)` / `bundleSource(bytes)` from
  this package read it, and `compileBundle(source, { keepSource, sourceUrl })`
  / `jsui_compile_bundle()` (C API) precompile a JS bundle.
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
| `/bundle.js` | last successful build (errors keep the previous one); the hot-updating dev bundle unless `--no-hmr` |
| `/bundle.js.map` | its source map (`--no-hmr` only) |
| `/assets/*` | emitted assets |
| `/__jsui/version` | plain-text opaque token; changes on every successful rebuild (with `?hmr=1`, failed builds too). `?since=<token>` long-polls (~25 s) until it differs |
| `/__jsui/hmr?from=<token>` | the hot update script from that version to the current one |
| `/__jsui/error` | current build error text (empty when OK) |
| `/` | index page (links the WASM web host if `web/` is built) |
| anything else | static files from the entry's directory |

Point a host at it: `jsui-viewer --dev http://127.0.0.1:5173/bundle.js`, the
web host (`/web/index.html?bundle=/bundle.js`), or Godot (`JsuiView.dev_mode`).
Build errors are printed with file/line and the last good bundle stays live.

### Hot updates

Saving a file updates the running UI without losing its state:

* A file whose exports are all React components re-renders in place; its
  components keep their state (`useState`, refs, scroll positions). If you
  change which hooks a component calls, that one component remounts.
* Any other module (constants, styles, helpers, JSON) re-runs together with
  the modules that import it, up to the nearest component-only files.
* An edit that no component file sits above (the entry file that calls
  `AppRegistry.registerComponent`), a change to a dependency or an asset, or a
  restarted dev server reloads the whole bundle instead.
* Build errors and errors thrown while re-running a module appear on the
  runtime's error overlay; the next good save clears it. A component that
  throws while rendering unmounts the root as usual, and the fix mounts it
  again.
* `module.hot` has `accept()`, `dispose((data) => ...)`, `data`, `decline()`
  and `invalidate()` for modules with side effects to clean up.

`--no-hmr` serves the plain bundle and reloads it on every change.

## Shared session: `jsui dev` and `--attach`

Every runtime that loads the dev server's bundle in dev mode joins its shared
session, whatever hosts it: a Godot `JsuiView` with `dev_mode` on, the native
viewer (`jsui-viewer --dev http://127.0.0.1:5173/bundle.js`), a browser page,
or `--headless`, a runtime inside the dev server. You and coding agents then
see and control the same app:

* Agents use the headless commands with `--attach <url>` instead of an entry
  (`jsui screenshot --attach`, `inspect --attach`, `run --attach`, and
  `jsui log --attach` for the feed), or `attach(url)` from `@orktes/jsui-cli`
  in scripts. They drive the live runtime, the last one to join;
  `--client <name|id>` picks another, `jsui clients --attach` lists them.
  With none connected, commands wait 30 s for one, then say how to open one.
* `http://127.0.0.1:5173/shared` shows the session: the runtimes, and a feed
  of who did what (your input in any runtime, the agent's steps, the UI's
  messages, calls and errors). With nothing else connected it runs the app
  itself in the browser's web host; with `--headless` it streams that
  runtime's frames and your mouse, keyboard, text and gamepads drive it.
* Saves hot-update every runtime like any dev host, so an agent can edit,
  save and screenshot in a loop while you play.

The runtime speaks a small HTTP protocol to the server (a held
`POST /__jsui/session/poll` for commands, `POST /__jsui/session/push` for
results and events), built into it for every platform; embedders name their
runtime or opt out with `jsui_set_dev_session(rt, name, enabled)`. `answer`
steps and `resize` need `--headless` (games and pages answer their own
Host.calls and own their window size).

View options (`--width`, `--height`, `--scale`, `--fps`, `--component`,
`--props`, `--state`) set up the headless runtime. `--scale 2` makes its
stream sharp on HiDPI screens. It is the CPU WebAssembly build from
`@orktes/jsui-web`. (`--shared` is the old name of `--headless`.)

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

* `npx @orktes/jsui-cli skill install` copies it to `./.claude/skills/jsui/`
  (`--global`: `~/.claude/skills/jsui/`); rerun after upgrading. Inside a
  project that has the CLI installed, `npx jsui skill install` does the same
  with that version. (Use the scoped name otherwise: the unscoped `jsui` npm
  package is not ours.)
* `npx @orktes/jsui-cli skill print --all` prints it (for agents that take instructions
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
