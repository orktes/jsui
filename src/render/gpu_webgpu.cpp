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
// Frames (Surface::renderCanvasTracked): when most of the frame changed it is
// painted straight into the target (one pass). When little changed, only the
// damage (a few scissored rects) is repainted into a retained texture owned
// by the context, which is then drawn into the target with one draw (targets
// such as canvas textures start cleared every frame). Raster images (decoded
// images, shadow masks) are uploaded once and cached by image id
// (CachingImageProvider), like Ganesh's resource cache does on the GL path.
//
// Pipeline cache (pipeline_cache.h): the browser has no threads to spare
// (and no file system: lists go through jsui_gpu_precompile_pipelines /
// jsui_gpu_save_pipelines), so it precompiles in slices after renders; Dawn
// native on a worker thread when the device has ImplicitDeviceSynchronization
// (see gpu_graphite.cpp), otherwise in slices too.
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
#include "render/graphite_images.h"
#include "render/host_textures.h"
#include "render/painter.h"
#include "render/pipeline_cache.h"
#include "runtime.h"

namespace graphite = skgpu::graphite;

namespace {

using jsui::graphite_support::CachingImageProvider;
using jsui::graphite_support::kDefaultCacheBytes;
using jsui::pipeline_cache::Precompiler;

struct WebGPUGpu final : JsuiGpuContext {
  WebGPUGpu() : JsuiGpuContext(Kind::WebGPU) {}
  ~WebGPUGpu() override { release(); }

  wgpu::Instance instance;
  wgpu::Device device;
  wgpu::Queue queue;
  // Outlives ctx (its pipeline callback).
  std::unique_ptr<Precompiler> precompiler = std::make_unique<Precompiler>(
#if defined(__EMSCRIPTEN__)
      jsui::pipeline_cache::Backend::BrowserWebGPU
#else
      jsui::pipeline_cache::Backend::Dawn
#endif
  );
  std::unique_ptr<graphite::Context> ctx;
  std::unique_ptr<graphite::Recorder> recorder;
  sk_sp<CachingImageProvider> images;
  // Host textures (JSUI_TEXTURE_WEBGPU, textures of `device`) drawn in this
  // context's frames, sampled in place.
  jsui::TextureWrapper textures{[this](const JsuiTexture& t) -> sk_sp<SkImage> {
    if (t.kind != JSUI_TEXTURE_WEBGPU || !recorder) return nullptr;
    wgpu::Texture texture(static_cast<WGPUTexture>(t.handle));
    wgpu::TextureFormat f = t.format ? wgpu::TextureFormat(t.format) : texture.GetFormat();
    SkColorType ct;
    if (f == wgpu::TextureFormat::RGBA8Unorm) ct = kRGBA_8888_SkColorType;
    else if (f == wgpu::TextureFormat::BGRA8Unorm) ct = kBGRA_8888_SkColorType;
    else return nullptr;
    graphite::BackendTexture bt = graphite::BackendTextures::MakeDawn(texture.Get());
    if (!bt.isValid()) return nullptr;
    return SkImages::WrapTexture(recorder.get(), bt, ct, jsui::textureAlphaType(t), nullptr);
  }};
  // What render() keeps between frames of one surface. Several surfaces of
  // a runtime (several canvases) can share this context: one entry each.
  struct Frames {
    sk_sp<SkSurface> retained;  // the last frame, for damage-only repaints
    // The last frame as a copy of the target (copy-back mode, see render()).
    wgpu::Texture back;
    sk_sp<SkImage> backImage;
    uint64_t retainedGen = 0;   // the key of the current retained frame
    int smallFrames = 0;        // consecutive frames with small damage
    int directFrames = 0;       // consecutive frames painted straight into the target
    uint64_t lastUsed = 0;
    // Composited opacity layers and raster-cached subtrees (painter.h), per
    // surface: entries age with the frames of the surface they belong to.
    std::unique_ptr<jsui::LayerCache> layers = std::make_unique<jsui::LayerCache>();
    void drop() {
      retained.reset();
      backImage.reset();
      back = nullptr;
    }
  };
  std::unordered_map<const jsui::Surface*, Frames> frames;
  uint64_t nextGen = 0;     // retained frame keys, unique across the surfaces
  uint64_t renderCount = 0;
  bool lost = false;

