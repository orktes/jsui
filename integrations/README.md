# Integrations

| Host | What it is |
| --- | --- |
| [godot](godot/README.md) | GDExtension with a `JsuiView` Control, rendering on Godot's own Vulkan / Metal device |
| [android](android/README.md) | NDK NativeActivity app, Skia on OpenGL ES (EGL), touch, keys and gamepads |
| [ios](ios/README.md) | UIKit app, Skia on Metal, touch, keyboards and game controllers |
| [raylib](raylib/README.md) | raylib 6.2 game loop; Skia draws the UI into a `RenderTexture2D` on raylib's GL context |
| [sdl3](sdl3/README.md) | SDL3 game loop on `SDL_Renderer`; Skia into SDL's own texture on Metal, Vulkan or GL (CPU dirty-rect upload elsewhere) |
| [sokol](sokol/README.md) | sokol_app + sokol_gfx (GL backend); Skia draws the UI straight into the swapchain framebuffer |
| [unity](unity/README.md) | Unity 6 package (`com.orktes.jsui`): native plugin over the C API + a `JsuiView` component, with the same example game |
| [unreal](unreal/README.md) | Unreal Engine 5 plugin: `UJsuiWidget` (UMG) / `SJsuiView` (Slate) over the JSUI shared library, Skia Graphite on the Metal RHI's queue, with the same example game |

Programs in other languages use the C API through [bindings](../bindings/README.md)
(Rust and Go so far, each with a GPU desktop example app).

## The engine examples

`raylib`, `sdl3` and `sokol` are small, complete hosts of one shared HUD,
[shared/ui/app.jsx](shared/ui/app.jsx), each in a few hundred lines of C++ over
[the C API](../include/jsui/jsui.h). They show the three things every game
needs from its UI layer:

* **Input pass-through.** Every event goes to the UI first. `jsui_send_*`
  returns whether the UI consumed it (a handler, a responder, a focused
  element), not merely whether the point was over painted UI. What it did not
  consume drives the game: click a unit to select it, drag empty space to
  orbit or pan, wheel to zoom, Space spawns a unit, Esc / Start pauses. The
  counters in the HUD's panel show what reached the game.
* **Selection.** A left press the UI passed on and released within 4 px is a
  click: the game picks the unit under it with its own raycast (raylib
  `GetRayCollisionBox`, sokol `jsui_example::rayBox` in
  [shared/example_host.h](shared/example_host.h), a distance test in SDL3's
  2D arena; Unity and Unreal their own rays) and highlights it, or clears the
  selection on empty ground. It tells the UI with `game.selected` (an index
  into `entities`, -1 for none); the HUD styles that unit's nameplate, shows
  its name in the panel and answers with `ui:selected {index}`.
* **Per-frame data.** Each frame the game projects its units to the screen and
  writes `x, y, visible, hue` per unit into the `entities` frame buffer
  (`jsui_map_buffer` / `jsui_commit_buffer`). The nameplates follow them with
  `useHostBufferValue`, moved by the runtime with no JS per frame. Slower data
  (fps, unit count, paused, selected) goes to the `game` shared state (`jsui_set_state`),
  and the HUD's buttons answer with `Host.postMessage('game:spawn' | 'game:clear'
  | 'game:pause' | 'game:quit')`.
* **JS on its own thread**, as in React Native (`cfg.js_thread =
  JSUI_JS_THREAD_ON`, the examples' default): the game's frames never wait
  for the HUD's JS. The nameplates (a host buffer), hover styles, scrolling
  and hit testing run natively every frame; what JS renders lands a frame or
  more later. `--js-thread off` runs JS inside `jsui_tick` instead.
* **Getting pixels on screen**, a different way in each:

  | Example | Default | `--cpu` |
  | --- | --- | --- |
  | raylib | Skia Ganesh (GL) into the texture of a `RenderTexture2D`, only when the UI changed; raylib composites it with `BLEND_ALPHA_PREMULTIPLY` | `jsui_render_rgba_incremental`, the dirty rectangle through `UpdateTextureRec` |
  | sdl3 | Skia into the `SDL_Texture` SDL composites (`SDL_BLENDMODE_BLEND_PREMULTIPLIED`), on SDL's renderer API: Graphite on Metal (IOSurface textures) or Vulkan (our `VkImage`), Ganesh on OpenGL, after `SDL_FlushRenderer`; CPU upload on other renderers | the dirty rectangle into a streaming `SDL_Texture` |
  | sokol | Skia Ganesh over the sokol_gfx pass in the swapchain's GL framebuffer (`JSUI_GL_NO_CLEAR`), then `sg_reset_state_cache()` | the kept CPU buffer into a write-transient `sg_image`, drawn with a premultiplied sokol_gl quad |

### Build and run

```sh
npm install
npx jsui build integrations/shared/ui/app.jsx -o integrations/shared/ui/dist/bundle.js
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DJSUI_BUILD_RAYLIB=ON -DJSUI_BUILD_SDL3=ON -DJSUI_BUILD_SOKOL=ON
cmake --build build --target jsui-raylib jsui-sdl3 jsui-sokol
./build/jsui-raylib
./build/jsui-sdl3
./build/jsui-sokol
```

The engines are downloaded at configure time (pinned versions, SHA-256
checked) and linked statically; `-DJSUI_RAYLIB_DIR=`, `-DJSUI_SDL3_DIR=` and
`-DJSUI_SOKOL_DIR=` use a local source tree instead. On Linux they need the X11
and GL development packages (`libx11-dev libxrandr-dev libxinerama-dev
libxcursor-dev libxi-dev libgl1-mesa-dev`).

Every example takes the same options:

| Option | |
| --- | --- |
| `bundle.js` or a URL | the UI to run (default: the bundle built above) |
| `--dev` | poll the dev server and hot-update / reload on change: `npx jsui dev integrations/shared/ui/app.jsx`, then `./build/jsui-raylib --dev http://127.0.0.1:5173/bundle.js` |
| `--cpu` | the CPU raster path instead of the GPU one |
| `--width W --height H` | window size in logical px (default 1280 x 720) |
| `--frames N --screenshot out.png` | run N frames, save the composed frame (scene + UI), exit |
| `--selftest` | scripted check of the pass-through contract (below), then exit |
| `--js-thread on\|off\|lockstep` | where the HUD's JS runs (default `on`: its own thread; `off`: inside `jsui_tick`; `lockstep`: its own thread, waited for every tick); printed at start as `JS thread: ...` |
| `--no-ui` | the same game without JSUI (no runtime): the baseline for measuring the UI's cost |
| `--bench S [--bench-warmup S]` | vsync off, time S seconds of frames after a warm-up (default 3 s), print `bench: ... mean X ms (Y fps) p50 p90 p99 max`, exit. sokol on macOS times each frame's work (CPU + `glFinish`), as the OS paces its GL view at the display refresh |

