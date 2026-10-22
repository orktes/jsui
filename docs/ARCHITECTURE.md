# JSUI architecture

JSUI is a small, embeddable UI runtime for games: React (JSX) components
written exactly like React Native code, executed by QuickJS, laid out by Yoga
and painted by Skia. It is a "browser-shaped" stack (JS + layout + 2D paint +
resource loading + input) without a browser: the whole runtime is a static
library with a C API, and the same core compiles to WebAssembly.

```
 ┌───────────────────────── app bundle (JS) ──────────────────────────┐
 │  import { View, Text, Image, StyleSheet } from 'react-native'       │
 │  react + react-reconciler + @jsui/react (host config, components,   │
 │  event system, StyleSheet)  — bundled by @jsui/cli (esbuild)        │
 └───────────────┬───────────────────────────────▲─────────────────────┘
   __jsui_native.* (create/setProps/append…)     │ __jsui_dispatchEvent(ev)
 ┌───────────────▼───────────────────────────────┴─────────────────────┐
 │ JsEngine (QuickJS-ng): globals, timers, rAF, fetch, fonts, messages │
 ├──────────────────────────────────────────────────────────────────────┤
 │ Document / Node tree ── Yoga layout ── Text (skparagraph) ── Images  │
 │ InputManager: hit test, pointer/keyboard/gamepad, focus navigation   │
 │ Painter (Skia): backgrounds, gradients, borders, radii, shadows,     │
 │                 transforms, opacity, clipping, images, text          │
 ├──────────────────────────────────────────────────────────────────────┤
 │ ResourceLoader: file, http (dev server), embedder callback, browser  │
 └───────────────┬──────────────────────────────────────────────────────┘
          C API  │ include/jsui/jsui.h
   ┌─────────────┼───────────────┬─────────────────────┬──────────────┐
   │ jsui-render │ jsui-viewer   │ web/ (WASM + canvas)│ game engine  │
   │ (headless)  │ (window, dev) │                     │ (Godot, ...) │
   └─────────────┴───────────────┴─────────────────────┴──────────────┘
```

## Why these choices

* **C++20 core.** Skia and Yoga are C++, QuickJS is C. One language, no FFI
  layer, a single static library, and Emscripten builds it for the browser.
  Game engines (Godot GDExtension, Unreal, Unity native plugins) consume C/C++
  directly. The public surface is a C header so any language can bind it.
