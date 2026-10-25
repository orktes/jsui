# JSUI in sokol

[main.cpp](main.cpp) runs the shared engine-example HUD
([../shared/ui/app.jsx](../shared/ui/app.jsx)) over a 3D scene drawn with
sokol_gfx through sokol_gl (GL backend, `SOKOL_GLCORE`): spinning units over a
ring floor, an orbit camera, and a nameplate per unit.

```sh
npx jsui build integrations/shared/ui/app.jsx -o integrations/shared/ui/dist/bundle.js
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DJSUI_BUILD_SOKOL=ON
cmake --build build --target jsui-sokol
./build/jsui-sokol             # --cpu, --dev <url>, --selftest, --frames N --screenshot out.png, --js-thread off
```

* **Rendering.** After the scene's swapchain pass (`sg_end_pass()`), Skia draws
  the UI over it into the GL framebuffer of the frame's swapchain (`sglue_swapchain().gl.framebuffer`) with `JSUI_GL_NO_CLEAR`,
  on sokol_app's GL context; `sg_reset_state_cache()` then tells sokol_gfx
  that GL state changed behind its back, and `sg_commit()` ends the frame. The
  runtime keeps its last frame in a texture of its own and repaints only what
  changed, so an idle UI costs one textured draw. `--cpu`: the CPU raster is
  kept up to date incrementally and written each frame into a write-transient
  `sg_image` (sokol_gfx's transient images do not keep their contents between
  frames), drawn with a premultiplied sokol_gl quad in the same pass.
* **Input.** sokol_app events. Mouse and touch positions come in framebuffer
  pixels and are divided by `sapp_dpi_scale()`. sokol's key codes are GLFW's
  ([../shared/glfw_keys.h](../shared/glfw_keys.h)); text from `CHAR` events.
  sokol_app has no gamepad API, so this host forwards none. Unconsumed: left
  drag orbits, wheel zooms, Space spawns, Esc pauses.
* **Per-frame data.** Each unit projected with the scene's view-projection
  matrix into the `entities` buffer.
* **Exit code.** sokol_app's run loop returns nothing, so the cleanup callback
  ends the process with the self-test's verdict.
* sokol has no releases; [CMakeLists.txt](CMakeLists.txt) pins a commit of its
  master branch.
