// Content of the Embed page, per target. Function names, signatures,
// properties and flags are taken from include/jsui/jsui.h, web/jsui-web.js,
// integrations/godot and packages/cli.
import { colors } from '../../theme.js';

const GODOT = {
  id: 'godot',
  label: 'Godot 4',
  eyebrow: 'Godot 4 · GDExtension',
  color: colors.cyan,
  title: 'A JsuiView node',
  lead: 'JsuiView is a Control that runs a bundle, ticks it every frame and draws it as a premultiplied texture. Input reaches it through Godot’s GUI pipeline; anything the UI does not consume falls through to _unhandled_input.',
  facts: [
    ['Class', 'JsuiView (Control)'],
    ['Godot', '4.3 and later'],
    ['Library', 'libjsui_godot'],
    ['Bundles', 'res:// user:// http://'],
  ],
  blocks: [
    {
      kind: 'code',
      lang: 'sh',
      title: 'build the extension and the demo UI',
      code: `
# From the repository root (godot-cpp is fetched at godot-4.3-stable)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DJSUI_BUILD_GODOT=ON
cmake --build build --target jsui_godot
# -> integrations/godot/demo/bin/libjsui_godot.linux.x86_64.so

npx jsui build integrations/godot/demo/ui/app.jsx -o integrations/godot/demo/ui/dist/bundle.js
godot --path integrations/godot/demo
`,
    },
    {
      kind: 'pair',
      items: [
        {
          kind: 'code',
          lang: 'gd',
          title: 'main.gd',
          code: `
extends Control

@onready var ui = $JsuiView   # bundle_url = "res://ui/dist/bundle.js"

func _ready() -> void:
    ui.message_received.connect(_on_ui_message)
    ui.js_error.connect(func(msg): printerr("[jsui] ", msg))
    ui.grab_focus()   # keyboard + gamepad go to the UI

func _on_ui_message(name: String, data, json: String) -> void:
    match name:
        "ui:ping":
            ui.post_message("godot:pong", {"count": data.count})

func _on_player_hp_changed(hp: int) -> void:
    ui.post_message("hud", {"hp": hp})
`,
        },
        {
          kind: 'code',
          lang: 'js',
          title: 'ui/app.jsx',
          code: `
import { useState } from 'react';
import { Host, Pressable, Text, useHostMessage } from 'react-native';

function Hud() {
  const [hud, setHud] = useState({ hp: 100 });
  useHostMessage('hud', setHud);   // ui.post_message("hud", ...)
  return (
    <Pressable
      onPress={() => Host.postMessage('ui:ping', { count: 1 })}
    >
      <Text>HP {hud.hp}</Text>
    </Pressable>
  );
}
`,
        },
      ],
    },
    {
      kind: 'table',
      title: 'Properties',
      head: ['PROPERTY', 'TYPE', 'WHAT IT DOES'],
      rows: [
        ['bundle_url', 'String', 'res://…, user://…, an absolute path or http://… (dev server). Changing it restarts the view.'],
        ['dev_mode', 'bool', 'Poll the dev server, reload on change, show the error overlay.'],
        ['pixel_ratio', 'float', '0 follows the canvas → screen scale (stretch mode, Camera2D zoom, parent scale).'],
        ['autostart', 'bool', 'Start in _ready (default true); otherwise call start().'],
        ['forward_gamepad', 'bool', 'Forward joypad events received while the view has focus.'],
      ],
    },
    {
      kind: 'pair',
      items: [
        {
          kind: 'table',
          title: 'Methods',
          rows: [
            ['start() / stop()', 'Create the runtime and load bundle_url / destroy it.'],
            ['reload()', 'Re-evaluate the bundle in the same runtime.'],
            ['post_message(name, data)', 'Send to JS; data is JSON-encoded.'],
            ['post_message_json(name, json)', 'Same with a JSON string.'],
            ['get_texture() / get_frame_image()', 'The drawn ImageTexture / a CPU copy of the last frame.'],
            ['get_last_error()', 'Last uncaught JS error, or "".'],
          ],
        },
        {
          kind: 'table',
          title: 'Signals',
          rows: [
            ['message_received(name, data, json)', 'JS called Host.postMessage(name, data).'],
            ['js_error(message)', 'A new uncaught JS error.'],
            ['frame_rendered(dirty_rect)', 'The texture was updated (device px).'],
          ],
        },
      ],
    },
    {
      kind: 'note',
      color: colors.cyan,
      text: 'Exporting: res:// bundles are read through FileAccess, so they work from .pck files. Add the bundle folder (e.g. ui/dist/*) to the export preset’s non-resource filter so the raw .js and image files ship.',
    },
  ],
};

