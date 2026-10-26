// The viewer's Metal plumbing for the Graphite renderer (macOS): the window's
// CAMetalLayer, the retained texture JSUI paints into, presenting it and
// reading it back. Plain C entry points so main.cpp stays C++.
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <cstdint>
#include <cstring>

extern "C" {

// Sets up `layer` (a CAMetalLayer, from SDL_Metal_GetLayer) on the system
// default device; returns the device and a queue (retained), or 0.
int viewer_mtl_init(void* layer, float r, float g, float b, void** device, void** queue) {
  id<MTLDevice> d = MTLCreateSystemDefaultDevice();
  if (!d) return 0;
  CAMetalLayer* l = (__bridge CAMetalLayer*)layer;
  l.device = d;
  l.pixelFormat = MTLPixelFormatBGRA8Unorm;
  l.framebufferOnly = NO;  // the frame is blitted in
  // The UI is premultiplied and may be translucent: Core Animation composites
  // it over the layer's background, the viewer's backdrop colour.
  l.opaque = NO;
  CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
  CGFloat rgba[4] = {r, g, b, 1};
  CGColorRef bg = CGColorCreate(cs, rgba);
  l.backgroundColor = bg;
  CGColorRelease(bg);
  CGColorSpaceRelease(cs);
  *device = (__bridge_retained void*)d;
  *queue = (__bridge_retained void*)[d newCommandQueue];
  return 1;
}

const char* viewer_mtl_name(void* device) {
  return [((__bridge id<MTLDevice>)device).name UTF8String];
}

// The texture JSUI paints into (BGRA8, kept between frames for incremental repaints).
void* viewer_mtl_texture(void* device, int w, int h) {
  MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                width:w
                                                                               height:h
                                                                            mipmapped:NO];
  td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  td.storageMode = MTLStorageModePrivate;
  return (__bridge_retained void*)[(__bridge id<MTLDevice>)device newTextureWithDescriptor:td];
}

// Copies `texture` into the layer's next drawable and presents it. Ordered
// after the render JSUI committed to the same queue.
int viewer_mtl_present(void* layer, void* queue, void* texture) {
  @autoreleasepool {
    CAMetalLayer* l = (__bridge CAMetalLayer*)layer;
    id<MTLTexture> t = (__bridge id<MTLTexture>)texture;
    l.drawableSize = CGSizeMake(t.width, t.height);
    id<CAMetalDrawable> drawable = [l nextDrawable];
    if (!drawable) return 0;
    id<MTLCommandBuffer> cb = [(__bridge id<MTLCommandQueue>)queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:t toTexture:drawable.texture];
    [blit endEncoding];
    [cb presentDrawable:drawable];
    [cb commit];
    return 1;
  }
}

// Reads `texture` back as RGBA (w * h * 4 bytes into `out`).
int viewer_mtl_read(void* device, void* queue, void* texture, int w, int h, uint8_t* out) {
  @autoreleasepool {
    id<MTLDevice> d = (__bridge id<MTLDevice>)device;
    size_t row = size_t(w) * 4;
    id<MTLBuffer> buf = [d newBufferWithLength:row * h options:MTLResourceStorageModeShared];
    if (!buf) return 0;
    id<MTLCommandBuffer> cb = [(__bridge id<MTLCommandQueue>)queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:(__bridge id<MTLTexture>)texture
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(w, h, 1)
                        toBuffer:buf
               destinationOffset:0
          destinationBytesPerRow:row
        destinationBytesPerImage:row * h];
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    const uint8_t* p = static_cast<const uint8_t*>(buf.contents);
    for (size_t i = 0; i < row * h; i += 4) {  // BGRA -> RGBA
      out[i] = p[i + 2];
      out[i + 1] = p[i + 1];
      out[i + 2] = p[i];
      out[i + 3] = p[i + 3];
    }
    return 1;
  }
}

void viewer_mtl_release(void* obj) {
  if (obj) CFRelease(obj);
}

}  // extern "C"
