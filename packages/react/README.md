# @orktes/jsui-react

React Native's API on the JSUI game-UI runtime (QuickJS + Yoga + Skia).
Write ordinary React Native code; `@orktes/jsui-cli` aliases `react-native` to this
package when bundling.

```jsx
import { AppRegistry, View, Text, Pressable, StyleSheet } from 'react-native';

function App() {
  return (
    <View style={styles.menu}>
      <Pressable hasTVPreferredFocus onPress={() => Host.postMessage('start')}
        style={({ pressed, focused }) => [styles.button, (pressed || focused) && styles.active]}>
        <Text style={styles.label}>Start</Text>
      </Pressable>
    </View>
  );
}
AppRegistry.registerComponent('main', () => App); // starts rendering automatically
```

### Several root views

One bundle can show several independent UIs, like React Native root views:
register each component and let the host instantiate them by name
(`jsui_surface_create(rt, "Nameplate", "{\"name\":\"Orc\"}", 160, 40, 1)` in
C; the Godot and web integrations expose the same). Each surface gets its
initial props, its own size (`useWindowDimensions()`), input and focus, and
shares state, fonts, images and the JS context with the others.

```jsx
AppRegistry.registerComponent('Hud', () => Hud);
AppRegistry.registerComponent('Nameplate', () => Nameplate); // ({ name }) => ...
```

`RootTagContext` gives a component its surface's root tag. The first
registered component auto-starts on the main surface unless the host picks
one. Hosts may push props to many surfaces every frame: unchanged props cost
nothing, and the changed ones re-render in one batch on the next tick, only
the surfaces whose props changed.

React 19.3.1 with `react-reconciler` 0.34.0 (pinned; both run on QuickJS:
no DOM, no Node APIs).

## What is supported

* **Components:** `View`, `Text` (nested, `onPress`, `numberOfLines`),
  `Image` (`source` as `{uri}`, `require()`d asset with intrinsic size, number,
  array; `resizeMode`, `tintColor`, `onLoad`/`onError`), `ImageBackground`,
  `ScrollView` (`horizontal`, `contentContainerStyle`, `onScroll`,
  `scrollTo`/`scrollToEnd` via ref), `FlatList` / `SectionList` /
  `VirtualizedList` (not virtualized; rows memoized; separators, header /
  footer / empty, `numColumns`, `inverted`, `onEndReached`, `scrollToIndex`),
  `Pressable`, `TouchableOpacity`, `TouchableHighlight`,
  `TouchableWithoutFeedback`, `TouchableNativeFeedback`, `Button`,
  `TextInput` (native editing: caret, selection, clipboard, undo; controlled /
  uncontrolled `value`, controlled `selection`, `onChange`, `onChangeText`,
  `onSelectionChange`, `onSubmitEditing`, `onKeyPress`, `submitBehavior`,
  `maxLength`, `multiline`, `secureTextEntry`, `selectTextOnFocus`,
  `caretHidden`, `cursorColor`, `selectionColor`; ref `focus` / `blur` /
  `clear` / `isFocused` / `setSelection`), `ActivityIndicator`, `Switch`,
  `Modal` (portal to the root, focus scope, Escape / gamepad B →
  `onRequestClose`), `SafeAreaView`, `KeyboardAvoidingView`, `StatusBar`.
* **APIs:** `StyleSheet` (`create`, `flatten`, `compose`, `absoluteFill`,
  `hairlineWidth`), `Animated`, `Easing`, `PanResponder`, `Platform`
  (`OS === 'jsui'`; `select` prefers `jsui`, then `native`, then `default`),
  `Dimensions`, `useWindowDimensions`, `PixelRatio`, `AppRegistry`,
  `Appearance` / `useColorScheme` (`'dark'`), `I18nManager`, `BackHandler`
  (Escape / gamepad B), `Linking`, `Alert`, `Keyboard`, `AppState`,
  `InteractionManager`, `LayoutAnimation` (no-op), `DeviceEventEmitter`,
  `NativeEventEmitter`, `findNodeHandle`, `processColor`, `useAnimatedValue`.
  Every other `react-native` export exists as a harmless stub (warns once
  where it matters) so imports never fail.
* **Shims** (aliased by the CLI): `react-native-safe-area-context`,
  `expo-status-bar`, `expo-linear-gradient`, `react-native-linear-gradient`
  (rendered as a native `linear-gradient` background), `lottie-react-native`,
  `expo-font` (so `@expo-google-fonts/*` packages work unchanged).