const CAPI = {
  id: 'c',
  label: 'C API',
  eyebrow: 'C API · include/jsui/jsui.h',
  color: colors.gold,
  title: 'Any engine, one header',
  lead: 'The embedder owns the loop: forward input, tick, and paint when something changed. Paint into an RGBA buffer you upload as a texture, or straight into a GL framebuffer with the optional GPU backend.',
  facts: [
    ['Language', 'C ABI (C++20 inside)'],
    ['Linking', 'one static library'],
    ['Threads', 'call from the creating thread'],
    ['Units', 'logical px × pixel_ratio'],
  ],
  blocks: [
    {
      kind: 'code',
      lang: 'c',
      title: 'game.c',
      code: `
#include <jsui/jsui.h>

static void on_message(void* user, const char* name, const char* json) {
    game_on_ui_message(name, json);            /* Host.postMessage(name, data) */
}

JsuiConfig cfg;
jsui_config_init(&cfg);
cfg.width = 1280; cfg.height = 720;            /* logical px */
cfg.pixel_ratio = 2.0f;
cfg.on_message = on_message;
JsuiRuntime* rt = jsui_create(&cfg);
jsui_load_bundle_url(rt, "ui/bundle.js");      /* or jsui_load_bundle_source() */

while (running) {
    forward_input(rt);                         /* jsui_send_mouse_move, _key, _gamepad_button... */
    jsui_tick(rt, now_ms());                   /* timers, JS, animations, layout */
    if (jsui_needs_redraw(rt)) {
        jsui_render_rgba(rt, pixels, w, h, w * 4);   /* premultiplied RGBA8888 */
        upload_texture(pixels);
    }
    if (hp_changed)
        jsui_post_message(rt, "hud", "{\\"hp\\":42}");  /* useHostMessage('hud', ...) */
}
jsui_destroy(rt);
`,
    },
    {
      kind: 'pair',
      items: [
        {
          kind: 'code',
          lang: 'c',
          title: 'GPU backend (OpenGL / GLES / WebGL)',
          code: `
/* once, with the game's GL context current */
JsuiGpuContext* gpu = jsui_gpu_create_gl(NULL, NULL);

/* per frame */
jsui_tick(rt, now_ms);
jsui_render_gl(rt, gpu, 0 /* default FBO */, fb_w, fb_h,
               JSUI_GL_NO_CLEAR);    /* HUD over the scene */
/* re-apply the GL state your engine caches */

jsui_gpu_destroy(gpu);
`,
        },
        {
          kind: 'code',
          lang: 'c',
          title: 'incremental CPU paint',
          code: `
/* Keep the buffer between frames: only damaged
   pixels are repainted, and you upload just that. */
JsuiRect dirty;
if (jsui_render_rgba_incremental(rt, pixels, w, h,
                                 w * 4, &dirty))
    upload_region(pixels, dirty.x, dirty.y,
                  dirty.width, dirty.height);

/* Sleep while idle: tick at display rate only
   while something moves on its own. */
if (!jsui_wants_frame(rt)) wait_for_input();
`,
        },
      ],
    },
    {
      kind: 'groups',
      title: 'The whole API',
      items: [
        ['Lifecycle', ['jsui_config_init', 'jsui_create', 'jsui_destroy', 'jsui_last_error']],
        ['Content', ['jsui_load_bundle_url', 'jsui_load_bundle_source', 'jsui_reload', 'jsui_register_font']],
        ['Frame loop', ['jsui_set_viewport', 'jsui_tick', 'jsui_tick_native', 'jsui_needs_redraw', 'jsui_wants_frame', 'jsui_pending_loads']],
        ['Paint', ['jsui_render_rgba', 'jsui_render_rgba_incremental', 'jsui_render_gl', 'jsui_render_gl_target', 'jsui_render_png', 'jsui_gpu_create_gl']],
        ['Input', ['jsui_send_mouse_move', 'jsui_send_mouse_button', 'jsui_send_mouse_wheel', 'jsui_send_mouse_leave', 'jsui_send_touch', 'jsui_send_key', 'jsui_send_text', 'jsui_send_gamepad_button', 'jsui_send_gamepad_axis', 'jsui_set_gamepad_connected', 'jsui_get_cursor']],
        ['Data & resources', ['jsui_post_message', 'cfg.on_message', 'cfg.load', 'jsui_resource_complete', 'jsui_set_clipboard_callbacks']],
      ],
    },
    {
      kind: 'note',
      color: colors.gold,
      text: 'Every byte the runtime reads (bundle, fetch(), images, fonts) goes through a loader. Set cfg.load to serve them from your pak files or virtual file system and answer with jsui_resource_complete(rt, id, status, content_type, data, size).',
    },
  ],
};

