# JSUI for Godot 4 (GDExtension)

`JsuiView` is a Godot `Control` that runs a JSUI bundle (React Native–style
JS) and draws it. It is the Godot counterpart of `tools/jsui-viewer`: it
ticks the runtime every frame and repaints only what changed. On Godot's
Forward+ / Mobile renderers Skia draws on the GPU straight into a texture of
Godot's RenderingDevice (Skia Graphite on Godot's own Vulkan device or Metal
queue); on the Compatibility (OpenGL) renderer and headless it paints with
`jsui_render_rgba_incremental` into a CPU buffer and uploads an
`ImageTexture` (see [Rendering](#rendering)). It forwards Godot input and
bridges game data both ways: shared state
(`set_state` → `useHostState`), calls (`Host.call` → `call_requested` →
`resolve_call`, and `call_ui` → `Host.handle`) and messages.

```
integrations/godot/
  CMakeLists.txt        GDExtension target (jsui_godot), fetches godot-cpp
  src/                  JsuiView, key/gamepad mapping, entry point
  demo/                 Godot 4.7 project: main.tscn + main.gd + jsui.gdextension
  demo/ui/app.jsx       demo UI: examples/showcase/interactions.jsx + a bridge strip
  smoke-test.sh         runs the demo headless with an input self-test at 1x and
                        2x content scale and saves the frames (used by CI);
                        GODOT_RENDERING=vulkan runs it on Vulkan (GPU renderer)
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

That build uses the prebuilt Skia m124, which has no Vulkan, Metal or
Graphite: the view then always uses the CPU renderer. For the GPU renderer,
build Skia m157 from source first (Linux: Graphite + Vulkan; macOS:
Graphite + Metal; both also Graphite + Dawn for the godot-aw3 fork; about
25 minutes on 4 cores, done once) and point CMake at it:

```sh
scripts/build-skia-desktop.sh            # -> third_party/skia-desktop
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DJSUI_BUILD_GODOT=ON -DJSUI_SKIA_DIR=third_party/skia-desktop
cmake --build build --target jsui_godot
# configure prints "JSUI_VULKAN: ON" (Linux) / "JSUI_GRAPHITE_METAL: ON" (macOS)
# and "JSUI_GRAPHITE_DAWN: ON (Dawn ada567700e13..., ...)"
```

CMake options:

| Option | Default | |
| --- | --- | --- |
| `JSUI_BUILD_GODOT` | `OFF` | build the GDExtension (the default build is unaffected) |
| `JSUI_GODOT_CPP_TAG` | `10.0.0-stable` | godot-cpp tag to fetch |
| `JSUI_GODOT_API_VERSION` | `4.3` | Godot API to build against (4.3 … 4.7): the oldest Godot that loads the library. Keep `compatibility_minimum` in `demo/jsui.gdextension` equal to it |
| `JSUI_GODOT_CPP_DIR` | | use an existing godot-cpp checkout instead |
| `JSUI_GODOT_OUTPUT_DIR` | `integrations/godot/demo/bin` | where the library is written |
| `JSUI_SKIA_DIR` | prebuilt m124 | `third_party/skia-desktop` (from `scripts/build-skia-desktop.sh`) enables the GPU renderer |
| `JSUI_VULKAN` / `JSUI_GRAPHITE_METAL` | on when that Skia has them | the Vulkan / Metal Graphite backends |
| `JSUI_GRAPHITE_DAWN` | on when that Skia has Dawn | the Dawn Graphite backend (godot-aw3's `webgpu` driver); no Dawn is linked |

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
  `Texture2DRD`, `RenderingDevice`, `RenderingServer` canvas items,
  `InputEvent*` with `KeyLocation`), so 4.3 is the default: one binary
  serves every supported Godot. (It calls the 4.4+
  `RenderingServer.get_current_rendering_driver_name` by name when it
  exists.) Build with
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
# Vulkan (Forward+) with the GPU renderer; Mesa's lavapipe is enough
# (apt install mesa-vulkan-drivers). Fails on the CPU fallback or when the GPU
# frame differs from the CPU raster of the same frame (GPU_MAX_DIFF, mean
# channel difference, default 1.5) and saves both as <out>-gpu.png / -cpu.png.
GODOT_RENDERING=vulkan SIZES=1280x720 xvfb-run -a -s "-screen 0 1920x1080x24" \
  integrations/godot/smoke-test.sh build/godot-vulkan.png
# The same under the Khronos validation layer (apt install
# vulkan-validationlayers) with synchronization validation, as CI runs it:
# fails on any validation error, e.g. a hazard between JSUI's Graphite work and
# Godot's frame on the textures they share.
VK_VALIDATION=sync GODOT_RENDERING=vulkan SIZES=1280x720 xvfb-run -a -s "-screen 0 1920x1080x24" \
  integrations/godot/smoke-test.sh build/godot-vulkan.png
# The same with Godot's RenderingServer on its own thread (--render-thread
# separate, as some games run it; GODOT_RENDER_THREAD takes any of Godot's
# modes).
GODOT_RENDERING=vulkan GODOT_RENDER_THREAD=separate SIZES=1280x720 xvfb-run -a -s "-screen 0 1920x1080x24" \
  integrations/godot/smoke-test.sh build/godot-vulkan-separate.png
```

Runs with a display (Vulkan, or `GODOT_DISPLAY=x11`) also check what Godot
actually drew: `get_frame_image()` reads JsuiView's own texture, which can be
right while the canvas item shows something else (Godot's magenta / black
placeholder texture, say). The view is frozen over an opaque backdrop of a
known colour, the viewport texture is read back, and the run fails when the
view's rect differs from the frame blended over that colour by a mean channel
difference above `VIEWPORT_MAX_DIFF` (default 2); both are saved as
`<out>-shown.png` / `<out>-expected.png`.

`main.gd` takes a few more arguments for checking the renderer by hand:
`--force-cpu` (sets `force_cpu`), `--expect-renderer vulkan|metal|cpu`,
`--max-gpu-diff D` (with `--capture`: compare the GPU frame with the CPU
raster), `--max-viewport-diff D` (with `--capture` and a display: compare
what the viewport shows with the frame), `--max-render-waits N` (with
`--self-test` on a GPU renderer: 30 frames of hover springs and of four
surface views of the runtime changing every frame may make at most N
main-thread waits on the render thread, and must queue exactly one
render-thread job per frame; the Vulkan smoke runs pass 0),
`--bench N` (N frames of hover
springs, then of list scrolling, with vsync off; prints frame and render
times) and `--no-world` (no 3D scene).
The project uses the Compatibility renderer by default (it runs everywhere);
pass `--rendering-method forward_plus` (or `mobile`) for the GPU renderer.

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
| `force_cpu` | bool | `false` | always use the CPU renderer, also on Vulkan / Metal |
| `runtime_view` | NodePath | empty | show a surface of that JsuiView's runtime instead of running a bundle of its own (see [Several views, one runtime](#several-views-one-runtime)) |
| `component` | String | `""` | the `AppRegistry` key this view shows. On a runtime view: the main surface's component (empty: the bundle's first registered one) |
| `initial_props` | Dictionary | `{}` | the component's props; changing them re-renders it with the new props (state kept) |
| `auto_size` | bool | `false` | size the view to the component's content: the view's minimum size follows the laid-out size |

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
| `get_texture()` | the `Texture2D` being drawn (premultiplied RGBA): a `Texture2DRD` with the GPU renderer, an `ImageTexture` with the CPU one |
| `get_frame_image()` | CPU copy of the last frame, straight alpha, works with `--headless` (with the GPU renderer it reads the texture back: slow, for tests and screenshots) |
| `get_cpu_frame_image()` | the current frame painted by the CPU raster, straight alpha, whatever the renderer (to compare with `get_frame_image()`) |
| `get_renderer()` | `"vulkan"`, `"metal"` or `"cpu"` (`""` before the first frame) |
| `get_render_stats()` | Dictionary: `renderer`, `renders`, `render_usec_total` / `_last` / `_max` (time in the view's render call, CPU side), `dirty_pixels_total`; GPU renderer: `gpu_jobs` (the view's frames handed to the render thread without waiting), `render_thread_jobs` (the render-thread jobs those went in: one per frame for all the views of a runtime; the runtime view's count), `gpu_frame_in_flight` (the last frame handed over is not painted yet), `blocking_waits` / `main_wait_usec_total` (times the main thread waited for the render thread, and for how long), `gpu_job_usec_total` / `jsui_render_usec_total` (render-thread time of the view's GPU renders, and of `jsui_render_gpu_target` in them) |
| `get_pending_loads()` | bundle/image/font/fetch loads in flight |
| `get_last_error()` | last uncaught JS error, or `""` |
| `get_effective_pixel_ratio()` | the ratio actually used |
| `get_root_tag()` | the React Native root tag of the view's surface (1: a runtime's main surface, 0: not running) |
| `is_running()` | |

