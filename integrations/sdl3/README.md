# JSUI in SDL3

[main.cpp](main.cpp) runs the shared engine-example HUD
([../shared/ui/app.jsx](../shared/ui/app.jsx)) over a 2D arena drawn with
SDL3's `SDL_Renderer` (SDL 3.4.18): glowing units on orbits over a grid, a
pan / zoom camera, and a nameplate per unit.

```sh
npx jsui build integrations/shared/ui/app.jsx -o integrations/shared/ui/dist/bundle.js
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DJSUI_BUILD_SDL3=ON
cmake --build build --target jsui-sdl3
./build/jsui-sdl3              # --renderer metal|direct3d12|vulkan|opengl|software, --dev <url>, --selftest
```

* **Rendering.** SDL picks the renderer backend (Metal on macOS, Direct3D on
  Windows, Vulkan or OpenGL on Linux) and the UI does not depend on it: the
  runtime rasterises into a buffer kept between frames
  (`jsui_render_rgba_incremental`), only the damaged rectangle is uploaded to a
  streaming `SDL_PIXELFORMAT_RGBA32` texture, and SDL composites it with
  `SDL_BLENDMODE_BLEND_PREMULTIPLIED`. Nothing is painted or uploaded while the
  UI is idle. The scene and the UI are drawn in logical px
  (`SDL_SetRenderScale` by the pixel density); the UI texture has the output's
  pixel size, so it is sharp on HiDPI screens. (For the GPU path on an SDL GL
  window see [tools/jsui-viewer](../../tools/jsui-viewer).)
* **Input.** SDL events: mouse, wheel, touch, keys (scancode -> DOM `code`,
  keycode -> DOM `key`), text input, and gamepads in the W3C standard layout
  with analog triggers as buttons 6 / 7. Unconsumed: left drag pans, wheel
  zooms, Space spawns, Esc / Start pauses, the right stick pans.
* **Per-frame data.** Each unit's screen position into the `entities` buffer.
* **Cursor and clipboard.** `jsui_get_cursor()` -> SDL system cursors;
  `SDL_GetClipboardText` / `SDL_SetClipboardText`.
