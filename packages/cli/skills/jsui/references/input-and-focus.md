# Input, focus and gamepad

The engine forwards mouse, touch, keyboard and gamepad (W3C standard mapping)
to JSUI. JSUI does hit testing, focus navigation, scrolling and text editing
itself; your components see React Native events.

## Focus

* Focusable: `Pressable`, Touchables, `Button`, `TextInput`, and any element
  with `focusable` (or `onFocus` via `useFocus().focusProps`). Never
  `disabled` ones or anything under `display: 'none'`.
* Initial focus: `hasTVPreferredFocus` or `autoFocus` on one element per
  screen (applied when it mounts). Programmatic: `ref.current.focus()` /
  `blur()`, or `focus(ref)` / `blur(ref)` from `@orktes/jsui-react`.
* Navigation: D-pad, left stick and arrow keys move focus **spatially** to
  the nearest focusable element in that direction; Tab / Shift+Tab move in
  tree order. Holding repeats after 400 ms, then every 120 ms. Override a
  direction with `nextFocusUp` / `nextFocusDown` / `nextFocusLeft` /
  `nextFocusRight` (node handles: `findNodeHandle(ref.current)`).
* Activation: A, Enter and Space press the focused element (`onPress`).
* Focus scopes: a `Modal` (or a View with `accessibilityViewIsModal` /
  `aria-modal`) traps navigation while shown, focuses its preferred child
  and restores the previous focus when it closes.
* Focus moved by keys / gamepad scrolls the element into view inside its
  ScrollViews (with 32 px of context); pointer focus never scrolls.
* `onFocus(e)` gets `e.nativeEvent.focusVisible` (true when the last input
  was a key or gamepad, like `:focus-visible`): show focus rings only then if
  you like. `Pressable` style functions get `focused` already.

```jsx
import { useFocus } from '@orktes/jsui-react';

function Card({ item }) {
  const { focused, focusProps } = useFocus();             // any View can take focus
  return (
    <View {...focusProps} style={[styles.card, focused && styles.cardFocused]}
      onKeyDown={(e) => e.nativeEvent.key === 'Enter' && open(item)}>
      ...
    </View>
  );
}
```

A settings row: focusable, Left / Right change its value instead of moving
focus (`onGamepadButton` and `onKeyDown` work on any element, including
Pressables, and fire on the focused element and its ancestors):

```jsx
function OptionRow({ testID, label, value, options, onChange, first }) {
  const i = options.indexOf(value);
  const step = (d) => onChange(options[Math.max(0, Math.min(options.length - 1, i + d))]);
  const dir = (e) => {
    const k = e.nativeEvent.key, b = e.buttonIndex;
    if (b === 14 || k === 'ArrowLeft') return -1;
    if (b === 15 || k === 'ArrowRight') return 1;
    return 0;
  };
  const handle = (e) => {
    const d = dir(e);
    if (d && (e.pressed !== false)) { step(d); e.preventDefault(); }   // preventDefault: no focus move
  };
  return (
    <Pressable testID={testID} hasTVPreferredFocus={first} onPress={() => step(1)}
      onGamepadButton={handle} onKeyDown={handle}
      style={({ focused }) => [styles.row, focused && styles.rowFocused]}>
      <Text style={styles.rowLabel}>{label}</Text>
      <Text testID={testID + '-value'} style={styles.rowValue}>{String(value)}</Text>
    </Pressable>
  );
}
```

Gamepad events carry `buttonIndex`, `pressed` (false on release), `value`
and `repeat` directly on the event; key events carry `key` / `code` on
`e.nativeEvent`. `hasTVPreferredFocus` / `autoFocus` take focus whenever the
element mounts, so a tab whose rows remount focuses its first row again.

Design every screen for the controller first: one obvious initial focus,
a strong focused style (border color, glow via `boxShadow`, scale spring), a
layout where spatial navigation is unambiguous (rows and columns), and
button hints (A Select, B Back) in a footer.

## Gamepad

```jsx
import { useGamepad } from '@orktes/jsui-react';

const pad = useGamepad({
  index: 0,
  onButton: (e) => {                       // e.buttonIndex, e.pressed, e.value, e.repeat
    if (e.buttonIndex === 4 && e.pressed) { prevTab(); e.preventDefault(); }   // LB
    if (e.buttonIndex === 5 && e.pressed) { nextTab(); e.preventDefault(); }   // RB
  },
  onAxis: (e) => {},                       // e.axis (0 LX, 1 LY, 2 RX, 3 RY), e.value -1..1
});
pad.connected; pad.buttons[0]; pad.axes[1];
```

Buttons: 0 A, 1 B, 2 X, 3 Y, 4 LB, 5 RB, 6 LT, 7 RT, 8 Back/View, 9
Start/Menu, 10 LS, 11 RS, 12 Up, 13 Down, 14 Left, 15 Right, 16 Home.
`preventDefault()` stops the default action (focus move, activation,
scrolling) for that event. Any element can also take `onGamepadButton` /
`onGamepadAxis` props (focused element and its ancestors).

* B: open `Modal`s get `onRequestClose`; otherwise
  `BackHandler.addEventListener('hardwareBackPress', fn)` handlers run (return
  `true` when handled). Escape closes modals too.
* Right stick scrolls the focused / hovered ScrollView; the left stick does
  when nothing in the scope is focusable.

## Keyboard

`onKeyDown` / `onKeyUp` on any element (bubbling from the focused element;
RN-web shape: `e.nativeEvent.key`, `code`, `shiftKey`, `ctrlKey`, `altKey`,
`metaKey`, `repeat`). `e.preventDefault()` stops the default action. With no
TextInput focused, PageUp / PageDown / Home / End / Space scroll.

## Pointer and touch

`onPress*`, `onHover*` (Pressable), `onPointerDown` / `Move` / `Up` /
`Enter` / `Leave` on Views, `onWheel`, the RN responder system
(`onStartShouldSetResponder`, `onResponderGrant` / `Move` / `Release`) and
`PanResponder` (with `vx` / `vy` velocities). Touch drags inside a ScrollView
scroll it unless a responder keeps the gesture
(`onShouldBlockNativeResponder`, default true for PanResponder).
`pointerEvents: 'none' | 'box-none' | 'box-only'` works as in RN.

## Input pass-through (HUDs over a game)

Every input the engine sends reports whether the UI consumed it; the rest
goes to the game. "Consumed" means something in the UI handles it (a press /
pointer / hover / responder handler, a TextInput, a scrollable ScrollView,
`preventDefault`, a focus move or activation) — **not** that the point is
painted. So:

* A full-screen HUD root with no handlers lets clicks through to the world.
  Mark decorative overlays `pointerEvents="none"` / `"box-none"` anyway.
* A backdrop that must block the game (pause menu, modal dim) needs a
  handler: `onStartShouldSetResponder={() => true}`.
* A press decides its whole gesture: a drag the game started over empty UI
  stays the game's.
* Godot `JsuiView.input_pass_through` (default on) and the web host's
  `passThrough: true` route by this; `jsui_hit_test(x, y)` asks without
  sending. `jsui run` steps report `consumed` for each input, and the
  `hitTest` step checks a point.

## Text input

`TextInput` is edited natively: caret, selection (shift+arrows, mouse drag,
double-click word), clipboard (Ctrl/Cmd+C / X / V / A), undo / redo,
`multiline`, `secureTextEntry`. Enter fires `onSubmitEditing` (single line).
There is no on-screen keyboard: on a controller-only platform provide your
own keyboard component. `jsui run`: click the input, then `{"type": "..."}`.