| Signal | |
| --- | --- |
| `message_received(name: String, data: Variant, json: String)` | JS called `Host.postMessage(name, data)`. `data` is the parsed payload |
| `state_changed(key: String, value: Variant)` | JS changed a UI-owned key with `HostState.set` / `patch` (`value` null: removed) |
| `call_requested(id: int, name: String, args: Variant)` | JS called `await Host.call(name, args)`. Answer with `resolve_call` / `reject_call`, now or later |
| `call_cancelled(id: int)` | a call ended unanswered (JS timeout / abort, reload, `stop()`): drop its work |
| `ui_call_completed(id: int, ok: bool, result: Variant)` | a `call_ui` call settled (also when the runtime stopped first) |
| `js_error(message: String)` | a new uncaught JS error |
| `frame_rendered(dirty_rect: Rect2i)` | the texture was updated. The rect is in device pixels. With a separate render thread and the GPU renderer it comes at the start of the next frame's processing of the runtime view (the render ran on that thread before the frame was drawn) |

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

### Several views, one runtime

Like React Native root views, one bundle and one JS context can feed many
views: a HUD, a nameplate over each character, a small widget inside an
existing Godot UI. The bundle registers each component:

```js
AppRegistry.registerComponent('Hud', () => Hud);
AppRegistry.registerComponent('Nameplate', () => Nameplate);
AppRegistry.registerComponent('Badge', () => Badge);
```