const WEB = {
  id: 'web',
  label: 'Web',
  eyebrow: 'Browser · web/jsui-web.js',
  color: colors.purple,
  title: 'A canvas and one import',
  lead: 'The WebAssembly build runs the same runtime on a <canvas>. createJsui() forwards DOM input and the Gamepad API, ticks on requestAnimationFrame and blits frames with WebGL, or draws with Skia on WebGL directly. This website is exactly that.',
  facts: [
    ['Module', 'web/dist/jsui.{js,wasm}'],
    ['GPU module', 'web/dist-webgl/'],
    ['Renderers', "'auto' 'gpu' 'webgl' '2d'"],
    ['Instances', 'one module, many runtimes'],
  ],
  blocks: [
    {
      kind: 'code',
      lang: 'js',
      title: 'index.html',
      code: `
<canvas id="ui" style="width: 100vw; height: 100vh"></canvas>
<script type="module">
  import { createJsui } from './jsui-web.js';

  const jsui = await createJsui({
    canvas: document.getElementById('ui'),
    bundleUrl: './bundle.js',
    renderer: 'auto',                 // 'gpu' + moduleUrl: './dist-webgl/jsui.js'
  });

  jsui.on('message', (name, data) => {  // Host.postMessage(name, data)
    if (name === 'buy') shop.buy(data.item);
  });
  jsui.on('error', (message) => console.error(message));
  jsui.postMessage('hud', { hp: 42 });  // useHostMessage('hud', ...)
</script>
`,
    },
    {
      kind: 'pair',
      items: [
        {
          kind: 'table',
          title: 'Options',
          rows: [
            ['canvas', 'Required. Sized by CSS; the backing store follows devicePixelRatio.'],
            ['bundleUrl / bundleSource', 'Load a bundle by URL, or pass its source.'],
            ['devMode / devPoll', 'Error overlay + live reload (runtime polls http:// origins).'],
            ['pixelRatio', 'Fixed ratio; default tracks window.devicePixelRatio.'],
            ['moduleUrl', 'Default ./dist/jsui.js next to jsui-web.js.'],
            ['renderer', "'auto' | 'gpu' | 'webgl' | '2d'."],
            ['autoFocus · gamepads', 'Focus the keyboard sink · poll the Gamepad API (both default true).'],
            ['autoStart · trapTab', 'Start the frame loop · keep Tab inside the UI (both default true).'],
          ],
        },
        {
          kind: 'table',
          title: 'Instance',
          rows: [
            ["on(type, cb) / off()", "'message' (name, data), 'error', 'log', 'frame'. on() returns an unsubscribe."],
            ['postMessage(name, data)', 'Send JSON-able data to the UI.'],
            ['loadUrl() / loadSource()', 'Swap the bundle.'],
            ['reload()', 'Reload the current bundle.'],
            ['registerFont(family, bytes)', 'Register a font from an ArrayBuffer.'],
            ['whenIdle()', 'Resolves once loads are done and the frame is painted.'],
            ['start() / stop() / frame()', 'Drive the loop yourself if you need to.'],
            ['readPixels() / destroy()', 'Read the last frame / free everything.'],
          ],
        },
      ],
    },
  ],
};

