# Animation and performance

## Animated

React Native's `Animated` with both drivers. Animations never re-render
React: the JS driver patches styles once per frame; the **native driver**
runs in the runtime itself (no JS per frame, keeps going while JS is busy).

```jsx
import { Animated, Easing, Pressable } from 'react-native';
import { useAnimatedValue } from 'react-native';   // or useRef(new Animated.Value(0)).current

function Glow({ focused, children }) {
  const t = useAnimatedValue(0);
  useEffect(() => {
    Animated.spring(t, { toValue: focused ? 1 : 0, useNativeDriver: true, tension: 180, friction: 14 }).start();
  }, [focused]);
  return (
    <Animated.View style={{
      transform: [{ scale: t.interpolate({ inputRange: [0, 1], outputRange: [1, 1.04] }) }],
      opacity: t.interpolate({ inputRange: [0, 1], outputRange: [0.75, 1] }),
      borderColor: t.interpolate({ inputRange: [0, 1], outputRange: ['rgba(255,255,255,0.1)', '#f2c14e'] }),
      borderWidth: 1,
    }}>{children}</Animated.View>
  );
}
```

* Everything from RN: `Animated.Value`, `ValueXY`, `Color`, `timing`,
  `spring` (tension / friction, bounciness / speed or stiffness / damping /
  mass), `decay`, `sequence`, `parallel`, `stagger`, `loop`, `delay`,
  `add` / `subtract` / `multiply` / `divide` / `modulo` / `diffClamp`,
  `interpolate` (numbers, colors, `'45deg'` strings, `extrapolate`),
  `Animated.event` (also native, e.g. `onScroll` parallax),
  `createAnimatedComponent`, `Animated.View/Text/Image/ScrollView/FlatList`.
* **Native driver rules (same as RN):** only paint keys: `transform`,
  `opacity`, colors (`backgroundColor`, `borderColor`, `color`, `tintColor`,
  ...), border radii, `shadowOpacity` / `shadowRadius`, LottieView
  `progress`, ShaderView uniforms. Layout keys (`width`, `height`, `top`,
  `left`, `margin`, `flex`) throw "not supported by native animated module".
  Animate `transform: [{ scaleX }]` for a bar fill (with `transformOrigin:
  'left'`) instead of `width`, or use the JS driver for layout.
* A value used by a native animation stays native; don't also start a JS
  animation on it.
* Native animations run on the runtime clock, so `jsui screenshot --wait` /
  the `wait` step see them exactly.
* `Animated.hostBuffer(key, index)` / `useHostBufferValue` is a native value
  that follows game data each frame (see game-data.md).

Good game UI motion: springs for focus / press (fast, slightly
underdamped), `Animated.stagger` for list entrances (30-60 ms apart),
`Easing.out(Easing.cubic)` for screen transitions (200-350 ms), looping
`timing` for idle shimmer / pulse, and nothing that blocks input.

## LayoutAnimation

`LayoutAnimation.configureNext(LayoutAnimation.Presets.spring)` (or
`easeInEaseOut`, `linear`, `LayoutAnimation.create(duration, type, prop)`)
before a state change animates every layout change of the next commit:
moved / resized views glide, inserted views fade or scale in, removed ones
fade out. Good for chips, inventory grids, expanding panels. `onLayout` and
`measure` report final positions at once.

## Performance

A frame costs what changed, not the whole tree:

* Re-renders: keep state local; memoize rows (`React.memo`) and callbacks;
  read shared state with `useHostStateSelector(key, pick)` so a component
  re-renders only when its part changes.
* Never put per-frame data (positions, timers, health ticking) in React
  state. Use `Animated` values, native-driver animations or host buffers.
  Countdown text that changes once per second is fine as state.
* Lists: use `FlatList` with `keyExtractor` (and `getItemLayout` for fixed
  rows) for long lists; it virtualizes.
* Painting is incremental: an animating view repaints only its old and new
  rect. Large blurred shadows and `filter: blur()` over big areas cost the
  most on the CPU raster; prefer gradients for big glows.
* Images: size them close to their displayed size; `require` them so their
  size is known before load (no layout jump).
* Fonts: load custom fonts at startup (`useFonts`) and show the screen once
  loaded to avoid a reflow.
* Hosts can measure: `jsui_set_profiling` / `getPerf()` (web) /
  `JsuiView.profiling` + `get_perf()` (Godot) give µs per phase (js, commit,
  layout, paint) and counters (`bridge_calls`, `layout_passes`).