One JsuiView runs the bundle (the runtime view); the others set
`runtime_view` to it and pick a `component`:

```gdscript
@onready var ui: JsuiView = $UI   # bundle_url = "res://ui/dist/bundle.js", component = "Hud"

func spawn_nameplate(enemy):
    var plate := JsuiView.new()
    plate.component = "Nameplate"
    plate.initial_props = {"name": enemy.name, "hp": enemy.hp}
    plate.size = Vector2(160, 40)
    enemy.add_child(plate)
    plate.runtime_view = plate.get_path_to(ui)   # attaches (autostart): the surface starts
    enemy.hp_changed.connect(func(hp): plate.initial_props = {"name": enemy.name, "hp": hp})
```

* Each view has its own size, pixel ratio, input, focus, cursor, renderer
  and texture; JS state, `HostState`, fonts and images are shared. Inside a
  component, `useWindowDimensions()` is its view's size and
  `useContext(RootTagContext)` its root tag.
* The runtime view ticks the runtime once per frame and then paints every
  surface view whose frame changed (surface views are not ticked on their
  own, so the cost of a frame is one JS tick however many views there are).
  With the GPU renderer the views share one Skia context (the runtime
  view's), and with a separate render thread their frames go to that
  thread as one job per frame (see Rendering).
* The runtime's signals (`message_received`, `state_changed`,
  `call_requested`, …) are emitted by the runtime view. On a surface view the
  state, call, message and `reload()` methods act on the runtime view.
* `stop()` / `start()` on a surface view destroy / recreate its surface;
  on the runtime view they take every surface with them, and the surfaces
  come back with the new runtime. Freeing a surface view unmounts its
  component.
* `auto_size` makes a surface view take its component's laid-out size (a
  component with a fixed or content size, like a badge or a button inside a
  Godot `Container`).

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
  the damaged region. Idle frames do no work at all.
