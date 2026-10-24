// Metal device / queue / target texture for jsui-paint-bench's graphite and
// metal backends (plain C entry points so main.cpp stays C++).
#import <Metal/Metal.h>

extern "C" {

int bench_mtl_init(void** device, void** queue) {
  id<MTLDevice> d = MTLCreateSystemDefaultDevice();
  if (!d) return 0;
  id<MTLCommandQueue> q = [d newCommandQueue];
  *device = (__bridge_retained void*)d;
  *queue = (__bridge_retained void*)q;
  return 1;
}

void* bench_mtl_texture(void* device, int w, int h) {
  id<MTLDevice> d = (__bridge id<MTLDevice>)device;
  MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                width:w
                                                                               height:h
                                                                            mipmapped:NO];
  td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  td.storageMode = MTLStorageModePrivate;
  return (__bridge_retained void*)[d newTextureWithDescriptor:td];
}

// Waits until everything submitted to `queue` so far has run on the GPU.
void bench_mtl_wait(void* queue) {
  id<MTLCommandQueue> q = (__bridge id<MTLCommandQueue>)queue;
  id<MTLCommandBuffer> cb = [q commandBuffer];
  [cb commit];
  [cb waitUntilCompleted];
}

void bench_mtl_release(void* obj) {
  if (obj) CFRelease(obj);
}

}  // extern "C"