const DEV = {
  id: 'dev',
  label: 'Dev mode',
  eyebrow: 'Dev mode · live reload',
  color: colors.green,
  title: 'Save, and every host reloads',
  lead: 'jsui dev rebuilds on every change and serves the bundle over HTTP. Any host in dev mode polls the server’s version endpoint and reloads when it changes, keeping the last good frame on screen and showing errors in an overlay.',
  facts: [
    ['Server', '127.0.0.1:5173'],
    ['Bundle', '/bundle.js'],
    ['Version', '/__jsui/version'],
    ['Build errors', '/__jsui/error'],
  ],
  blocks: [
    {
      kind: 'code',
      lang: 'sh',
      title: 'terminal',
      code: `
npx jsui dev app.jsx --port 5173

# then point any host at it
jsui-viewer --dev "http://127.0.0.1:5173/bundle.js"
godot --path integrations/godot/demo -- --dev "http://127.0.0.1:5173/bundle.js"
`,
    },
    {
      kind: 'pair',
      items: [
        {
          kind: 'code',
          lang: 'c',
          title: 'your engine',
          code: `
JsuiConfig cfg;
jsui_config_init(&cfg);
cfg.dev_mode = 1;   /* poll + reload + error overlay */
JsuiRuntime* rt = jsui_create(&cfg);
jsui_load_bundle_url(rt, "http://127.0.0.1:5173/bundle.js");
`,
        },
        {
          kind: 'code',
          lang: 'js',
          title: 'a web page',
          code: `
const jsui = await createJsui({
  canvas,
  bundleUrl: 'http://127.0.0.1:5173/bundle.js',
  devMode: true,
});
`,
        },
      ],
    },
    {
      kind: 'steps',
      items: [
        ['Poll', 'The runtime GETs /__jsui/version about every 500 ms (2 s while the server is down). The body is an opaque token.'],
        ['Reload', 'A new token tears down the JS context, document and image cache, then loads the bundle again.'],
        ['Keep the frame', 'The last good frame stays on screen until the new bundle renders, and keeps showing if it throws.'],
        ['Overlay', 'Uncaught errors are painted over the frame with the stack. Escape dismisses; the next good load clears it.'],
      ],
    },
  ],
};

const BUILD = {
  id: 'build',
  label: 'Build',
  eyebrow: 'Production · @jsui/cli',
  color: colors.orange,
  title: 'One file for QuickJS',
  lead: 'jsui build bundles your app with esbuild into a single IIFE script that QuickJS runs as is, with react-native aliased to @jsui/react and every asset emitted next to it.',
  blocks: [
    {
      kind: 'code',
      lang: 'sh',
      title: 'terminal',
      code: `
npm install
npx jsui build app.jsx -o dist/bundle.js --minify
#   dist/bundle.js         one IIFE script (ES2020, UTF-8)
#   dist/assets/*          images and fonts: <name>-<hash>.<ext>

npx jsui build app.jsx -o dist/bundle.js --sourcemap --dev   # NODE_ENV=development
`,
    },
    {
      kind: 'pair',
      items: [
        {
          kind: 'table',
          title: 'What the bundle contains',
          rows: [
            ['react-native', 'Aliased to @jsui/react, deep paths included.'],
            ['react', 'The copy @jsui/react was built against: one React per bundle.'],
            ['JSX / TypeScript', 'Automatic JSX runtime; types stripped.'],
            ['.jsui.js / .native.js', 'Platform extensions resolve first.'],
            ['require(png | ttf | …)', 'Emitted to assets/ and evaluated to a URL relative to the bundle; images register their size.'],
            ['Shims', 'safe-area-context, expo-status-bar, linear-gradient packages.'],
          ],
        },
        {
          kind: 'code',
          lang: 'js',
          title: 'build.mjs',
          code: `
import { build } from '@jsui/cli';

await build({
  entry: 'app.jsx',
  outfile: 'dist/bundle.js',
  minify: true,
});
`,
        },
      ],
    },
    {
      kind: 'code',
      lang: 'sh',
      title: 'the runtime itself',
      code: `
cmake -S . -B build -G Ninja && cmake --build build   # static lib, jsui-viewer, jsui-render
scripts/build-wasm.sh                                 # -> web/dist/jsui.{js,wasm}
WEBGL=1 scripts/build-wasm.sh                         # -> web/dist-webgl/ (Skia on WebGL)
`,
    },
  ],
};

export const TARGETS = [GODOT, CAPI, WEB, DEV, BUILD];