* Renderer, chosen on the first frame from the RenderingServer's driver:

  | Godot renderer / driver | JsuiView renderer |
  | --- | --- |
  | godot-aw3's `webgpu` driver (`WebGPUInterop` singleton, same Dawn revision) | `webgpu`: Skia Graphite on Godot's `WGPUDevice` and queue ([below](#webgpu-dawn-on-the-godot-aw3-fork)) |
  | Forward+ / Mobile on Vulkan (Linux, Windows) | `vulkan`: Skia Graphite on Godot's `VkDevice` and main queue |
  | Forward+ / Mobile on Metal (macOS, Apple Silicon) | `metal`: Skia Graphite on Godot's `MTLDevice` and command queue |
  | Compatibility (OpenGL), headless, D3D12, `force_cpu` | `cpu`: CPU raster + `ImageTexture` upload |

  It also falls back to `cpu` (with a warning) when the library was built
  against a Skia without that backend, when Skia rejects the device, when
  `WebGPUInterop` reports another Dawn revision or when a GPU frame fails.
  Godot 4.3 cannot name its driver: there, Linux is taken as Vulkan and
  other platforms use `cpu`.
* GPU renderer: per view there are two RenderingDevice textures of the
  view's pixel size. Skia renders into the first (colour + input attachment;
  it keeps the last frame, so a frame repaints only its damage, split into
  up to four scissored rects when changes are far apart), all on Godot's render
  thread, with one Skia context for all the views of a runtime. JSUI submits
  its work to Godot's queue during `_process`, before Godot submits its
  frame. When Godot renders on a separate thread (`--render-thread
  separate`), the runtime view queues the frame's renders of all its views
  (each into its own textures) there as one job
  (`RenderingServer.call_on_render_thread`) and does not wait: the queue is
  in order, so it still runs before that frame is drawn. The renders use the
  runtime, so the next call into it on any of its views (input, `set_state`,
  `initial_props`, the next `_process`, ...) waits until the job has run.
  Godot syncs with its render thread after `_process` every frame, so in
  steady state nothing waits; only calls into the runtime's views later in
  the same frame (after the runtime view's `_process`) can. Creating the
  textures (start, resize), `get_frame_image()` and freeing a view still
  wait for the render thread (a view, its surface and the shared context go
  only after the renders queued with them have run). On Vulkan the render is bracketed by
  full pipeline barriers and leaves the image in `TRANSFER_SRC_OPTIMAL`, the
  layout Godot's render graph tracks for a copy source. Godot then copies the
  damaged rect into the second texture (`RenderingDevice.texture_copy`),
  which the canvas item samples through a `Texture2DRD`, so Godot does that
  texture's barriers itself. The `Texture2DRD` makes its RenderingServer
  texture on the render thread: with a separate one (`--render-thread
  separate`) JsuiView waits for that before it hands the texture to its
  canvas item, which would otherwise keep Godot's placeholder (a magenta /
  black checkerboard). Nothing is read back or uploaded. New textures
  (first frame, resize) are used from the next drawn frame; the old ones stay
  on screen until then.
* CPU renderer: a changed frame uploads the whole texture (Godot 4 has no
  sub-rectangle texture upload).
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
* Pass-through (`input_pass_through`, default on): only the points where the
  UI captures input count as inside the view (`_has_point` asks
  `jsui_hit_test`), so a click on an empty or decorative part of a full-screen
  HUD goes to the Godot controls behind the view and to the game's
  `_unhandled_input`, and doesn't take keyboard focus. "Captures" means
  something in the UI handles it (onPress, pointer / hover / responder
  handlers, TextInput, a ScrollView that can scroll), not that the point is
  painted; views with `pointerEvents="box-none"` / `"none"` let input
  through. A backdrop that should block the game (a pause menu) needs a
  handler, e.g. `onStartShouldSetResponder={() => true}`. A drag the game
  started keeps going to the game when it crosses UI, and a press the UI took
  keeps its drag. Turn it off to make the whole rect a Control that receives
  (and bubbles) every event. `hit_test(position)` returns `HIT_POINTER` /
  `HIT_SCROLL` flags for any local point, e.g. for the game's own picking.
  Each surface view hit-tests its own surface.

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
* CPU renderer: the Godot 4.3 API has no sub-rectangle texture upload, so
  the dirty rect saves only CPU paint work, not GPU upload bandwidth.
* GPU renderer, Vulkan: Vulkan queues need external synchronisation. JSUI
  submits to Godot's main queue from the render thread, which is safe
  against Godot's own frame submission, but Godot's background transfer
  worker can share that queue (when the device has no separate transfer
  queue) and submits under a lock the extension cannot take. Loading
  textures on a worker thread while a view renders could, in theory, race.
  Godot does not expose its enabled device extensions, so Skia assumes core
  Vulkan 1.1 only (it logs a `VK_KHR_driver_properties` warning once).
* GPU renderer, Metal: compiles in CI (macOS job) but has not been run yet;
  see [Verifying Metal on a Mac](#verifying-metal-on-a-mac). Intel Macs that
  run Forward+ on MoltenVK go through the Vulkan path, untested.
* The macOS library is named `.universal` but a source-built Skia is for the
  host architecture only.
* Built and tested on Linux x86_64 (CI: headless, OpenGL and Vulkan via
  lavapipe). Windows (Vulkan / D3D12) is not built or tested.

## Verifying Metal on a Mac

On an Apple Silicon Mac with Xcode command line tools, Ninja, Node 22 and
Godot 4.7.2:

```sh
scripts/build-skia-desktop.sh            # Skia m157, Graphite + Metal (~30 min)
cmake -S . -B build-mac -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DJSUI_BUILD_GODOT=ON -DJSUI_BUILD_TESTS=ON -DJSUI_SKIA_DIR=third_party/skia-desktop
#   configure must print "JSUI_GRAPHITE_METAL: ON"
cmake --build build-mac --target jsui_godot jsui-tests
./build-mac/jsui-tests                    # all PASS (vulkan_* SKIP)
npm ci && npx jsui build integrations/godot/demo/ui/app.jsx -o integrations/godot/demo/ui/dist/bundle.js

GODOT=/Applications/Godot.app/Contents/MacOS/Godot
cd integrations/godot/demo
$GODOT --headless --path . --import
# 1. Self-test + GPU-vs-CPU comparison on Metal (Forward+):
$GODOT --path . --rendering-method forward_plus --rendering-driver metal -- \
  --capture /tmp/metal.png --self-test --expect-renderer metal --max-gpu-diff 1.5
#   expect "JsuiView: rendering with Skia Graphite on metal", every check PASS,
#   "gpu-vs-cpu: metal ... ok", and /tmp/metal-gpu.png looking like /tmp/metal-cpu.png
# 2. The same at 2x (Retina): add --resolution 2560x1440 and -- --window 2560x1440
# 3. Frame cost, GPU vs CPU:
$GODOT --path . --rendering-method forward_plus --rendering-driver metal -- --bench 240 --no-world
$GODOT --path . --rendering-method forward_plus --rendering-driver metal -- --bench 240 --no-world --force-cpu
# 4. Interactive: run it, hover / scroll / type, resize the window; with the
#    Metal API validation layer on, nothing should be reported:
MTL_DEBUG_LAYER=1 $GODOT --path . --rendering-method forward_plus --rendering-driver metal
```

## WebGPU (Dawn) on the godot-aw3 fork

The [godot-aw3](https://github.com/orktes/godot-aw3) Godot fork has a
`webgpu` rendering driver: Forward+ on native Dawn, with Dawn linked
statically into the engine. Under it JsuiView renders with Skia Graphite on
Godot's own Dawn device, with the same two-texture and `texture_copy` design
as Vulkan / Metal (`get_renderer()` returns `"webgpu"`).

The same `libjsui_godot` serves both engines. It chooses the backend at run
time, on the first frame:

1. If `Engine.has_singleton("WebGPUInterop")` and that singleton's
   `get_dawn_revision()` equals the Dawn revision the library was built
   against (`jsui_dawn_revision()`), JsuiView uses `webgpu`.
2. Otherwise it uses `vulkan` or `metal` on those drivers, as on stock Godot.
3. Otherwise it uses `cpu`.

If the revisions differ, JsuiView pushes one warning naming both and uses
`cpu`. The `webgpu.h` ABI changes between Dawn revisions, so there is no
partial fallback.

* The extension links no Dawn and needs no exported symbols. Skia and JSUI
  are compiled against Dawn's headers at the revision Skia m157 pins.
  Every `wgpu*` call goes through a generated trampoline (`tools/dawn-shim`)
  into a table that `jsui_gpu_create_dawn_procs` fills, all or nothing, by
  asking the engine's `wgpuGetProcAddress` for each name. If any entry point
  does not resolve, no context is made and JsuiView uses `cpu`.
* `scripts/build-skia-desktop.sh` always builds Graphite's Dawn backend
  next to Vulkan (Linux) / Metal (macOS). It uses Skia's own Dawn checkout
  (`third_party/externals/dawn`), only for headers generated by Dawn's code
  generator (`scripts/patches/skia-m157-dawn-header-only.patch`), and
  installs them into `third_party/skia-desktop/dawn/include` with the
  revision in `dawn/REVISION`. CMake turns `JSUI_GRAPHITE_DAWN` on when the
  Skia has Dawn and configure prints
  `JSUI_GRAPHITE_DAWN: ON (Dawn ada567700e13…, …)`.

### The WebGPUInterop contract (what the fork provides)

| | |
| --- | --- |
| Dawn | exactly `ada567700e13aecf7989d78b21f7d330b522ea80`, the revision Skia m157 (`3c6589d488`) pins in its DEPS (`DAWN_REVISION` in `scripts/build-skia-desktop.sh`) |
| Singleton | `Engine.register_singleton("WebGPUInterop", obj)`, with `obj` a plain `Object`. Registered only while the WebGPU rendering driver is active (stock Godot has none) and before the first frame is drawn, i.e. once the RenderingDevice exists |
| `get_proc_address() -> int` | the address of Dawn's `WGPUProc wgpuGetProcAddress(WGPUStringView procName)`, as an int64 |
| `get_instance() -> int` | the `WGPUInstance` the device comes from. Graphite calls `wgpuInstanceProcessEvents` on it while it waits for GPU work |
| `get_device() -> int` | the RenderingDevice's `WGPUDevice` |
| `get_queue() -> int` | the `WGPUQueue` Godot submits its frames to |
| `get_dawn_revision() -> String` | the 40-character Dawn commit hash the engine links (lower case; surrounding whitespace is ignored) |

All methods take no arguments. Pointer values are returned as `int`
(int64). The methods are called on Godot's render thread, so they must be
safe to call there. They should just return stored values.

`wgpuGetProcAddress` must resolve every function that the revision's
`webgpu.h` declares, the free functions included, as Dawn's own
`dawn::native::GetProcs().getProcAddress` does. JSUI looks up each name
once, with an explicit length (not `WGPU_STRLEN`). JSUI adds its own
references to the instance, device and queue (`wgpu*AddRef`) and releases
them when the view is freed. The objects must stay valid until then.

RenderingDevice native handles are webgpu.h handles as well:
`get_driver_resource(DRIVER_RESOURCE_TEXTURE, rid, 0)` returns the
`WGPUTexture` of an RD texture. JsuiView creates its render target with
`COLOR_ATTACHMENT | INPUT_ATTACHMENT | SAMPLING | CAN_COPY_FROM | CAN_COPY_TO
| CAN_UPDATE` in `DATA_FORMAT_R8G8B8A8_UNORM`. On the WGPUTexture that must
include `RenderAttachment` and `TextureBinding` (or `CopySrc`) usage, in
`RGBA8Unorm`. JSUI submits to the same `WGPUQueue` from the render thread
during `_process`, before Godot submits its frame, so queue order alone
handles synchronisation. `DRIVER_RESOURCE_TOPMOST_OBJECT` /
`LOGICAL_DEVICE` / `COMMAND_QUEUE` should return the instance, device and
queue too, but JSUI takes those from the singleton.

### Building and running

```sh
scripts/build-skia-desktop.sh        # -> third_party/skia-desktop (Graphite: Vulkan or Metal, + Dawn)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DJSUI_BUILD_GODOT=ON \
      -DJSUI_SKIA_DIR=third_party/skia-desktop
#   configure prints "JSUI_GRAPHITE_DAWN: ON (Dawn ada567700e13..., ...)"
cmake --build build --target jsui_godot

GODOT=/path/to/godot-aw3/bin/godot.macos.editor.arm64   # built with webgpu=yes, Dawn at the revision above
cd integrations/godot/demo
$GODOT --path . --rendering-method forward_plus --rendering-driver webgpu -- \
  --capture /tmp/webgpu.png --self-test --expect-renderer webgpu --max-gpu-diff 1.5
$GODOT --path . --rendering-method forward_plus --rendering-driver webgpu --disable-vsync -- --bench 240 --no-world
```

On stock Godot, `FAKE_WEBGPU_INTEROP=<rev> integrations/godot/smoke-test.sh`
(the demo's `--fake-webgpu-interop <rev>`) registers a stand-in singleton
with null handles. CI uses it to check that another revision gives the
warning and the CPU renderer.

To move to another Dawn, the Skia pin and the fork's Dawn must move
together. Bump `SKIA_COMMIT` / `DAWN_REVISION` in
`scripts/build-skia-desktop.sh`, which fails if Skia's DEPS pin a different
Dawn, and rebuild the fork against the new revision.
