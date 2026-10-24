// The Metal GPU path of the SDL3 example (see gpu.h): Skia Graphite on SDL's
// MTLDevice, through IOSurfaces.
//
// SDL's Metal renderer exposes its device (the CAMetalLayer's) but neither its
// MTLCommandQueue nor its textures' MTLTextures. What it does offer is
// SDL_PROP_TEXTURE_CREATE_METAL_PIXELBUFFER_POINTER: a texture made around a
// CVPixelBuffer's IOSurface. So the UI texture is an IOSurface that both sides
// see, SDL through its own MTLTexture of it and Skia through ours, on the same
// device but on our own queue:
//
//   canvas_   a private texture Skia renders into, kept between frames, so a
//             frame repaints only its damage (JSUI_GPU_INCREMENTAL)
//   slots_    kSlots IOSurface textures; each repaint blits the canvas into
//             the next one (one GPU copy, a few MB) and SDL draws that one
//
// Two queues and two texture objects per surface mean no automatic hazard
// tracking between Skia's work and SDL's, so the order is kept by hand: the
// blit is committed when the UI is painted (before the arena is drawn) and
// waited for in beforePresent, i.e. before SDL submits the frame that samples
// the slot; by then it has normally long finished, so the wait costs nothing.
// The other direction, SDL's earlier frames still sampling a slot when it is
// written again, is what the rotation covers: a slot is reused kSlots repaints
// later, past the frames the layer keeps in flight (maximumDrawableCount 3).
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "gpu.h"

namespace sdl3_gpu {

namespace {

constexpr int kSlots = 3;

class MetalTarget : public UiTarget {
 public:
  MetalTarget(SDL_Renderer* r, id<MTLDevice> device, id<MTLCommandQueue> queue, JsuiGpuContext* gpu)
      : renderer_(r), device_(device), queue_(queue), gpu_(gpu) {}
  ~MetalTarget() override {
    beforePresent();
    release();
    jsui_gpu_destroy(gpu_);
  }

  std::string describe() const override { return "Skia Graphite Metal · IOSurface texture"; }
  std::string device() const override { return device_.name.UTF8String; }

  bool resize(int width, int height) override {
    beforePresent();
    release();
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                 width:NSUInteger(width)
                                                                                height:NSUInteger(height)
                                                                             mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModePrivate;
    canvas_ = [device_ newTextureWithDescriptor:d];
    if (!canvas_) return false;
    NSDictionary* attrs = @{
      (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
      (id)kCVPixelBufferMetalCompatibilityKey : @YES,
    };
    for (Slot& s : slots_) {
      if (CVPixelBufferCreate(kCFAllocatorDefault, size_t(width), size_t(height), kCVPixelFormatType_32BGRA,
                              (__bridge CFDictionaryRef)attrs, &s.buffer) != kCVReturnSuccess) {
        return false;
      }
      // SDL_PIXELFORMAT_ARGB8888: BGRA bytes, which SDL views as BGRA8Unorm.
      SDL_PropertiesID p = SDL_CreateProperties();
      SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_ARGB8888);
      SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STATIC);
      SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, width);
      SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, height);
      SDL_SetPointerProperty(p, SDL_PROP_TEXTURE_CREATE_METAL_PIXELBUFFER_POINTER, s.buffer);
      s.texture = SDL_CreateTextureWithProperties(renderer_, p);
      SDL_DestroyProperties(p);
      if (!s.texture) return false;
      SDL_SetTextureBlendMode(s.texture, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
      SDL_SetTextureScaleMode(s.texture, SDL_SCALEMODE_LINEAR);
      MTLTextureDescriptor* sd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                    width:NSUInteger(width)
                                                                                   height:NSUInteger(height)
                                                                                mipmapped:NO];
      sd.usage = MTLTextureUsageShaderRead;  // a blit destination needs no usage flag
      s.view = [device_ newTextureWithDescriptor:sd iosurface:CVPixelBufferGetIOSurface(s.buffer) plane:0];
      if (!s.view) return false;
    }
    width_ = width;
    height_ = height;
    holds_ = false;
    shown_ = 0;
    return true;
  }

  SDL_Texture* texture() const override { return slots_[shown_].texture; }

  bool paint(JsuiRuntime* rt, bool force) override {
    if (!force && holds_ && !jsui_needs_redraw(rt)) return true;
    JsuiGpuTarget t{};
    t.texture = (__bridge void*)canvas_;
    t.format = MTLPixelFormatBGRA8Unorm;
    t.width = width_;
    t.height = height_;
    t.flags = holds_ && !force ? JSUI_GPU_INCREMENTAL : 0;
    JsuiRect dirty;
    if (jsui_render_gpu_target(rt, gpu_, &t, &dirty) != 0) return false;
    holds_ = true;
    // The canvas into the next slot, after Skia's work (same queue, tracked).
    shown_ = (shown_ + 1) % kSlots;
    id<MTLCommandBuffer> cb = [queue_ commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:canvas_ toTexture:slots_[shown_].view];
    [blit endEncoding];
    [cb commit];
    pending_ = cb;
    return true;
  }

  void beforePresent() override {
    if (pending_) [pending_ waitUntilCompleted];
    pending_ = nil;
  }

 private:
  struct Slot {
    CVPixelBufferRef buffer = nullptr;
    SDL_Texture* texture = nullptr;
    id<MTLTexture> view = nil;  // ours, on the same IOSurface
  };

  void release() {
    for (Slot& s : slots_) {
      if (s.texture) SDL_DestroyTexture(s.texture);
      if (s.buffer) CVPixelBufferRelease(s.buffer);
      s = Slot{};
    }
    canvas_ = nil;
  }

  SDL_Renderer* renderer_;
  id<MTLDevice> device_;
  id<MTLCommandQueue> queue_;
  JsuiGpuContext* gpu_;
  id<MTLTexture> canvas_ = nil;
  Slot slots_[kSlots];
  int shown_ = 0;
  id<MTLCommandBuffer> pending_ = nil;  // the last blit, until beforePresent
  int width_ = 0, height_ = 0;
  bool holds_ = false;  // the canvas holds the runtime's last frame
};

}  // namespace

std::unique_ptr<UiTarget> createMetalTarget(SDL_Renderer* renderer) {
  CAMetalLayer* layer = (__bridge CAMetalLayer*)SDL_GetRenderMetalLayer(renderer);
  id<MTLDevice> device = layer.device;
  if (!device) return nullptr;
  id<MTLCommandQueue> queue = [device newCommandQueue];
  JsuiGpuContext* gpu = jsui_gpu_create_metal((__bridge void*)device, (__bridge void*)queue);
  if (!gpu) return nullptr;
  return std::make_unique<MetalTarget>(renderer, device, queue, gpu);
}

}  // namespace sdl3_gpu
