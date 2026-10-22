// WebGPU backend: Skia Graphite (Dawn backend) drawing into an embedder's
// WebGPU texture (jsui_gpu_create_webgpu / jsui_render_webgpu* in jsui.h).
//
// In the browser the device is the page's GPUDevice imported through
// Emdawnwebgpu (web/jsui-webgpu.js), the target the canvas' current texture.
// On Dawn native (a game engine's WebGPU renderer, e.g. Godot's WebGPU fork)
// the embedder passes its own instance / device / texture; nothing here is
// browser specific except that the browser context cannot block on the GPU
// (no tick function: Graphite then never waits, see DawnBackendContext.h).
//
// Frames: the runtime paints into a retained texture owned by the context,
// repainting only the damaged region (Runtime::renderCanvasIncremental), and
// that texture is drawn into the target with one draw. Raster images (decoded
// images, shadow masks) are uploaded once and cached by image id
// (CachingImageProvider), like Ganesh's resource cache does on the GL path.
#include "render/gpu.h"

#if defined(JSUI_WEBGPU)

#include <webgpu/webgpu_cpp.h>

#include <cstdint>
#include <memory>
#include <unordered_map>

#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImage.h"
#include "include/core/SkSurface.h"
#include "include/gpu/graphite/BackendTexture.h"
#include "include/gpu/graphite/Context.h"
#include "include/gpu/graphite/ContextOptions.h"
#include "include/gpu/graphite/GraphiteTypes.h"
#include "include/gpu/graphite/Image.h"
#include "include/gpu/graphite/ImageProvider.h"
#include "include/gpu/graphite/Recorder.h"
#include "include/gpu/graphite/Recording.h"
#include "include/gpu/graphite/Surface.h"
#include "include/gpu/graphite/dawn/DawnBackendContext.h"
#include "include/gpu/graphite/dawn/DawnGraphiteTypes.h"
#include "render/painter.h"
#include "runtime.h"

namespace graphite = skgpu::graphite;

namespace {

constexpr size_t kDefaultCacheBytes = 256u << 20;

// Graphite draws raster SkImages through an ImageProvider; the default one
// uploads them again for every recording. Cache the GPU copy per image id
// (the cached SkImage keeps the raster image's id from being reused), and
// drop entries not used for a while or over budget.
class CachingImageProvider final : public graphite::ImageProvider {
 public:
  sk_sp<SkImage> findOrCreate(graphite::Recorder* recorder, const SkImage* image,
                              SkImage::RequiredProperties props) override {
    if (image->isTextureBacked()) return sk_ref_sp(const_cast<SkImage*>(image));
    uint64_t key = (uint64_t(image->uniqueID()) << 1) | (props.fMipmapped ? 1 : 0);
    auto it = entries_.find(key);
    if (it != entries_.end()) {
      it->second.lastUsed = frame_;
      return it->second.gpu;
    }
    sk_sp<SkImage> gpu = SkImages::TextureFromImage(recorder, image, props);
    if (!gpu) return nullptr;
    size_t bytes = image->imageInfo().computeMinByteSize();
    entries_[key] = {sk_ref_sp(const_cast<SkImage*>(image)), gpu, bytes, frame_};
    bytes_ += bytes;
    return gpu;
  }

  // Once per frame: evicts images unused for kMaxAge frames, and the least
  // recently used ones while over budget.
  void endFrame() {
    frame_++;
    constexpr uint64_t kMaxAge = 600;
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (frame_ - it->second.lastUsed > kMaxAge) {
        bytes_ -= it->second.bytes;
        it = entries_.erase(it);
      } else {
        ++it;
      }
    }
    while (bytes_ > budget_ && !entries_.empty()) {
      auto oldest = entries_.begin();
      for (auto it = entries_.begin(); it != entries_.end(); ++it)
        if (it->second.lastUsed < oldest->second.lastUsed) oldest = it;
      if (oldest->second.lastUsed == frame_ - 1) break;  // everything is in use
      bytes_ -= oldest->second.bytes;
      entries_.erase(oldest);
    }
  }
  void clear() {
    entries_.clear();
    bytes_ = 0;
  }
  void setBudget(size_t bytes) { budget_ = bytes; }

