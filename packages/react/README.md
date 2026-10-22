# @jsui/react

React Native's API on the JSUI game-UI runtime (QuickJS + Yoga + Skia).
Write ordinary React Native code; `@jsui/cli` aliases `react-native` to this
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
  `TextInput` (controlled / uncontrolled, `onChangeText`, `onSubmitEditing`,
  `maxLength`, `multiline`, `secureTextEntry`), `ActivityIndicator`, `Switch`,
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
  (rendered as a native `linear-gradient` background).

## JSUI extras

```js
import { Host, useHostMessage, Fonts, useFonts, useGamepad, useFocus } from '@jsui/react';

Host.postMessage('buy', { item: 'sword' });          // -> embedder on_message
const off = Host.onMessage('hp', (hp) => ...);        // <- jsui_post_message
useHostMessage('hp', (hp) => setHp(hp));
await Fonts.load('Orbitron', require('./orbitron.ttf'), { weight: 700 });
const pad = useGamepad({ onButton: (e) => e.buttonIndex === 9 && (openMenu(), e.preventDefault()) });
const { focused, focusProps } = useFocus();           // <View {...focusProps} />
```

Focus props on any element: `focusable`, `autoFocus`, `hasTVPreferredFocus`,
`onFocus`, `onBlur`, `onKeyDown` / `onKeyUp` (RN-web style: `e.key`,
`e.code`, modifiers), `nextFocusUp/Down/Left/Right`. Pressables are focusable
by default; Enter / Space and gamepad A press the focused one.

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

`npm test -w @jsui/react` (Node `node:test`, bridge mocked in
`test/mock-native.js`).
