# Native components

A native component is a view class your host registers by name. The class
draws straight into the view with Skia, and the React app uses it like any
React Native native component:

```jsx
import { requireNativeComponent } from 'react-native';
const Radar = requireNativeComponent('Radar');

<Radar range={120} accent="#4fd1ff" style={{ width: 200, height: 200 }} />
```

Use one when a view should draw **game state the host already has**, every
frame or whenever it changes, without turning that state into React props:
a minimap, a radar, a damage graph, a custom gauge, a node editor's wires.
React still lays the view out, styles it (background, border, radius, clip,
opacity, transforms), moves focus to it and handles its touches; the class
only draws the content box.

What else exists, and when it is the better fit:

| You want | Use |
| --- | --- |
| A picture the engine already renders on the GPU (3D view, camera, video) | `<HostTexture>` (host textures) |
| Vector art defined in JS (icons, charts from props) | `react-native-svg` / `@shopify/react-native-skia` |
| A per-pixel effect | `ShaderView` (SkSL) |
| Drawing from the host's own data, with host code | **a native component** |

The live demo is the site's [/features/native](https://orktes.github.io/jsui/#/features/native)
(a JS-drawn radar and gauge); the engine examples' HUD (raylib, sokol, SDL3)
has a radar drawn in C++ from the game's units:
[integrations/shared/radar.h](../integrations/shared/radar.h).

## C++ (`jsui/jsui_component.hpp`)

```cpp
#include "jsui/jsui_component.hpp"

struct Radar : jsui::Component {
  void draw(jsui::Canvas& c, const JsuiDrawInfo& info) override {
    float r = std::min(info.width, info.height) / 2;
    c.circle(r, r, r - 1, jsui::Paint(0xff33ff66).stroke(2));
    float sweep = float(info.time_ms / 1000 * 90);
    c.arc(0, 0, 2 * r, 2 * r, sweep - 40, 40, true, jsui::Paint(0x6633ff66));
    for (const Unit& u : game.units()) c.circle(r + u.x, r + u.y, 3, jsui::Paint(0xffff4444));
  }
  void command(const char* name, const char* args) override {
    if (!strcmp(name, "ping")) emit("pinged", "{\"count\":3}");
  }
};

jsui::registerComponent<Radar>(rt, "Radar", JSUI_COMPONENT_CLOCKED);
```

One instance per mounted view, default-constructed. `rt()` and `view()` are
set before the first `props()`. Everything runs on the runtime's thread (the
one calling `jsui_tick`), so a class can read game state it shares with that
thread without locks.

## C (`jsui/jsui.h`, "Native components")

```c
JsuiComponentClass cls;
jsui_component_class_init(&cls);
cls.user = &game;
cls.flags = JSUI_COMPONENT_CLOCKED;              // repaint every frame (else: on change only)
cls.create = radar_create;                        // (user, rt, view) -> instance
cls.destroy = radar_destroy;                      // (instance, view)
cls.props = radar_props;                          // (instance, view, json): React changed props
cls.draw = radar_draw;                            // (instance, canvas, info): required
cls.command = radar_command;                      // (instance, view, name, args_json)
jsui_register_component(rt, "Radar", &cls);       // the struct is copied
```

- **Register before the bundle renders** if the app checks
  `hasNativeComponent(name)` at its first render. Views mounted earlier bind
  when their class appears, and registering a name again replaces the class
  (views rebind; `destroy` then `create` per view).
  `jsui_unregister_component(rt, name)` unbinds every view.
- **Repaints.** The painter records a view's draw once and replays that
  recording until something changes: its props (Animated ones included), its
  size, an image or texture it drew loading or updating,
  `jsui_component_invalidate(rt, view, name)` (`view` 0: every view of class
  `name`; both null/0: every view), or, for `JSUI_COMPONENT_CLOCKED`
  classes, every frame. Invalidate from your game loop when the state a
  non-clocked class draws changes; a radar or a minimap that moves every
  frame should just be clocked.
- **Props.** In `draw`, read them with `jsui_prop_number`, `jsui_prop_bool`,
  `jsui_prop_color` (ARGB), `jsui_prop_string`, `jsui_prop_numbers` (an array)
  and `jsui_prop_json` (anything). Animated props come through at their
  current value. The `props` callback gets the plain props as one JSON
  object (no `style`, no functions, no Animated ones: read those in `draw`)
  when they change, for classes that keep derived state.
- **Commands** from JS (`codegenNativeCommands`, `dispatchCommand`,
  `UIManager.dispatchViewManagerCommand`) arrive in `command` with their
  arguments as a JSON array.
- **Events** go back with `jsui_component_emit(rt, view, "select", json)`:
  the view's `onSelect` prop gets them in the next tick, with
  `e.nativeEvent` = the JSON object. It returns 0 when the view has no such
  prop, so a class can skip work nobody listens to.

### The canvas

`draw` gets a `JsuiCanvas*` whose origin is the view's content box
(inside its border and padding) and a `JsuiDrawInfo` (`view`, `width`,
`height`, `pixel_ratio`, `time_ms`, the runtime clock). Drawing outside the
box is clipped to the view's (rounded) bounds.

- State: `save`, `save_layer(alpha)`, `restore`, `translate`, `scale`,
  `rotate` (degrees), `clip_rect`, `clip_rrect`, `clip_path`.
- Shapes: `clear`, `draw_rect`, `draw_rrect`, `draw_circle`, `draw_oval`,
  `draw_line`, `draw_arc` (degrees clockwise from +x), `draw_path`.
- Text: `draw_text(utf8, x, baseline_y, font, paint)` returns its width;
  `jsui_canvas_measure_text`. `JsuiFont` uses the runtime's fonts (anything
  `Fonts.load` / `jsui_load_font` registered), with `align` left / center /
  right.