  // The surface's entry; surfaces that stopped drawing here (destroyed, or
  // moved to another context) lose theirs after a while.
  Frames& framesOf(const jsui::Surface* s) {
    renderCount++;
    auto it = frames.find(s);
    if (it == frames.end()) {
      constexpr uint64_t kMaxIdle = 600;
      for (auto i = frames.begin(); i != frames.end();)
        i = renderCount - i->second.lastUsed > kMaxIdle ? frames.erase(i) : std::next(i);
      it = frames.emplace(s, Frames{}).first;
    }
    it->second.lastUsed = renderCount;
    return it->second;
  }
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
#if defined(__EMSCRIPTEN__)
    // No internal MSAA. Browser WebGPU has none of the features Graphite
    // needs to make it cheap (ExpandResolveTexture load ops, transient /
    // memoryless attachments, MSAA-render-to-single-sampled), so the first
    // path or complex clip in a pass made Graphite render the whole pass into
    // a 4x MSAA texture: a full-surface draw to load it from the target, the
    // 4x samples stored to memory, and a second full-surface pass to resolve
    // it back. On a tiler (Apple GPUs) that is most of the frame's GPU time.
    // Paths are rasterised into Graphite's (cached) path atlas instead, with
    // the CPU raster's anti-aliasing.
    opts.fInternalMultisampleCount = graphite::SampleCount::k1;
#endif
    precompiler->configure(opts);  // the pipeline cache's callback
    ctx = graphite::ContextFactory::MakeDawn(backend, opts);
    if (!ctx) return false;
    ctx->setMaxBudgetedBytes(cacheBytes);
    images = sk_make_sp<CachingImageProvider>();
    images->setBudget(cacheBytes / 2);
    graphite::RecorderOptions ropts;
    ropts.fImageProvider = images;
    ropts.fGpuBudgetInBytes = cacheBytes;
    // Every Recording is inserted, in the order it was snapped. Without
    // this Graphite assumes they may be played back in any order and evicts
    // its text, path and clip atlases after every snap(): every glyph a frame
    // draws was rasterised and uploaded again (134 glyphs per frame of the
    // interactions showcase's steady animation, ~560 per full repaint).
    ropts.fRequireOrderedRecordings = true;
    recorder = ctx->makeRecorder(ropts);
    if (!recorder) return false;
    precompiler->attach(ctx.get(), precompileMode());
    pipelines = precompiler.get();
    return true;
  }

  Precompiler::Mode precompileMode() {
#if defined(__EMSCRIPTEN__)
    return Precompiler::Mode::Sliced;
#else
    return device.HasFeature(wgpu::FeatureName::ImplicitDeviceSynchronization) ? Precompiler::Mode::Thread
                                                                               : Precompiler::Mode::Sliced;
#endif
  }

  void dropRetained() { frames.clear(); }

