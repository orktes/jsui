// The sokol implementations (compiled as Objective-C on Apple platforms),
// with the backend CMake picked: SOKOL_METAL on macOS when JSUI has Skia
// Graphite Metal (Skia draws the UI on sokol_gfx's MTLDevice and queue),
// SOKOL_GLCORE elsewhere (Skia draws with OpenGL on sokol_app's context).
#define SOKOL_IMPL
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#include "util/sokol_gl.h"

#include "sokol_extra.h"

#if defined(SOKOL_METAL)
#import <Metal/Metal.h>

// sokol has no API for these: they reach into its state (_sapp / _sg),
// which this translation unit can see.

static id<MTLBuffer> example_capture_buf = nil;
static int example_capture_w = 0, example_capture_h = 0;

void example_gpu_finish(void) {
  // An empty command buffer on sokol_gfx's queue completes after
  // everything committed before it (sg_commit's frame, JSUI's renders).
  id<MTLCommandBuffer> cb = [_sg.mtl.cmd_queue commandBuffer];
  [cb commit];
  [cb waitUntilCompleted];
}

void example_framebuffer_readable(void) { _sapp.mtl.layer.framebufferOnly = NO; }

int example_capture_framebuffer(const void* drawable, int width, int height) {
  if (!drawable || nil == _sg.mtl.cmd_buffer) return 0;
  id<MTLTexture> tex = [(__bridge id<CAMetalDrawable>)drawable texture];
  if (tex.framebufferOnly) return 0;
  NSUInteger row = (NSUInteger)width * 4;
  if (nil == example_capture_buf || example_capture_buf.length < row * (NSUInteger)height) {
    _SG_OBJC_RELEASE(example_capture_buf);
    example_capture_buf = [_sg.mtl.device newBufferWithLength:row * (NSUInteger)height
                                                      options:MTLResourceStorageModeShared];
  }
  example_capture_w = width;
  example_capture_h = height;
  // Recorded into sokol_gfx's frame command buffer after its passes, so it
  // runs before the drawable is presented by sg_commit.
  id<MTLBlitCommandEncoder> blit = [_sg.mtl.cmd_buffer blitCommandEncoder];
  [blit copyFromTexture:tex
               sourceSlice:0
               sourceLevel:0
              sourceOrigin:MTLOriginMake(0, 0, 0)
                sourceSize:MTLSizeMake((NSUInteger)width, (NSUInteger)height, 1)
                  toBuffer:example_capture_buf
         destinationOffset:0
    destinationBytesPerRow:row
  destinationBytesPerImage:row * (NSUInteger)height];
  [blit endEncoding];
  return 1;
}

int example_read_capture(unsigned char* rgba) {
  if (nil == example_capture_buf) return 0;
  example_gpu_finish();
  // The drawable is BGRA8: swap to RGBA.
  const unsigned char* src = (const unsigned char*)example_capture_buf.contents;
  size_t n = (size_t)example_capture_w * (size_t)example_capture_h;
  for (size_t i = 0; i < n; i++) {
    rgba[i * 4 + 0] = src[i * 4 + 2];
    rgba[i * 4 + 1] = src[i * 4 + 1];
    rgba[i * 4 + 2] = src[i * 4 + 0];
    rgba[i * 4 + 3] = src[i * 4 + 3];
  }
  return 1;
}
#endif
