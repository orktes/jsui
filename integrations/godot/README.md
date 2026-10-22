# JSUI for Godot 4 (GDExtension)

`JsuiView` is a Godot `Control` that runs a JSUI bundle (React Native–style
JS) and draws it. It is the Godot counterpart of `tools/jsui-viewer`: it
ticks the runtime every frame, paints with `jsui_render_rgba_incremental`
into a CPU buffer and uploads an `ImageTexture` only when pixels changed. It
forwards Godot input and bridges `postMessage` both ways.

```
integrations/godot/
  CMakeLists.txt        GDExtension target (jsui_godot), fetches godot-cpp
  src/                  JsuiView, key/gamepad mapping, entry point
  demo/                 Godot 4.3 project: main.tscn + main.gd + jsui.gdextension
  demo/ui/app.jsx       demo UI: examples/screens/main-menu.jsx + a bridge card
  smoke-test.sh         runs the demo headless and saves a frame (used by CI)
```

## Build

Requirements: CMake ≥ 3.20, Ninja, clang (or gcc / MSVC), python3 (godot-cpp
generates its bindings with it), Node 22 for the bundle.

```sh
# From the repository root. godot-cpp is fetched at godot-4.3-stable.
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
| `JSUI_GODOT_CPP_TAG` | `godot-4.3-stable` | godot-cpp tag to fetch |
| `JSUI_GODOT_CPP_DIR` | | use an existing godot-cpp checkout instead |
| `JSUI_GODOT_OUTPUT_DIR` | `integrations/godot/demo/bin` | where the library is written |

The library statically links the JSUI runtime (QuickJS, Yoga, Skia) and only
exports the GDExtension entry point (`jsui_godot_init`). It is built against
the 4.3 extension API and loads in Godot 4.3 and later. One build serves the
editor and debug/release runs. On Windows, copy `icudtl.dat` (CMake puts it
next to the DLL) beside the game executable as for any JSUI host.

## Run the demo

```sh
cd integrations/godot/demo
godot --path . --import   # once: registers the extension and imports assets
godot --path .            # or open the project in the editor and press F5
```

The window shows the main-menu screen with a "Godot bridge" card. Every
second `main.gd` sends `godot:stats` (fps, frame, version) to the UI. The
"Ping Godot" button (click it, or press Enter / gamepad A, since it has
focus) posts `ui:ping`. `main.gd` prints it and answers with `godot:pong`.
Clicks the UI does not handle fall through to `_unhandled_input`. F5 reloads
the bundle.

Headless check (no GPU needed). This is what CI runs:

```sh
integrations/godot/smoke-test.sh build/godot-smoke.png        # downloads Godot 4.3 unless $GODOT is set
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
| `pixel_ratio` | float | `0` | device px per logical px; `0` follows the canvas→screen scale (stretch mode, Camera2D zoom, parent scale) |
| `autostart` | bool | `true` | start in `_ready` (otherwise call `start()`) |
| `forward_gamepad` | bool | `true` | forward joypad events received while focused |

Changing `bundle_url` or `dev_mode` on a running view restarts it.

| Method | |
| --- | --- |
| `start()` / `stop()` | create the runtime and load `bundle_url` / destroy it |
| `reload()` | re-evaluate the bundle (same runtime) |
| `post_message(name, data = null)` | send to JS: `data` is JSON-encoded. JS receives it with `useHostMessage(name, cb)` / `Host.onMessage` |
| `post_message_json(name, json)` | same with a JSON string |
| `get_texture()` | the `ImageTexture` being drawn (premultiplied RGBA) |
| `get_frame_image()` | CPU copy of the last frame, straight alpha, works with `--headless` |
| `get_pending_loads()` | bundle/image/font/fetch loads in flight |
| `get_last_error()` | last uncaught JS error, or `""` |
| `get_effective_pixel_ratio()` | the ratio actually used |
| `is_running()` | |

| Signal | |
| --- | --- |
| `message_received(name: String, data: Variant, json: String)` | JS called `Host.postMessage(name, data)`. `data` is the parsed payload |
| `js_error(message: String)` | a new uncaught JS error |
| `frame_rendered(dirty_rect: Rect2i)` | the texture was updated. The rect is in device pixels |

Signals are emitted after the runtime returns (after the tick or the input
call), so handlers can call back into the view safely.

### Rendering

* The view's logical size is the control's size. The runtime rasterises at
  `size * pixel_ratio` device pixels, and the texture is drawn scaled back to
  the control's rect.
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
| wheel buttons (with `factor`), `InputEventPanGesture` | `jsui_send_mouse_wheel`, 40 px per notch. Shift+wheel scrolls horizontally |
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
* Godot 4.3 cannot upload part of a texture, so the dirty rect saves only
  CPU paint work, not GPU upload bandwidth.
* Built and tested on Linux x86_64 (CI). The CMake target also names the
  Windows and macOS libraries the `.gdextension` expects, but those builds
  are untested.
