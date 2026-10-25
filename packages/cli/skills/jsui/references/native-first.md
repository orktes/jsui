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
| Typewriter / text revealed character by character (dialogue, subtitles, quest text), styled spans included | `<Animated.Text reveal={value} revealFraction revealFade={2}>` + a native `Animated.timing` | slicing the string and re-rendering per character (relayouts, words jump lines) |
| A timer / clock that pauses, resumes, rewinds or runs at another speed (match timer, slow motion, game-paused HUD) | `Animated.clock({ paused, speed, running })` with `pause()` / `resume()` / `reset(t)` / `setSpeed(x)` | stopping and restarting timings by hand, a JS interval |
| Choreography: sequence / parallel / stagger / loop | `Animated.sequence(...)` etc. with every step `useNativeDriver: true` (delays too): runs as one native group | mixing JS-driven steps in (the whole composite then hops through JS between steps) |
| Hover / press / focus feedback (highlight, press-down scale, focus ring) | `hoveredStyle` / `pressedStyle` / `focusedStyle` / `focusVisibleStyle` / `disabledStyle` + `transition` on any element (Pressable, View, Text, Image): swapped by the runtime on input | `style={({pressed, hovered}) => ...}` or `onHoverIn` → `setState` (a JS round trip and a re-render per change) |
| Children reacting to the parent's hover / press / focus (card hover lights up its title and icon) | `group` on the parent, `groupHoveredStyle` / `groupPressedStyle` / `groupFocusedStyle` / `groupFocusVisibleStyle` on descendants | passing hover state down through props |
| Driving anything else from interaction (a glow's intensity, an icon's rotation, a derive) | `useInteraction()` → `interaction={ix}` on the element, then `ix.hovered` / `ix.pressed` / `ix.focused` (native 0..1 values) | listeners → `setState` |
| A descendant reading its group's state (tooltips, popouts, reveal-on-hover buttons) | `useGroupState(name?)` under a `<Group>` or `<Pressable group>`: the same native values, plus `.instant.hovered` (0 / 1 at once) | prop drilling `hovered` + re-renders |
| Showing / hiding without a render (popouts, warnings, HUD pieces that toggle with game data) | `<Visibility visible={node or bool} transition={120}>` | `{cond && <X/>}` driven by per-frame data |
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
  listener, wheel with an `onWheel`, D-pad with an `onGamepadButton` or
  `useGamepad({ onButton })`) wait up to 100 ms for JS's answer, unless you
  declare it (next section).

Check with a stall: the viewer's F7 (or `jsui_debug_stall_js(rt, 200)`)
keeps JS busy; everything above the second bullet must keep moving.

## Input without waiting on JS

A handler that *could* call `e.preventDefault()` makes the runtime wait for
JS before it moves focus, presses, types or scrolls. Say what the handler
does instead, and the runtime acts at once while the handler still runs
(later, for its side effects):

| The handler... | Declare | Effect |
|---|---|---|
| only observes (logging, sounds, analytics, a hotkey that is not a navigation key) | `onKeyDown={passive(fn)}` (also `onTextInput`, `onGamepadButton`, `onGamepadAxis`, `onWheel`; `import { passive } from '@orktes/jsui-react'`) | Tab / arrows / D-pad / wheel never wait |
| always takes certain keys (a menu that handles arrows itself, a field that eats Tab) | `preventDefaultKeys={['ArrowLeft', 'ArrowRight']}` (`key` or `code`; the element and its descendants) | those keys' default actions never run, at once; others are untouched |
| always takes certain gamepad buttons (LB / RB tab switching) | `preventDefaultButtons={[4, 5]}` | same, for button indices |
| uses `useGamepad` callbacks | `useGamepad({ onButton, passive: true })` or `vetoes: [4, 5]` (only those buttons may be vetoed) | D-pad navigation never waits |
| needs to decide at runtime (`if (canClose) e.preventDefault()`) | nothing: it waits (keep it quick) | |

```jsx
import { passive } from '@orktes/jsui-react';

<View focusable onKeyDown={passive((e) => log(e.key))} />           // never vetoes
<View preventDefaultKeys={['ArrowLeft', 'ArrowRight']}                // the carousel owns left / right
      onKeyDown={passive((e) => step(e.key === 'ArrowLeft' ? -1 : 1))} />
```

