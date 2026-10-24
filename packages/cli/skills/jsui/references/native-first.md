# Native-first patterns: keep JS off the frame

JSUI runs React on QuickJS. Anything that changes **every frame** should be
evaluated by the runtime natively instead: it costs nanoseconds, never lags a
frame behind the game, and keeps moving while JS is busy (a big commit, a
GC pause) or not ticked at all (`jsui_tick_native`). Reach for these by
default; use React state only for things that change now and then.

| You want | Use | Not |
|---|---|---|
| Motion: fades, slides, springs, loops | `Animated.*` with `useNativeDriver: true` | `setState` in `setInterval` / rAF |
| UI following game data (markers, nameplates, bars) | `useHostBufferValue` / `Animated.hostBuffer` | `useHostBuffer` + render per frame |
| A value **computed** from animated / game values (clamp to screen, distance fade, conditional offsets) | `Animated.derive(inputs, fn)` | `addListener` → compute → `setValue` / `setState` (one frame late, JS every frame) |
| Procedural motion (bob, pulse, orbit) without an animation | `Animated.derive([Animated.clock()], (t) => ...)` | a JS timer |
| Scroll-linked effects | `Animated.event` with `useNativeDriver: true` + `interpolate` / `derive` | `onScroll` → `setState` |

## Animated.derive

```jsx
import { Animated, clamp, Expr } from 'react-native';

const SCREEN_W = 1920;

function Nameplate({ i, offset /* Animated.Value */ }) {
  const x = useHostBufferValue('units', i * 3);
  const hp = useHostBufferValue('units', i * 3 + 2);
  const cam = useHostBufferValue('camera', 0);
  const { left, fill, fade } = useMemo(() => ({
    // Constants (SCREEN_W) and Animated values (offset) are captured.
    left: Animated.derive([x, cam], (ux, cx) => clamp(ux - cx + offset, 0, SCREEN_W - 120)),
    fill: Animated.derive([hp], (h) => h / 100),
    fade: Animated.derive([x, cam], (ux, cx) => {
      const d = Math.abs(ux - cx);
      if (d > 1500) return 0;
      return 1 - Expr.smoothstep(900, 1500, d);
    }),
  }), [x, hp, cam, offset]);
  return (
    <Animated.View style={{ opacity: fade, transform: [{ translateX: left }] }}>
      <Animated.View style={[styles.fill, { transform: [{ scaleX: fill }] }]} />
    </Animated.View>
  );
}
```

- `fn` gets the inputs' current values as numbers and returns a number.
  Write it **inline** in the `derive(...)` call (or define it once with
  `Animated.expr(fn)` and pass that): `jsui build` keeps its source and
  captures the names it reads.
- The body is a small math subset: numbers, the parameters, `const` / `let`
  locals, `+ - * / % **`, comparisons, `&& || !`, `?:`, `if (...) return ...;
  else ...`, `Math.*` (sin, cos, atan2, min, max, hypot, pow, sqrt, round,
  floor, abs, sign, PI, ...) and the helpers `clamp`, `lerp` / `mix`, `step`,
  `smoothstep`, `fract`, `wrap` (positive modulo), importable from
  `react-native` (also as `Expr.clamp`, ...). Booleans are 1 / 0; NaN
  becomes 0.
- Captured names: numbers and booleans become constants (their value when
  `derive` ran: re-create it in `useMemo` deps when they change), Animated
  nodes become extra inputs. Anything else (arrays, objects, function calls,
  loops, strings) is a build warning, and the node throws `cannot run
  natively` when it would have to: precompute it in JS, or pass it in as an
  input.
- It goes native automatically when any input is native (a native-driven
  value, a host buffer, the clock). Otherwise the JS driver evaluates the
  same program (identical results).
- `Animated.clock()` is the frame clock in seconds; a mounted clock keeps
  the host ticking (`jsui_wants_frame`).
