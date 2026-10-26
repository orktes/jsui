---
name: jsui
description: Build in-game UIs with JSUI (React Native components on QuickJS + Yoga + Skia, embedded in Godot, the web or any C/C++ engine). Use when writing or changing a JSUI screen, HUD, menu or widget, wiring it to game data, adding animation, gamepad focus, Lottie, shaders, SVG / Skia vector drawing or host textures, embedding JSUI in an engine, or screenshotting and scripting a JSUI app to check it.
---

# JSUI

JSUI runs **React Native code** as a game UI: QuickJS runs the JS, Yoga lays
it out (RN flexbox), Skia paints it (GPU or CPU). Engines embed it through a
C API, the `JsuiView` Godot node, or `@orktes/jsui-web` on a canvas. You write
ordinary RN components; there is no DOM, no HTML, no CSS files.

Packages: `@orktes/jsui-cli` (bundler, dev server, headless tools, this skill),
`@orktes/jsui-react` (the `react-native` API), `@orktes/jsui-web` (browser host).

## Rules that matter most

1. **Import RN components from `'react-native'`** (the CLI aliases it to
   `@orktes/jsui-react`). Import JSUI extras (`Host`, `useHostState`,
   `useGamepad`, `useFocus`, `HostTexture`, `ShaderView`, `sksl`, `Fonts`, `Sound`,
   `UISounds`, `useMusic`, ...)
   from `'@orktes/jsui-react'`. `lottie-react-native`,
   `react-native-safe-area-context`, `expo-linear-gradient`,
   `react-native-linear-gradient`, `expo-font` (so `@expo-google-fonts/*`),
   `react-native-svg` and `@shopify/react-native-skia` work as imports too
   (shims). Custom fonts: `useFonts({ Game:
   require('./Game.ttf') })`, see components-and-styles.md.
2. **Register the root:** `AppRegistry.registerComponent('main', () => App)`.
   It starts automatically. (The headless tools also accept a file that only
   `export default`s a component.)
3. **Text only inside `<Text>`.** Every string must be in a `<Text>`; styles
   don't inherit from Views (only nested Texts inherit text styles).
4. **RN flexbox defaults:** `flexDirection: 'column'`, `flexShrink: 0`,
   `alignItems: 'stretch'`, `position: 'relative'`, numbers are logical px.
   Use `StyleSheet.create`, `flex`, `gap`, `%` sizes and `aspectRatio`.
5. **Gamepad first.** Every `Pressable` / `Touchable` / `Button` is focusable;
   D-pad / stick / arrows / Tab move focus spatially and A / Enter / Space
   press. Always style the focused state (`({ pressed, focused, hovered }) =>`
   style functions), and give each screen an initial focus
   (`hasTVPreferredFocus` or `autoFocus`). Modals trap focus; B / Escape calls
   `onRequestClose`.
6. **Animate with `Animated` and `useNativeDriver: true`** (no JS per
   frame), composites (`sequence` / `parallel` / `stagger` / `loop`) too.
   Layout keys (`width`, `top`, ...) work on the native driver but relayout
   every frame: prefer a transform when it does the job. Hover / press /
   focus feedback is `hoveredStyle` / `pressedStyle` / `focusVisibleStyle`
   (+ `transition`, `group` / `groupHoveredStyle` for children), not
   `style={({pressed}) => ...}` or `onHoverIn` → `setState`.