 private:
  struct Entry {
    sk_sp<SkImage> raster, gpu;
    size_t bytes;
    uint64_t lastUsed;
  };
  std::unordered_map<uint64_t, Entry> entries_;
  size_t bytes_ = 0, budget_ = kDefaultCacheBytes / 2;
  uint64_t frame_ = 0;
};

struct WebGPUGpu final : JsuiGpuContext {
  WebGPUGpu() : JsuiGpuContext(Kind::WebGPU) {}
  ~WebGPUGpu() override { release(); }

  wgpu::Instance instance;
  wgpu::Device device;
  wgpu::Queue queue;
  std::unique_ptr<graphite::Context> ctx;
  std::unique_ptr<graphite::Recorder> recorder;
  sk_sp<CachingImageProvider> images;
  sk_sp<SkSurface> retained;  // the last frame, for damage-only repaints
  bool lost = false;
  size_t cacheBytes = kDefaultCacheBytes;

  bool init() {
    graphite::DawnBackendContext backend;
    backend.fInstance = instance;
    backend.fDevice = device;
    backend.fQueue = queue;
#if defined(__EMSCRIPTEN__)
    backend.fTick = nullptr;  // the browser cannot block on the GPU
#else
    backend.fTick = instance ? graphite::DawnNativeProcessEventsFunction : nullptr;
#endif
    graphite::ContextOptions opts;
    ctx = graphite::ContextFactory::MakeDawn(backend, opts);
    if (!ctx) return false;
    ctx->setMaxBudgetedBytes(cacheBytes);
    images = sk_make_sp<CachingImageProvider>();
    images->setBudget(cacheBytes / 2);
    graphite::RecorderOptions ropts;
    ropts.fImageProvider = images;
    ropts.fGpuBudgetInBytes = cacheBytes;
    recorder = ctx->makeRecorder(ropts);
    return recorder != nullptr;
  }

  void release() {
    retained.reset();
    if (images) images->clear();
    recorder.reset();
    // A non-yielding context cannot wait for in-flight GPU work in its
    // destructor: submitted work keeps its own references to Dawn objects, so
    // dropping ours is safe (Dawn / the browser free them when done).
    if (ctx) ctx->submit();
    ctx.reset();
  }

  bool isLost() {
    if (!lost && ctx && ctx->isDeviceLost()) lost = true;
    return lost;
  }

  // Submits what the recorder holds. False on failure.
  bool flush() {
    std::unique_ptr<graphite::Recording> recording = recorder->snap();
    if (!recording) return false;
    graphite::InsertRecordingInfo info;
    info.fRecording = recording.get();
    if (ctx->insertRecording(info) != graphite::InsertStatus::kSuccess) return false;
    ctx->submit(graphite::SyncToCpu::kNo);
    ctx->checkAsyncWorkCompletion();
    images->endFrame();
    return true;
  }
};

WebGPUGpu* asWebGPU(JsuiGpuContext* g) {
  return g && g->kind == JsuiGpuContext::Kind::WebGPU ? static_cast<WebGPUGpu*>(g) : nullptr;
}

}  // namespace

