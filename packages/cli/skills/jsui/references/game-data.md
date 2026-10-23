# Game data: state, calls, messages, frame buffers, several views

Four channels connect the game and the UI. Pick by how often data changes
and whether an answer is needed.

| | Game → UI | UI → game | Use for |
| --- | --- | --- | --- |
| Shared state | `jsui_set_state` / `jsui_patch_state` → `useHostState(key)` | `HostState.set` / `patch` → `on_state` | what the UI shows: currency, inventory, quests, settings |
| Frame data | `jsui_set_buffer` → `useHostBuffer*`, `Animated.hostBuffer` | | numbers changing every frame: positions, health bars, cooldowns |
| Calls | `jsui_call_ui` → `Host.handle(name, fn)` | `await Host.call(name, args)` → `on_call` → `jsui_resolve_call` / `jsui_reject_call` | requests with an answer: buy, equip, confirm |
| Messages | `jsui_post_message` → `useHostMessage(name, fn)` | `Host.postMessage(name, data)` → `on_message` | fire-and-forget events: "play sound", "level up" |

All imports below are from `@orktes/jsui-react`.

## Shared state

```jsx
const gold = useHostState('gold', 0);                              // fallback while unset
const { items } = useHostState('inventory', { items: {} });
const hp = useHostStateSelector('player', (p) => p.hp, 100);       // re-renders only when hp changes
HostState.set('settings', { volume: 0.8 });                         // UI-owned key -> engine on_state
HostState.patch('settings', { volume: 0.5 });                       // RFC 7386 merge patch
HostState.get('gold'); HostState.subscribe('gold', cb); HostState.keys();
```

* Values are JSON. The engine can set them before the bundle loads and they
  survive hot reloads.
* One key per independently changing thing (`gold`, `inventory`, `quests`).
  Keep collections as objects keyed by id so the engine can patch one entry
  (`{"items": {"potion": {"count": 2}}}`; `null` deletes).
* Unchanged parts stay `===` across updates, so `React.memo` rows skip.
* Values are frozen in development: copy before modifying.

## Calls

```jsx
async function buy(item) {
  try {
    const { gold } = await Host.call('shop.buy', { item }, { timeout: 5000 });
  } catch (e) {
    setError(e.message);                 // engine's jsui_reject_call message, or timeout
  }
}
Host.handle('ui.confirm', async ({ text }) => await showConfirm(text));   // engine -> UI, returns JSON
useHostHandler('ui.flash', ({ text }) => toast(text));                    // while mounted
```

Without an engine handler the promise rejects; always handle errors.
`jsui run` answers calls with `launch({ calls: { 'shop.buy': {...} } })` or
`app.handle(name, fn)` in JS scripts.

## Messages

```jsx
Host.postMessage('audio.play', { cue: 'menu_select' });
useHostMessage('toast', (data) => push(data));
Host.onMessage('hp', (hp) => ...);   // non-hook; returns unsubscribe
```

## Frame data buffers

For values that change every frame. The engine pushes typed arrays (no
JSON); a layout like `x, y, hp` per enemy is a convention between both
sides.

```c
float enemies[30 * 3];
jsui_set_buffer(rt, "enemies", JSUI_BUFFER_F32, enemies, 30 * 3);   // every frame
```

```jsx
const count = useHostBufferSelector('enemies', (b) => b.length / 3, 0);   // re-renders when the count changes

function Nameplate({ i }) {
  const x = useHostBufferValue('enemies', i * 3);       // native Animated values: no JS per frame
  const y = useHostBufferValue('enemies', i * 3 + 1);
  const hp = useHostBufferValue('enemies', i * 3 + 2);
  return (
    <Animated.View style={[styles.plate, { transform: [{ translateX: x }, { translateY: y }] }]}>
      <Animated.View style={[styles.hpFill, { transform: [{ scaleX: hp }] }]} />
    </Animated.View>
  );
}
```

Also `HostBuffer.get(key)` (typed array view), `useHostBuffer(key)`
(re-renders every change: avoid for per-frame data), `useHostBufferListener
(key, cb)` (no re-render). Use `interpolate` on buffer values for colors /
ranges.

## Several root views (surfaces)

One bundle can feed many independent views (HUD, a nameplate per
character, a widget inside an existing engine UI). Register each:

```jsx
AppRegistry.registerComponent('Hud', () => Hud);
AppRegistry.registerComponent('Nameplate', () => Nameplate);   // ({ name, level }) => ...
```

The engine creates surfaces by name with props (`jsui_surface_create(rt,
"Nameplate", "{\"name\":\"Orc\"}", 160, 40, 1)`, Godot `JsuiView.component`
+ `initial_props` + `runtime_view`, web `jsui.createSurface(canvas,
{component, initialProps})`). Each has its own size (`useWindowDimensions`),
focus and input; state, fonts, images and JS are shared. The first
registered component starts on the main view unless the engine picks one.
Headless tools: `--component Name --props '{...}'`.
