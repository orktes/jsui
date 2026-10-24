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
| Numbers that count up / tick (scores, XP, timers, ammo) | `<Animated.Text text={Animated.format(value, {...})} />` | `addListener` → `setState` → re-render |
| Choreography: sequence / parallel / stagger / loop | `Animated.sequence(...)` etc. with every step `useNativeDriver: true` (delays too): runs as one native group | mixing JS-driven steps in (the whole composite then hops through JS between steps) |
| Hover / press / focus feedback (highlight, press-down scale, focus ring) | `hoveredStyle` / `pressedStyle` / `focusedStyle` / `focusVisibleStyle` / `disabledStyle` + `transition` on any element (Pressable, View, Text, Image): swapped by the runtime on input | `style={({pressed, hovered}) => ...}` or `onHoverIn` → `setState` (a JS round trip and a re-render per change) |
| Children reacting to the parent's hover / press / focus (card hover lights up its title and icon) | `group` on the parent, `groupHoveredStyle` / `groupPressedStyle` / `groupFocusedStyle` / `groupFocusVisibleStyle` on descendants | passing hover state down through props |
| Driving anything else from interaction (a glow's intensity, an icon's rotation, a derive) | `useInteraction()` → `interaction={ix}` on the element, then `ix.hovered` / `ix.pressed` / `ix.focused` (native 0..1 values) | listeners → `setState` |
| A descendant reading its group's state (tooltips, popouts, reveal-on-hover buttons) | `useGroupState(name?)` under a `<Group>` or `<Pressable group>`: the same native values, plus `.instant.hovered` (0 / 1 at once) | prop drilling `hovered` + re-renders |
| Showing / hiding without a render (popouts, warnings, HUD pieces that toggle with game data) | `<Visibility visible={node or bool} transition={120}>` | `{cond && <X/>}` driven by per-frame data |
| UI bound to a field of shared host state (health, ammo, name) that must update on the frame the game sets it | `useHostStateBind('player', 'health.current')`: a native value for styles, `Animated.Text`, `derive`, `Visibility` | `useHostState` → render (waits for the next JS tick, re-renders) |
| Animating size / position (`width`, `height`, `top`, `flex`) | the native driver works for layout keys too (relayout per frame); prefer transforms when they do the job | JS-driven layout animation |

## State styles: hover, press, focus without JS

```jsx
<Pressable
  onPress={buy}
  group                                   // descendants can follow this element's state
  style={styles.card}                     // the base, as usual
  hoveredStyle={{ backgroundColor: '#2a3550', transform: [{ scale: 1.03 }] }}
  pressedStyle={{ transform: [{ scale: 0.97 }] }}
  focusVisibleStyle={{ borderColor: '#7cf' }}  // keyboard / gamepad focus only
  disabledStyle={{ opacity: 0.4 }}
  disabled={!canAfford}
  transition={{ duration: 120, easing: 'easeOut', pressed: 40 }}
>
  <Text style={styles.title} groupHoveredStyle={{ color: '#fff' }}>Buy</Text>
  <Image source={coin} style={styles.icon} groupPressedStyle={{ opacity: 0.6 }} />
</Pressable>
```

- The runtime merges the active states' styles over `style` every frame
  input changes (no JS, no render), so the feedback shows on the frame of
  the input even while JS is busy. React re-renders keep the overlays.
- States: `hovered` (mouse over it or a descendant), `pressed` (a primary
  press that began on it, while the pointer stays over it; Enter / Space /
  gamepad A on it while focused; a click shorter than a frame still shows
  one frame), `focused` (any focus), `focusVisible` (focus from keyboard /
  gamepad, like CSS `:focus-visible`: use this for focus rings in game UIs,
  so a mouse click does not leave a ring behind), `disabled` (the element's
  `disabled` prop; it also suppresses hovered / pressed).
- Blend order, later wins: group states, then hovered, focused,
  focusVisible, pressed, disabled.
- `transition`: ms, or `{ duration, easing, delay }` (`'linear'`, `'ease'`
  (default), `'easeIn'`, `'easeOut'`, `'easeInOut'`, or `[x1, y1, x2, y2]`),
  with per-state overrides (`{ duration: 150, pressed: 0, focusVisible: {
  duration: 80 } }`). Numbers, colors (a missing base fades in from
  transparent), transforms with the same functions (a missing base starts
  from identity), `shadowOffset` interpolate; other keys switch halfway.
  Layout keys (`borderWidth`, `padding`, `width`) work and relayout.
- Group styles: `group` (or `group="card"`) on an ancestor;
  `groupHoveredStyle` etc. on any descendant follow the nearest group (or
  the one named by `groupName="card"`). Group focus is focus *within* the
  group. A descendant without its own `transition` uses the group's.
