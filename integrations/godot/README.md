# JSUI for Godot 4 (GDExtension)

`JsuiView` is a Godot `Control` that runs a JSUI bundle (React Native–style
JS) and draws it. It is the Godot counterpart of `tools/jsui-viewer`: it
ticks the runtime every frame, paints with `jsui_render_rgba_incremental`
into a CPU buffer and uploads an `ImageTexture` only when pixels changed. It
forwards Godot input and bridges game data both ways: shared state
(`set_state` → `useHostState`), calls (`Host.call` → `call_requested` →
`resolve_call`, and `call_ui` → `Host.handle`) and messages.

```
integrations/godot/
  CMakeLists.txt        GDExtension target (jsui_godot), fetches godot-cpp
  src/                  JsuiView, key/gamepad mapping, entry point
  demo/                 Godot 4.7 project: main.tscn + main.gd + jsui.gdextension
  demo/ui/app.jsx       demo UI: examples/showcase/interactions.jsx + a bridge strip
  smoke-test.sh         runs the demo headless with an input self-test at 1x and
                        2x content scale and saves the frames (used by CI)
```

## Build

Requirements: CMake ≥ 3.20, Ninja, clang (or gcc / MSVC), python3 (godot-cpp
generates its bindings with it), Node 22 for the bundle.

```sh
# From the repository root. godot-cpp is fetched at 10.0.0-stable.
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DJSUI_BUILD_GODOT=ON
cmake --build build --target jsui_godot
# -> integrations/godot/demo/bin/libjsui_godot.linux.x86_64.so

# The demo UI bundle (+ its image assets) into demo/ui/dist/
npm ci
npx jsui build integrations/godot/demo/ui/app.jsx -o integrations/godot/demo/ui/dist/bundle.js
```

CMake options:

| Option | Default | |
| --- | --- | --- |
| `JSUI_BUILD_GODOT` | `OFF` | build the GDExtension (the default build is unaffected) |
| `JSUI_GODOT_CPP_TAG` | `10.0.0-stable` | godot-cpp tag to fetch |
| `JSUI_GODOT_API_VERSION` | `4.3` | Godot API to build against (4.3 … 4.7): the oldest Godot that loads the library. Keep `compatibility_minimum` in `demo/jsui.gdextension` equal to it |
| `JSUI_GODOT_CPP_DIR` | | use an existing godot-cpp checkout instead |
| `JSUI_GODOT_OUTPUT_DIR` | `integrations/godot/demo/bin` | where the library is written |

The library statically links the JSUI runtime (QuickJS, Yoga, Skia) and only
exports the GDExtension entry point (`jsui_godot_init`). One build serves the
editor and debug/release runs. On Windows, copy `icudtl.dat` (CMake puts it
next to the DLL) beside the game executable as for any JSUI host.

### Godot versions

The demo project is saved with Godot 4.7 (the current stable release,
4.7.2, is what CI runs), and the library loads in **Godot 4.3 and later**
(`compatibility_minimum = "4.3"`):

* Since 10.x, godot-cpp is versioned independently of Godot. One godot-cpp
  release builds against any Godot API from 4.3 to 4.7 (`GODOTCPP_API_VERSION`,
  set here from `JSUI_GODOT_API_VERSION`).
* GDExtensions are forward- but not backward-compatible: a library built
  against the 4.3 API loads in 4.3 … 4.7, one built against 4.7 only in 4.7+.
  So the API version is the real minimum, and `compatibility_minimum` states
  it.
* `JsuiView` uses nothing newer than the 4.3 API (`ImageTexture`, `Image`,
  `RenderingServer` canvas items, `InputEvent*` with `KeyLocation`), so 4.3
  is the default: one binary serves every supported Godot. Build with
  `-DJSUI_GODOT_API_VERSION=4.7` (and raise `compatibility_minimum`) only to
  use newer APIs.
* CI runs the demo in 4.7.2 and in 4.3 (`GODOT_VERSION=4.3-stable
  smoke-test.sh`) to keep both ends of the range honest.

