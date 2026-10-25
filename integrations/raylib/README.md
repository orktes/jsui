# JSUI in raylib

[main.cpp](main.cpp) runs the shared engine-example HUD
([../shared/ui/app.jsx](../shared/ui/app.jsx)) over a raylib 6.2 3D scene: units
orbiting a pillar, an orbit camera, and a nameplate per unit.

```sh
npx jsui build integrations/shared/ui/app.jsx -o integrations/shared/ui/dist/bundle.js
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DJSUI_BUILD_RAYLIB=ON
cmake --build build --target jsui-raylib
./build/jsui-raylib            # --cpu, --dev <url>, --selftest, --frames N --screenshot out.png, --js-thread off
```

* **Rendering.** `jsui_gpu_create_gl(rlGetProcAddress)` makes a Skia Ganesh
  context on raylib's own GL 3.3 context. When `jsui_needs_redraw()` says the
  UI changed, Skia draws it into the texture of a `RenderTexture2D`
  (`JsuiGLTarget.texture`), after `rlDrawRenderBatchActive()`. The texture
  keeps the last frame, so with `JSUI_GL_INCREMENTAL` only what changed is
  repainted into it (no full-frame copy); then
  `restoreRaylibState()` puts back the GL state raylib assumes. Each frame raylib
  draws that texture over the scene with `BLEND_ALPHA_PREMULTIPLY` (flipped:
  GL textures are bottom-up). An idle UI costs one textured quad.
  `--cpu`: `jsui_render_rgba_incremental` into a kept buffer, and only the
  dirty rectangle goes through `UpdateTextureRec`.
* **Input.** raylib is polled, so each frame's state is turned into events:
  mouse moves, button presses and releases (`IsMouseButtonPressed`), the wheel,
  keys from `GetKeyPressed()` (plus repeats and releases of held keys), text
  from `GetCharPressed()`, and gamepads 0-3 mapped to the W3C standard layout.
  raylib's key codes are GLFW's; [../shared/glfw_keys.h](../shared/glfw_keys.h)
  maps them to DOM `key` / `code`. Unconsumed: left drag orbits, wheel zooms,
  Space spawns, Esc / Start pauses, the right stick orbits.
  `SetExitKey(KEY_NULL)`: Esc belongs to the UI.
* **Per-frame data.** `GetWorldToScreen` of each unit into the `entities`
  buffer (logical px; raylib's screen coordinates are logical with
  `FLAG_WINDOW_HIGHDPI`).
* **Cursor and clipboard.** `jsui_get_cursor()` -> `SetMouseCursor`;
  `GetClipboardText` / `SetClipboardText` for TextInput copy and paste.