* **QuickJS-ng** — small, fast startup, ES2023, embeddable, compiles to WASM.
  The engine is isolated behind `JsEngine`, so a different engine (or the
  browser's own JS when running as WASM) can be slotted in later.
* **Yoga** — the layout engine React Native uses, so flexbox behaves exactly
  like RN (column default, `flexShrink: 0`, `position: relative`, etc.).
* **Skia** — what Chrome and Flutter paint with; gives us anti-aliased rounded
  rects, blurs for shadows, gradients, text shaping (HarfBuzz + ICU via
  skparagraph) and image codecs. Native builds use prebuilt static libraries
  (aseprite/skia m124); WASM uses the same milestone built with
  `scripts/build-skia-wasm.sh`.
* **CPU raster first.** The runtime paints into an RGBA buffer the embedder
  uploads as a texture. Deterministic (great for reference tests) and works
  everywhere. A GPU (Ganesh GL / Graphite) backend can be added behind the
  same `paintDocument()` call.
* **React Native semantics, written as `react-native` imports.** App code
  imports from `'react-native'`; the JSUI bundler aliases that to
  `@jsui/react`, and the Chrome reference build aliases it to
  `react-native-web`. Code an LLM (or person) writes for RN therefore runs
  unchanged in JSUI and in the browser reference.

## Repository layout

| Path | What |
| --- | --- |
| `include/jsui/jsui.h` | Public C API (embedding, input, resources, messages) |
| `src/core/` | `Value`, `Node`, `Document`, style parsing (`style.cpp`) |
| `src/render/` | Skia painter, image cache |
| `src/text/` | Font manager, paragraph building and Yoga measure for `Text` |
| `src/js/` | QuickJS host (`JsEngine`) and bridge globals |
| `src/input/` | Hit testing, event routing, focus and gamepad navigation |
| `src/platform/` | Resource loader backends (file, http, emscripten) |
| `src/net/` | Minimal HTTP/1.1 client for dev servers (native) |
| `src/runtime.*`, `src/capi.cpp` | Runtime object and C API wrappers |
| `tools/jsui-render` | Headless renderer: bundle → PNG (tests, CI) |
| `tools/jsui-viewer` | Windowed host with dev mode (live reload) |
| `web/` | Browser host for the WASM build |
| `packages/react` | `@jsui/react`: reconciler, components, StyleSheet, events |
| `packages/cli` | `@jsui/cli`: `jsui build` / `jsui dev` (esbuild + dev server) |
| `examples/` | Realistic game UI examples (shared by reference tests) |
| `tests/native/` | C++ unit tests (`jsui-tests`) |
| `tests/reference/` | Pixel comparison against react-native-web in headless Chrome |
| `assets/fonts/` | Bundled default font (Inter, OFL) |

## Frame loop

The embedder owns the loop (see `jsui.h`):

1. Forward input with `jsui_send_*`. The InputManager hit-tests against the
   last layout and calls `__jsui_dispatchEvent` synchronously.
2. `jsui_tick(now)`: deliver finished resource loads, run due timers, run
   `requestAnimationFrame` callbacks, drain promise jobs, run Yoga if dirty,
   emit `onLayout` events.
3. If `jsui_needs_redraw()`, `jsui_render_rgba()` paints the tree.

Everything is single-threaded except resource I/O, which completes onto a
queue drained in `jsui_tick`.

## JS bridge contract

Native globals (installed by `JsEngine`):

| Global | Meaning |
| --- | --- |
| `__jsui_native.rootId` | id of the root container node |
| `createNode(type) → id` | `type` ∈ `View`, `Text`, `RawText`, `Image`, `ScrollView`, `TextInput` |
| `setProps(id, props)` | replaces all props; `props.style` is a **flattened** style object; functions are dropped; `props.__events` lists event names the element listens to (e.g. `["press","pointerMove","layout"]`) |
| `setText(id, string)` | content of a `RawText` |
| `appendChild(p, c)`, `insertBefore(p, c, before)`, `removeChild(p, c)` | tree edits (React semantics) |
| `destroyNode(id)` | free a detached subtree |
| `measure(id) → {x, y, width, height, pageX, pageY}` | layout of a node |
| `focus(id)`, `blur(id)`, `scrollTo(id, x, y, animated)` | imperative methods |
| `postMessage(name, json)` | to the embedder's `on_message` |
| `loadFont(family, url, weight, italic) → Promise` | register a font from a URL |
| `reload()` | dev mode: reload the bundle |

Standard globals: `console`, `setTimeout`/`setInterval` (+ clear),
`requestAnimationFrame`/`cancelAnimationFrame`, `queueMicrotask`,
`performance.now()`, `fetch` (`Response.text/json/arrayBuffer`, `Headers`),
`TextEncoder`/`TextDecoder`, `URL` (basic), `globalThis.window = globalThis`.

Callbacks the bundle installs (all optional):

* `__jsui_dispatchEvent(event) → number` — native → JS event. Bitmask: `1` a
  handler ran, `2` `preventDefault()` was called.
* `__jsui_onHostMessage(name, json)` — from `jsui_post_message`.
* `__jsui_onLayout(id, {x, y, width, height})` — for `onLayout` listeners.

### Event objects

Native dispatches plain objects; `@jsui/react` turns them into React-style
synthetic events, does capture/bubble along its instance tree and maps them to
React Native props (`onPress`, `onPressIn/Out`, `onLongPress`, `onHoverIn/Out`,
`onPointerDown/Move/Up/Enter/Leave`, `onKeyDown/Up`, `onFocus/onBlur`,
`onScroll`, `onWheel`, `onChangeText`).

```
{ type: 'pointerdown'|'pointermove'|'pointerup'|'pointercancel'|'pointerenter'|'pointerleave'
        |'click'|'wheel'|'keydown'|'keyup'|'textinput'|'focus'|'blur'|'scroll'
        |'gamepadbutton'|'gamepadaxis'|'gamepadconnected',
  target: <node id>,              // hit node, focused node for keys
  timestamp: <ms>,
  // pointer
  x, y,                           // viewport coords (pageX/pageY)
  localX, localY,                 // relative to target (locationX/Y)
  pointerId, pointerType: 'mouse'|'touch', button, buttons,
  // wheel
  deltaX, deltaY,
  // keys
  key, code, repeat, shiftKey, ctrlKey, altKey, metaKey,
  // text
  text,
  // gamepad
  gamepad, buttonIndex, value, axis,
  // scroll
  contentOffset: {x, y}, contentSize: {width, height}, layoutMeasurement: {width, height} }
```

`pointerenter`/`pointerleave` are dispatched per node (no bubbling), like the DOM.
Gamepad D-pad / stick moves focus spatially between `focusable` nodes and A
activates the focused node (dispatched as `click`), unless the gamepad event
was `preventDefault()`-ed.

### Input routing details (`src/input/`)

* **Default actions** (Tab focus moves, Enter/Space/A → `click`, D-pad/stick/arrow
  navigation, wheel scrolling) are skipped when JS calls `preventDefault()`.
  `__jsui_dispatchEvent` returns a bitmask: `1` = a handler ran, `2` =
  default prevented (a boolean `true` is treated as `3` for compatibility).
  A JS Pressable that handles Enter/Space/A itself prevents the default so no
  extra `click` follows. Exception: focus-on-press always happens.
* **`__events` drives native decisions.** Names are matched case-insensitively
  by substring: `press`/`click` = pressable (focusable by default, `pointer`
  cursor); `move`/`responder`/`pan`/`drag` = wants `pointermove` (moves are not
  sent to JS unless a node on the path lists one); `hover`/`enter`/`leave`/`over`
  = wants `pointerenter`/`pointerleave` (sent only to such nodes); `wheel`;
  `"*"` = everything. Any pointer-ish name makes `jsui_send_*` report "consumed".
* **Pointer:** mouse `pointerId` 0, touch `id + 1`. Implicit capture while a
  button / touch is down; `click` (primary button only) targets the nearest
  common ancestor of the down and up nodes. Wheel `deltaY > 0` scrolls down
  (DOM convention); over a `horizontal` ScrollView a vertical wheel scrolls
  sideways. A touch dragged > 8 px inside a scrollable ScrollView sends
  `pointercancel` and scrolls it.
* **Focusable:** `focusable: true`, pressable nodes (unless `focusable: false`),
  `TextInput`; never `disabled: true` or inside `display: none`. Extra event
  fields: `focus` has `focusVisible` (true when the last input was a key or
  gamepad, like `:focus-visible`) and `relatedTarget`; `blur` has
  `relatedTarget`; synthesized clicks have `pointerType: 'keyboard'|'gamepad'`;
  `gamepadbutton` has `pressed` and `repeat`; `gamepadconnected` has
  `connected` and `id`.
* **Focus scopes:** the last displayed node with `accessibilityViewIsModal` or
  `aria-modal` traps Tab and spatial navigation; focus moves into it when it
  appears (its `autoFocus`/`hasTVPreferredFocus` child or first focusable) and
  is restored to the previously focused node when it is removed or hidden.
  `nextFocusUp/Down/Left/Right` (node ids) override spatial navigation.
* Stick: deadzone 0.5 on the dominant axis; held D-pad / stick repeats after
  400 ms, then every 120 ms. Focus changes scroll the node into view in its
  ScrollView ancestors.

## Styles

Style keys follow React Native, including RN 0.76+ web-style additions:

* Layout: all Yoga props (`flex*`, `align*`, `justifyContent`, `gap`,
  `rowGap`, `columnGap`, `margin*`, `padding*`, `position`, `top/right/bottom/left`,
  `inset*`, `width/height/min*/max*`, `aspectRatio`, `display`, `overflow`,
  `borderWidth*`, `direction`, `flexWrap`, `boxSizing`). Numbers are px,
  strings may be `"50%"` or `"auto"`.
* Paint: `backgroundColor`, `opacity`, `borderColor*`, `borderRadius*`
  (corner and logical variants), `borderStyle`, `boxShadow` (CSS string or
  RN object array, incl. `inset` and `spread`), legacy
  `shadowColor/shadowOffset/shadowOpacity/shadowRadius`,
  `experimental_backgroundImage` / `backgroundImage` (`linear-gradient`,
  `radial-gradient`), `transform` (RN array or CSS string), `transformOrigin`,
  `filter` (`blur`, `brightness`, `contrast`, `saturate`, `grayscale`),
  `zIndex`, `pointerEvents`, `cursor`.
* Text: `color`, `fontFamily`, `fontSize`, `fontWeight`, `fontStyle`,
  `lineHeight`, `letterSpacing`, `textAlign`, `textTransform`,
  `textDecorationLine`, `textDecorationColor`, `textShadow*`; props
  `numberOfLines`, `ellipsizeMode`.
* Image: `resizeMode` (style or prop), `tintColor`; `source={{uri}}` or
  `source={require('./a.png')}` (the bundler emits a URL).

Colors are CSS colors (`#rgb[a]`, `#rrggbb[aa]`, `rgb[a]()`, `hsl[a]()`, names).
The default font is Inter (bundled); `fontFamily` falls back to it.

## Builds

* Native: `cmake -S . -B build -G Ninja && cmake --build build`. Dependencies
  are fetched at configure time (QuickJS-ng, Yoga via git; Skia prebuilt zip).
* WASM: `scripts/build-skia-wasm.sh` (once), then `emcmake cmake -S . -B build-wasm`.
  The `web/` host loads `jsui.wasm`, forwards DOM input, blits frames to a canvas.
* JS: `npm install` at the repo root (npm workspaces), `npx jsui build app.jsx`
  or `npx jsui dev app.jsx` (serves `bundle.js` and a reload endpoint).

## Dev mode

`jsui dev app.jsx` rebuilds on change and serves `http://localhost:5173/bundle.js`.
Point any host at it (`jsui-viewer --dev http://localhost:5173/bundle.js`, or
`cfg.dev_mode = 1` + `jsui_load_bundle_url`). In dev mode the runtime long-polls
`/__jsui/version` on the bundle's origin and reloads when it changes; JS errors
are shown in an on-screen overlay instead of a blank frame.

## Reference tests

`tests/reference` renders each example twice — with JSUI (`jsui-render`) and
with react-native-web in headless Chromium (Playwright), same viewport, same
Inter font files — and compares the PNGs with a per-pixel tolerance. A failing
case writes `expected/actual/diff` images and an HTML report.