## Run the demo

```sh
cd integrations/godot/demo
godot --path . --import   # once: registers the extension and imports assets
godot --path .            # or open the project in the editor and press F5
```

The window shows the interactions showcase (`examples/showcase/interactions.jsx`,
an AETHERFALL game hub): menu buttons with hover / press springs, tabs, a
paging carousel (drag or Shift+wheel), a momentum armory list (wheel,
trackpad, drag), a TextInput that recruits squad members on Enter
(LayoutAnimation chips), a spring modal ("Continue" or "Deploy expedition")
and gamepad / keyboard focus navigation (D-pad, arrows, Tab; A / Enter
presses). Click into the view (or call `grab_focus()`) to give it the
keyboard and gamepad.

The "Godot" strip at the bottom left is the bridge. `main.gd` keeps the
shared state key `godot.stats` (fps, frame, version) up to date with
`set_state` (the first value is set before the view starts), and owns the
`player` key that the showcase's profile chip reads. "Ping" awaits
`Host.call('godot.ping')`, which `main.gd` answers in `call_requested` with
`resolve_call`, paying out credits with `patch_state('player', ...)`; the
chip updates. The self-test calls into the UI with `call_ui('ui.flash',
...)`, which shows a toast. Clicks the UI does not handle fall through to
`_unhandled_input`. F5 reloads the bundle (the state survives it).

Headless check (no GPU needed). This is what CI runs. `main.gd --self-test`
drives the UI through Godot's own input pipeline (`Input.parse_input_event`):
hover, click (and checks that JSUI saw exactly the logical point it aimed
at), gamepad D-pad focus + A, wheel and trackpad-pan scrolling, click to
focus + typing + Enter in the TextInput, the Ping call, `call_ui` and an
unhandled click. The UI reports what happened as `showcase:event` messages
and the script prints `PASS` / `FAIL` per check. It runs at a 1280x720 window
(content scale 1) and at 2560x1440 (content scale 2, like a Retina Mac).
The headless display server ignores `--resolution`, so `main.gd` takes
`--window WxH` too.

```sh
integrations/godot/smoke-test.sh build/godot-smoke.png        # downloads Godot 4.7.2 unless $GODOT is set
GODOT_VERSION=4.3-stable integrations/godot/smoke-test.sh     # the oldest supported Godot
GODOT_DISPLAY=x11 xvfb-run -a integrations/godot/smoke-test.sh # real OpenGL renderer + window capture
```

### Dev mode (live reload)

```sh
npx jsui dev integrations/godot/demo/ui/app.jsx --port 5173
godot --path integrations/godot/demo -- --dev http://127.0.0.1:5173/bundle.js
```

In dev mode the runtime polls the dev server (`/__jsui/version`), reloads
when you save, and shows JS errors in an on-screen overlay. You can also set
`bundle_url = "http://127.0.0.1:5173/bundle.js"` and `dev_mode = true` on the
node in the inspector.

## JsuiView reference

| Property | Type | Default | |
| --- | --- | --- | --- |
| `bundle_url` | String | `""` | `res://…`, `user://…`, an absolute path, or `http://…` |
| `dev_mode` | bool | `false` | poll the dev server, reload on change, error overlay |
| `pixel_ratio` | float | `0` | device px per logical px; `0` follows the control→window-pixel scale (stretch / content scale, HiDPI windows, Camera2D zoom, parent scale) |
| `autostart` | bool | `true` | start in `_ready` (otherwise call `start()`) |
| `forward_gamepad` | bool | `true` | forward joypad events received while focused |

Changing `bundle_url` or `dev_mode` on a running view restarts it.