## JSUI extras

```js
import { Host, useHostMessage, Fonts, useFonts, useGamepad, useFocus } from '@orktes/jsui-react';

Host.postMessage('buy', { item: 'sword' });          // -> embedder on_message
const off = Host.onMessage('hp', (hp) => ...);        // <- jsui_post_message
useHostMessage('hp', (hp) => setHp(hp));
await Fonts.load('Orbitron', require('./orbitron.ttf'), { weight: 700 }); // see Fonts below
const pad = useGamepad({ onButton: (e) => e.buttonIndex === 9 && (openMenu(), e.preventDefault()) });
const { focused, focusProps } = useFocus();           // <View {...focusProps} />
```

Frame data (`jsui_set_buffer`: typed arrays the game pushes every frame, no
JSON): `HostBuffer.get(key)` (a typed array over the runtime's memory),
`useHostBuffer(key)` (re-renders on every change), `useHostBufferSelector(key,
selector, fallback)` (only when the result changes), `useHostBufferListener(key,
cb)` (no re-render), and `useHostBufferValue(key, index)` /
`Animated.hostBuffer(key, index)`: a native Animated node, so views follow the
buffer without JS running per frame.

Host textures (`jsui_set_texture`: video frames, render targets or any
picture the host draws itself): `<HostTexture name="trailer"
resizeMode="contain" style={...} />` shows texture `name` like an `Image`
(sized by the texture unless styled), `useHostTexture(name)` returns
`{ width, height }` once the host set it (else `null`), and `HostTextures.get /
keys / subscribe` read the same. New contents repaint the view without any
JS.

Audio (played by the host: Godot's audio buses, Web Audio in the browser,
the built-in miniaudio backend natively; see docs/AUDIO.md):
`Sound.play(source, { bus, volume, pitch, pan, loop, fadeIn, offset })`
returns a voice (`stop({ fade })`, `pause()`, `resume()`, `setVolume(v, {
fade })`, `ended` promise), `Sound.preload`, `Sound.setBusVolume / setBusMuted /
stopAll`, `useSound(source)` (a preloaded player), `useMusic(source, { fadeIn,
fadeOut, volume })` (plays while mounted, crossfades), and interaction sounds:
`<UISounds sounds={{ hoverIn, focus, press }}>` for every pressable element
inside, or a `sounds` prop per Pressable / Touchable.

Lottie: `<LottieView source={require('./anim.json')} autoPlay loop />` with
lottie-react-native's props (`source` as an object, JSON text or `{uri}`,
`progress` (also an `Animated` value, on the native driver too), `speed`,
`loop`, `autoPlay`, `resizeMode`, `colorFilters`, `onAnimationLoaded`,
`onAnimationFinish`, `onAnimationFailure`) and ref methods (`play(start?,
end?)`, `pause`, `resume`, `reset`). The runtime plays it with Skia's Skottie:
no JS per frame. `import LottieView from 'lottie-react-native'` works too
(the CLI aliases it).

Shaders: `<ShaderView source={sksl\`...\`} uniforms={{...}} images={{...}} />`
paints the view with your own SkSL (Skia runtime effects, on every
renderer). `uniforms` take numbers, arrays, colors and `Animated` values
(also inside arrays; they become `uniform:<name>` style keys, so the native
driver and `Animated.hostBuffer` drive them with no JS per frame). `time`
(seconds since mount; the view then repaints every frame), `resolution`
and `pixelRatio` are filled in when declared and not given; `paused` stops
the clock. `images` gives child shaders (`uniform shader name;`): an image
source or `{ texture: 'name' }` (a host texture), with `fit` and `tile`. A
child named `content` that `images` leaves out is the view's own painting,
for post-processing. `onError` reports compile errors; `compileShader(src)`
returns `{ error }` or `{ uniforms, children }`.

Focus props on any element: `focusable`, `autoFocus`, `hasTVPreferredFocus`,
`onFocus`, `onBlur`, `onKeyDown` / `onKeyUp` (RN-web style: `e.key`,
`e.code`, modifiers), `nextFocusUp/Down/Left/Right`. Pressables are focusable
by default; Enter / Space and gamepad A press the focused one.

### Fonts

Inter (400-700, italic) is built in. Other fonts (TTF, OTF, TTC, WOFF,
WOFF2, variable) load at runtime; with the CLI, `require('./x.ttf')` is a URL
relative to the bundle. The API is expo-font compatible, plus extras:

```js
const [loaded, error] = useFonts({
  Cinzel: require('./Cinzel.ttf'),                                   // weight / style from the file
  Game: [                                                            // several faces under one family
    { source: require('./Game-Regular.otf'), weight: 400 },
    { source: require('./Game-Bold.otf'), weight: 'bold' },
    { uri: 'fonts/Game-Italic.woff2', weight: 400, style: 'italic' },
  ],
});                                                                  // already loaded: [true, null] at once

await Fonts.load('Mono', require('./Mono.ttf'), { weight: 700, style: 'normal' });
await Fonts.loadAsync('Runes', new Uint8Array(bytes));             // or loadAsync({ Family: source })
Fonts.isLoaded('Mono'); Fonts.isLoading('Game'); Fonts.getLoadedFonts();
await Fonts.unloadAsync('Mono'); await Fonts.unloadAllAsync();     // only fonts loaded here; Inter stays
Fonts.setFallbacks(['Noto Color Emoji', 'Noto Sans JP']);         // for glyphs the styled font lacks
Fonts.setDefaultFamily('Game');                                    // when a style names no fontFamily
```

Sources: URL string, `require(...)`, `{ uri }`, `{ default }` (ESM interop),
`ArrayBuffer` / typed array (registered synchronously). `weight`: number or
`'bold'` / `'normal'` / `'semibold'` ...; `style`: `'italic'` / `'normal'`;
omitted, both come from the file. Identical loads run once; a failed load
rejects (and `useFonts` returns `[false, error]`) and can be retried. Styles:
`fontFamily` takes a fallback list (`'Game, Noto Sans JP'`),
`fontVariationSettings` (`"'wght' 650"` or `{ wght: 650 }`) and
`fontFeatureSettings` (`"'ss01' 1, 'liga' 0"` or `{ ss01: 1 }`). Hosts can
register fonts too (`jsui_register_font`, Godot `register_font`, web
`registerFont` / `loadFont`).

## How it works (and why it is fast)

* Host config maps React's mutations onto `__jsui_native` (see
  `docs/ARCHITECTURE.md`). `View` is the host element itself (no wrapper
  fiber). Props cross the bridge only when something native-visible changed:
  React props are pre-checked by reference, then the flattened style and
  other values are deep-compared against what was last sent.
* Events: `__jsui_dispatchEvent` builds the path from the instance map, runs
  `onXxxCapture` root→target then `onXxx` target→root with RN-shaped
  synthetic events, and returns a bitmask (1 = handled, 2 =
  `preventDefault()`) so native can skip default actions. Discrete events
  render synchronously (React's sync lane, flushed in the same tick).
* `Animated` never re-renders React: one shared `requestAnimationFrame` loop
  advances all animations, and each animated component gets one
  `__jsui_native.patchStyle(id, partialStyle)` per frame (falls back to
  `setProps` with a merged style when the host lacks `patchStyle`).
  `useNativeDriver` is accepted and ignored.
* Uncaught render errors are logged with the component stack and handed to
  `__jsui_reportError` (if the host defines it) or rethrown from a timer so
  the runtime's error overlay / `jsui_last_error` sees them.

## Performance (QuickJS, `jsui-render`, Release, shared 4-core box)

`bench/commit-bench.jsx`, 500 host Views, ms per synchronous commit (ranges
over 3 runs on a loaded machine):

| Scenario | ms | bridge calls |
| --- | --- | --- |
| mount 500 views | 20–25 | 501 |
| re-render 500 plain components, nothing changed | 10–17 | 0 |
| re-render 500 `memo` components, nothing changed (parent only) | 2.9–4.0 | 0 |
| 50 of 500 `memo` components changed | 4.3–5.8 | 50 |
| 500 of 500 changed | 14–17 | 500 |
| `Animated`: 500 values set, opacity only (no React) | ~1.7 | 500 |
| `Animated`: 500 values set, opacity + transform | 5–8 | 500 |
| raw `__jsui_native.patchStyle` × 500 (opacity) | ~0.4 | 500 |

QuickJS is an interpreter, so React's own reconciliation dominates; keep
large lists/grids `memo`ized.

The steady state of a HUD costs nothing in React: animations go through
`patchStyle`, and state changes re-render only the components that changed
(use `memo` for large lists / grids).

## Tests

`npm test -w @orktes/jsui-react` (Node `node:test`, bridge mocked in
`test/mock-native.js`).