  void release() {
    if (precompiler) precompiler->shutdown();  // its PrecompileContext holds the device
    dropRetained();
    if (images) images->clear();
    textures.clear();
    recorder.reset();
    if (!ctx) return;
    // Without a tick function Graphite cannot wait for in-flight GPU work in
    // its destructor (it aborts if some is outstanding). The web host destroys
    // a context only once the queue reported its work done; collect that here.
    ctx->checkAsyncWorkCompletion();
#if defined(__EMSCRIPTEN__)
    if (ctx->hasUnfinishedGpuWork() && !ctx->isDeviceLost()) {
      // Still busy (an embedder that did not wait): leak the context rather
      // than abort. Its GPU objects are freed with the device.
      SkDebugf("jsui: WebGPU context destroyed with GPU work in flight; leaking it\n");
      (void)ctx.release();
      return;
    }
#endif
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
    textures.endFrame();
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
  g->dropRetained();
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
  for (auto& e : g->frames) e.second.layers->clear();
  g->recorder->freeGpuResources();
  g->ctx->freeGpuResources();
}

int render(Surface& rt, JsuiGpuContext* gpu, const JsuiWebGPUTarget& t) {
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

  // Where the frame is painted (Surface::renderCanvasTracked):
  //   * Most of the frame changed (an animated full-screen UI): straight into
  //     the target with a full repaint, one pass, no copy.
  //   * Small damage (a spinner, a hover, a few looping animations spread
  //     over the screen): only the damage, as a few scissored rects
  //     (Surface::CanvasChoice::splitDamage: rect clips are scissors on
  //     Graphite too). "Small" is the damaged area, not its bounds: two small
  //     animations in opposite corners have bounds as large as the screen,
  //     and a full repaint of a full-screen UI (backgrounds, gradients, every
  //     panel and text run) costs far more on the CPU (recording every draw)
  //     and on the GPU than the damage.
  // The target does not keep its pixels (canvas textures start cleared every
  // frame), so damage-only frames need the last frame from elsewhere:
  //   * Copy-back (a target with CopySrc usage, such as the web canvas): the
  //     frame's one pass draws the last frame into the target, then the
  //     damage over it; afterwards the damage rects are copied from the
  //     target into `back`, which then holds this frame. One render pass and
  //     a full-frame read of `back`, like the direct frame's single pass.
  //   * Otherwise (or JSUI_WEBGPU_NO_CLEAR, which composites over the
  //     target): the damage is painted into a retained render target, a pass
  //     that loads and stores all of it, and one draw copies that into the
  //     target in a second pass. On a tiler (Apple GPUs) at 4K those two
  //     full-frame load / store passes cost more than a full repaint.
  //   After direct frames the last frame is made valid again once the damage
  //   stayed small for kRetainAfter frames: copy-back copies a full repaint
  //   of the target; the retained target is repainted fully (switching back
  //   costs a full repaint plus the copy, more than another direct frame).
  constexpr int kRetainAfter = 3, kFreeRetainedAfter = 300;
  const bool noClear = (t.flags & JSUI_WEBGPU_NO_CLEAR) != 0;
  const bool copyBack = !noClear && (texture.GetUsage() & wgpu::TextureUsage::CopySrc);
  SkImageInfo info = target->imageInfo();
  WebGPUGpu::Frames& f = g->framesOf(&rt);
  if (f.retained && (f.retained->width() != w || f.retained->height() != h ||
                      f.retained->imageInfo().colorType() != info.colorType() || copyBack))
    f.retained.reset();
  if (f.back && (int(f.back.GetWidth()) != w || int(f.back.GetHeight()) != h ||
                  f.back.GetFormat() != texture.GetFormat() || !copyBack)) {
    f.backImage.reset();
    f.back = nullptr;
  }
  if (t.flags & JSUI_WEBGPU_NO_RETAIN) f.drop();
  const bool haveLast = copyBack ? bool(f.backImage) : bool(f.retained);
  const void* key = haveLast ? reinterpret_cast<const void*>(uintptr_t(f.retainedGen)) : nullptr;
  bool viaRetained = false, failed = false;
  bool copyAll = false;  // copy-back: the whole target into `back` after the frame
  const bool opacityLayers = !(t.flags & JSUI_WEBGPU_NO_LAYER_CACHE), raster = !(t.flags & JSUI_WEBGPU_NO_RASTER_CACHE),
             backdrop = !(t.flags & JSUI_WEBGPU_NO_BACKDROP);
  f.layers->setModes(opacityLayers, raster, backdrop);
  jsui::TextureScope textureScope(&g->textures);
  jsui::LayerCacheScope layerScope(opacityLayers || raster || backdrop ? f.layers.get() : nullptr);
  rt.renderCanvasTracked(key, w, h, [&](const SkIRect&, bool keyValid) -> Surface::CanvasChoice {
    int64_t area = 0;
    for (SkRegion::Iterator it(rt.trackedDamage()); !it.done(); it.next())
      area += int64_t(it.rect().width()) * it.rect().height();
    bool large = area * 2 >= int64_t(w) * h;
    f.smallFrames = large ? 0 : f.smallFrames + 1;
    bool retain = !(t.flags & JSUI_WEBGPU_NO_RETAIN) && (keyValid ? !large : f.smallFrames >= kRetainAfter);
    if (!retain) {
      // A last frame unused for a while (a UI that animates all over) is
      // freed: a full-size texture of memory.
      if (++f.directFrames > kFreeRetainedAfter) f.drop();
      return {target->getCanvas(), nullptr, !noClear};
    }
    f.directFrames = 0;
    if (copyBack) {
      if (!f.back) {
        wgpu::TextureDescriptor desc;
        desc.size = {uint32_t(w), uint32_t(h), 1};
        desc.format = texture.GetFormat();
        desc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
        f.back = g->device.CreateTexture(&desc);
        f.backImage = f.back ? SkImages::WrapTexture(g->recorder.get(),
                                                       graphite::BackendTextures::MakeDawn(f.back.Get()),
                                                       kPremul_SkAlphaType, info.refColorSpace())
                               : nullptr;
        if (!f.backImage) {
          f.back = nullptr;
          return {target->getCanvas(), nullptr, true};  // direct this time
        }
        f.retainedGen = ++g->nextGen;
        keyValid = false;
      }
      viaRetained = true;
      Surface::CanvasChoice c{target->getCanvas(), reinterpret_cast<const void*>(uintptr_t(f.retainedGen)), true};
      if (keyValid) {
        // The last frame first (every pixel), then the damage over it.
        SkPaint paint;
        paint.setBlendMode(SkBlendMode::kSrc);
        target->getCanvas()->drawImage(f.backImage, 0, 0, SkSamplingOptions(), &paint);
        c.splitDamage = true;
      } else {
        copyAll = true;  // a full repaint (the runtime clears the target)
      }
      return c;
    }
    if (!f.retained) {
      SkImageInfo ri = SkImageInfo::Make(w, h, info.colorType(), kPremul_SkAlphaType, info.refColorSpace());
      f.retained = SkSurfaces::RenderTarget(g->recorder.get(), ri, skgpu::Mipmapped::kNo, &props);
      if (!f.retained) {
        failed = true;
        return {};
      }
      f.retainedGen = ++g->nextGen;
    }
    viaRetained = true;
    Surface::CanvasChoice c{f.retained->getCanvas(), reinterpret_cast<const void*>(uintptr_t(f.retainedGen)), true};
    c.splitDamage = true;
    return c;
  }, /*growCurves=*/false);
  f.layers->endFrame();
  if (failed) return -1;
  if (viaRetained && !copyBack) {
    SkPaint paint;
    paint.setBlendMode(noClear ? SkBlendMode::kSrcOver : SkBlendMode::kSrc);
    target->getCanvas()->drawImage(SkSurfaces::AsImage(f.retained), 0, 0, SkSamplingOptions(), &paint);
  }
  target.reset();
  {
    Perf::Scope gpuScope(rt.doc.perf, Phase::Gpu);
    if (!g->flush()) return -1;
  }
  rt.doc.perf.counters.pipelineCompiles += g->precompiler->takeCompiles();
  if (viaRetained && copyBack) {
    // After Graphite's submit (same queue): `back` becomes this frame.
    wgpu::CommandEncoder enc = g->device.CreateCommandEncoder();
    wgpu::TexelCopyTextureInfo src, dst;
    src.texture = texture;
    dst.texture = f.back;
    auto copy = [&](const SkIRect& r) {
      src.origin = dst.origin = {uint32_t(r.x()), uint32_t(r.y()), 0};
      wgpu::Extent3D size = {uint32_t(r.width()), uint32_t(r.height()), 1};
      enc.CopyTextureToTexture(&src, &dst, &size);
    };
    SkIRect all = SkIRect::MakeWH(w, h);
    if (copyAll) {
      copy(all);
    } else {
      // The rects the frame repainted (a few covering the damage region,
      // which can be dozens of bands), not the region's rects.
      for (SkIRect r : rt.trackedRects())
        if (r.intersect(all)) copy(r);
    }
    wgpu::CommandBuffer cb = enc.Finish();
    g->queue.Submit(1, &cb);
  }
  if (g->isLost()) return -1;
  g->precompiler->afterFrame();
  return 0;
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
int render(Surface&, JsuiGpuContext*, const JsuiWebGPUTarget&) { return -1; }
}  // namespace webgpu
}  // namespace jsui

#endif