`--selftest` waits for the HUD to report where its Spawn button is, then
clicks through the host's own mouse handling (the code real events run): a
click on empty space must reach the game (and `jsui_hit_test` must report
nothing there), a click on Spawn must be consumed and its `game:spawn` message
must add a unit, a click on a unit must select it, one on empty ground must
clear the selection, and another must select again, each time answered by the
HUD's `ui:selected`. Before the clicks it stalls JS for 300 ms
(`jsui_debug_stall_js`): with the JS thread the game must keep drawing (no
frame longer than half the stall, `selftest: JS thread on: N game frames
during a 300 ms JS stall`) and `jsui_sync` must finish after it; with
`--js-thread off` the stall must hold the frame. It prints `selftest: ok` and exits 0. CI runs it for each example under Xvfb, on Mesa's
GL and on the CPU path (the `engines` job in `.github/workflows/ci.yml`).

### Using one in your engine

The pattern is the same in any loop:

```c
JsuiConfig cfg;
jsui_config_init(&cfg);
cfg.width = window_w; cfg.height = window_h; cfg.pixel_ratio = dpi_scale;
cfg.on_message = on_ui_message;                 /* Host.postMessage(name, data) */
cfg.js_thread = JSUI_JS_THREAD_ON;              /* JS on its own thread: frames never wait for it */
JsuiRuntime* rt = jsui_create(&cfg);
jsui_load_bundle_url(rt, "ui/bundle.js");

for (;;) {
    for each input event:
        if (!jsui_send_mouse_button(rt, x, y, button, down, mods))
            game_handle_click(x, y);            /* the UI did not take it */
    jsui_set_state(rt, "game", game_json);      /* no-op when unchanged */
    float* b = jsui_map_buffer(rt, "entities", JSUI_BUFFER_F32, n * 4);
    write_unit_screen_positions(b);
    jsui_commit_buffer(rt, "entities");
    jsui_tick(rt, now_ms);
    if (jsui_needs_redraw(rt)) paint_ui();      /* GL target or CPU buffer */
    draw_scene(); composite_ui(); present();
}
```

With the JS thread, what the game sets (state, buffers, messages) reaches JS
at the next tick and what JS does with it shows a tick or more later. When a
change must show on this frame (a scene cut, a screenshot) tick with
`jsui_tick_ex(rt, now_ms, JSUI_TICK_SYNC_JS)`. A loop that sleeps while idle
sets `cfg.on_js_ready` (called on the JS thread) to wake itself; one that
ticks every vsync needs nothing.

GL hosts: Skia leaves its own GL state behind (blend, scissor, bound
framebuffer / program / vertex array, pixel store). Flush the engine's batch
before `jsui_render_gl*` and re-apply what the engine caches afterwards
(raylib: see `restoreRaylibState` in raylib/main.cpp; sokol:
`sg_reset_state_cache()`).

[unity](unity/README.md) is the same game as a Unity project, on a Unity
package whose native plugin exports the C API to C# (P/Invoke) and whose
`JsuiView` component does the forwarding, ticking and compositing above.
[unreal](unreal/README.md) is the same game as an Unreal Engine 5 C++
project, on a plugin that calls the C API of a prebuilt JSUI shared library
and whose `UJsuiWidget` does the forwarding (an input preprocessor), ticking
and compositing. Bevy would be a plugin along the same lines; it is not in the
tree yet.
