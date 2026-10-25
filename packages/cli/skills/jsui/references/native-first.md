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
| Popouts, tooltips, dropdowns that must not be clipped by a ScrollView / `overflow: 'hidden'` panel or covered by later siblings | `<Overlay placement="top" offset={8}>` around the popout (placed natively against its parent or `anchor` ref, flips / shifts on screen) | `zIndex` on the hovered item, `measure` + absolute coordinates, rendering the popout at the screen root by hand |
| UI bound to a field of shared host state (health, ammo, name) that must update on the frame the game sets it | `useHostStateBind('player', 'health.current')`: a native value for styles, `Animated.Text`, `derive`, `Visibility` | `useHostState` → render (waits for the next JS tick, re-renders) |
| Animating size / position (`width`, `height`, `top`, `flex`) | the native driver works for layout keys too (relayout per frame); prefer transforms when they do the job | JS-driven layout animation |

## JS thread: what runs where

On native hosts (the engine plugins, the viewer, the examples, mobile) the
bundle's JS runs on a thread of its own, as React Native's does; the
browser, the CLI tools and the tests run it inside the tick (single
threaded, same frames). With the thread:

- **Every frame, natively, also while JS is busy** (a big commit, a GC, a
  slow handler): native-driven `Animated` (composites, springs, `derive`,
  `clock`), host-buffer values, `useHostStateBind`, state styles and group
  styles, `Visibility`, scrolling and flings, `LayoutAnimation`, focus
  movement, hit testing, layout and paint. Build the parts that must never
  hitch from these; that is what this page is about.
- **On the JS thread, a frame or more later:** renders and commits, effects,
  timers, `requestAnimationFrame`, event handlers (`onPress`, `onScroll`
  without `Animated.event`), `useHostState` re-renders, JS-driven animations.
  A commit applies whole on the next tick, never half.
- **`measure` is async-ish.** `ref.measure` / `measureInWindow` /
  `measureLayout` answer through their callback after the layout that
  follows the current commit (as in React Native), not inside the call; the
  synchronous `measure` read returns the last frame's layout. Use
  `onLayout` or the callback; never read layout right after `setState`.
- **Same-frame host → UI changes** (a scene cut that swaps the HUD, a
  screenshot) are the host's job: it ticks with `JSUI_TICK_SYNC_JS`
  (`jsui_tick_ex`), or calls `jsui_sync` / `SyncJs()` / `sync_js()`, which
  waits for JS to finish what was sent. For a field that must follow the game
  on the very frame, bind it natively (`useHostStateBind`, a host buffer)
  instead.
- **Default actions JS may veto** (Tab / Enter / arrows with a `keyDown`
  listener, wheel with an `onWheel`) wait up to 100 ms for JS's answer; keep
  those handlers quick, or leave them off the focused path.

Check with a stall: the viewer's F7 (or `jsui_debug_stall_js(rt, 200)`)
keeps JS busy; everything above the second bullet must keep moving.

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
  the input even while JS is busy. React re-renders keep the overlays, and
  so do natively animated writes to a key a state style sets (a press scale
  over an animated `transform`): the write updates the base and shows again
  once the state lets go.
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
- A Visibility is drawn inside its parent: a parent with `overflow:
  'hidden'` (ScrollView, clipped panels) clips it and later siblings cover
  it. For popouts, wrap it in an `<Overlay>` (next section).

## Overlays: popouts above everything, placed natively

```jsx
import { Overlay, OverlayArrow, Visibility, useGroupState } from 'react-native';

function SlotPopout({ item }) {
  const slot = useGroupState();
  return (
    // Placed above the slot (its parent) by the runtime every frame; flips
    // below it when there is no room, slides sideways to stay on screen.
    <Overlay placement="top" offset={10} collisionPadding={8} pointerEvents="none">
      <Visibility visible={slot.instant.hovered} transition={120} effect="scale">
        <OverlayArrow style={styles.arrow} />  {/* first: the body covers its inner half */}
        <View style={styles.popout}>
          <Text style={styles.name}>{item.name}</Text>
        </View>
      </Visibility>
    </Overlay>
  );
}

function Inventory({ items }) {
  return (
    <ScrollView style={styles.grid}>  {/* clips its content: not the popouts */}
      {items.map((it) => (
        <Pressable key={it.id} group style={styles.slot} onPress={() => equip(it)}>
          <Image source={it.icon} style={styles.icon} />
          <SlotPopout item={it} />
        </Pressable>
      ))}
    </ScrollView>
  );
}
```

- `<Overlay>`'s children are drawn in the surface's overlay layer: above
  every other view (zIndex of the rest does not matter, a `Modal` included)
  and outside every clip. For everything else it stays where it is
  rendered: hover / press chains go through its parent (the pointer on the
  popout keeps the slot's group hovered, so an interactive popout stays
  open while the pointer moves onto it, as long as there is no gap: use
  `offset={0}` and padding, or a hover transition), events bubble to the
  parent's handlers, context and `useGroupState` work, Tab order follows
  the anchor, and it takes its parent's transform and opacity (a scaled
  stage scales its popouts; a fading screen fades them).
- The runtime places it every frame after layout, with no JS: it follows
  scrolling, `Animated` transforms of the anchor or its ancestors and
  layout changes. `anchor`: `'parent'` (default) or a ref / node id;
  `placement`: `top` / `bottom` / `left` / `right` / `center` / `auto`,
  with `-start` / `-end` to align an edge (`'bottom-start'` for a
  dropdown); `offset` (gap) and `crossOffset`; `flip` (default on) and
  `collision="shift"` (default) with `collisionPadding` keep it inside the
  viewport, or inside `boundary` (a ref, e.g. the screen root of a stage
  shown scaled in a page); `matchWidth` (`true` / `'min'`) sizes a dropdown
  to its anchor; `hideWhenClipped` (default on) hides it while the anchor is
  scrolled out of view. It is not shown while its parent is hidden
  (`display: 'none'`, `<Visibility mode="remove">`).
- `<OverlayArrow style>`: a view the runtime moves to the edge facing the
  anchor, centred on it after flips and shifts (a 45-degree rotated square
  with borders, drawn before the body).
- The overlay sizes to its content (up to the viewport width); give the
  popout a width. Without `pointerEvents="none"` it takes hits like any
  view (a tooltip that should let the pointer through needs it).

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
`minDigits` (zero padding: `minDigits: 2` for clocks), `time`) or a plain
numeric node. Combine with `Animated.derive` for computed numbers, e.g. a
countdown: `Animated.format(Animated.derive([t], (s) => Math.ceil(s)), { suffix: 's' })`.

`time: 'h:mm:ss'` (or `'m:ss'`, `'mm:ss'`, ...) formats whole seconds
(floored, at least 0) as clock fields; a doubled letter pads to two digits.
Without an `h` field the minutes are the total minutes (`'m:ss'` shows
`75:03`). A match timer counting down from 5 minutes, all native:

```jsx
const left = useMemo(() => new Animated.Value(300, { useNativeDriver: true }), []);
const label = useMemo(() => Animated.format(left, { time: 'm:ss' }), [left]);
useEffect(() => {
  Animated.timing(left, { toValue: 0, duration: 300000, easing: Easing.linear, useNativeDriver: true }).start();
}, []);
return <Animated.Text text={label} />;  // "5:00" ... "0:00"
```

A timer that has to pause: `left.stopAnimation()` and start a new timing
from where it stopped (`Animated.clock()` cannot pause).

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
