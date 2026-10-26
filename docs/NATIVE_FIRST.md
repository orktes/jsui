# Native-first UI: keeping JS off the frame

JSUI runs React on QuickJS. React is the right tool for structure and for
things that change now and then: a screen opens, an item is bought, a
setting flips. It is the wrong tool for anything that changes **every
frame** or must answer **on the frame of the input**. Every frame of JS
work is a frame where a commit, a GC pause or a slow handler can hitch the
UI. On native hosts, JS also runs on its own thread, so it answers a frame
or more late.

JSUI's answer is the same as React Native's Animated native driver, taken
further. You declare what should happen, React commits that description
once, and the runtime evaluates it natively every frame. It costs
nanoseconds, never lags behind the game, and keeps moving while JS is busy
or not ticked at all (`jsui_tick_native`).

This page explains when to use each feature. The full API with worked
examples is in the agent skill's
[native-first reference](../packages/cli/skills/jsui/references/native-first.md),
and how each one works inside the runtime is in
[ARCHITECTURE.md](ARCHITECTURE.md).

## Which feature for what

| You want | Use | Instead of |
|---|---|---|
| Fades, slides, springs, loops, choreography | `Animated.*` with `useNativeDriver: true` (sequence / parallel / stagger / loop run as one native group) | `setState` in a timer or rAF |
| A value computed from animated or game values (clamp to screen, distance fade, conditional offsets) | `Animated.derive(inputs, fn)`: the arrow function is compiled to a native expression | `addListener`, then compute, then `setValue` (a frame late, JS every frame) |
| Procedural motion (bob, pulse, orbit) | `Animated.derive([Animated.clock()], (t) => ...)` | a JS timer |
| Timers that pause, resume, rewind or change speed | `Animated.clock({ paused, speed, running })` with `pause()`, `resume()`, `reset(t)` and `setSpeed(x)`; `running` can be a native value (a host-state bind) | restarting timings by hand |
| Numbers and clocks on screen (score, XP, ammo, `m:ss`) | `<Animated.Text text={Animated.format(value, { decimals, grouping, prefix, time: 'm:ss' })} />` | a listener that re-renders a `<Text>` |
| Typewriter text (dialogue, subtitles) | `<Animated.Text reveal={value} revealFraction revealFade={2}>` driven by a native timing: paint only, no relayout | slicing the string per character |
| UI that follows game data (markers, nameplates, bars) | host buffers: `useHostBufferValue` / `Animated.hostBuffer` | reading the buffer and rendering per frame |
| A field of shared host state (health, ammo, name) on the frame the game sets it | `useHostStateBind('player', 'health.current')`: a native value for styles, text, `derive` and `Visibility` | `useHostState`, which re-renders on the next JS tick |
| Hover, press and focus feedback | `hoveredStyle`, `pressedStyle`, `focusedStyle`, `focusVisibleStyle`, `disabledStyle` and `transition` on any element | `style={({ pressed }) => ...}` or `onHoverIn` setting state |
| Children that react to a parent's state (a card's title lights up on hover) | `group` on the parent, `groupHoveredStyle` etc. on the children | passing `hovered` down as props |
| Anything else driven by interaction (a glow, a rotation, a derive) | `useInteraction()` and `interaction={ix}`, then `ix.hovered` etc. (native 0..1 values) | listeners setting state |
| A descendant reading its group's state | `useGroupState(name?)`; `.instant.hovered` switches at once | prop drilling and re-renders |
| Showing and hiding with data or hover | `<Visibility visible={node or bool} transition={120}>`: children stay mounted | `{cond && <X/>}` driven by per-frame data |
| Popouts, tooltips and dropdowns that must escape clipping and later siblings | `<Overlay placement="top" offset={8}>` with an optional `<OverlayArrow>`: placed against the anchor every frame, flipped and shifted to stay on screen | a `zIndex` on the hovered item, or `measure` and absolute coordinates |
| Per-frame logic that is not a math expression (a game-feel loop, physics, a state machine) setting Animated values | a worklet: `runOnUI(() => { ... requestAnimationFrame(step) ... })()` from `react-native-worklets`, calling `value.setValue(x)` on Animated values it captured (runs on the UI thread, writes the native nodes directly) | a JS `requestAnimationFrame` loop calling `setValue` |
| Key, button and wheel defaults (Tab, Enter, arrows, D-pad) that act at once | `passive(fn)` handlers, `preventDefaultKeys` / `preventDefaultButtons`, and `useGamepad({ passive })` or `vetoes` | handlers that could call `preventDefault`, which make the runtime wait for JS |

The rule of thumb: if a `setState`, `addListener` or timer would run every
frame or on every input event, one of these does the job natively.

## What runs where

On native hosts (the engine plugins, the viewer, the examples and mobile)
the bundle's JS runs on a thread of its own, as React Native's does. The
browser, the CLI and the tests run it inside the tick.

