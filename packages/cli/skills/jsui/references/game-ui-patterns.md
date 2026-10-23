# Game UI patterns

What makes a JSUI screen feel like a shipped game rather than a web page.

## Layout

* Design at 1280x720 or 1920x1080 logical px (16:9) and test other sizes
  (`jsui screenshot --width 2560 --height 1080`, `--width 1024 --height 768`).
  Size from `useWindowDimensions()` or flex, not hard-coded screen pixels.
* Keep important content inside a title-safe margin (about 5% of each edge:
  `padding: '4%'` or `useSafeAreaInsets()` from `react-native-safe-area-context` when the host sets insets).
* Typical structure: a full-bleed background (image, gradient, or the game
  itself behind a HUD), a top bar (title, currencies, player chip), a content
  row (menu column, detail panel), a footer with button hints.
* Use `position: 'absolute'` for overlays (HUD corners, toasts, nameplates)
  and flex for everything else.

## Visual language

* Dark translucent panels: `backgroundColor: 'rgba(10,14,24,0.72)'`,
  1 px borders in `rgba(255,255,255,0.08-0.16)`, radius 8-16.
* One accent color for focus and primary actions (gold, cyan); a second for
  warnings (red); rarity colors for items.
* Depth: `boxShadow: '0 12px 32px rgba(0,0,0,0.45)'` on panels; glow on focus
  with a colored `boxShadow` or `textShadowColor` + `textShadowRadius`.
* Gradients for backgrounds and fills: `experimental_backgroundImage:
  'linear-gradient(90deg, #f2c14e, #ff7a3d)'`.
* Uppercase, letter-spaced headings (`textTransform: 'uppercase',
  letterSpacing: 2-8`), `fontVariant: ['tabular-nums']` for counters and
  timers so digits don't jitter.
* Load a display font for titles with `useFonts`; Inter (built in) for body.

## Focus and controller

* One focused element at all times on menu screens (`hasTVPreferredFocus`).
* The focused style must be obvious from across a room: accent border,
  brighter background, a left bar or chevron, a small spring scale (1.03-1.06)
  on the native driver.
* Lay out rows and columns so spatial navigation is predictable; use
  `nextFocus*` only to fix the odd jump.
* LB / RB switch tabs, B goes back (`BackHandler` / `Modal onRequestClose`),
  Start opens menus. Show the hints in a footer that matches the bindings.
* Keep hover (mouse) and focus (pad) the same visual state so both inputs
  feel native.

## Motion

* Entrance: stagger list items (`Animated.stagger(40, items.map(...))`,
  translateY 12 → 0 + opacity), 250-400 ms total.
* Focus / press: springs (`tension 180-260, friction 12-18`), press scale
  0.96-0.98.
* Numbers that change (gold, XP) can count up with an `Animated.Value`
  listener at a few updates per second, or just pop with a scale spring.
* Idle life: slow looping shimmer or pulse on the primary action
  (`Animated.loop`, native driver), or a `ShaderView` background.
* `LayoutAnimation` for lists that gain or lose items (chips, inventory).

## Common screens

* **Main menu:** background art + scrim gradient, logo, vertical menu of
  Pressables, news card, player chip, footer hints.
* **HUD:** root `View` with `pointerEvents="box-none"`; health / ammo in
  corners driven by host buffers or `useHostStateSelector`; minimap as a
  `HostTexture` or Image with markers from a buffer; toasts via
  `useHostMessage`.
* **Inventory:** grid (`flexWrap: 'wrap'` or `FlatList numColumns`) of
  focusable slots, a detail panel that follows the focused slot (`onFocus`
  sets state), rarity borders, compare stats.
* **Settings:** tabs (LB / RB), rows with left / right value pickers
  (`onKeyDown` / `useGamepad` Left / Right while focused), sliders as
  pressable tracks; persist with `HostState.set('settings', ...)`.
* **Dialogs:** `Modal transparent` with a blocking backdrop
  (`onStartShouldSetResponder={() => true}`), focus on the safe choice, B
  cancels.
* **Loading:** progress from shared state, tips carousel, spinner via
  `ActivityIndicator` or a Lottie.

## Checklist before you call a screen done

1. `npx jsui screenshot` at 1280x720 and at another aspect ratio; read both.
2. `npx jsui inspect`: every interactive element is `[pressable]`, has a
   `testID`, and one is `[focused]`.
3. A `jsui run` script walks the focus order with `press` steps and
   `expect ... "focused": true`, presses A on each action and checks the
   resulting message / state.
4. No JS errors or warnings in the output.