namespace jsui {
namespace webgpu {

bool available() { return true; }

JsuiGpuContext* create(void* instance, void* device, void* queue) {
  if (!device) return nullptr;
  auto* g = new WebGPUGpu;
  // The C++ wrappers add a reference to handles they are constructed from.
  g->instance = wgpu::Instance(static_cast<WGPUInstance>(instance));
  g->device = wgpu::Device(static_cast<WGPUDevice>(device));
  g->queue = queue ? wgpu::Queue(static_cast<WGPUQueue>(queue)) : g->device.GetQueue();
#if defined(__EMSCRIPTEN__)
  if (!g->instance) g->instance = wgpu::CreateInstance(nullptr);
#endif
  if (!g->init()) {
    delete g;
    return nullptr;
  }
  return g;
}

void destroy(JsuiGpuContext* gpu) { delete asWebGPU(gpu); }

void contextLost(JsuiGpuContext* gpu) {
  WebGPUGpu* g = asWebGPU(gpu);
  if (!g) return;
  g->lost = true;
  g->retained.reset();
  if (g->images) g->images->clear();
}

void purge(JsuiGpuContext* gpu, size_t cacheBytes) {
  WebGPUGpu* g = asWebGPU(gpu);
  if (!g || !g->ctx) return;
  if (cacheBytes) {
    g->cacheBytes = cacheBytes;
    g->ctx->setMaxBudgetedBytes(cacheBytes);
    g->images->setBudget(cacheBytes / 2);
  }
  g->images->clear();
  g->recorder->freeGpuResources();
  g->ctx->freeGpuResources();
}

int render(Runtime& rt, JsuiGpuContext* gpu, const JsuiWebGPUTarget& t) {
  WebGPUGpu* g = asWebGPU(gpu);
  if (!g || !t.texture || g->isLost()) return -1;
  wgpu::Texture texture(static_cast<WGPUTexture>(t.texture));
  int w = t.width > 0 ? t.width : int(texture.GetWidth());
  int h = t.height > 0 ? t.height : int(texture.GetHeight());
  if (w <= 0 || h <= 0) return -1;

  graphite::BackendTexture backendTex = graphite::BackendTextures::MakeDawn(texture.Get());
  SkSurfaceProps props = jsui::surfaceProps();
  sk_sp<SkSurface> target = SkSurfaces::WrapBackendTexture(g->recorder.get(), backendTex, nullptr, &props);
  if (!target) return -1;

  if (t.flags & JSUI_WEBGPU_NO_RETAIN) {
    g->retained.reset();
    rt.renderCanvas(target->getCanvas(), w, h, (t.flags & JSUI_WEBGPU_NO_CLEAR) == 0);
  } else {
    SkImageInfo info = target->imageInfo();
    if (!g->retained || g->retained->width() != w || g->retained->height() != h ||
        g->retained->imageInfo().colorType() != info.colorType()) {
      SkImageInfo ri = SkImageInfo::Make(w, h, info.colorType(), kPremul_SkAlphaType, info.refColorSpace());
      g->retained = SkSurfaces::RenderTarget(g->recorder.get(), ri, skgpu::Mipmapped::kNo, &props);
      if (!g->retained) return -1;
    }
    // Damage-only repaint of the retained frame (a new surface, size or an
    // intervening full render makes it a full repaint).
    rt.renderCanvasIncremental(g->retained->getCanvas(), g->retained.get(), w, h);
    SkPaint paint;
    paint.setBlendMode((t.flags & JSUI_WEBGPU_NO_CLEAR) ? SkBlendMode::kSrcOver : SkBlendMode::kSrc);
    target->getCanvas()->drawImage(SkSurfaces::AsImage(g->retained), 0, 0, SkSamplingOptions(), &paint);
  }
  target.reset();
  if (!g->flush()) return -1;
  return g->isLost() ? -1 : 0;
}

}  // namespace webgpu
}  // namespace jsui

#else  // !JSUI_WEBGPU

namespace jsui {
namespace webgpu {
bool available() { return false; }
JsuiGpuContext* create(void*, void*, void*) { return nullptr; }
void destroy(JsuiGpuContext*) {}
void contextLost(JsuiGpuContext*) {}
void purge(JsuiGpuContext*, size_t) {}
int render(Runtime&, JsuiGpuContext*, const JsuiWebGPUTarget&) { return -1; }
}  // namespace webgpu
}  // namespace jsui

#endif
