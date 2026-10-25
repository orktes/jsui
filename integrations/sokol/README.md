# JSUI in sokol

[main.cpp](main.cpp) runs the shared engine-example HUD
([../shared/ui/app.jsx](../shared/ui/app.jsx)) over a 3D scene drawn with
sokol_gfx through sokol_gl: spinning units over a ring floor, an orbit
camera, and a nameplate per unit. sokol_gfx runs on Metal (`SOKOL_METAL`) on
macOS when JSUI was built with Skia Graphite Metal (`JSUI_GRAPHITE_METAL`,
a Skia from `scripts/build-skia-desktop.sh`), and on OpenGL (`SOKOL_GLCORE`)
otherwise and on every other platform; `-DJSUI_SOKOL_GL=ON` keeps GL on macOS.

```sh
npx jsui build integrations/shared/ui/app.jsx -o integrations/shared/ui/dist/bundle.js
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DJSUI_BUILD_SOKOL=ON
cmake --build build --target jsui-sokol
./build/jsui-sokol             # --cpu, --dev <url>, --selftest, --frames N --screenshot out.png, --js-thread off
```

* **Rendering (Metal).** `jsui_gpu_create_metal(sg_mtl_device(),
  sg_mtl_command_queue())`: Skia Graphite on sokol_gfx's own device and
  queue. The UI goes into an `sg_image` the game keeps (`color_attachment`
  usage, so its `MTLTexture` is a render target), before the frame's pass:
  `jsui_render_gpu_target` with `JSUI_GPU_INCREMENTAL` repaints only what
  changed and commits to sokol's queue, ahead of the command buffer
  `sg_commit()` commits, and nothing is rendered on frames where nothing
  changed. The scene pass then draws the image with a premultiplied sokol_gl
  quad.
* **Rendering (GL).** After the scene's swapchain pass (`sg_end_pass()`), Skia draws
  the UI over it into the GL framebuffer of the frame's swapchain (`sglue_swapchain().gl.framebuffer`) with `JSUI_GL_NO_CLEAR`,
  on sokol_app's GL context; `sg_reset_state_cache()` then tells sokol_gfx
  that GL state changed behind its back, and `sg_commit()` ends the frame. The
  runtime keeps its last frame in a texture of its own and repaints only what
  changed, so an idle UI costs one textured draw.
* **`--cpu`** (either backend). The CPU raster is kept up to date
  incrementally and written each frame into a write-transient `sg_image`
  (sokol_gfx's transient images do not keep their contents between frames),
  drawn with a premultiplied sokol_gl quad in the scene's pass.
* **Input.** sokol_app events. Mouse and touch positions come in framebuffer
  pixels and are divided by `sapp_dpi_scale()`. sokol's key codes are GLFW's
  ([../shared/glfw_keys.h](../shared/glfw_keys.h)); text from `CHAR` events.
  sokol_app has no gamepad API, so this host forwards none. Unconsumed: left
  drag orbits, wheel zooms, Space spawns, Esc pauses.
* **Per-frame data.** Each unit projected with the scene's view-projection
  matrix into the `entities` buffer.
* **`--bench` on macOS.** sokol_app's view is paced at the display refresh
  whatever the swap interval (GL) or display sync (Metal, driven by a
  `CADisplayLink`), so `--bench` times each frame's work there: the CPU
  plus waiting for the GPU (`glFinish`, or an empty command buffer on
  sokol's queue on Metal).
* **Exit code.** sokol_app's run loop returns nothing, so the cleanup callback
  ends the process with the self-test's verdict.
* sokol has no releases; [CMakeLists.txt](CMakeLists.txt) pins a commit of its
  master branch.
