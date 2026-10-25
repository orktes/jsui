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
`resolve_call`, and `call_ui` → `Host.handle`) and messages, and shows
Godot's own textures in the UI (`set_texture` → `<HostTexture>`: a video, a
SubViewport, any `Texture2D`; see [Host textures](#host-textures)). It plays
the UI's sounds (`Sound.play`, `useMusic`, `<UISounds>`) with
`AudioStreamPlayer`s on Godot's audio buses, so they follow the game's volume
settings and effects (see [Audio](#audio)).

```
integrations/godot/
  CMakeLists.txt        GDExtension target (jsui_godot), fetches godot-cpp
  src/                  JsuiView, its audio handler (jsui_audio.*), key/gamepad mapping, entry point
  demo/                 Godot 4.7 project: main.tscn + main.gd + jsui.gdextension
  demo/ui/app.jsx       demo UI: examples/showcase/interactions.jsx + a bridge strip
  demo/ui/sounds/       the demo's UI sounds and music loop (make-sounds.py synthesizes them)
  demo/cinema/          host textures demo: a VideoStreamPlayer TV and a
                        SubViewport preview inside a JSUI UI (ui/cinema.jsx);
                        fetch-video.sh downloads the clip
  smoke-test.sh         runs the demo headless with an input self-test at 1x and
                        2x content scale and saves the frames (used by CI);
                        GODOT_RENDERING=vulkan runs it on Vulkan (GPU renderer)
  record-pipelines.sh   records demo/jsui-pipelines.vulkan.bin, the GPU pipeline
                        list the demo ships (see Rendering)
  web-link-check.c      link check of the threaded web library (see
                        "Threaded web build")
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
Graphite + Metal; Windows, from Git Bash with LLVM's clang-cl: Graphite +
Vulkan and Ganesh + Direct3D 12; all also Graphite + Dawn for the godot-aw3
fork; about 25 minutes on 4 cores, done once) and point CMake at it (the
release's Windows `.dll` is built this way):

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

The UI has sound: `<UISounds>` gives every pressable a tick on hover / focus
and a chime on press (bus `ui`), and `useMusic` loops a quiet pad (bus
`music`) that the strip's "Music" button mutes. They play on the "UI" and
"Music" buses of the demo's `default_bus_layout.tres`.

Headless check (no GPU needed). This is what CI runs. `main.gd --self-test`
drives the UI through Godot's own input pipeline (`Input.parse_input_event`):
hover, click (and checks that JSUI saw exactly the logical point it aimed
at), gamepad D-pad focus + A, wheel and trackpad-pan scrolling, click to
focus + typing + Enter in the TextInput, the Ping call, `call_ui`, host
textures (`set_texture` in a probe view of its own: set before start,
replaced, kept across a restart, removed; with a renderer also updated in
place and a live SubViewport; on Vulkan this is the GPU copy path), UI
audio (the demo's music and interaction sounds, then a probe view driven
with `call_ui`: players on the mapped buses with the voice's pitch and volume,
the UI's bus volume, ramps, STOP with a fade, loops, pause, `stop_all_sounds`,
sounds that play to the end, WAV / MP3 / Ogg decoded from `user://` bytes, a
missing file and `audio_enabled` off, each checked against what JS saw) and an
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
`--js-thread on|off|lockstep` (sets `js_thread`; `JS_THREAD=off` in
`smoke-test.sh`),
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
times) and `--no-world` (no 3D scene). `--hud` runs the engine examples'
game instead of the showcase: units orbiting a tower under the shared HUD
(`integrations/shared/ui/app.jsx`, built to
`integrations/shared/ui/dist/bundle.js`), with nameplates following the units
every frame through the `entities` buffer. `--no-ui` runs the same game without
JSUI, and `--bench-seconds S` (vsync off, `--bench-warmup S`) prints the same
`bench:` line as the other engines' `--bench`, so the two give the UI's frame
cost. For the pipeline cache:
`--pipeline-cache P` (sets `pipeline_cache_path`; a path outside `user://`
leaves the shipped list out), `--tour` (with `--self-test`: also clicks
through the tabs, the carousel and the deploy panel and scrolls the list),
`--first-scroll` (waits for the precompile, then prints the worst frame, the
worst JsuiView render and the pipelines compiled during the list's first
scroll), `--expect-warm-pipelines` (fails unless the cache was loaded and the
self-test / scroll compiled no pipeline) and `--ui-fixed-fps N` (sets
`fixed_fps`; with `--first-scroll` the scroll starts at 2 s of UI time). In
`smoke-test.sh` they are `PIPELINE_CACHE=<file>`, `TOUR=1`, `FIRST_SCROLL=1`,
`EXPECT_WARM_PIPELINES=1` and `FIXED_FPS=N` (and `NO_WORLD=1`). Which
pipelines a frame needs depends on the frame rate (the raster cache and
opacity layers keep a subtree's texture, or not, by frames), so CI records
a cache in its Vulkan run, adds the first scroll on a 60 fps UI clock, and
checks that the same scroll then compiles nothing
(`JSUI_LOG_PIPELINES=compiled` names what the frames compiled):

```sh
GODOT_RENDERING=vulkan SIZES=1280x720 PIPELINE_CACHE=build/godot-pipelines.bin xvfb-run -a -s "-screen 0 1920x1080x24" \
  integrations/godot/smoke-test.sh build/godot-vulkan.png
GODOT_RENDERING=vulkan SIZES=1280x720 PIPELINE_CACHE=build/godot-pipelines.bin FIRST_SCROLL=1 FIXED_FPS=60 \
  xvfb-run -a -s "-screen 0 1920x1080x24" integrations/godot/smoke-test.sh
JSUI_LOG_PIPELINES=compiled GODOT_RENDERING=vulkan SIZES=1280x720 PIPELINE_CACHE=build/godot-pipelines.bin FIRST_SCROLL=1 FIXED_FPS=60 \
  EXPECT_WARM_PIPELINES=1 xvfb-run -a -s "-screen 0 1920x1080x24" integrations/godot/smoke-test.sh
```
The project uses the Compatibility renderer by default (it runs everywhere);
pass `--rendering-method forward_plus` (or `mobile`) for the GPU renderer.

### Host textures demo

```sh
integrations/godot/demo/cinema/fetch-video.sh   # once: the Big Buck Bunny trailer, 4.4 MB
godot --path integrations/godot/demo res://cinema/cinema.tscn
godot --path integrations/godot/demo --rendering-method forward_plus res://cinema/cinema.tscn   # GPU path
```

`cinema/cinema.tscn` is an in-game TV and a character preview, both drawn by
Godot and framed by a JSUI UI (`ui/cinema.jsx`, component `Cinema`). A
`VideoStreamPlayer` plays the trailer (Ogg Theora, what Godot decodes) and
`cinema.gd` hands its texture to the view with
`set_texture("tv", video.get_video_texture(), true)`; the play / pause,
restart and mute buttons are JSUI `Pressable`s that post messages, and the
progress bar follows `set_buffer("cinema.progress", ...)` without JS per
frame. A `SubViewport` renders a spinning 3D figure every frame
(`set_texture("preview", viewport.get_texture(), true)`), shown in a round
frame with colour swatches. The video is not in the repository:
`fetch-video.sh` downloads it from Wikimedia Commons (falling back to the
Internet Archive's copy of the film; `--full` gets the whole film). Without
it the TV shows "NO SIGNAL" and the rest runs. *Big Buck Bunny* is
© 2008 Blender Foundation (www.bigbuckbunny.org), licensed CC BY 3.0; the
scene shows the credit under the TV.

### Dev mode (live reload)

```sh
npx jsui dev integrations/godot/demo/ui/app.jsx --port 5173
godot --path integrations/godot/demo -- --dev http://127.0.0.1:5173/bundle.js
```

In dev mode the runtime polls the dev server (`/__jsui/version`), reloads
when you save, and shows JS errors in an on-screen overlay. You can also set
`bundle_url = "http://127.0.0.1:5173/bundle.js"` and `dev_mode = true` on the
node in the inspector.

The view also joins the dev server's shared session as `Godot: <node name>`:
coding agents attached to the server (`jsui screenshot|inspect|run --attach
http://127.0.0.1:5173`) then drive and screenshot the UI inside the running
game, and see what you click and press in it (`jsui log --attach`). See the
CLI's README.

## JS thread

By default the bundle's JS runs on a thread of its own, as React Native's
does (`js_thread` = `JS_THREAD_ON`). The view still ticks in
`NOTIFICATION_PROCESS` on the main thread, but the tick never waits for
JS: timers, events, React's renders and commits and garbage collection run
on the JS thread, and each tick applies what JS finished since the last one
(a commit whole, never half). What native does every frame keeps going
while JS is busy: `Animated` values driven by `set_buffer` data
(`useHostBufferValue` nameplates), scrolling and flings, hover / press
styles, LayoutAnimations, hit testing, layout and paint. A 50 ms commit or
a collection costs the game no frame.

What changes for the game:

* What JS does in answer to the game shows a frame or more later. Signals
  (`message_received`, `state_changed`, `call_requested`, ...) still come on
  the main thread, from the view's `_process`.
* When a change must show on this very frame (a scene cut, a screenshot,
  state the UI must not show late), call `sync_js_next_frame()` before the
  view's next process: that tick waits for JS (up to 250 ms). `sync_js()`
  waits right away.
* `input_pass_through` and the input the UI consumes are decided natively
  (the hit test, listeners the bundle declared), never by waiting for JS; a
  key whose default action JS may veto (Tab, Enter) waits for JS's answer up
  to 100 ms.
* `JS_THREAD_OFF` runs JS inside the tick (the main thread waits for it);
  `JS_THREAD_LOCKSTEP` runs it on its thread but waits every tick, which
  gives the frames of off (tests). Both apply on `start()`.

The demo's self-test starts with `debug_stall_js(400)` and requires
Godot's frames to go on meanwhile (`PASS js stall`).

## JsuiView reference

| Property | Type | Default | |
| --- | --- | --- | --- |
| `bundle_url` | String | `""` | `res://…`, `user://…`, an absolute path, or `http://…` |
| `dev_mode` | bool | `false` | poll the dev server, reload on change, error overlay |
| `pixel_ratio` | float | `0` | device px per logical px; `0` follows the control→window-pixel scale (stretch / content scale, HiDPI windows, Camera2D zoom, parent scale) |
| `autostart` | bool | `true` | start in `_ready` (otherwise call `start()`) |
| `forward_gamepad` | bool | `true` | forward joypad events received while focused |
| `force_cpu` | bool | `false` | always use the CPU renderer, also on Vulkan / Metal |
| `js_thread` | int | `JS_THREAD_ON` | where the bundle's JS runs: `JS_THREAD_ON` on a thread of its own (React Native's JS thread: frames never wait for JS), `JS_THREAD_OFF` inside the tick on the main thread, `JS_THREAD_LOCKSTEP` on its thread but waited for every tick (the frames of off). Applies on `start()`; the runtime view's counts. See [JS thread](#js-thread) |
| `runtime_view` | NodePath | empty | show a surface of that JsuiView's runtime instead of running a bundle of its own (see [Several views, one runtime](#several-views-one-runtime)) |
| `component` | String | `""` | the `AppRegistry` key this view shows. On a runtime view: the main surface's component (empty: the bundle's first registered one) |
| `initial_props` | Dictionary | `{}` | the component's props; changing them re-renders it with the new props (state kept) |
| `auto_size` | bool | `false` | size the view to the component's content: the view's minimum size follows the laid-out size |
| `profiling` | bool | `false` | time every frame phase (`jsui_set_profiling`), read with `get_perf()` |
| `pipeline_cache_path` | String | `"user://jsui-pipelines.bin"` | the GPU pipeline cache of the runtime's Skia context (the renderer's name goes before the extension); empty: none. Applies when the context is created; a surface view uses its runtime view's. See [Rendering](#rendering) |
| `audio_enabled` | bool | `true` | play the UI's sounds; off: nothing plays and every voice ends as failed (voices playing stop) |
| `audio_bus` | String | `"Master"` | the Godot bus for JSUI buses that have no Godot bus of their own (see [Audio](#audio)) |
| `audio_bus_map` | Dictionary | `{}` | JSUI bus → Godot bus, e.g. `{"sfx": "Effects"}`; wins over the same-name match |
| `fonts` | Array[Font] | `[]` | `FontFile`s registered on every new runtime, family / weight / style from the file (see [Fonts](#fonts)). Assigning the array registers the fonts it adds on a running runtime; fonts it drops stay until the next `start()` |
| `fixed_fps` | int | `0` | `0`: the runtime's clock (animations, timers, scrolling) is the wall clock. `N`: every frame advances it by 1/N s however long frames take, so the UI draws the frames of a machine running at N fps on any machine (tests, recording a pipeline list). The runtime view's applies |

Changing `bundle_url` or `dev_mode` on a running view restarts it.

| Method | |
| --- | --- |
| `start()` / `stop()` | create the runtime and load `bundle_url` / destroy it |
| `reload()` | re-evaluate the bundle (same runtime) |
| `post_message(name, data = null)` | send to JS: `data` is JSON-encoded. JS receives it with `useHostMessage(name, cb)` / `Host.onMessage` |
| `post_message_json(name, json)` | same with a JSON string |
| `sync_js(timeout_ms = 250) -> bool` | with JS on its own thread: waits until JS has handled everything sent to it (`jsui_sync`), applies it and lays out, emits the signals for what it did, and repaints on the next frame. `true` when JS is left idle, `false` after `timeout_ms` (negative: no limit). Without the thread: `true` at once |
| `sync_js_next_frame()` | the next tick waits for JS (`JSUI_TICK_SYNC_JS`, up to 250 ms): what the game set before it (state, messages, input) shows on that frame. For scene cuts, screenshots, state that must not lag |
| `debug_stall_js(ms)` | keeps JS busy for `ms` (`jsui_debug_stall_js`), as a long commit or a collection would: with the thread the game's frames go on, without it this call takes `ms` |
| `get_active_js_thread() -> int` | the `JS_THREAD_*` mode the runtime runs in (`JS_THREAD_OFF` while not running, or where the build has no threads) |
| `set_state(key, value) -> int` | shared state (`jsui_set_state`): `value` (Dictionary, Array, number, String…) is JSON-encoded, `null` removes the key. JS reads it with `useHostState(key)` and re-renders on change. Returns 1 changed, 0 unchanged, -1 invalid |
| `patch_state(key, patch) -> int` | RFC 7386 merge patch of a key (`jsui_patch_state`): Dictionaries merge, `null` members delete. Keep big collections as Dictionaries keyed by id |
| `get_state(key) -> Variant` | current value (also keys the UI set with `HostState.set`) |
| `set_state_json` / `patch_state_json` / `get_state_json` | the same with JSON strings |
| `set_buffer(key, data) -> int` | frame data (`jsui_set_buffer`): numbers pushed every frame without JSON, e.g. enemy positions for nameplates. `data` is a `PackedFloat32Array`, `PackedFloat64Array`, `PackedInt32Array`, `PackedByteArray`, or a `PackedVector2Array` / `PackedVector3Array` / `PackedColorArray` (their components in order). JS reads it with `useHostBuffer` / `useHostBufferSelector`, and `useHostBufferValue(key, index)` moves `Animated` views natively, without JS per frame. Returns 1 changed, 0 unchanged, -1 unsupported type. Set before `start()`: applied when it starts |
| `remove_buffer(key) -> int` | removes a buffer (1 if it existed) |
| `set_texture(name, texture, live = false) -> int` | shows a Godot `Texture2D` in the UI as host texture `name` (`<HostTexture name=...>`, `useHostTexture(name)`): an `ImageTexture`, a `ViewportTexture`, a `VideoStreamPlayer`'s texture, a `Texture2DRD`… `live`: its contents change every frame (a video, a viewport), so they are copied every frame; otherwise only when the texture emits `changed` or on `update_texture`. Calling it again with another texture replaces it. Kept by the view across `stop()` / `start()`; set before `start()` it is there for the first frame. Returns 1. See [Host textures](#host-textures) |
| `remove_texture(name) -> int` | stops showing it (`<HostTexture>` draws nothing, `useHostTexture` returns null); 1 if it existed |
| `update_texture(name) -> int` | the texture's contents changed (for a non-live texture that does not emit `changed`, e.g. a `Texture2DRD` the game draws into); -1: unknown name |
| `register_font(family, font, weight = 0, italic = -1) -> bool` | registers a font with the runtime (every view of it): `font` is a `FontFile` (or a `FontVariation` of one), a `PackedByteArray`, or a path (`res://`, `user://`, absolute) of a TTF / OTF / TTC / WOFF / WOFF2 file. `family` `""`: the name in the file; `weight` `0` / `italic` `-1`: the file's, otherwise overrides (1..1000, `0` / `1`). Kept and re-applied to new runtimes; called before `start()` it is there for the first frame. `false`: unreadable or not a font. See [Fonts](#fonts) |
| `unregister_font(family) -> bool` | removes every face of the family; `true` if it was registered |
| `set_font_fallbacks(families: PackedStringArray)` | families tried in order for glyphs the styled font lacks (emoji, CJK); empty clears it. Every registered font is searched after them anyway |
| `set_default_font_family(family)` | the family of text whose style names none (`"Inter"` by default) |
| `resolve_call(id, result = null) -> int` | answer a `call_requested` call; the UI's `await Host.call(...)` resolves with `result`. Any later frame is fine. -1: unknown or cancelled id |
| `resolve_call_json(id, json)` / `reject_call(id, message)` | JSON result / reject (the promise rejects with `Error(message)`) |
| `call_ui(name, args = null, callback = Callable()) -> int` | run the UI's `Host.handle(name, fn)` / `useHostHandler` handler; `callback(ok: bool, result)` (and `ui_call_completed`) runs once it settles: `result` is the handler's value, or the error message when `ok` is false |
| `get_texture()` | the `Texture2D` being drawn (premultiplied RGBA): a `Texture2DRD` with the GPU renderer, an `ImageTexture` with the CPU one |
| `get_frame_image()` | CPU copy of the last frame, straight alpha, works with `--headless` (with the GPU renderer it reads the texture back: slow, for tests and screenshots) |
| `get_cpu_frame_image()` | the current frame painted by the CPU raster, straight alpha, whatever the renderer (to compare with `get_frame_image()`) |
| `get_renderer()` | `"vulkan"`, `"metal"`, `"d3d12"`, `"webgpu"` or `"cpu"` (`""` before the first frame) |
| `get_render_stats()` | Dictionary: `renderer`, `renders`, `render_usec_total` / `_last` / `_max` (time in the view's render call, CPU side), `dirty_pixels_total`, `tick_usec_last` (the last `jsui_tick`); GPU renderer: `gpu_jobs` (the view's frames handed to the render thread without waiting), `render_thread_jobs` (the render-thread jobs those went in: one per frame for all the views of a runtime; the runtime view's count), `gpu_frame_in_flight` (the last frame handed over is not painted yet), `blocking_waits` / `main_wait_usec_total` (times the main thread waited for the render thread, and for how long), `gpu_job_usec_total` / `jsui_render_usec_total` (render-thread time of the view's GPU renders, and of `jsui_render_gpu_target` in them); `pipelines` (the runtime's pipeline cache, `jsui_gpu_get_pipeline_stats`: `mode` `"thread"` / `"sliced"`, `queued`, `precompiled`, `failed`, `compiled` on demand, `added` to the file, `unserializable`, `known`, and the `file`) |
| `get_perf(reset = true)` | Dictionary (`jsui_get_perf`) since the last reset: exclusive time per phase in µs (`js_us`, `commit_us`, `animated_us`, `style_us`, `layout_us`, `input_us`, `paint_us`, `gpu_submit_us`, `other_us`; with `profiling` on) and counters (`image_decodes`, `image_decode_us`, `pipeline_compiles`, `paragraph_builds`, `text_measures`, `layout_passes`, `bridge_calls`) and JS garbage collection (`gc_collections`, `gc_full_collections`, `gc_us`, `gc_max_us`); with the JS thread: `js_busy_ms` (JS thread time in tasks), `js_batches_applied`, `js_apply_ms` (main-thread time applying them), `js_lag_frames` (ticks since JS last delivered while it had work), `js_sync_ms` (time waiting in syncs) |
| `get_pending_loads()` | bundle/image/font/fetch loads in flight |
| `get_last_error()` | last uncaught JS error, or `""` |
| `get_effective_pixel_ratio()` | the ratio actually used |
| `get_root_tag()` | the React Native root tag of the view's surface (1: a runtime's main surface, 0: not running) |
| `is_running()` | |
| `stop_all_sounds(bus = "", fade_ms = 0)` | stops the UI's voices of one JSUI bus (`""`: all), fading out over `fade_ms`; JS sees them end as `stopped` |
| `get_audio_voices() -> Array` | the UI's voices now: Dictionaries with `voice`, `url`, `bus` (JSUI), `godot_bus`, `player` (its `AudioStreamPlayer`, null while the sound loads), `volume` (voice × fade × the UI's bus volume), `pitch`, `loop`, `paused`, `loading` |
| `resolve_audio_bus(bus) -> String` | the Godot bus JSUI bus `bus` plays on now |

| Signal | |
| --- | --- |
| `message_received(name: String, data: Variant, json: String)` | JS called `Host.postMessage(name, data)`. `data` is the parsed payload |
| `state_changed(key: String, value: Variant)` | JS changed a UI-owned key with `HostState.set` / `patch` (`value` null: removed) |
| `call_requested(id: int, name: String, args: Variant)` | JS called `await Host.call(name, args)`. Answer with `resolve_call` / `reject_call`, now or later |
| `call_cancelled(id: int)` | a call ended unanswered (JS timeout / abort, reload, `stop()`): drop its work |
| `ui_call_completed(id: int, ok: bool, result: Variant)` | a `call_ui` call settled (also when the runtime stopped first) |
| `js_error(message: String)` | a new uncaught JS error |
| `sound_played(voice: int, url: String, bus: String)` | a UI sound started playing (`bus`: the JSUI bus; `get_audio_voices()` has its player) |
| `sound_ended(voice: int, status: int)` | a UI voice ended: `SOUND_FINISHED` (0), `SOUND_STOPPED` (1) or `SOUND_FAILED` (-1), as JS hears it |
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

func _process(_delta):
    # Every frame: screen positions of the enemies for their nameplates.
    var pos := PackedVector2Array()
    for e in enemies:
        pos.append(camera.unproject_position(e.global_position + Vector3.UP * 2))
    ui.set_buffer("enemies", pos)   # x0, y0, x1, y1, ... in JS

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

### Host textures

`set_texture(name, texture, live)` shows any Godot `Texture2D` inside the
UI. JSUI plays or renders nothing itself: the game decodes the video, renders
the viewport or draws the texture, and the UI lays it out with
`<HostTexture name="tv" resizeMode="contain" />` (clipped by rounded
corners, transformed, composited like an image). `useHostTexture(name)`
returns `{width, height}` (or null) for layout. When only a texture changes,
no JS runs: the runtime repaints the views that show it.

```gdscript
ui.set_texture("tv", $VideoStreamPlayer.get_video_texture(), true)   # live: copied every frame
ui.set_texture("preview", $SubViewport.get_texture(), true)
ui.set_texture("portrait", load("res://hero.png"))                   # static: copied once
ui.remove_texture("portrait")
```

How the pixels reach Skia depends on the view's renderer:

* GPU renderer (Vulkan, Metal, WebGPU): the view keeps a *mirror* texture of
  its own per host texture, in the format Skia samples (RGBA8 / BGRA8 UNORM),
  and copies the Godot texture into it on Godot's render thread with
  `RenderingDevice.texture_copy` (recorded in Godot's frame, so Godot orders
  it after whatever drew the source). Skia samples the mirror, never Godot's
  texture: Godot may free or replace its texture at any time (a
  `texture_replace`, a resized viewport) and its layout is Godot's business.
  The copy of frame N is shown from frame N+1, so a host texture lags one
  frame behind the game. A live texture costs one GPU copy per frame; a
  static one is copied when it changes. A mirror is reused while the source's
  size and format stay; a new one (and its native handle, resolved with
  `get_driver_resource` on the render thread) is made when they change, and
  the old one is freed once no frame in flight uses it.
  * Vulkan: Skia assumes a sampled texture is in
    `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` (`JsuiTexture` has no layout
    field). After the copy Godot leaves the mirror in `TRANSFER_DST`, so the
    view then samples each copied mirror in a one-texel compute dispatch
    (a tiny "layout pass" in Godot's frame); Godot's render graph transitions
    it to `SHADER_READ_ONLY_OPTIMAL` and orders the copy before JSUI's work.
    The cost is one dispatch per frame for all copied textures. CI runs this
    under synchronization validation.
  * Metal: the mirror is an `MTLTexture` the view makes itself (tracked,
    so Metal orders the copy and Skia's sampling) and gives to Godot with
    `texture_create_from_extension`.
  * WebGPU (godot-aw3): the mirror's `WGPUTexture`; Skia reads its format.
  * Formats: the source must be a 2D, single-layer, single-sample
    `RGBA8` / `BGRA8` UNORM or SRGB texture created with
    `TEXTURE_USAGE_CAN_COPY_FROM_BIT` (`ImageTexture`, viewports,
    `VideoStreamPlayer` textures are; a `Texture2DRD` must be created with
    it). SRGB sources are copied into UNORM mirrors (the bytes, no
    conversion), like JSUI's other images. Anything else (HDR viewports'
    `RGBA16F`, compressed textures, a missing usage bit) falls back to the
    CPU path below for that texture, with one warning.
* CPU renderer (Compatibility / OpenGL, headless, `force_cpu`), and any view
  of a runtime that has a CPU view: `get_image()`, converted to RGBA8, is
  passed to `jsui_set_texture_pixels`. On a renderer this reads the texture
  back from the GPU (a stall), every frame for a live texture: fine for a
  small preview, costly for a 1080p video. Static textures are read once and
  again on `changed` / `update_texture`.

Alpha: host textures are treated as straight (unpremultiplied) alpha, like
Godot's textures; a `ViewportTexture` as premultiplied (viewport render
targets with a transparent background hold premultiplied colour).

Host textures belong to the runtime: on a surface view (`runtime_view`) they
are set on the runtime view and every surface can show them. The view keeps
the `Texture2D` referenced until `remove_texture` or until the view is freed.

### Audio

The UI asks for sounds (`Sound.play(require('./confirm.wav'), { bus: 'ui' })`,
`useMusic`, `<UISounds>` / `sounds` on pressables; see `include/jsui/jsui.h`
"Audio") and JsuiView plays them through Godot: each voice is an
`AudioStreamPlayer`, an internal child of the runtime view (pooled, not shared
with other views), on one of Godot's buses. UI sounds thus go through the
game's mixer, effects and volume settings like any other sound, and the
library's own audio backend stays out of the way.

* **Buses.** A JSUI bus (`ui`, `sfx`, `music`, any name) plays on the Godot bus
  `audio_bus_map` names for it, else on the Godot bus with the same name
  (case-insensitive: `music` → `Music`), else on `audio_bus` (default
  `"Master"`). The UI's own bus volume and mute (`Sound.setBusVolume`,
  `setBusMuted`) scale its voices; JsuiView never changes `AudioServer` bus
  volumes, so a game's settings menu stays in charge of those.
* **Loading.** `res://` and `user://` sounds load with `ResourceLoader` when
  Godot imported them (the editor imports the bundle's `.wav` / `.ogg` /
  `.mp3` files in `ui/dist/assets/`; an exported game has only the imported
  copies), otherwise as bytes through `FileAccess`. Other URLs (a dev
  server's `http://`) are read through the runtime's loader (`jsui_fetch`).
  Bytes are decoded by their header: WAV (PCM 8 / 16 / 24 / 32-bit, float;
  mono or stereo), Ogg Vorbis, MP3. Loaded streams are cached per URL, so
  `Sound.preload` / `useSound` make the first play instant; a play that comes
  while its sound loads starts when it is there.
* **Voices.** Volume (× fade × the UI's bus volume) is `volume_db`, pitch is
  `pitch_scale`, the start offset `play(from_position)`. A looping voice plays
  a copy of the stream with its loop flag on. Fades (fade in, fade out on
  stop, `setVolume` / bus ramps) advance every frame. Pause / resume use
  `stream_paused`. A voice has finished when its player stops on its own; JS
  hears every end (`finished`, `stopped`, `failed`) on the next tick.
* **The game's side.** `sound_played` / `sound_ended` report the UI's voices,
  `get_audio_voices()` lists them with their players, `stop_all_sounds()`
  stops them (e.g. when pausing the game) and `audio_enabled = false` silences
  the UI entirely.

```gdscript
ui.audio_bus = "SFX"                          # buses the game has no match for
ui.audio_bus_map = {"ui": "Interface"}        # explicit JSUI -> Godot buses
ui.sound_played.connect(func(voice, url, bus): print("UI sound ", url, " on ", bus))
ui.stop_all_sounds("music", 300)              # fade the UI's music out
```

Surface views (`runtime_view`) play through their runtime view, whose
`audio_*` properties apply.

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
  | Forward+ / Mobile on Direct3D 12 (Windows) | `d3d12`: Skia Ganesh on Godot's `ID3D12Device` and DIRECT queue (Graphite has no D3D backend; no pipeline cache) |
  | Compatibility (OpenGL), headless, `force_cpu` | `cpu`: CPU raster + `ImageTexture` upload |

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
* GPU pipeline cache: Skia compiles a GPU pipeline (shaders + render state)
  the first time the UI draws something new, inside that frame: a list's
  first scroll over rounded images, a panel's first entrance, by tens of
  milliseconds on a cold driver. The runtime's Skia context records every
  pipeline it creates in the runtime view's `pipeline_cache_path`, appended
  as they appear (`user://jsui-pipelines.bin` becomes
  `user://jsui-pipelines.vulkan.bin`: one file per renderer), and from the
  next start on precompiles them off the frame: on a worker thread on Vulkan
  and Metal, and on a Dawn device that is not thread safe in ~2 ms slices
  after renders (at most one per frame), on the render thread. For a
  `user://` path the same file under `res://`
  (`res://jsui-pipelines.vulkan.bin`) is precompiled too, so a player's
  first session is warm as well: the demo ships one, recorded with
  `record-pipelines.sh` (the self-test and tour, and the list's first scroll
  on 30 to 144 fps UI clocks, since what a frame draws depends on the frame
  rate; keys are fitted to the GPU that loads them; record again after
  updating JSUI's Skia). A cache of another Skia build or
  renderer is started afresh and a damaged one repaired, with one log line
  each. `get_render_stats().pipelines` shows what it did.
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

### Fonts

The runtime ships Inter. Any other font the UI's `fontFamily` names is
registered by the game or loaded by the UI itself; either way it belongs to
the runtime and every view of it, and text already on screen relayouts.

```gdscript
@export var title_font: FontFile   # e.g. res://fonts/Display-Bold.ttf

func _ready():
    ui.register_font("Display", title_font)                  # weight / style from the file
    ui.register_font("Display", "res://fonts/Display-Italic.woff2", 400, 1)
    ui.register_font("", "res://fonts/NotoSansJP.otf")      # family name from the file
    ui.set_font_fallbacks(["Noto Sans JP", "Noto Color Emoji"])
    ui.set_default_font_family("Display")                  # text without a fontFamily
```

```jsx
<Text style={{ fontFamily: 'Display', fontWeight: '700' }}>Victory</Text>
```

* Or list the fonts in the inspector: the `fonts` property takes `FontFile`
  resources (drag `.ttf` / `.otf` / `.woff2` files onto it), registered with
  the family, weight and style in each file whenever the runtime starts.
* The view keeps every `register_font` / `unregister_font` call and the
  fallback and default-family settings, and replays them, after `fonts`, on
  each new runtime (`start()`, a new `bundle_url`), like `set_state`. Calls on
  a surface view go to its runtime view; a surface view's own `fonts` are
  registered there when it attaches.
* A `FontFile`'s data is the file it was imported from, so its weight and
  style come from that file; a `FontVariation`'s settings are not applied
  (pass `weight` / `italic`, or use `fontVariationSettings` in the UI).
  `SystemFont` and bitmap fonts have no font file and are refused.
* The UI can load fonts itself: `Fonts.load('Display', 'fonts/Display.woff2')`
  (`useFonts` / `Fonts.loadAsync` too) resolves against the bundle URL and,
  for a `res://` / `user://` bundle, reads the file through `FileAccess`.
  Godot imports `.ttf` / `.otf` / `.woff` / `.woff2` files as `FontFile`s, and
  an export ships only that imported copy unless the file matches the
  non-resource filter (below); a font file the filter leaves out is read from
  its imported `FontFile` instead, so both work. A bundle loaded from an
  `http://` dev server loads its fonts from the dev server.

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
imported copies. A shipped pipeline list needs the filter too
(`jsui-pipelines.*.bin`). Sounds need no filter: JsuiView loads their
imported copies (`ResourceLoader`).

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
* Host textures, GPU renderer: one frame of latency (the copy is shown the
  next frame) and one GPU copy per live texture per frame. Only RGBA8 /
  BGRA8 sources take the GPU path. The Vulkan path is exercised on lavapipe
  in CI; Metal and WebGPU have not been run yet. Headless, Godot's dummy
  renderer keeps no image updates and renders no viewports, so there only
  static `ImageTexture`s show (the self-test checks the rest with a
  renderer).
* Audio: `pan` is not applied (an `AudioStreamPlayer` has no pan, and a bus
  per voice costs more than UI sounds are worth). FLAC and Ogg Opus files
  are not supported (Godot has no streams for them), nor ADPCM WAVs given as
  bytes (an imported one plays). A sound's first play loads it
  synchronously when it is a `res://` / `user://` file (preload big ones).
* The macOS library is named `.universal` but a source-built Skia is for the
  host architecture only.
* Tested in Godot on Linux x86_64 (CI: headless, OpenGL and Vulkan via
  lavapipe). Windows is built in CI with the Vulkan and Direct3D 12 GPU
  paths, and the library's Vulkan and D3D12 backends are tested there
  (lavapipe, WARP), but the GDExtension has not been run in Godot on
  Windows yet. D3D12: src stays in `COPY_SOURCE` between frames (as on
  Vulkan); Ganesh's legacy resource barriers assume Godot's tracked state
  for it is that, which should hold in both of Godot's barrier modes
  (legacy states and enhanced barriers) but is unverified.

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

### Threaded web build

The fork's web export runs Forward+ on browser WebGPU (Emdawnwebgpu) in a
threaded build (`variant/thread_support=true`). It has no GDExtension side
modules (`variant/extensions_support=false`, as AW3's preset uses). So on the
web the integration is **linked into the export template**. The fork's
`modules/jsui` registers it at startup, and JsuiView then renders with Skia
Graphite on Godot's `WGPUDevice` (`get_renderer()` is `"webgpu"`). JS runs on
its own pthread (`get_active_js_thread()` is `JS_THREAD_ON`).

Static linking was chosen over a side module for two reasons. The template
needs no dynamic linking (`dlink_enabled`, which costs size and forbids
`proxy_to_pthread`). And one link checks every symbol at build time. Its
problem is that the engine and Skia both bring HarfBuzz, FreeType, ICU,
libpng, zlib, libwebp and libjpeg-turbo, at other versions. JSUI's side is
therefore linked into **one relocatable object whose symbols are all local**,
except the entry point and Emscripten's EM_JS markers
(`tools/wasm-localize`: `wasm-ld -r`, then the object's symbol table is
rewritten; neither `wasm-ld` nor `llvm-objcopy` can localize wasm symbols).
The two sets of libraries then neither clash nor bind to each other.
Undefined references (libc, libc++, Emscripten's runtime, the engine's
`wgpu*`) resolve against the engine.

```sh
scripts/build-godot-web.sh
# -> integrations/godot/demo/bin/libjsui_godot.web.threads.wasm32.a (~31 MB, one object)
```

`scripts/build-godot-web.sh` does the following:

* Sets up emsdk **4.0.20**, the fork's (`misc/web-export/wasm.sh`
  `EMSDK_VERSION`), in `third_party/emsdk-4.0.20`. The library and the engine
  must share one compiler and libc++.
* Builds Skia m157 threaded with `scripts/build-skia-wasm-mt.sh` into
  `third_party/skia-wasm-mt`: Graphite on WebGPU plus CPU raster. It uses the
  same pin and `skia-m157-emdawnwebgpu.patch` as the browser module's build,
  against that emsdk's **own** Emdawnwebgpu port (`--use-port=emdawnwebgpu`,
  Dawn release v20251002.162335 / `01940842b667`), the `webgpu.h` the fork
  compiles its driver against. Everything is compiled with `-pthread`,
  `-sSUPPORT_LONGJMP=wasm` and `-fno-exceptions`, as Godot's web build is:
  wasm-ld refuses objects without atomics in a shared-memory program, the
  longjmp ABIs do not mix, and with wasm longjmp Emscripten links no C++
  throw support. The build takes about 4 minutes on 16 cores.
* Configures JSUI with `-DJSUI_WASM_THREADS=ON -DJSUI_BUILD_GODOT=ON
  -DJSUI_HOST_JS=OFF -DJSUI_EMDAWNWEBGPU_PORT=emdawnwebgpu`. On threaded
  Emscripten:
  * `JsThread` uses pthreads (`JSUI_NO_JS_THREAD` only without
    `__EMSCRIPTEN_PTHREADS__`).
  * `JSUI_GRAPHITE_DAWN` calls `wgpu*` directly. There is no shim, and
    `jsui_gpu_create_dawn_procs` ignores the proc address.
    `jsui_dawn_revision()` is the port's Dawn revision.
  * Graphite gets no tick function (the browser cannot block on the GPU) and
    no internal MSAA. Pipeline precompiles are sliced on the render thread,
    since no other thread touches the device.
  * The non-threaded npm modules (`scripts/build-wasm.sh`) are unchanged.
* Links the archive (`jsui_godot_web` target) and runs a link check
  (`web-link-check.c`). The check is a threaded program with its own
  `hb_*` / `FT_*` / `png_*` / `inflate` symbols that links the library with
  Godot's flags, and its calls must still reach its own copies. CI's
  `godot-web` job runs the whole script.

In the fork (branch `jsui-web-threads`, `modules/jsui/README.md`), the
library is linked into the template with the same emsdk on `PATH`:

```sh
scons platform=web target=template_release webgpu=yes threads=yes \
  module_jsui_enabled=yes jsui_lib=$JSUI/integrations/godot/demo/bin/libjsui_godot.web.threads.wasm32.a
```

The module passes `jsui_godot_init` to
`GDExtensionManager::add_builtin_extension("res://jsui.gdextension", ...)`
(added there). `load_extensions()` loads built-in extensions ahead of the
project's extension list, so the project keeps its `.gdextension` and the
list finds it loaded. `WebGPUInterop.get_dawn_revision()` reports the port's
Dawn revision on the web, which JsuiView checks as on native.

Exporting the demo for the web:

* `project.godot` has `renderer/rendering_method.web="forward_plus"`, so the
  export sets `renderingDriver: "webgpu"`.
* Use a Web preset with `custom_template/*` set to the fork's
  `godot.web.template_release.wasm32.zip`, `variant/thread_support=true`,
  `variant/extensions_support=false` and `threads/emscripten_pool_size=16`.
  JSUI needs one pool thread for its JS thread: the thread starts while the
  main thread waits for it, so it must come from the pool.
* Set the non-resource filter to `ui/dist/*,hud/ui/*,*.bin`.
* Give the bundle's images (`ui/dist/assets/*.png|jpg`) the "Keep File"
  import mode (`importer="keep"` in their `.import`). JSUI reads them raw
  through `FileAccess`, and an imported image is exported only as its
  `.ctex`.
* For `--hud`, copy `integrations/shared/ui/dist/bundle.js` to
  `hud/ui/bundle.js`. An export cannot reach the repository path `--hud`
  uses, and `main.gd` takes the copy when it exists.
* Serve the export with COOP/COEP (`misc/web-export/wasm.sh serve`, or any
  server that sends `Cross-Origin-Opener-Policy: same-origin` and
  `Cross-Origin-Embedder-Policy: require-corp`). Godot's command-line user
  args go in `GODOT_CONFIG.args` after a `"--"`.

What was run (Chrome 155, headed, Apple M1 Ultra, Metal through Dawn):

* The demo shows the showcase over the 3D world at 60 fps, with
  `JsuiView: rendering with Skia Graphite on webgpu (Dawn 01940842b667)`.
* Real browser input works (Playwright mouse and keyboard on the canvas):
  hover, the Ping button (`Host.call` → `resolve_call`, credits update),
  typing plus Enter in the TextInput (a new chip), and wheel scrolling the
  armory list.
* `--self-test`: 49 checks pass, among them hover, click, mapping, gamepad,
  wheel, pan, typing, ping, `call_ui`, hit testing, pass-through, surfaces,
  Lottie, shaders and audio, and `js stall`: "JS thread on: 26 frames during
  a 400 ms JS stall, the longest 17.5 ms". 8 fail, for the reasons in the
  limits below: the 7 host-texture checks, which read the frame back, and
  `audio decode`, which needs the raw sound files in the pack.
* `--hud` runs the shared HUD over the orbiting units, with `webgpu` in its
  panel.
* Frame cost of the UI: the main-thread time of Godot's
  `requestAnimationFrame` callback, at 60 fps, 1200 frames per run, two
  rounds. With `--hud` it is 1.64 / 1.91 ms mean (p90 2.8 / 3.0 ms). With
  `--hud --no-ui` it is 0.87 / 0.91 ms (p90 1.3 / 1.4 ms). So the UI adds
  about 0.9 ms per frame on the main thread; JS runs on its own thread.

Limits of the web build:

* `get_frame_image()` returns the CPU raster of the frame: browser WebGPU
  cannot read a texture back synchronously, and the fork's
  `texture_get_data` returns nothing there. Host textures (GPU-only) are
  missing from that image. Whether they draw on the web GPU path has not
  been checked.
* There is no pipeline cache file. Pipelines compile on first use, and the
  shipped `jsui-pipelines.webgpu.bin` (native Dawn) is ignored with one
  warning.
* Dev mode and `http://` bundles have not been tried. The browser loader's
  `fetch()` glue would run on the JS thread's worker.
* Only `template_release` without Mono has been built. AW3's Mono template
  needs the same `module_jsui_enabled` / `jsui_lib` flags in
  `TEMPLATE_FLAGS`.