- Use `style={({ pressed }) => ...}` only when the change is not a style
  (different children, text).

```jsx
const ix = useInteraction();
const glow = useMemo(() => ix.hovered.interpolate({ inputRange: [0, 1], outputRange: [0, 0.8] }), [ix]);
<Pressable interaction={ix} transition={200} onPress={open}>
  <Animated.View style={[styles.glow, { opacity: glow }]} />
  ...
</Pressable>
```

`useInteraction()` values follow the element's `transition` and work
anywhere a native Animated node does (styles, `interpolate`,
`Animated.derive`); they read 0 on the JS driver.

## Group state, Visibility and popouts

```jsx
import { Group, Visibility, useGroupState, Animated } from 'react-native';

function Tooltip({ item }) {
  const slot = useGroupState();  // the nearest <Group> / <Pressable group>
  return (
    <Visibility
      visible={slot.instant.hovered}         // any Animated node: nonzero shows
      transition={{ duration: 120, easing: 'easeOut' }}
      effect="scale"                         // 'fade' (default with a transition) | 'scale' | 'none'
      style={styles.tooltip}                 // position: 'absolute' next to the slot
    >
      <Text style={styles.tipTitle}>{item.name}</Text>
      <Text style={styles.tipBody}>{item.description}</Text>
    </Visibility>
  );
}

function Slot({ item }) {
  return (
    <Pressable group onPress={() => equip(item)} style={styles.slot} hoveredStyle={styles.slotHover} transition={100}>
      <Image source={item.icon} style={styles.icon} />
      <Tooltip item={item} />
    </Pressable>
  );
}
```

- `useGroupState(name?)` returns `{ hovered, pressed, focused,
  focusVisible, disabled }`: native values, 0..1 eased by the group's
  `transition`. `.instant.hovered` etc. switch 0 / 1 on the frame the state
  changes: use them for `Visibility` (the Visibility has its own
  transition) and the eased ones to drive styles directly
  (`opacity: slot.hovered`, `slot.hovered.interpolate(...)`,
  `Animated.derive([slot.hovered], ...)`). With a name it finds an outer
  `<Group name="...">` / `<Pressable group="...">`.
- `<Group name? transition? style>` is a View that is a group, for when the
  group itself is not pressable.
- `<Visibility visible mode effect scale transition style>`: `visible` is a
  boolean or an Animated node; the runtime applies it on the frame it
  changes. `mode="remove"` (default) takes it out of layout, paint and hit
  testing once hidden (like `display: 'none'`), `mode="hide"` keeps its
  layout. The children stay mounted (state kept, no mount cost on show).
- Popouts are drawn inside their parent: a parent with `overflow: 'hidden'`
  (ScrollView, clipped panels) clips them. Put the slot's popout outside the
  clipping container, or give the container room.

## Host state bound natively

```jsx
const hp = useHostStateBind('player', 'health.current');  // jsui_set_state("player", ...)
const name = useHostStateBind('player', 'name');
const low = useMemo(() => Animated.derive([hp], (h) => h > 0 && h < 25), [hp]);
<Animated.View style={[styles.bar, { width: hp }]} />
<Animated.Text text={name} />
<Visibility visible={low}><Text style={styles.warn}>LOW HEALTH</Text></Visibility>
```

The runtime re-reads the field the moment the host calls `jsui_set_state`
/ `jsui_patch_state` (also under `jsui_tick_native`), so the UI shows the
value in the frame the game set it, with no JS tick and no render.
Numbers, booleans (1 / 0) and strings (for `text`) bind; the path is
`'a.b.0.c'` or an array. JS still sees the state through `useHostState`.
Use frame-data buffers instead for values that change every frame.

## Animated.Text with a live number

```jsx
const xp = useAnimatedValue(0, { useNativeDriver: true });
const label = useMemo(() => Animated.format(xp, { decimals: 0, grouping: ',', prefix: 'XP ' }), [xp]);
useEffect(() => {
  Animated.timing(xp, { toValue: 25000, duration: 1200, useNativeDriver: true }).start();
}, []);
return <Animated.Text style={styles.xp} text={label} />;  // "XP 0" ... "XP 25,000", no re-renders
```

`text` takes any Animated node: `Animated.format` (`decimals` (default:
shortest), `grouping` (thousands separator), `prefix`, `suffix`,
`minDigits` (zero padding: `minDigits: 2` for clocks)) or a plain numeric
node. Combine with `Animated.derive` for computed numbers, e.g. a countdown:
`Animated.format(Animated.derive([t], (s) => Math.ceil(s)), { suffix: 's' })`.

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
