# Headless tools: screenshot, inspect, run

`@orktes/jsui-cli` runs a JSUI app in Node on JSUI's CPU WebAssembly runtime
(from `@orktes/jsui-web`): no browser, GPU or native build. The clock is
simulated (60 fps by default), so animations, timers and springs advance
exactly by the time you ask for, and the same steps always give the same
pixels. An entry is built with the same esbuild setup as `jsui build`; a file
that only `export default`s a component is registered as `main`.

## Commands

```sh
npx jsui screenshot app.jsx -o shot.png [--wait 1000] [--width 1280 --height 720 --scale 1]
npx jsui inspect app.jsx [--find '<selector>'] [--json] [--wait 1000]
npx jsui run app.jsx steps.json [--keep-going]
npx jsui run app.jsx script.mjs
```

Common options: `--component Name` (AppRegistry key), `--props '{"json":1}'`,
`--state key=json` (repeatable; set before the bundle runs), `--fps 60`,
`--bundle` (the entry is an already built bundle.js), `--dev` (development
React: clearer errors).

* `screenshot` waits until images and fonts are loaded, advances `--wait` ms
  (default 1000, enough for entrance animations), writes the PNG (`--scale 2`
  for a 2x image). Read the PNG to check the result.
* `inspect` prints the view tree, one element per line:
  `Text #gold "1250 gold" [pressable focused] 64,120 180x24` (type, testID,
  text, flags, x,y width x height in logical px). `--json` gives
  `{roots: [{rootTag, viewport, children: [{id, type, testID, text, label,
  value, placeholder, rect, pressable, focused, disabled, children}]}],
  focused}`.
* `run` executes steps and prints one JSON line per step, then a summary
  `{"done": true, "ok", "failed", "messages", "calls", "errors"}`. It stops
  at the first failed step (`--keep-going` continues) and exits 1 on a failed
  step or an uncaught JS error.

JS errors and `console.warn` / `console.error` go to stderr in every command.

## Selectors

| Selector | Matches |
| --- | --- |
| `#play` | `testID="play"` (or `nativeID`) |
| `text=Continue` | an element whose own text is exactly "Continue" (trimmed) |
| `Continue` | text (or label, value, placeholder) containing "continue", case-insensitive |
| `label=Close` | `accessibilityLabel` / `aria-label` |
| `type=TextInput` | host type: `View`, `Text`, `Image`, `ScrollView`, `TextInput`, `LottieView`, `ShaderView` |
| `... >> 2` | the third match |
| `[640, 360]` | a point (logical px) |

Text matches the `Text` element; clicking it hits its pressable parent, as a
user would. Prefer `testID`s on anything a test touches.

## Steps (JSON)

The action is the step's first key.

```json
[
  {"wait": 500},
  {"idle": 3000},
  {"screenshot": "shots/menu.png"},
  {"click": "#play", "hold": 50},
  {"tap": "text=Settings"},
  {"hover": "#card-2"},
  {"drag": "#slider-knob", "to": [600, 400], "duration": 300, "touch": false},
  {"wheel": "#list", "dy": 240},
  {"key": "ArrowDown", "count": 2, "mods": ["shift"]},
  {"type": "Valkyrie\n"},
  {"press": "A"},
  {"press": "Down", "count": 3},
  {"stick": "lx", "value": 1, "duration": 300},
  {"state": "gold", "value": 1250},
  {"patch": "inventory", "value": {"items": {"potion": {"count": 2}}}},
  {"buffer": "enemies", "values": [100, 200, 1], "type": "f32"},
  {"message": "toast", "data": {"text": "Saved"}},
  {"callUi": "ui.flash", "args": {"text": "hi"}},
  {"run": "Settings", "props": {"tab": "audio"}},
  {"resize": [1920, 1080, 1]},
  {"find": "#play"},
  {"expect": "#play", "focused": true},
  {"expect": "text=Victory", "count": 1},
  {"expect": "#gold", "text": "1250 gold"},
  {"expect": "#row-vsync", "text": "On"},
  {"expectMissing": "#spinner"},
  {"expectState": "settings", "value": {"volume": 50, "vsync": true}},
  {"expectState": "settings", "match": {"volume": 50}},
  {"answer": "settings.apply", "value": {"ok": true}},
  {"answer": "settings.apply", "error": "Disk full"},
  {"expectMessage": "audio.play"},
  {"getState": "settings"},
  {"tree": true},
  {"hitTest": [640, 360]}
]
```

