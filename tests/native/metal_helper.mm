// Metal device, target texture and read-back for metal_test.cpp (plain C
// entry points so the test stays C++).
#import <Metal/Metal.h>

#include <cstring>

extern "C" {

int test_mtl_init(void** device, void** queue) {
  id<MTLDevice> d = MTLCreateSystemDefaultDevice();
  if (!d) return 0;
  id<MTLCommandQueue> q = [d newCommandQueue];
  if (!q) return 0;
  *device = (__bridge_retained void*)d;
  *queue = (__bridge_retained void*)q;
  return 1;
}

// An RGBA8 render target JSUI draws into (private, as an engine's are).
void* test_mtl_texture(void* device, int w, int h) {
  id<MTLDevice> d = (__bridge id<MTLDevice>)device;
  MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                width:NSUInteger(w)
                                                                               height:NSUInteger(h)
                                                                            mipmapped:NO];
  td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  td.storageMode = MTLStorageModePrivate;
  return (__bridge_retained void*)[d newTextureWithDescriptor:td];
}

// Copies the texture into `out` (w * h RGBA8 pixels) on `queue`, after what
// was committed to it before, and waits for it.
int test_mtl_read(void* device, void* queue, void* texture, int w, int h, void* out) {
  id<MTLDevice> d = (__bridge id<MTLDevice>)device;
  id<MTLCommandQueue> q = (__bridge id<MTLCommandQueue>)queue;
  id<MTLTexture> t = (__bridge id<MTLTexture>)texture;
  const NSUInteger rowBytes = NSUInteger(w) * 4;
  id<MTLBuffer> buf = [d newBufferWithLength:rowBytes * NSUInteger(h) options:MTLResourceStorageModeShared];
  if (!buf) return 0;
  id<MTLCommandBuffer> cb = [q commandBuffer];
  id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
  [blit copyFromTexture:t
                   sourceSlice:0
                   sourceLevel:0
                  sourceOrigin:MTLOriginMake(0, 0, 0)
                    sourceSize:MTLSizeMake(NSUInteger(w), NSUInteger(h), 1)
                      toBuffer:buf
             destinationOffset:0
        destinationBytesPerRow:rowBytes
      destinationBytesPerImage:rowBytes * NSUInteger(h)];
  [blit endEncoding];
  [cb commit];
  [cb waitUntilCompleted];
  if (cb.status != MTLCommandBufferStatusCompleted) return 0;
  std::memcpy(out, buf.contents, rowBytes * NSUInteger(h));
  return 1;
}

void test_mtl_release(void* obj) {
  if (obj) CFRelease(obj);
}

}  // extern "C"
