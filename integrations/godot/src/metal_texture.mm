// A Metal texture with Metal's own hazard tracking for JsuiView's src and
// its host texture copies (see jsui_gpu_render.cpp). The godot-aw3 fork's
// Metal driver defaults to explicit barriers (GODOT_MTL_SYNC_MODE),
// creating its textures untracked in heaps: nothing then orders Skia
// Graphite's render into src (its own command buffers on Godot's queue)
// before Godot's src -> dst copy (or Godot's copy into a host texture copy
// before Skia samples it). A tracked texture of our own, imported with
// texture_create_from_extension, lets Metal order the two on the shared queue.
#import <Metal/Metal.h>

#include "metal_texture.h"

namespace jsui_godot {

void* newTrackedMetalTexture(void* device, int width, int height, bool bgra) {
  id<MTLDevice> dev = (__bridge id<MTLDevice>)device;
  if (!dev || width <= 0 || height <= 0) return nullptr;
  MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:(bgra ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatRGBA8Unorm)
                                                                                   width:NSUInteger(width)
                                                                                  height:NSUInteger(height)
                                                                               mipmapped:NO];
  desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  desc.storageMode = MTLStorageModePrivate;
  desc.hazardTrackingMode = MTLHazardTrackingModeTracked;
  id<MTLTexture> tex = [dev newTextureWithDescriptor:desc];
  if (!tex) return nullptr;
  tex.label = bgra ? @"JsuiView texture (tracked, BGRA)" : @"JsuiView texture (tracked)";
  return (__bridge_retained void*)tex;  // +1, released by releaseMetalTexture
}

void releaseMetalTexture(void* texture) {
  if (texture) CFRelease(texture);
}

}  // namespace jsui_godot