Built in: Pressables (Enter / Space press without waiting; Tab and
arrows move on), TextInput (typing, Backspace, caret keys; Enter when it
submits), Modal (Escape / Back / B with `onRequestClose`), `BackHandler`
(waits for the back key / B only), and the gamepad state behind
`useGamepad`. An `onKeyDown` you pass to a Pressable or TextInput makes it
wait again unless it is `passive(...)`. Focus traps (`Modal`,
`accessibilityViewIsModal`), `nextFocusUp/Down/Left/Right` and `focusable`
are native already.

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

A timer that has to pause: use a controllable clock (below), or
`left.stopAnimation()` and start a new timing from where it stopped.

## Clocks you control: Animated.clock({...})

`Animated.clock()` is the shared frame clock (always running). Pass options
and you get a clock of your own: an Animated value counting seconds times
`speed` from `start`, run natively (no JS per frame). `pause()`,
`resume()`, `reset(t)` (default: `start`; keeps running or paused) and
`setSpeed(x)` (0 holds, negative counts down) are one command each. A
paused clock, or one nothing reads, does not keep the host ticking
(`jsui_wants_frame`).

```jsx
// A match timer counting down from 5:00 that pauses with the game menu.
const clock = useMemo(() => Animated.clock({ start: 300, speed: -1 }), []);
const label = useMemo(() => Animated.format(clock, { time: 'm:ss' }), [clock]);
useHostMessage('menu', (open) => (open ? clock.pause() : clock.resume()));
return <Animated.Text text={label} />;
```

`running`: an Animated node that holds the clock while it is 0, natively:
the frame the host changes it, without JS. A HUD clock that stops whenever
the game sets `game.playing` to false:

```jsx
const playing = useHostStateBind('game', 'playing');  // true / false from the host
const t = useMemo(() => Animated.clock({ running: playing }), [playing]);
const spin = useMemo(() => Animated.derive([t], (s) => (s * 90) % 360), [t]);
```

Options: `start` (seconds, default 0), `speed` (default 1), `paused`
(start paused), `running` (an Animated node, e.g. `useHostStateBind`,
`ix.hovered`, a value). It is an `Animated.Value` otherwise: use it in
styles, `interpolate`, `derive`, `Animated.format`; `setValue(t)` is
`reset(t)`, and running `Animated.timing` on it pauses the clock. Pausing
keeps the time at the moment JS paused it.

## Typewriter: Animated.Text reveal

`reveal` shows only the first N graphemes (user-perceived characters:
spaces count, an emoji or a letter with its accents is one) of a Text and
its nested spans. The paragraph is laid out once with all of its text, so
nothing re-wraps or moves while it types, and a change only repaints (no
React render, no relayout). Drive it with a native timing:

```jsx
const shown = useMemo(() => new Animated.Value(0, { useNativeDriver: true }), []);
useEffect(() => {
  const a = Animated.timing(shown, { toValue: 1, duration: 3000, easing: Easing.linear, useNativeDriver: true });
  a.start(({ finished }) => finished && setTyped(true));  // completion: the timing's callback
  return () => a.stop();
}, []);
return (
  <Animated.Text style={styles.line} reveal={shown} revealFraction revealFade={2}>
    The gate did not fall. It was <Text style={styles.em}>opened</Text>.
  </Animated.Text>
);
// Skip: shown.setValue(1)
```

- `reveal`: how many units show (a number or an Animated node). A fraction
  rounds up (2.3 shows 3); 0 shows nothing; `null` / leaving it out shows
  everything.
- `revealFraction`: `reveal` is 0..1 of the whole text instead, so the
  timing needs no character count (and a language switch keeps its place).
  Time it from the text length, e.g. `duration: text.length * 18`.
- `revealMode="words"`: units are words (a word with the spaces after it;
  CJK and other unspaced scripts by dictionary words).
- `revealFade={n}`: each unit fades in over `n` units of reveal (unit k's
  opacity is `clamp((reveal - k) / n, 0, 1)`): a soft typing edge. It is in
  units, not milliseconds, so it scrubs and reverses with the value.
- Works on a plain `<Text reveal={n}>` too (a re-render per change), and on
  the JS driver (patched without a render). Only the outermost Text of a
  paragraph reveals; spans inside follow it.
- Per character sounds or effects: listen on the value only if you must
  (`addListener` costs JS per frame); `Animated.timing` completion is the
  usual hook.

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
  the host ticking (`jsui_wants_frame`). For one that pauses or runs at
  another speed: `Animated.clock({...})` (above).
