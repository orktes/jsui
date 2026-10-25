// What main.cpp needs from sokol that sokol has no API for, implemented in
// sokol_impl.c next to sokol's own state. Metal backend only.
#pragma once

#if defined(SOKOL_METAL)
#ifdef __cplusplus
extern "C" {
#endif
// Waits until the GPU finished everything committed to sokol_gfx's queue
// (glFinish's counterpart, for --bench).
void example_gpu_finish(void);
// Lets the swapchain's drawables be read back (CAMetalLayer.framebufferOnly
// off), for --screenshot. Call before the first frame.
void example_framebuffer_readable(void);
// Records a copy of `drawable`'s texture (CAMetalDrawable) into sokol_gfx's
// frame command buffer: call after the last pass, before sg_commit.
// Returns 0 when the drawable cannot be read.
int example_capture_framebuffer(const void* drawable, int width, int height);
// After sg_commit: waits for the copy and writes it as RGBA8 rows (top row
// first, width * height * 4 bytes).
int example_read_capture(unsigned char* rgba);
#ifdef __cplusplus
}
#endif
#endif