- Pictures: `draw_image(uri)` (like `Image`'s `uri`, cached; false while it
  loads, the view repaints when it arrives) and `draw_texture(key)` (a host
  texture; new frames repaint the view).
- `JsuiPaint` (`jsui_paint_init` for defaults): color, fill / stroke /
  both, stroke width, caps, joins, blend mode (`SRC_OVER`, `PLUS`,
  `MULTIPLY`, `SCREEN`, `SRC`, `DST_OUT`, `DST_IN`), antialias, blur
  (a mask blur sigma, for glows), dashes, and a `JsuiGradient` (linear,
  radial, sweep; colors with optional stops).
- Paths: `jsui_path_create` / `jsui_path_from_svg("M0 0 L10 10 Z")`,
  `move_to`, `line_to`, `quad_to`, `cubic_to`, `close`, `add_rect`,
  `add_circle`, `add_arc`, `set_even_odd`; destroy them with
  `jsui_path_destroy`. Keep paths you redraw every frame instead of
  rebuilding them.
- `jsui_canvas_skia(canvas)` returns the `SkCanvas*` for code built against
  the library's own Skia version. Leave the canvas as you found it
  (balanced saves; JSUI restores anything left over).

## The web (`@orktes/jsui-web`)

Page JS registers classes with the same model:

```js
jsui.registerComponent('Radar', {
  clocked: true,
  create(view) { return { selected: -1 }; },          // per-view state, view.state
  props(state, props, view) {},
  command(state, name, args, view) { view.emit('select', { index: 2 }); },
  draw(ctx, view) {                                     // view: { id, width, height, pixelRatio, time, state }
    const r = Math.min(view.width, view.height) / 2;
    ctx.circle(r, r, r - 1, { color: '#3f6', style: 'stroke', width: 2 });
    ctx.arc(0, 0, 2 * r, 2 * r, view.time / 10, 40, true,
      { gradient: { type: 'sweep', colors: ['#3f60', '#3f6a'] } });
    ctx.text('3 CONTACTS', r, 2 * r - 12, { size: 10, weight: 700, align: 'center' }, { color: '#3f6' });
  },
});
```

`ctx` mirrors the C canvas: `save`, `restore`, `translate`, `scale`,
`rotate`, `clipRect`, `clipRRect`, `clipPath(svg)`, `clear`, `rect`,
`rrect`, `circle`, `oval`, `line`, `arc`, `path(svg)` (parsed once,
cached), `polyline([x0, y0, ...])`, `text`, `measureText`, `image(uri)`,
`texture(name)`, and the prop readers `number`, `color`, `prop`. Paints are
plain objects (`{ color, style, width, cap, join, blend, blur, dash,
gradient }`), colors CSS strings or `0xAARRGGBB` numbers; see the top of
[web/jsui-components.js](../web/jsui-components.js).

**Worker mode** runs the runtime in a Worker, where page functions can't
go. Put the classes in a module that imports nothing DOM-related and
exports them by name, and pass its URL:

```js
// components.js
export const Radar = { clocked: true, draw(ctx, view) { /* ... */ } };

const jsui = await createJsui({ canvas, bundleUrl, worker: true, components: './components.js' });
// or later: await jsui.loadComponents('./more-components.js');
```

`components` also takes an object (`{ Radar }`) on the main thread. Either
way the classes are registered before the bundle runs.

## React

```jsx
import { codegenNativeCommands, hasNativeComponent, requireNativeComponent } from 'react-native';

const Radar = requireNativeComponent('Radar');
const Commands = codegenNativeCommands({ supportedCommands: ['select'] });

function Minimap() {
  const ref = useRef(null);
  const flash = useRef(new Animated.Value(0)).current;
  if (!hasNativeComponent('Radar')) return <Placeholder />;   // a host without the class
  return (
    <Pressable onPress={(e) => Commands.select(ref.current, e.nativeEvent.locationX, e.nativeEvent.locationY)}>
      <Radar
        ref={ref}
        range={120}                  // props: jsui_prop_* / props(json)
        flash={flash}                // Animated on the native driver: no React render
        onSelect={(e) => setTarget(e.nativeEvent.index)}   // jsui_component_emit
        style={{ width: 200, height: 200, borderRadius: 100 }}
      />
    </Pressable>
  );
}
```

- `requireNativeComponent(name)` (and `codegenNativeComponent(name)`)
  returns a component. Children render on top of what the class draws.
- `codegenNativeCommands({ supportedCommands })` returns one function per
  command, `(ref, ...args)`; `dispatchCommand(ref, name, args)` does the
  same untyped. Commands reach the class in order with other UI updates.
- `hasNativeComponent(name)` and `UIManager.hasViewManagerConfig(name)` say
  whether the host registered the class, so an app can run on a host
  without it.
- Code written for React Native's native components (Fabric `codegen`
  specs, `requireNativeComponent`) ports by registering a class of the same
  name on the host.

## How it works

`NativeComponentView` is a host element whose node type is `Component`.
The class name is in its `component` prop. Its props reach native code as a
JSON object; Animated props are kept as paint-only `prop:<name>` style keys,
so the native driver repaints the view without relayout or a React commit.
The painter calls the class's `draw` inside the view's clip and content-box
translation, and keeps the recording as the node's paint cache like any
other view (`src/render/native_component.cpp`, `src/render/painter.cpp`).
Commands go through the command buffer (`Op::ComponentCommand`), so they
run in order with the commit that mounted the view. Events are queued and
delivered as `componentevent` in the tick, which also works with the JS
thread on native hosts and the host JS engine on the web.

See also [ARCHITECTURE.md](ARCHITECTURE.md) "Native components".