* `expect` with `text` / `textContains` checks the element's own text or,
  for a View, one of the Texts inside it (so `#row-vsync` with `"text":
  "On"` works when "On" is a child Text).
* `expectState` with `value` is an exact deep-equal; with `match` only the
  keys given are compared (recursively).
* Gamepad buttons: `A B X Y LB RB LT RT Back Start LS RS Up Down Left Right
  Home` (or indices 0-16).
* Input steps report `"consumed"`: whether the UI took the input (false
  means it would pass through to the game).
* `Host.call`s from the UI are recorded in the summary's `calls`. Unanswered
  ones are rejected (`no answer for Host.call(...)`). Answer them with
  `answer` steps, a top-level `{"calls": {"settings.apply": {"value":
  {...}}}, "steps": [...]}` (or `{"error": "..."}` to reject), or
  `app.handle` in a JS script.
* Async work in the UI (a call's answer, a fetch, a state update that
  re-renders after an effect) lands a frame or two later. Put a
  `{"wait": 100}` (or `{"idle": 1000}`) between the action and the
  `expect` that checks its result.
* A mouse click also hovers. A Pressable that was clicked looks hovered and
  focused while another element may still show gamepad focus elsewhere:
  this is expected; use `press` steps to test controller focus.

## JS scripts and the `launch()` API

```js
// check-menu.mjs  ->  npx jsui run src/app.jsx check-menu.mjs
export default async function (app) {
  app.handle('shop.buy', ({ item }) => ({ gold: 900 }));        // answer Host.call
  await app.press('Down');
  const quit = await app.get('#quit');
  if (!quit.focused) throw new Error('Down should focus Quit');
  await app.click('#shop');
  await app.wait(400);
  await app.screenshot('shop.png');
  return { calls: app.calls.length };
}
```

Or from any Node script / test:

```js
import { launch } from '@orktes/jsui-cli';

const app = await launch({ entry: 'src/app.jsx', width: 1280, height: 720, state: { gold: 10 },
  calls: { 'shop.buy': ({ item }) => ({ gold: 5 }) } });
await app.ready();
await app.click('#buy');
await app.wait(100);                     // let the call's answer render
app.setState('gold', 99);
await app.frame();                       // state reaches the UI on the next frame
console.log(app.getState('gold'), app.messages, app.errors);
app.close();
```

`app` methods: `ready()`, `wait(ms)`, `frame()` (one frame: state and
buffers you set reach the UI), `idle(maxMs)`, `screenshot(file?)` →
PNG Buffer, `tree()`, `find(sel)` / `get(sel)` (throws) / `findAll(sel)`,
`point(sel)`, `click(sel, {hold, button, mods})`, `tap`, `hover`, `drag(from,
to, {duration, touch})`, `wheel(sel, dy, dx)`, `key(key, {code, mods, count,
hold})`, `type(text)`, `press(button, {count, hold, pad})`, `stick(axis,
value, {duration})`, `setState`, `patchState`, `getState`, `setBuffer(key,
values, type)`, `postMessage(name, data)`, `callUi(name, args)`,
`handle(name, valueOrFn)`, `run(component, props)`, `resize(w, h, scale)`,
`hitTest(sel)`, `close()`. `tree()` returns `{roots: [{rootTag, viewport,
children: [element]}], focused}` with elements shaped as in `inspect
--json`; `find` returns one element without `children`, `findAll` the
matches with theirs. Records: `messages`, `calls`, `stateWrites`,
`logs`, `errors`, `time` (UI clock, ms).

## Limits

* CPU raster only: host textures the test does not set render nothing, and
  GPU-only differences are not visible (they are rare; the GPU and CPU
  renderers are compared in JSUI's CI).
* Element order among siblings follows creation order; positions are
  layout positions (transforms are not applied to `rect`).
* `jsui inspect` needs a bundle built with `@orktes/jsui-react` 0.4+.