7. **Per-frame game data never goes through React state.** Use frame-data
   buffers (`useHostBufferValue` / `Animated.hostBuffer`) for positions and
   bars; use shared state (`useHostState`) for things that change now and then.
   Values computed from them (clamped to the screen, faded by distance,
   conditional offsets) are `Animated.derive(inputs, fn)` expressions, which
   the runtime evaluates natively every frame: never `addListener` →
   compute → `setValue`/`setState`. Fields of shared state shown as
   they change (health, ammo) bind natively with `useHostStateBind`;
   things that appear with data or hover use `<Visibility visible={node}>`
   and `useGroupState()`; popouts, tooltips and dropdowns go in an
   `<Overlay placement="top">` (drawn above everything and outside clips,
   placed against the anchor natively, never a `zIndex` hack). Typewriter
   text (dialogue, subtitles) is
   `<Animated.Text reveal={value} revealFraction>` driven by a native
   timing, never a re-render per character; timers that pause or change
   speed are `Animated.clock({...})` (`pause()` / `resume()` / `reset()` /
   `setSpeed()`, or `running: node`). Per-frame logic beyond an
   expression goes in a `react-native-worklets` worklet (`runOnUI` + its
   `requestAnimationFrame`, setting captured `Animated.Value`s). See
   [references/native-first.md](references/native-first.md).
   On native hosts JS runs **on its own thread** (as in React Native): the
   frame never waits for it, so what is native keeps the UI right while JS
   is busy, and what JS renders lands a frame or more later (see "JS
   thread" there). Input waits for JS only when a handler might
   `preventDefault()`: mark observers `passive(fn)` and declare owned keys
   with `preventDefaultKeys` / `preventDefaultButtons` (see "Input without
   waiting on JS" there).
8. **Give important elements a `testID`** so you (and tests) can find, click
   and check them with `jsui inspect` / `jsui run`.
9. **Always look at what you built.** With `jsui dev` running and a runtime
   in its session (below), `npx jsui screenshot --attach <url>` after each
   save; otherwise `npx jsui screenshot app.jsx -o shot.png`. Open the PNG.
   Check focus with a `press` step.
10. No browser APIs: no `document`, `window.addEventListener`, CSS, `<div>`,
    `localStorage`. `fetch`, timers, `requestAnimationFrame`, `TextEncoder`,
    `URL`, `AbortController` and `performance.now()` exist. Native builds have
    no TLS (`https://` needs the engine's loader); bundle assets instead.

## Project setup

```sh
npm install --save-dev @orktes/jsui-cli          # brings @orktes/jsui-react and the headless runtime
                                                 # (install it first: the unscoped `jsui` npm package is not ours)
npx jsui build src/app.jsx -o dist/bundle.js     # one IIFE script + dist/assets/*
npx jsui dev src/app.jsx                         # http://127.0.0.1:5173/bundle.js, live reload
```

`require('./x.png')` / `import x from './x.png'` (png jpg webp gif svg ttf otf
json mp4 ...) emit assets next to the bundle. `name@2x.png` is a 2x image
(half the intrinsic size). JSON is inlined. `.jsui.js` then `.native.js`
platform files resolve first. TypeScript is stripped (no type checking).

## The fast loop: the dev server's shared session

The quickest way to iterate, and the one to use whenever a person is around.
Every runtime that loads the dev bundle in dev mode joins the dev server's
**shared session**: the game (a Godot `JsuiView` with `dev_mode` on), the
native viewer, the browser page at `/shared`, or a headless runtime inside the
dev server. You drive **the live one** (the last to join) with the same
commands as the headless tools, on the screen the person sees, and read what
they did there. Every save hot-updates it in place (component state is kept),
so you see a change in about a second on the screen you already navigated to.
Keep screens and components in their own modules: an edit to the entry file
(the one calling `AppRegistry.registerComponent`) reloads the whole app.

In a project with `@orktes/jsui-cli` installed (see Project setup; outside
one, use `npx @orktes/jsui-cli` instead of `npx jsui`):

```sh
npx jsui dev src/app.jsx                       # http://127.0.0.1:5173: session for any runtime
npx jsui dev src/app.jsx --headless [--width 1920 --height 1080 --scale 2]
                                               # ... plus a runtime inside the server (no window needed)
U=http://127.0.0.1:5173
npx jsui clients --attach $U                   # which runtimes are connected (* = live)
npx jsui screenshot --attach $U -o shot.png    # exactly what the live runtime shows now
npx jsui inspect --attach $U                   # live view tree (--find '#play' --json)
npx jsui run --attach $U press Down click '#play' screenshot shot.png   # steps as words, no file
npx jsui run --attach $U steps.json            # or a steps file, as jsui run
npx jsui log --attach $U --since 0             # what the person clicked / pressed, UI messages, calls, errors
```

* Pick the runtime the work needs. **Standalone UI** (menus, tools that
  don't need the game): `--headless`, or the person opens `$U/shared`, which
  runs the app in the browser's web host. **UI inside the game** (HUD over
  the 3D scene, game state, host calls): the game must be running with its
  `JsuiView` on `$U/bundle.js` in dev mode; ask the person to start it.
  Native viewer: `jsui-viewer --dev $U/bundle.js`.
* Check first: `npx jsui clients --attach $U`. No server: start `jsui dev`
  in the background. No runtime: commands wait up to 30 s for one to join,
  then fail with how to open one; start one with `--headless` when the work
  doesn't need the game, else ask the person and send them the `/shared`
  link.
* Several connected: `--client <name|id>` (on screenshot / inspect / run)
  picks one, e.g. `--client godot`, `--client headless`, `--client browser`.
* Loop: edit, save, `screenshot --attach`, look, repeat. Use `run --attach`
  to navigate to the screen you're working on or to check interactions;
  `{"reload": true}` starts over with fresh component state.
* It is the person's session: the clock is real (`{"wait": 500}` takes
  500 ms), they may be playing at the same time, and state you set stays set.
  Read `jsui log` before assuming what is on screen, and tell them what
  you're about to drive. Your steps show in the `/shared` activity feed.
* In a game or browser runtime the host answers Host.calls and owns the
  window size: `answer` steps and `resize` work with `--headless` only.
  A paused game or a background browser tab stops ticking, and commands time
  out.
* For checks that must repeat exactly (tests, CI, before/after comparisons)
  use the standalone headless commands below: they run on a simulated clock.

## Minimal screen

```jsx
import React, { useState } from 'react';
import { AppRegistry, Pressable, StyleSheet, Text, View } from 'react-native';
import { Host, useHostState } from '@orktes/jsui-react';

function MenuButton({ label, onPress, first, testID }) {
  return (
    <Pressable testID={testID} onPress={onPress} hasTVPreferredFocus={first}
      style={({ pressed, focused, hovered }) => [s.btn, (focused || hovered) && s.btnActive, pressed && s.btnPressed]}>
      {({ focused }) => <Text style={[s.label, focused && s.labelActive]}>{label}</Text>}
    </Pressable>
  );
}

function App() {
  const gold = useHostState('gold', 0);                      // game -> UI (jsui_set_state)
  return (
    <View style={s.root}>
      <Text style={s.title}>AETHERFALL</Text>
      <Text testID="gold" style={s.gold}>{gold} gold</Text>
      <MenuButton testID="play" first label="Play" onPress={() => Host.postMessage('menu.play')} />
      <MenuButton testID="quit" label="Quit" onPress={() => Host.postMessage('menu.quit')} />
    </View>
  );
}

const s = StyleSheet.create({
  root: { flex: 1, padding: 64, gap: 12, backgroundColor: '#0b0f1a', justifyContent: 'center' },
  title: { color: '#f5e6c8', fontSize: 56, fontWeight: '700', letterSpacing: 8, marginBottom: 24 },
  gold: { color: '#f2c14e', fontSize: 18 },
  btn: { width: 320, paddingVertical: 14, paddingHorizontal: 20, borderRadius: 10, borderWidth: 1,
    borderColor: 'rgba(255,255,255,0.12)', backgroundColor: 'rgba(255,255,255,0.04)' },
  btnActive: { borderColor: '#f2c14e', backgroundColor: 'rgba(242,193,78,0.14)' },
  btnPressed: { transform: [{ scale: 0.97 }] },
  label: { color: '#c8cfdd', fontSize: 20, fontWeight: '600', letterSpacing: 2 },
  labelActive: { color: '#fff' },
});

AppRegistry.registerComponent('main', () => App);
```

## Check your work headlessly (no browser, GPU or dev server)

The CLI runs the bundle on JSUI's CPU WebAssembly runtime inside Node, on a
simulated clock, so results are deterministic.

```sh
npx jsui screenshot src/app.jsx -o shot.png              # --width 1920 --height 1080 --scale 2 --wait 1500
npx jsui screenshot src/app.jsx --state gold=1250 --state 'player={"name":"Ada"}'
npx jsui inspect src/app.jsx                             # the view tree: type, #testID, text, [pressable focused], x,y wxh
npx jsui inspect src/app.jsx --find '#play' --json
npx jsui run src/app.jsx steps.json                      # scripted input; one JSON result per step
npx jsui run src/app.jsx press Down expect '#quit' focused=true   # the same steps as words
```

```json
[
  {"press": "Down"},
  {"expect": "#quit", "focused": true},
  {"click": "#play"},
  {"expectMessage": "menu.play"},
  {"state": "gold", "value": 99},
  {"expect": "#gold", "text": "99 gold"},
  {"screenshot": "after.png"}
]
```

Steps as words: an action word takes the next word as its value (JSON when
it parses: `wait 500`, `click [640,360]`), `name=value` words add options to
that step (`press A count=2`, `state gold value=99`, `expect '#gold'
text='99 gold'`), and a JSON word is a step. Quote `'#testID'` in the
shell (a bare `#` starts a comment).

Selectors: `#testID`, `text=Exact text`, `Contains text`, `label=...`,
`type=TextInput`, a trailing ` >> 1` for the 2nd match, or `[x, y]`.
`jsui run --help` lists every step; `jsui run app.jsx script.mjs` runs a JS
script (`export default async (app) => { await app.click('#play'); ... }`).
Details: [references/testing-tools.md](references/testing-tools.md).

Workflow: write the screen, screenshot it and **look at the PNG**, fix layout,
then script the interactions (focus order, presses, state changes) with
`expect` steps. A JS error fails the command and is printed.

## Where to find more

Read the reference that matches the task:

| Task | Reference |
| --- | --- |
| Components, props, styles, images, fonts (loading, weights, fallbacks, variable fonts), what's not supported | [references/components-and-styles.md](references/components-and-styles.md) |
| Focus, gamepad, keyboard, pointer, scrolling, modals, input pass-through | [references/input-and-focus.md](references/input-and-focus.md) |
| Native-first patterns: keep JS off the frame (`Animated.derive`, `Animated.clock` and pausable `Animated.clock({...})`, typewriter `Animated.Text reveal`, host-buffer values, state styles `hoveredStyle` / `pressedStyle` / group styles, `useInteraction`, `useGroupState`, `Visibility`, hover popouts and `<Overlay>`, `useHostStateBind`, worklets (`react-native-worklets`: `runOnUI`, `runOnJS`, ...), what to use instead of state); the JS thread (what runs where, `measure`, same-frame syncs) | [references/native-first.md](references/native-first.md) |
| `Animated` (native driver), springs, `LayoutAnimation`, performance | [references/animation-and-performance.md](references/animation-and-performance.md) |
| Game data: shared state, calls, messages, per-frame buffers, several root views | [references/game-data.md](references/game-data.md) |
| Host textures (video, render targets), LottieView, ShaderView / SkSL, vector drawing (`react-native-svg`, `@shopify/react-native-skia` Canvas) | [references/media-and-effects.md](references/media-and-effects.md) |
| Sound: UI sounds on hover / focus / press (`UISounds`), music (`useMusic`), `Sound.play`, buses | [references/audio.md](references/audio.md) |
| Embedding: C API, Godot `JsuiView`, web host, dev mode | [references/embedding.md](references/embedding.md) |
| `jsui dev` session + `--attach`, `jsui screenshot` / `inspect` / `run`, `launch()` / `attach()` APIs, selectors, all steps | [references/testing-tools.md](references/testing-tools.md) |
| Game UI patterns: layout for 16:9, focus visuals, HUD, menus, lists | [references/game-ui-patterns.md](references/game-ui-patterns.md) |

`npx @orktes/jsui-cli skill print --all` prints this skill with every reference;
`npx @orktes/jsui-cli skill install` copies it into `.claude/skills/jsui/`
(`--global`: `~/.claude/skills/jsui/`).