| Method | |
| --- | --- |
| `start()` / `stop()` | create the runtime and load `bundle_url` / destroy it |
| `reload()` | re-evaluate the bundle (same runtime) |
| `post_message(name, data = null)` | send to JS: `data` is JSON-encoded. JS receives it with `useHostMessage(name, cb)` / `Host.onMessage` |
| `post_message_json(name, json)` | same with a JSON string |
| `set_state(key, value) -> int` | shared state (`jsui_set_state`): `value` (Dictionary, Array, number, String…) is JSON-encoded, `null` removes the key. JS reads it with `useHostState(key)` and re-renders on change. Returns 1 changed, 0 unchanged, -1 invalid |
| `patch_state(key, patch) -> int` | RFC 7386 merge patch of a key (`jsui_patch_state`): Dictionaries merge, `null` members delete. Keep big collections as Dictionaries keyed by id |
| `get_state(key) -> Variant` | current value (also keys the UI set with `HostState.set`) |
| `set_state_json` / `patch_state_json` / `get_state_json` | the same with JSON strings |
| `resolve_call(id, result = null) -> int` | answer a `call_requested` call; the UI's `await Host.call(...)` resolves with `result`. Any later frame is fine. -1: unknown or cancelled id |
| `resolve_call_json(id, json)` / `reject_call(id, message)` | JSON result / reject (the promise rejects with `Error(message)`) |
| `call_ui(name, args = null, callback = Callable()) -> int` | run the UI's `Host.handle(name, fn)` / `useHostHandler` handler; `callback(ok: bool, result)` (and `ui_call_completed`) runs once it settles: `result` is the handler's value, or the error message when `ok` is false |
| `get_texture()` | the `ImageTexture` being drawn (premultiplied RGBA) |
| `get_frame_image()` | CPU copy of the last frame, straight alpha, works with `--headless` |
| `get_pending_loads()` | bundle/image/font/fetch loads in flight |
| `get_last_error()` | last uncaught JS error, or `""` |
| `get_effective_pixel_ratio()` | the ratio actually used |
| `is_running()` | |

| Signal | |
| --- | --- |
| `message_received(name: String, data: Variant, json: String)` | JS called `Host.postMessage(name, data)`. `data` is the parsed payload |
| `state_changed(key: String, value: Variant)` | JS changed a UI-owned key with `HostState.set` / `patch` (`value` null: removed) |
| `call_requested(id: int, name: String, args: Variant)` | JS called `await Host.call(name, args)`. Answer with `resolve_call` / `reject_call`, now or later |
| `call_cancelled(id: int)` | a call ended unanswered (JS timeout / abort, reload, `stop()`): drop its work |
| `ui_call_completed(id: int, ok: bool, result: Variant)` | a `call_ui` call settled (also when the runtime stopped first) |
| `js_error(message: String)` | a new uncaught JS error |
| `frame_rendered(dirty_rect: Rect2i)` | the texture was updated. The rect is in device pixels |

Signals are emitted after the runtime returns (after the tick or the input
call), so handlers can call back into the view safely.

State set with `set_state` / `patch_state` is kept by the view as well, so it
is there before the bundle runs (even before `start()`) and is re-applied
when the runtime is recreated (`stop()` / `start()`, a new `bundle_url`);
`reload()` and dev-mode reloads keep it in the runtime anyway.

```gdscript
func _ready():
    ui.set_state("player", {"gold": 1250, "hp": 80})
    ui.set_state("inventory", {"items": {"sword": {"name": "Iron Sword", "count": 1}}})
    ui.call_requested.connect(_on_ui_call)

func _on_gold_changed(gold):
    ui.patch_state("player", {"gold": gold})   # only "gold" changes; only its readers re-render

func _on_ui_call(id, name, args):
    match name:
        "shop.buy":
            if try_buy(args.item): ui.resolve_call(id, {"gold": gold})
            else: ui.reject_call(id, "not enough gold")
        _:
            ui.reject_call(id, "unknown call " + name)
```

### Rendering

* The view's logical size is the control's size. The runtime rasterises at
  `size * pixel_ratio` device pixels, and the texture is drawn scaled back to
  the control's rect.
