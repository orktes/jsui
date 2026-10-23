---
name: jsui
description: Build in-game UIs with JSUI (React Native components on QuickJS + Yoga + Skia, embedded in Godot, the web or any C/C++ engine). Use when writing or changing a JSUI screen, HUD, menu or widget, wiring it to game data, adding animation, gamepad focus, Lottie, shaders or host textures, embedding JSUI in an engine, or screenshotting and scripting a JSUI app to check it.
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
   `useGamepad`, `useFocus`, `HostTexture`, `ShaderView`, `sksl`, `Fonts`, ...)
   from `'@orktes/jsui-react'`. `lottie-react-native`,
   `react-native-safe-area-context`, `expo-linear-gradient` and
   `react-native-linear-gradient` work as imports too (shims).
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
6. **Animate with `Animated` and `useNativeDriver: true`** for transforms,
   opacity and colors (no JS per frame). Layout keys (`width`, `top`, ...)
   can't use the native driver: animate a transform instead, or use
   `LayoutAnimation`.
7. **Per-frame game data never goes through React state.** Use frame-data
   buffers (`useHostBufferValue` / `Animated.hostBuffer`) for positions and
   bars; use shared state (`useHostState`) for things that change now and then.
8. **Give important elements a `testID`** so you (and tests) can find, click
   and check them with `jsui inspect` / `jsui run`.
9. **Always look at what you built:** `npx jsui screenshot app.jsx -o shot.png`,
   then open the PNG. Check focus with `jsui run` and a `press` step.
10. No browser APIs: no `document`, `window.addEventListener`, CSS, `<div>`,
    `localStorage`. `fetch`, timers, `requestAnimationFrame`, `TextEncoder`,
    `URL`, `AbortController` and `performance.now()` exist. Native builds have
    no TLS (`https://` needs the engine's loader); bundle assets instead.

## Project setup

```sh
npm install --save-dev @orktes/jsui-cli          # brings @orktes/jsui-react and the headless runtime
npx jsui build src/app.jsx -o dist/bundle.js     # one IIFE script + dist/assets/*
npx jsui dev src/app.jsx                         # http://127.0.0.1:5173/bundle.js, live reload
```

`require('./x.png')` / `import x from './x.png'` (png jpg webp gif svg ttf otf
json mp4 ...) emit assets next to the bundle. `name@2x.png` is a 2x image
(half the intrinsic size). JSON is inlined. `.jsui.js` then `.native.js`
platform files resolve first. TypeScript is stripped (no type checking).

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

## Check your work (headless, no browser or GPU)

The CLI runs the bundle on JSUI's CPU WebAssembly runtime inside Node, on a
simulated clock, so results are deterministic.

```sh
npx jsui screenshot src/app.jsx -o shot.png              # --width 1920 --height 1080 --scale 2 --wait 1500
npx jsui screenshot src/app.jsx --state gold=1250 --state 'player={"name":"Ada"}'
npx jsui inspect src/app.jsx                             # the view tree: type, #testID, text, [pressable focused], x,y wxh
npx jsui inspect src/app.jsx --find '#play' --json
npx jsui run src/app.jsx steps.json                      # scripted input; one JSON result per step
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
| Components, props, styles, images, fonts, what's not supported | [references/components-and-styles.md](references/components-and-styles.md) |
| Focus, gamepad, keyboard, pointer, scrolling, modals, input pass-through | [references/input-and-focus.md](references/input-and-focus.md) |
| `Animated` (native driver), springs, `LayoutAnimation`, performance | [references/animation-and-performance.md](references/animation-and-performance.md) |
| Game data: shared state, calls, messages, per-frame buffers, several root views | [references/game-data.md](references/game-data.md) |
| Host textures (video, render targets), LottieView, ShaderView / SkSL | [references/media-and-effects.md](references/media-and-effects.md) |
| Embedding: C API, Godot `JsuiView`, web host, dev mode | [references/embedding.md](references/embedding.md) |
| `jsui screenshot` / `inspect` / `run`, `launch()` API, selectors, all steps | [references/testing-tools.md](references/testing-tools.md) |
| Game UI patterns: layout for 16:9, focus visuals, HUD, menus, lists | [references/game-ui-patterns.md](references/game-ui-patterns.md) |

`npx jsui skill print --all` prints this skill with every reference;
`npx jsui skill install` copies it into `.claude/skills/jsui/`.