- **Natively, every frame, also while JS is busy:** native-driven
  `Animated` (including `derive`, `clock`, `format` and `reveal`),
  host-buffer values, host-state binds, state and group styles,
  `Visibility`, overlays, scrolling, `LayoutAnimation`, focus movement,
  declared default actions, hit testing, layout and paint; and worklets
  (`react-native-worklets`' `runOnUI` and friends: their own JS runtimes on
  the UI thread, with their own timers and `requestAnimationFrame`).
- **On the JS thread, a frame or more later:** renders, commits, effects,
  timers, `requestAnimationFrame`, event handlers, `useHostState`
  re-renders and JS-driven animations. A commit applies whole on the next
  tick, never half.
- **Layout reads are asynchronous.** `ref.measure`, `measureInWindow` and
  `measureLayout` answer through their callback after the layout that
  follows the current commit, as in React Native. Use `onLayout` or the
  callback.
- **Same-frame host-to-UI changes** (a scene cut that swaps the HUD, a
  screenshot) are the host's call. It ticks with `JSUI_TICK_SYNC_JS`
  (`jsui_tick_ex`), or calls `jsui_sync` (Godot `sync_js` /
  `sync_js_next_frame`, Unity and Unreal `SyncJs`), which waits until JS
  has finished what was sent. A field that must follow the game on the
  same frame is better bound natively.

To check a screen, stall JS on purpose: press F7 in the viewer or call
`jsui_debug_stall_js(rt, 200)`. Everything in the first bullet must keep
moving.

## Examples

A shop card with native feedback, a tooltip that escapes the grid and a
live price:

```jsx
import { useMemo } from 'react';
import { Animated, Overlay, OverlayArrow, Pressable, Text, View, Visibility, useGroupState } from 'react-native';

function Tip({ item }) {
  const card = useGroupState();
  return (
    <Overlay placement="top" offset={10} collisionPadding={8}>
      <Visibility visible={card.instant.hovered} transition={120} effect="scale">
        <OverlayArrow style={styles.arrow} />
        <View style={styles.tip}>
          <Text>{item.description}</Text>
        </View>
      </Visibility>
    </Overlay>
  );
}

function Card({ item, price }) {
  const label = useMemo(() => Animated.format(price, { grouping: ',', suffix: ' gold' }), [price]);
  return (
    <Pressable group onPress={() => buy(item)} style={styles.card}
               hoveredStyle={{ transform: [{ scale: 1.03 }] }} pressedStyle={{ transform: [{ scale: 0.97 }] }}
               focusVisibleStyle={{ borderColor: '#7cf' }} transition={{ duration: 120, pressed: 40 }}>
      <Text style={styles.name} groupHoveredStyle={{ color: '#fff' }}>{item.name}</Text>
      <Animated.Text style={styles.price} text={label} />
      <Tip item={item} />
    </Pressable>
  );
}
```

A HUD bar bound to game state, and a nameplate that fades with distance:

```jsx
const hp = useHostStateBind('player', 'health.current');
const max = useHostStateBind('player', 'health.max');
const fill = useMemo(() => Animated.derive([hp, max], (h, m) => Math.max(0, Math.min(1, h / m))), [hp, max]);
<Animated.View style={[styles.bar, { transformOrigin: 'left', transform: [{ scaleX: fill }] }]} />

// 'enemyDistance' is a Float32Array host buffer, one entry per enemy.
const dist = useMemo(() => Animated.hostBuffer('enemyDistance', i), [i]);
const opacity = useMemo(() => Animated.derive([dist], (d) => (d < 20 ? 1 : d > 40 ? 0 : (40 - d) / 20)), [dist]);
```

A match timer that pauses with the game, and dialogue that types itself:

```jsx
const running = useHostStateBind('game', 'running');
const clock = useMemo(() => Animated.clock({ start: 0, running }), [running]);
const time = useMemo(() => Animated.format(clock, { time: 'm:ss' }), [clock]);
<Animated.Text text={time} />

const t = useRef(new Animated.Value(0)).current;
useEffect(() => {
  Animated.timing(t, { toValue: 1, duration: line.length * 30, easing: Easing.linear, useNativeDriver: true }).start();
}, [line]);
<Animated.Text reveal={t} revealFraction revealFade={2}>{line}</Animated.Text>
```

A menu that owns its arrow keys, without making Tab wait for JS:

```jsx
import { passive } from '@orktes/jsui-react';

<View preventDefaultKeys={['ArrowLeft', 'ArrowRight']}
      onKeyDown={passive((e) => step(e.key === 'ArrowLeft' ? -1 : 1))} />
```

## Measuring

`jsui-bench <bundle.js> --react-stats` reports per-frame JS time, React
commits and renders, and native counters. `--js-thread on` shows what the
render thread pays with JS on its own thread. A screen built this way
spends close to 0 µs of JS per idle frame. The showcase screens are
measured this way in the project's perf notes.