* With `pixel_ratio = 0` the ratio is the scale from the control to window
  pixels: `get_viewport().get_final_transform()` (which holds the stretch
  transform) times `get_global_transform_with_canvas()`. On a Retina Mac
  (`DisplayServer.screen_get_scale()` = 2, `allow_hidpi` on) a 1280x720
  `canvas_items` project runs in a 2560x1440-pixel window, so the UI lays
  out at 1280x720 and renders at 2x. (`get_screen_transform()` is not used:
  for a root window that embeds subwindows, the default, it omits the
  stretch transform and would report 1x, so the UI would be upscaled and
  blurry.) Input needs no scaling: `_gui_input` positions are already in
  the control's local px, which are JSUI's logical px.
* JSUI produces premultiplied RGBA. The texture is drawn by an internal
  canvas item with a `CanvasItemMaterial` in `BLEND_MODE_PREMULT_ALPHA`, so
  translucent UI composites correctly over the game and the node's own
  `material` stays free.
* Rendering happens only when `jsui_needs_redraw` says so, and repaints only
  the damaged region. Godot 4 has no sub-rectangle texture upload, so a
  changed frame uploads the whole texture. Idle frames upload nothing.
* The runtime does not run inside the editor. The node draws a placeholder
  there.

### Input

| Godot | JSUI |
| --- | --- |
| `InputEventMouseMotion` / `MouseButton` (left, right, middle) | `jsui_send_mouse_move` / `jsui_send_mouse_button` (+ modifiers) |
| wheel buttons (with `factor`), `InputEventPanGesture` (macOS trackpad scrolling, including its momentum) | `jsui_send_mouse_wheel`, 40 px per notch. Shift+wheel scrolls horizontally. `InputEventMagnifyGesture` is not forwarded (it reaches the game) |
| mouse exit | `jsui_send_mouse_leave` |
| `InputEventScreenTouch` / `ScreenDrag` | `jsui_send_touch`. Mouse events emulated from touch are dropped |
| `InputEventKey` | `jsui_send_key` with DOM `key` / `code` (left/right modifiers via `location`), then `jsui_send_text` with the UTF-8 character for printable presses (not with Ctrl/Cmd) |
| `InputEventJoypadButton` / `JoypadMotion` | `jsui_send_gamepad_button` / `_axis`, W3C standard mapping. Triggers become buttons 6/7 with hysteresis |
| `Input.joy_connection_changed` | `jsui_set_gamepad_connected` |
| `jsui_get_cursor()` | `mouse_default_cursor_shape` |

* Focus: the view has `FOCUS_ALL` and grabs focus on click. Godot sends
  key and joypad events only to the focused control, so call `grab_focus()`
  to give the UI the keyboard and gamepad. Inside the UI, JSUI does its own
  spatial focus navigation (D-pad / stick / Tab). Losing Godot focus releases
  held keys and pad buttons so nothing stays stuck.
* Consumed events are `accept_event()`ed. Anything the UI did not handle
  continues through Godot (`mouse_filter` is `PASS`): parent controls, then
  `_unhandled_input`. For example, an unhandled Tab or arrow key can move
  Godot focus to another control.

### Loading and exporting

`res://` and `user://` bundles are read through `FileAccess`, so they work
from exported `.pck` files. Relative URLs in the bundle (images, fonts,
`fetch()`) resolve against the bundle URL. Keep the bundle in a subfolder
(`res://ui/dist/bundle.js`, not `res://bundle.js`). Such bundles cannot
`fetch()` `http://` URLs. Other bundle URLs (`http://` dev servers, absolute
paths) use JSUI's built-in loader.

When exporting, add the bundle folder to the preset's non-resource filter
(Export → Resources → "Filters to export non-resource files", e.g.
`ui/dist/*`). This ships the raw `.js` and image files and not only Godot's
imported copies.

## Limitations

* IME composition (pre-edit text) is not forwarded, only committed
  characters. The C API has no composition entry point yet.
* The Godot 4.3 API has no sub-rectangle texture upload, so the dirty rect saves only
  CPU paint work, not GPU upload bandwidth.
* Built and tested on Linux x86_64 (CI). The CMake target also names the
  Windows and macOS libraries the `.gdextension` expects, but those builds
  are untested.
