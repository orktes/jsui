# JSUI in SDL3

[main.cpp](main.cpp) runs the shared engine-example HUD
([../shared/ui/app.jsx](../shared/ui/app.jsx)) over a 2D arena drawn with
SDL3's `SDL_Renderer` (SDL 3.4.18): glowing units on orbits over a grid, a
pan / zoom camera, and a nameplate per unit.

```sh
npx jsui build integrations/shared/ui/app.jsx -o integrations/shared/ui/dist/bundle.js
# JSUI_SKIA_DIR: a Skia with Graphite (scripts/build-skia-desktop.sh) for the
# Metal / Vulkan paths; the default prebuilt Skia has the OpenGL one.
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DJSUI_BUILD_SDL3=ON \
      -DJSUI_SKIA_DIR=third_party/skia-desktop
cmake --build build --target jsui-sdl3
./build/jsui-sdl3              # --cpu, --renderer metal|vulkan|opengl|direct3d12|software, --dev <url>, --selftest
```

* **Rendering** ([gpu.h](gpu.h)). Skia draws the UI on the GPU straight into
  the `SDL_Texture` that SDL composites over the arena
  (`SDL_BLENDMODE_BLEND_PREMULTIPLIED`), on the renderer's own GPU API. The
  example asks SDL for the render driver of the best path this build has
  (`--renderer` overrides it) and prints `GPU: ...`:

  | SDL renderer | Path |
  | --- | --- |
  | `metal` (macOS, Graphite build) | Skia Graphite on SDL's `MTLDevice` (the layer's). SDL exposes neither its queue nor its textures' `MTLTexture`s, but makes textures around a `CVPixelBuffer`'s IOSurface: Skia renders into a private texture it keeps (only the damage is repainted) on its own queue, each repaint is blitted into the next of three IOSurface textures, and the blit is waited for before `SDL_RenderPresent` (normally long done) ([gpu_metal.mm](gpu_metal.mm)) |
  | `vulkan` (Linux / Windows, Graphite build) | Skia Graphite on SDL's `VkDevice` and graphics queue, into a `VkImage` of ours that the texture wraps (`SDL_PROP_TEXTURE_CREATE_VULKAN_TEXTURE_NUMBER`; SDL's own images lack the input-attachment usage Graphite needs), entered and left in `SHADER_READ_ONLY_OPTIMAL`, where SDL keeps sampled textures, after `SDL_FlushRenderer` submitted SDL's work; the instance is created here at Vulkan 1.1 (SDL's is 1.0) ([gpu.cpp](gpu.cpp)) |
  | `opengl` (any build with Ganesh GL) | Skia Ganesh in SDL's GL context into the texture's `GL_TEXTURE_2D` (`SDL_PROP_TEXTURE_OPENGL_TEXTURE_NUMBER`) after `SDL_FlushRenderer`, then the bindings SDL's GL renderer assumes are reset ([gpu.cpp](gpu.cpp)) |
  | anything else, or `--cpu` | the runtime rasterises into a buffer kept between frames (`jsui_render_rgba_incremental`) and only the damaged rectangle is uploaded to a streaming `SDL_PIXELFORMAT_RGBA32` texture |

  Either way nothing is painted while the UI is idle. Uncapped on a Mac Studio
  (M1 Ultra), 1280x720: `opengl` ~1200 fps on the GPU path vs ~790 with
  `--cpu`; `metal` ~410 vs ~310 (SDL's Metal renderer itself is the limit
  there). The scene and the UI are drawn in logical px (`SDL_SetRenderScale`
  by the pixel density); the UI texture has the output's pixel size, so it is
  sharp on HiDPI screens. Direct3D has no Skia backend: on Windows the
  example asks for SDL's `vulkan` driver (or `opengl`).
* **Input.** SDL events: mouse, wheel, touch, keys (scancode -> DOM `code`,
  keycode -> DOM `key`), text input, and gamepads in the W3C standard layout
  with analog triggers as buttons 6 / 7. Unconsumed: left drag pans, wheel
  zooms, Space spawns, Esc / Start pauses, the right stick pans.
* **Per-frame data.** Each unit's screen position into the `entities` buffer.
* **Cursor and clipboard.** `jsui_get_cursor()` -> SDL system cursors;
  `SDL_GetClipboardText` / `SDL_SetClipboardText`.
