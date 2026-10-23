# Embedding JSUI in an engine

The UI is one JS bundle (`npx jsui build app.jsx -o ui/dist/bundle.js`)
plus its `assets/` folder. Hosts load it, tick it once per frame, forward
input and draw the result.

## Godot 4 (GDExtension `JsuiView`)

`JsuiView` is a `Control` (Godot 4.3+). On Forward+ / Mobile Skia draws on the
GPU into a Godot texture (Vulkan / Metal); on Compatibility and headless it
uses the CPU raster.

```gdscript
@onready var ui: JsuiView = $UI    # bundle_url = "res://ui/dist/bundle.js"

func _ready():
    ui.set_state("player", {"gold": 1250, "hp": 80})          # before or after start
    ui.message_received.connect(func(name, data, _json): if name == "audio.play": play(data.cue))
    ui.call_requested.connect(_on_ui_call)
    ui.grab_focus()                                           # keyboard + gamepad go to the focused Control

func _process(_delta):
    ui.set_buffer("enemies", positions)                       # PackedVector2Array / PackedFloat32Array ...

func _on_ui_call(id, name, args):
    match name:
        "shop.buy": ui.resolve_call(id, {"gold": gold}) if try_buy(args.item) else ui.reject_call(id, "not enough gold")
        _: ui.reject_call(id, "unknown call " + name)
```

* Properties: `bundle_url` (`res://`, `user://`, path or `http://`),
  `dev_mode`, `pixel_ratio` (0 follows the window scale), `autostart`,
  `component` + `initial_props` (AppRegistry key and props), `runtime_view`
  (show another surface of a view's runtime), `auto_size`,
  `input_pass_through` (default on), `force_cpu`, `profiling`, `fixed_fps`.
* Methods: `post_message`, `set_state` / `patch_state` / `get_state`,
  `set_buffer` / `remove_buffer`, `set_texture(name, texture, live)` /
  `remove_texture` / `update_texture`, `resolve_call` / `reject_call`,
  `call_ui(name, args, callback)`, `reload`, `hit_test(pos)`,
  `get_frame_image()`, `get_perf()`.
* Signals: `message_received`, `state_changed`, `call_requested`,
  `call_cancelled`, `ui_call_completed`, `js_error`, `frame_rendered`.
* Unhandled input continues to `_unhandled_input`.
* Export: keep the bundle in a subfolder and add it to the export preset's
  non-resource filter (`ui/dist/*`).
* Dev: `npx jsui dev ui/app.jsx` and set `bundle_url =
  "http://127.0.0.1:5173/bundle.js"`, `dev_mode = true` (live reload,
  on-screen error overlay; F5 reloads in the demo).

## Web (`@orktes/jsui-web`)

```js
import { createJsui } from '@orktes/jsui-web';

const jsui = await createJsui({ canvas, bundleUrl: './bundle.js' });   // WebGPU, else WebGL, else CPU
jsui.on('message', (name, data) => {});
jsui.setState('gold', 1250);
jsui.handle('shop.buy', async ({ item }) => ({ gold: 1100 }));          // answers Host.call
await jsui.callUi('ui.flash', { text: 'Saved' });                       // Host.handle in the UI
jsui.setBuffer('enemies', new Float32Array(positions));
jsui.bindVideo('trailer', videoElement);                                // <HostTexture name="trailer" />
const hud = await createJsui({ canvas: hudCanvas, bundleUrl: './hud.js', passThrough: true });
const w = jsui.createSurface(el, { component: 'Clock', initialProps: {}, autoSize: true });
```

Serve `node_modules/@orktes/jsui-web/` (the `dist*/` WASM folders) as static
files; `.wasm` as `application/wasm`. `?dev=1` on the package's demo page
live-reloads from `jsui dev`.

## C / C++ (`include/jsui/jsui.h`)

```c
JsuiConfig cfg; jsui_config_init(&cfg);
cfg.width = 1280; cfg.height = 720; cfg.pixel_ratio = 2;
cfg.on_message = on_message; cfg.on_call = on_call;  cfg.on_state = on_state;
JsuiRuntime* rt = jsui_create(&cfg);
jsui_load_bundle_url(rt, "ui/dist/bundle.js");       // or jsui_load_bundle_source

// every frame
jsui_send_mouse_move(rt, x, y);                       // input: returns 1 if the UI consumed it
jsui_send_gamepad_button(rt, 0, 0 /*A*/, 1, 1.0f);
jsui_tick(rt, now_ms);
if (jsui_needs_redraw(rt))
  jsui_render_gpu_target(rt, gpu, &target, &dirty);   // or jsui_render_gl / _vulkan / _metal / _webgpu,
                                                      // or jsui_render_rgba_incremental into a CPU buffer
```

Data: `jsui_set_state` / `jsui_patch_state` / `jsui_get_state`,
`jsui_set_buffer` / `jsui_map_buffer` + `jsui_commit_buffer`,
`jsui_post_message`, `jsui_resolve_call` / `jsui_reject_call`,
`jsui_call_ui`, `jsui_set_texture` / `jsui_texture_updated`. Surfaces:
`jsui_surface_create(rt, "Hud", props_json, w, h, scale)`. Fonts:
`jsui_register_font`. Errors: `jsui_last_error`. `cfg.dev_mode = 1` polls the
dev server and shows an error overlay.

Native tools in the repo: `jsui-viewer bundle.js` (window, `--dev URL`),
`jsui-render bundle.js -o frame.png` and `jsui-record` (scripted video). For
quick checks prefer `npx jsui screenshot` / `run`, which need no native build.
