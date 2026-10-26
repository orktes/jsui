// Skia Graphite on an embedder's Vulkan, Metal or native Dawn device
// (jsui_gpu_create_vulkan, jsui_gpu_create_metal, jsui_gpu_create_dawn,
// jsui_render_gpu_target / jsui_render_vulkan in jsui.h).
//
// The embedder (e.g. Godot's RenderingDevice) owns the device, the queue and
// the target texture; this context only borrows them. Frames go through
// Surface::renderCanvasTracked: with JSUI_GPU_INCREMENTAL the target is the
// retained frame and only the damage bounds are repainted (clipped to that
// rect), otherwise the whole frame is. Raster images are uploaded once
// (CachingImageProvider, shared with the WebGPU backend).
//
// Vulkan synchronisation: each render submits, on the embedder's queue,
// [full barrier] [Skia's command buffer] [full barrier]. The barriers are
// global memory barriers (all commands, all memory accesses), recorded once
// into SIMULTANEOUS_USE command buffers, so the render is ordered after
// whatever the host submitted to the queue before (e.g. the previous frame
// still sampling the texture) and everything it submits afterwards sees the
// finished pixels, whatever stages / accesses it uses them with. Skia moves
// the image from `layout` to COLOR_ATTACHMENT and, through the recording's
// target state, to `final_layout` at the end.
//
// Dawn: everything goes through the embedder's WGPUQueue, whose submissions
// execute in order, so the render is ordered after what the host submitted
// before and before what it submits afterwards; Dawn tracks the texture's
// usage itself. Graphite waits for its own work (context teardown) by
// calling wgpuInstanceProcessEvents on the embedder's instance.
//
// Pipeline cache (pipeline_cache.h): Vulkan and Metal precompile on a worker
// thread. Graphite creates pipelines through its thread-safe resource
// provider (render passes, samplers; Metal's depth-stencil states exist up
// front), into a VkPipelineCache created without external synchronisation /
// on the MTLDevice, both safe to use from any thread. A Dawn device is
// thread safe only with ImplicitDeviceSynchronization (Dawn then locks it
// around every call); without it the cache precompiles in slices after
// renders, on the render thread (Graphite creates precompiled pipelines
// synchronously even where it creates the others asynchronously).
// The same thread-safe devices also get a small compile thread pool
// (ContextOptions::fExecutor): the new pipelines a frame needs compile in
// parallel, and precompiles queue behind them.
//
// Vulkan also keeps the driver's VkPipelineCache in a file (driver_cache.h,
// ContextOptions::fPersistentPipelineStorage): loaded when the context is
// made, saved by a thread of the context while pipelines appear, on
// jsui_gpu_purge and when the context is destroyed. The precompiles of the
// pipeline cache then hit the driver's cache rather than compile again.
#include "render/gpu.h"

#if defined(JSUI_VULKAN) || defined(JSUI_GRAPHITE_METAL) || defined(JSUI_GRAPHITE_DAWN)

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkExecutor.h"
#include "include/core/SkMilestone.h"
#include "include/core/SkSurface.h"
#include "include/gpu/MutableTextureState.h"
#include "include/gpu/graphite/BackendTexture.h"
#include "include/gpu/graphite/Context.h"
#include "include/gpu/graphite/ContextOptions.h"
#include "include/gpu/graphite/GraphiteTypes.h"
#include "include/gpu/graphite/Image.h"
#include "include/gpu/graphite/Recorder.h"
#include "include/gpu/graphite/Recording.h"
#include "include/gpu/graphite/Surface.h"
#include "render/driver_cache.h"
#include "render/gpu_layers.h"
#include "render/graphite_images.h"
#include "render/host_textures.h"
#include "render/painter.h"
#include "render/pipeline_cache.h"
#include "render/shader_view.h"
#include "runtime.h"

#if defined(JSUI_VULKAN)
#include "include/gpu/graphite/vk/VulkanGraphiteContext.h"
#include "include/gpu/graphite/vk/VulkanGraphiteTypes.h"
#include "include/gpu/vk/VulkanBackendContext.h"
#include "include/gpu/vk/VulkanExtensions.h"
#include "include/gpu/vk/VulkanMemoryAllocator.h"
#include "include/gpu/vk/VulkanMutableTextureState.h"
#include "src/gpu/GpuTypesPriv.h"  // skgpu::ThreadSafe
#include "src/gpu/vk/vulkanmemoryallocator/VulkanMemoryAllocatorPriv.h"
#if defined(_WIN32)
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#endif

#if defined(JSUI_GRAPHITE_METAL)
#include "include/gpu/graphite/mtl/MtlBackendContext.h"
#include "include/gpu/graphite/mtl/MtlGraphiteTypes_cpp.h"
#endif

#if defined(JSUI_GRAPHITE_DAWN)
#include <webgpu/webgpu_cpp.h>

#include "include/gpu/graphite/dawn/DawnBackendContext.h"
#include "include/gpu/graphite/dawn/DawnGraphiteTypes.h"
#if defined(JSUI_DAWN_DIRECT)
// Threaded WASM inside a program that links Emdawnwebgpu (godot-aw3's web
// export): wgpu* are ordinary functions of the same program, so there is no
// table to fill. The browser cannot block on the GPU, and the device belongs
// to the thread that drives the engine's renderer: Graphite gets no tick
// function and no compile threads (see makeDawn), as gpu_webgpu.cpp.
namespace {
int jsui_dawn_proc_count() { return 1; }
int jsui_dawn_fill_from_get_proc(void*) { return 1; }
int jsui_dawn_fill_from_process() { return 1; }
const char* jsui_dawn_header_revision() { return JSUI_DAWN_REVISION_STR; }
}  // namespace
#else
// tools/dawn-shim (generated): the table behind the wgpu* trampolines.
extern "C" {
int jsui_dawn_proc_count(void);
int jsui_dawn_fill_from_get_proc(void* get_proc);
int jsui_dawn_fill_from_process(void);
const char* jsui_dawn_header_revision(void);
}
#endif
#endif

namespace graphite = skgpu::graphite;

namespace {

using jsui::graphite_support::CachingImageProvider;
using jsui::graphite_support::kDefaultCacheBytes;
using jsui::pipeline_cache::Precompiler;

enum class Api { Vulkan, Metal, Dawn };

#if defined(JSUI_VULKAN)
// The few Vulkan entry points this file calls itself (Skia loads its own).
struct VkFns {
  PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
  PFN_vkGetDeviceProcAddr getDeviceProcAddr = nullptr;
  PFN_vkCreateCommandPool createCommandPool = nullptr;
  PFN_vkDestroyCommandPool destroyCommandPool = nullptr;
  PFN_vkAllocateCommandBuffers allocateCommandBuffers = nullptr;
  PFN_vkBeginCommandBuffer beginCommandBuffer = nullptr;
  PFN_vkEndCommandBuffer endCommandBuffer = nullptr;
  PFN_vkCmdPipelineBarrier cmdPipelineBarrier = nullptr;
  PFN_vkQueueSubmit queueSubmit = nullptr;
  PFN_vkQueueWaitIdle queueWaitIdle = nullptr;
  PFN_vkGetPhysicalDeviceProperties getPhysicalDeviceProperties = nullptr;
};

PFN_vkGetInstanceProcAddr loaderGetInstanceProcAddr() {
  static PFN_vkGetInstanceProcAddr fn = [] {
    void* sym = nullptr;
#if defined(_WIN32)
    HMODULE lib = GetModuleHandleA("vulkan-1.dll");
    if (!lib) lib = LoadLibraryA("vulkan-1.dll");
    if (lib) sym = reinterpret_cast<void*>(GetProcAddress(lib, "vkGetInstanceProcAddr"));
#else
    // Engines load the loader themselves (Godot: volk on libvulkan.so.1);
    // the same library hands out the same dispatchable handles.
    sym = dlsym(RTLD_DEFAULT, "vkGetInstanceProcAddr");
    const char* names[] = {
#if defined(__APPLE__)
        "libvulkan.1.dylib", "libvulkan.dylib", "libMoltenVK.dylib",
#else
        "libvulkan.so.1", "libvulkan.so",
#endif
    };
    for (const char* n : names) {
      if (sym) break;
      if (void* lib = dlopen(n, RTLD_NOW | RTLD_LOCAL)) sym = dlsym(lib, "vkGetInstanceProcAddr");
    }
#endif
    return reinterpret_cast<PFN_vkGetInstanceProcAddr>(sym);
  }();
  return fn;
}
#endif

#if defined(JSUI_GRAPHITE_METAL)
constexpr unsigned kMTLPixelFormatRGBA8Unorm = 70;
constexpr unsigned kMTLPixelFormatRGBA8Unorm_sRGB = 71;
constexpr unsigned kMTLPixelFormatBGRA8Unorm = 80;
constexpr unsigned kMTLPixelFormatBGRA8Unorm_sRGB = 81;
#endif

struct GraphiteGpu final : JsuiGpuContext {
  explicit GraphiteGpu(Api a) : JsuiGpuContext(Kind::Graphite), api(a) {}
  ~GraphiteGpu() override { release(); }

  const Api api;
  // Outlives ctx (its pipeline callback).
  std::unique_ptr<Precompiler> precompiler = std::make_unique<Precompiler>(
      api == Api::Vulkan  ? jsui::pipeline_cache::Backend::Vulkan
      : api == Api::Metal ? jsui::pipeline_cache::Backend::Metal
#if defined(__EMSCRIPTEN__)
                          : jsui::pipeline_cache::Backend::BrowserWebGPU);
#else
                          : jsui::pipeline_cache::Backend::Dawn);
#endif
  // Outlives ctx too (Graphite's compile tasks; see makeCompileExecutor).
  std::unique_ptr<SkExecutor> compileExecutor;
#if defined(JSUI_VULKAN)
  // And this (ContextOptions::fPersistentPipelineStorage); Vulkan only.
  std::unique_ptr<jsui::driver_cache::Storage> driverStorage;
#endif
  std::unique_ptr<graphite::Context> ctx;
  std::unique_ptr<graphite::Recorder> recorder;
  std::unique_ptr<graphite::Recorder> warmRecorder;  // warmShaders' (never played back)
  sk_sp<CachingImageProvider> images;
  size_t cacheBytes = kDefaultCacheBytes;
  // Composited opacity layers, raster-cached subtrees and the static
  // backdrop (gpu_layers.h), per surface; textures of `recorder`.
  jsui::GpuLayerCaches layers{kDefaultCacheBytes};
  // Host textures drawn in this context's frames (wrapTexture).
  jsui::TextureWrapper textures{[this](const JsuiTexture& t) { return wrapTexture(t); }};

  // Render targets the host keeps across frames (JSUI_GPU_INCREMENTAL), as
  // wrapped in an earlier render: wrapping one is a Graphite texture with
  // its views, a Device and an SkSurface, made and dropped again every frame
  // (in the browser each view is a WebGPU call through JS). Dawn and Metal
  // only: their wrappers hold a reference to the texture, so a handle found
  // here is still the same texture (a VkImage handle may be reused).
  struct Target {
    const void* handle = nullptr;
    int width = 0, height = 0;
    unsigned format = 0;
    uint64_t lastUse = 0;
    sk_sp<SkSurface> surface;
  };
  static constexpr size_t kMaxTargets = 4;
  static constexpr uint64_t kTargetIdleRenders = 120;  // a target unused this long goes (and its texture with it)
  std::vector<Target> targets;
  uint64_t renders = 0;

  // The surface wrapping t's texture in an earlier render, if it is still
  // usable for t; drops what the host no longer keeps.
  sk_sp<SkSurface> findTarget(const JsuiGpuTarget& t) {
    renders++;
    sk_sp<SkSurface> found;
    for (size_t i = 0; i < targets.size();) {
      Target& e = targets[i];
      const bool same = e.handle == t.texture;
      if (same && (t.flags & JSUI_GPU_INCREMENTAL) && e.width == t.width && e.height == t.height &&
          e.format == t.format) {
        e.lastUse = renders;
        found = e.surface;
      } else if (same || e.lastUse + kTargetIdleRenders < renders) {
        targets.erase(targets.begin() + long(i));
        continue;
      }
      i++;
    }
    if (found) found->getCanvas()->restoreToCount(1);
    return found;
  }

  void keepTarget(const JsuiGpuTarget& t, const sk_sp<SkSurface>& surface) {
    if (!(t.flags & JSUI_GPU_INCREMENTAL) || api == Api::Vulkan) return;
    if (targets.size() >= kMaxTargets) {
      auto oldest = std::min_element(targets.begin(), targets.end(),
                                     [](const Target& a, const Target& b) { return a.lastUse < b.lastUse; });
      targets.erase(oldest);
    }
    targets.push_back(Target{t.texture, t.width, t.height, t.format, renders, surface});
  }

#if defined(JSUI_VULKAN)
  VkFns vk;
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t queueFamily = 0;
  std::unique_ptr<skgpu::VulkanExtensions> extensions;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer barrier = VK_NULL_HANDLE;  // one global barrier, submitted before and after
  bool submittedBarrier = false;
#endif
#if defined(JSUI_GRAPHITE_DAWN)
  wgpu::Instance dawnInstance;  // references held for the context's lifetime
  wgpu::Device dawnDevice;
  wgpu::Queue dawnQueue;
#endif

  Precompiler::Mode precompileMode() {
#if defined(JSUI_GRAPHITE_DAWN)
#if defined(__EMSCRIPTEN__)
    // The browser's device is used from one thread only (other threads
    // would each need their own import of it).
    if (api == Api::Dawn) return Precompiler::Mode::Sliced;
#else
    if (api == Api::Dawn)
      return dawnDevice.HasFeature(wgpu::FeatureName::ImplicitDeviceSynchronization) ? Precompiler::Mode::Thread
                                                                                     : Precompiler::Mode::Sliced;
#endif
#endif
    return Precompiler::Mode::Thread;
  }

  // Before the Context is made: the pipeline cache's callback and, where
  // the device may be used from other threads, parallel pipeline compiles.
  void configure(graphite::ContextOptions& opts) {
    precompiler->configure(opts);
#if SK_MILESTONE >= 140
    // Not when the tests force sliced precompiles: those compile on the
    // render thread, where a pool would make them asynchronous.
    if (precompileMode() == Precompiler::Mode::Thread && !jsui::pipeline_cache::forceSliced()) {
      compileExecutor = jsui::pipeline_cache::makeCompileExecutor();
      opts.fExecutor = compileExecutor.get();
    }
#endif
  }

  // Preloaded shaders (precompileShaders): with the compile pool, their
  // warm-up draws go to a recorder of their own that is snapped and never
  // played back. Snapping prepares the draws, which hands their pipelines to
  // the pool; the frames carry on, and the one that first draws a shader
  // finds its pipelines built (or waits for the rest of a compile already
  // under way). Without the pool the frames warm them one per frame
  // (ShaderEffects::warm), each frame building one pipeline itself.
  void warmShaders(jsui::ShaderEffects& shaders, SkColorType ct, const SkSurfaceProps& props) {
    if (!compileExecutor || !shaders.warmPending()) return;
    if (!warmRecorder) {
      // Its own image provider: the warm-up's image children are raster
      // images, uploaded through it (Graphite's default provider uploads
      // nothing, and the draw would be dropped).
      graphite::RecorderOptions ropts;
      ropts.fImageProvider = sk_make_sp<CachingImageProvider>();
      warmRecorder = ctx->makeRecorder(ropts);
    }
    if (!warmRecorder) return;
    sk_sp<SkSurface> s = SkSurfaces::RenderTarget(
        warmRecorder.get(), SkImageInfo::Make(64, 64, ct, kPremul_SkAlphaType), skgpu::Mipmapped::kNo, &props);
    if (!s) return;
    shaders.warmAll(s->getCanvas());
    s.reset();
    warmRecorder->snap();  // dropped: only its pipelines were wanted
  }

  // A host texture of this context's device, sampled in place.
  sk_sp<SkImage> wrapTexture(const JsuiTexture& t) {
    if (!recorder) return nullptr;
    const SkISize size = SkISize::Make(t.width, t.height);
    SkColorType ct = kRGBA_8888_SkColorType;
    graphite::BackendTexture bt;
    if (api == Api::Vulkan && t.kind == JSUI_TEXTURE_VULKAN) {
#if defined(JSUI_VULKAN)
      VkFormat vf = t.format ? VkFormat(t.format) : VK_FORMAT_R8G8B8A8_UNORM;
      if (vf == VK_FORMAT_B8G8R8A8_UNORM) ct = kBGRA_8888_SkColorType;
      else if (vf != VK_FORMAT_R8G8B8A8_UNORM) return nullptr;
      graphite::VulkanTextureInfo info;
      info.fSampleCount = graphite::SampleCount::k1;
      info.fMipmapped = skgpu::Mipmapped::kNo;
      info.fFormat = vf;
      info.fImageTiling = VK_IMAGE_TILING_OPTIMAL;
      info.fImageUsageFlags = t.usage ? VkImageUsageFlags(t.usage)
                                      : VkImageUsageFlags(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                                          VK_IMAGE_USAGE_TRANSFER_DST_BIT);
      info.fSharingMode = VK_SHARING_MODE_EXCLUSIVE;
      info.fAspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      // Host textures are in SHADER_READ_ONLY_OPTIMAL whenever a render runs
      // (jsui.h), and sampling leaves them there.
      bt = graphite::BackendTextures::MakeVulkan(size, info, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, queueFamily,
                                                 static_cast<VkImage>(t.handle), skgpu::VulkanAlloc());
#endif
    } else if (api == Api::Metal && t.kind == JSUI_TEXTURE_METAL) {
#if defined(JSUI_GRAPHITE_METAL)
      if (t.format == kMTLPixelFormatBGRA8Unorm) ct = kBGRA_8888_SkColorType;
      else if (t.format && t.format != kMTLPixelFormatRGBA8Unorm) return nullptr;
      bt = graphite::BackendTextures::MakeMetal(size, static_cast<CFTypeRef>(t.handle));
#endif
    } else if (api == Api::Dawn && t.kind == JSUI_TEXTURE_WEBGPU) {
#if defined(JSUI_GRAPHITE_DAWN)
      wgpu::Texture texture(static_cast<WGPUTexture>(t.handle));
      wgpu::TextureFormat f = t.format ? wgpu::TextureFormat(t.format) : texture.GetFormat();
      if (f == wgpu::TextureFormat::BGRA8Unorm) ct = kBGRA_8888_SkColorType;
      else if (f != wgpu::TextureFormat::RGBA8Unorm) return nullptr;
      bt = graphite::BackendTextures::MakeDawn(texture.Get());
#endif
    }
    if (!bt.isValid()) return nullptr;
    return SkImages::WrapTexture(recorder.get(), bt, ct, jsui::textureAlphaType(t), nullptr);
  }

  bool finishInit(const graphite::ContextOptions& opts) {
    (void)opts;
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
    precompiler->attach(ctx.get(), precompileMode(), compileExecutor.get());
    pipelines = precompiler.get();
#if defined(JSUI_VULKAN)
    if (driverStorage) {
      Precompiler* p = precompiler.get();
      driverStorage->start(ctx.get(), [p] { return p->pipelinesCreated(); });
      driverCache = driverStorage.get();
    }
#endif
    return true;
  }

  void release() {
    targets.clear();  // before the recorder
    layers.clear();
    if (precompiler) precompiler->shutdown();  // its PrecompileContext holds the device
    if (images) images->clear();
    textures.clear();
    warmRecorder.reset();
    recorder.reset();
    if (ctx) {
#if defined(__EMSCRIPTEN__)
      // No tick function: Graphite cannot wait for the GPU here (it aborts
      // if work is outstanding when it is destroyed). Collect what finished;
      // if some is still in flight, leak the context rather than abort (its
      // GPU objects go with the device).
      ctx->checkAsyncWorkCompletion();
      if (ctx->hasUnfinishedGpuWork() && !ctx->isDeviceLost()) {
        SkDebugf("jsui: WebGPU context destroyed with GPU work in flight; leaking it\n");
        (void)ctx.release();
        return;
      }
#else
      // Wait for everything submitted (Graphite needs its work finished
      // before it frees its resources).
      ctx->submit(graphite::SyncToCpu::kYes);
#endif
#if defined(JSUI_VULKAN)
      // The last save (the device is still the host's to destroy after this).
      if (driverStorage) driverStorage->finish();
      driverCache = nullptr;
#endif
      ctx.reset();
    }
#if defined(JSUI_VULKAN)
    if (pool != VK_NULL_HANDLE) {
      // The barrier command buffer may still be pending after the last render.
      if (submittedBarrier && vk.queueWaitIdle) vk.queueWaitIdle(queue);
      vk.destroyCommandPool(device, pool, nullptr);
      pool = VK_NULL_HANDLE;
    }
#endif
  }

  bool flush(SkSurface* target, skgpu::MutableTextureState* finalState) {
    std::unique_ptr<graphite::Recording> recording = recorder->snap();
    if (!recording) {
      SkDebugf("jsui: Graphite could not snap the frame's recording\n");
      return false;
    }
    graphite::InsertRecordingInfo info;
    info.fRecording = recording.get();
    info.fTargetSurface = finalState ? target : nullptr;
    info.fTargetTextureState = finalState;
    info.fFinishedContext = this;
    info.fFinishedProc = onFinished;
    graphite::InsertStatus status = ctx->insertRecording(info);
    if (status != graphite::InsertStatus::kSuccess) {
      SkDebugf("jsui: Graphite rejected the frame's recording (InsertStatus %d%s%s)\n",
               int(graphite::InsertStatus::V(status)), status.message().empty() ? "" : ": ", status.message().c_str());
      return false;
    }
    bool ok = ctx->submit(graphite::SyncToCpu::kNo);
    ctx->checkAsyncWorkCompletion();
    images->endFrame();
    textures.endFrame();
    return ok;
  }

  // Graphite's verdict on a frame's command buffer: kFailed when it could not
  // be recorded or submitted (in m157 a submitted one is reported as a
  // success whatever the GPU did with it; a lost Vulkan device also shows in
  // isDeviceLost). Runs from submit / checkAsyncWorkCompletion or, at the
  // latest, the context's teardown in release(), so `this` is alive.
  uint32_t failedFrames = 0;
  static void onFinished(graphite::GpuFinishedContext context, skgpu::CallbackResult result) {
    if (result == skgpu::CallbackResult::kSuccess) return;
    auto* g = static_cast<GraphiteGpu*>(context);
    g->failedFrames++;
    if (g->failedFrames <= 3 || g->failedFrames % 100 == 0)
      SkDebugf("jsui: a frame's GPU work failed to submit (%u so far)\n", g->failedFrames);
  }

#if defined(JSUI_VULKAN)
  bool initBarrier() {
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = queueFamily;
    if (vk.createCommandPool(device, &pci, nullptr, &pool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vk.allocateCommandBuffers(device, &ai, &barrier) != VK_SUCCESS) return false;
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
    if (vk.beginCommandBuffer(barrier, &bi) != VK_SUCCESS) return false;
    VkMemoryBarrier mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vk.cmdPipelineBarrier(barrier, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                          nullptr, 0, nullptr);
    return vk.endCommandBuffer(barrier) == VK_SUCCESS;
  }

  bool submitBarrier() {
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &barrier;
    if (vk.queueSubmit(queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) return false;
    submittedBarrier = true;
    return true;
  }
#endif
};

GraphiteGpu* asGraphite(JsuiGpuContext* g) {
  return g && g->kind == JsuiGpuContext::Kind::Graphite ? static_cast<GraphiteGpu*>(g) : nullptr;
}

#if defined(JSUI_VULKAN)
// VkFormat values JSUI accepts and the matching Skia colour types.
bool vkColorType(unsigned format, VkFormat* vf, sk_sp<SkColorSpace>* cs) {
  switch (format) {
    case 0:
    case VK_FORMAT_R8G8B8A8_UNORM: *vf = VK_FORMAT_R8G8B8A8_UNORM; return true;
    case VK_FORMAT_B8G8R8A8_UNORM: *vf = VK_FORMAT_B8G8R8A8_UNORM; return true;
    // sRGB targets encode on write: blending happens in linear light.
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_SRGB:
      *vf = VkFormat(format);
      *cs = SkColorSpace::MakeSRGB();
      return true;
    default: return false;
  }
}
#endif

#if defined(JSUI_GRAPHITE_METAL)
bool mtlColorSpace(unsigned format, sk_sp<SkColorSpace>* cs) {
  switch (format) {
    case 0:
    case kMTLPixelFormatRGBA8Unorm:
    case kMTLPixelFormatBGRA8Unorm: return true;
    case kMTLPixelFormatRGBA8Unorm_sRGB:
    case kMTLPixelFormatBGRA8Unorm_sRGB: *cs = SkColorSpace::MakeSRGB(); return true;
    default: return false;
  }
}
#endif

#if defined(JSUI_GRAPHITE_DAWN)
// WGPUTextureFormat values JSUI accepts (0: the texture's own format).
bool dawnColorSpace(wgpu::TextureFormat format, sk_sp<SkColorSpace>* cs) {
  switch (format) {
    case wgpu::TextureFormat::RGBA8Unorm:
    case wgpu::TextureFormat::BGRA8Unorm: return true;
    case wgpu::TextureFormat::RGBA8UnormSrgb:
    case wgpu::TextureFormat::BGRA8UnormSrgb: *cs = SkColorSpace::MakeSRGB(); return true;
    default: return false;
  }
}
#endif

}  // namespace

namespace jsui {
namespace graphite_gpu {

bool available() { return true; }

JsuiGpuContext* createVulkan(const JsuiVulkanDevice& d) {
#if defined(JSUI_VULKAN)
  if (!d.instance || !d.physical_device || !d.device || !d.queue) return nullptr;
  auto g = std::make_unique<GraphiteGpu>(Api::Vulkan);
  g->instance = static_cast<VkInstance>(d.instance);
  g->physicalDevice = static_cast<VkPhysicalDevice>(d.physical_device);
  g->device = static_cast<VkDevice>(d.device);
  g->queue = static_cast<VkQueue>(d.queue);
  g->queueFamily = d.queue_family;
  VkFns& vk = g->vk;
  vk.getInstanceProcAddr = d.get_instance_proc_addr ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(d.get_instance_proc_addr)
                                                    : loaderGetInstanceProcAddr();
  if (!vk.getInstanceProcAddr) return nullptr;
  VkInstance inst = g->instance;
  VkDevice dev = g->device;
  vk.getDeviceProcAddr =
      reinterpret_cast<PFN_vkGetDeviceProcAddr>(vk.getInstanceProcAddr(inst, "vkGetDeviceProcAddr"));
  if (!vk.getDeviceProcAddr) return nullptr;
  auto devFn = [&](const char* n) { return vk.getDeviceProcAddr(dev, n); };
  vk.createCommandPool = reinterpret_cast<PFN_vkCreateCommandPool>(devFn("vkCreateCommandPool"));
  vk.destroyCommandPool = reinterpret_cast<PFN_vkDestroyCommandPool>(devFn("vkDestroyCommandPool"));
  vk.allocateCommandBuffers = reinterpret_cast<PFN_vkAllocateCommandBuffers>(devFn("vkAllocateCommandBuffers"));
  vk.beginCommandBuffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(devFn("vkBeginCommandBuffer"));
  vk.endCommandBuffer = reinterpret_cast<PFN_vkEndCommandBuffer>(devFn("vkEndCommandBuffer"));
  vk.cmdPipelineBarrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(devFn("vkCmdPipelineBarrier"));
  vk.queueSubmit = reinterpret_cast<PFN_vkQueueSubmit>(devFn("vkQueueSubmit"));
  vk.queueWaitIdle = reinterpret_cast<PFN_vkQueueWaitIdle>(devFn("vkQueueWaitIdle"));
  vk.getPhysicalDeviceProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
      vk.getInstanceProcAddr(inst, "vkGetPhysicalDeviceProperties"));
  if (!vk.createCommandPool || !vk.destroyCommandPool || !vk.allocateCommandBuffers || !vk.beginCommandBuffer ||
      !vk.endCommandBuffer || !vk.cmdPipelineBarrier || !vk.queueSubmit || !vk.queueWaitIdle ||
      !vk.getPhysicalDeviceProperties)
    return nullptr;

  VkPhysicalDeviceProperties props{};
  vk.getPhysicalDeviceProperties(g->physicalDevice, &props);
  uint32_t api = d.api_version ? d.api_version : VK_API_VERSION_1_1;
  if (props.apiVersion < api) api = props.apiVersion;
  if (api < VK_API_VERSION_1_1) return nullptr;  // Graphite's minimum

  PFN_vkGetInstanceProcAddr gipa = vk.getInstanceProcAddr;
  PFN_vkGetDeviceProcAddr gdpa = vk.getDeviceProcAddr;
  skgpu::VulkanGetProc getProc = [gipa, gdpa](const char* name, VkInstance i, VkDevice dv) -> PFN_vkVoidFunction {
    if (dv != VK_NULL_HANDLE) return gdpa(dv, name);
    return gipa(i, name);
  };
  g->extensions = std::make_unique<skgpu::VulkanExtensions>();
  g->extensions->init(getProc, g->instance, g->physicalDevice, uint32_t(std::max(0, d.instance_extension_count)),
                      d.instance_extensions, uint32_t(std::max(0, d.device_extension_count)), d.device_extensions);

  skgpu::VulkanBackendContext backend;
  backend.fInstance = g->instance;
  backend.fPhysicalDevice = g->physicalDevice;
  backend.fDevice = g->device;
  backend.fQueue = g->queue;
  backend.fGraphicsQueueIndex = g->queueFamily;
  backend.fMaxAPIVersion = api;
  backend.fVkExtensions = g->extensions.get();
  backend.fDeviceFeatures2 = static_cast<const VkPhysicalDeviceFeatures2*>(d.device_features2);
  backend.fGetProc = getProc;
  backend.fMemoryAllocator = skgpu::VulkanMemoryAllocators::Make(backend, skgpu::ThreadSafe::kNo);
  if (!backend.fMemoryAllocator) return nullptr;

  graphite::ContextOptions opts;
  g->configure(opts);
  // The driver's pipeline cache of an earlier launch on this device and
  // driver (driver_cache.h), read by Graphite while it makes the context.
  jsui::driver_cache::DeviceId id;
  id.vendorID = props.vendorID;
  id.deviceID = props.deviceID;
  id.driverVersion = props.driverVersion;
  static_assert(sizeof(id.uuid) == VK_UUID_SIZE, "pipelineCacheUUID");
  std::memcpy(id.uuid, props.pipelineCacheUUID, VK_UUID_SIZE);
  if (std::string file = jsui::driver_cache::defaultFile(id); !file.empty()) {
    g->driverStorage = std::make_unique<jsui::driver_cache::Storage>(std::move(file), id);
    opts.fPersistentPipelineStorage = g->driverStorage.get();
  }
  g->ctx = graphite::ContextFactory::MakeVulkan(backend, opts);
  if (!g->finishInit(opts)) return nullptr;
  if (!g->initBarrier()) return nullptr;
  return g.release();
#else
  (void)d;
  return nullptr;
#endif
}

JsuiGpuContext* createMetal(void* device, void* queue) {
#if defined(JSUI_GRAPHITE_METAL)
  if (!device || !queue) return nullptr;
  auto g = std::make_unique<GraphiteGpu>(Api::Metal);
  graphite::MtlBackendContext backend;
  backend.fDevice.retain(static_cast<CFTypeRef>(device));
  backend.fQueue.retain(static_cast<CFTypeRef>(queue));
  graphite::ContextOptions opts;
  g->configure(opts);
  g->ctx = graphite::ContextFactory::MakeMetal(backend, opts);
  if (!g->finishInit(opts)) return nullptr;
  return g.release();
#else
  (void)device;
  (void)queue;
  return nullptr;
#endif
}

#if defined(JSUI_GRAPHITE_DAWN)
namespace {
// Graphite on the host's Dawn objects; the shim's table must be complete.
JsuiGpuContext* makeDawn(void* instance, void* device, void* queue) {
  auto g = std::make_unique<GraphiteGpu>(Api::Dawn);
  // The C++ wrappers add a reference to handles they are constructed from.
  g->dawnInstance = wgpu::Instance(static_cast<WGPUInstance>(instance));
  g->dawnDevice = wgpu::Device(static_cast<WGPUDevice>(device));
  g->dawnQueue = queue ? wgpu::Queue(static_cast<WGPUQueue>(queue)) : g->dawnDevice.GetQueue();
  graphite::DawnBackendContext backend;
  backend.fInstance = g->dawnInstance;
  backend.fDevice = g->dawnDevice;
  backend.fQueue = g->dawnQueue;
  graphite::ContextOptions opts;
#if defined(__EMSCRIPTEN__)
  backend.fTick = nullptr;  // the browser cannot block on the GPU
  // No internal MSAA: browser WebGPU lacks what makes it cheap (see
  // gpu_webgpu.cpp); paths go through the path atlas.
  opts.fInternalMultisampleCount = graphite::SampleCount::k1;
#else
  backend.fTick = graphite::DawnNativeProcessEventsFunction;
#endif
  g->configure(opts);
  g->ctx = graphite::ContextFactory::MakeDawn(backend, opts);
  if (!g->finishInit(opts)) return nullptr;
  return g.release();
}
}  // namespace
#endif

JsuiGpuContext* createDawn(void* instance, void* device, void* queue) {
#if defined(JSUI_GRAPHITE_DAWN)
  // Only where the host process exports every Dawn entry point.
  if (!instance || !device || jsui_dawn_fill_from_process() != jsui_dawn_proc_count()) return nullptr;
  return makeDawn(instance, device, queue);
#else
  (void)instance;
  (void)device;
  (void)queue;
  return nullptr;
#endif
}

JsuiGpuContext* createDawnProcs(void* getProcAddress, void* instance, void* device, void* queue) {
#if defined(JSUI_GRAPHITE_DAWN)
#if defined(JSUI_DAWN_DIRECT)
  (void)getProcAddress;  // linked directly; nothing to resolve
  if (!device) return nullptr;
#else
  if (!getProcAddress || !instance || !device) return nullptr;
#endif
  const int resolved = jsui_dawn_fill_from_get_proc(getProcAddress);
  if (resolved != jsui_dawn_proc_count()) {
    SkDebugf("jsui: the host's wgpuGetProcAddress resolved %d of %d Dawn entry points (JSUI is built against Dawn %s)\n",
             resolved, jsui_dawn_proc_count(), jsui_dawn_header_revision());
    return nullptr;
  }
  return makeDawn(instance, device, queue);
#else
  (void)getProcAddress;
  (void)instance;
  (void)device;
  (void)queue;
  return nullptr;
#endif
}

const char* dawnRevision() {
#if defined(JSUI_GRAPHITE_DAWN)
  return jsui_dawn_header_revision();
#else
  return nullptr;
#endif
}

bool owns(JsuiGpuContext* gpu) { return asGraphite(gpu) != nullptr; }

void destroy(JsuiGpuContext* gpu) { delete asGraphite(gpu); }

void purge(JsuiGpuContext* gpu, size_t cacheBytes) {
  GraphiteGpu* g = asGraphite(gpu);
  if (!g || !g->ctx) return;
  if (cacheBytes) {
    g->cacheBytes = cacheBytes;
    g->ctx->setMaxBudgetedBytes(cacheBytes);
    g->images->setBudget(cacheBytes / 2);
    g->layers.setBudget(cacheBytes);
  }
  g->images->clear();
  g->layers.clear();
  g->targets.clear();
  g->recorder->freeGpuResources();
  g->ctx->freeGpuResources();
#if defined(JSUI_VULKAN)
  // Hosts purge when the app goes to the background, where it may be
  // killed without a shutdown: save the driver's cache now.
  if (g->driverStorage) g->driverStorage->sync();
#endif
}

namespace {

// Whether a render into `t` repaints only the damage (`*key`: the retained
// target) and clears first; `discard`: the target's contents are undefined.
void retainedTarget(Api api, const JsuiGpuTarget& t, const void** key, bool* clear, bool* discard) {
  *discard = false;
#if defined(JSUI_VULKAN)
  if (api == Api::Vulkan) *discard = VkImageLayout(t.layout) == VK_IMAGE_LAYOUT_UNDEFINED;
#else
  (void)api;
#endif
  const bool incremental = (t.flags & JSUI_GPU_INCREMENTAL) && !*discard;
  *key = incremental ? t.texture : nullptr;
  *clear = !(t.flags & JSUI_GPU_NO_CLEAR) || *discard;
}

// The frame for target `t`, as the render below chooses it: always into the
// target; it holds just our frame afterwards (so the next frame may be
// incremental) unless the frame was composited over it. Scattered damage
// (several small animations) as a few scissored passes rather than their
// bounding box (CanvasChoice::splitDamage).
Surface::CanvasChoice targetChoice(SkCanvas* canvas, const JsuiGpuTarget& t, bool clear) {
  Surface::CanvasChoice c{canvas, clear ? t.texture : nullptr, clear};
  c.splitDamage = true;
  return c;
}

}  // namespace

int record(Surface& rt, JsuiGpuContext* gpu, const JsuiGpuTarget& t, std::unique_ptr<Surface::RecordedFrame>* out) {
  out->reset();
  // Only what never changes after creation is read here (this may be
  // another thread than the context's).
  GraphiteGpu* g = asGraphite(gpu);
  if (!g || !g->ctx || !t.texture || t.width <= 0 || t.height <= 0) return -1;
  const void* key;
  bool clear, discard;
  retainedTarget(g->api, t, &key, &clear, &discard);
  *out = rt.recordFrame(key, t.width, t.height, targetChoice(nullptr, t, clear), /*growCurves=*/true, /*gpu=*/true);
  if ((*out)->painted.isEmpty() && key) {
    out->reset();
    return 0;  // the target holds the frame already
  }
  return 1;
}

int render(Surface& rt, JsuiGpuContext* gpu, const JsuiGpuTarget& t, JsuiRect* dirty, Surface::RecordedFrame* frame) {
  if (dirty) *dirty = JsuiRect{0, 0, 0, 0};
  GraphiteGpu* g = asGraphite(gpu);
  if (!g || !g->ctx || !t.texture || t.width <= 0 || t.height <= 0) return -1;
  if (g->ctx->isDeviceLost()) {
    g->layers.clear();
    return -1;
  }
  const SkISize size = SkISize::Make(t.width, t.height);
  sk_sp<SkColorSpace> cs;
  graphite::BackendTexture backendTex;
  bool discard = false;      // the target's contents are undefined: full repaint
  bool needsTransition = false;  // Vulkan: `layout` != the final layout
  std::unique_ptr<skgpu::MutableTextureState> finalState;
  SkSurfaceProps props = jsui::surfaceProps();
  sk_sp<SkSurface> surface = g->api == Api::Vulkan ? nullptr : g->findTarget(t);

  if (surface) {
    // Wrapped in an earlier render (GraphiteGpu::targets).
  } else if (g->api == Api::Vulkan) {
#if defined(JSUI_VULKAN)
    VkFormat vf;
    if (!vkColorType(t.format, &vf, &cs)) return -1;
    VkImageUsageFlags usage =
        t.usage ? VkImageUsageFlags(t.usage)
                : VkImageUsageFlags(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    // Graphite renders only into colour attachments that are also input
    // attachments (it reads the destination through them for some blends).
    constexpr VkImageUsageFlags kNeeded = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    if ((usage & kNeeded) != kNeeded) {
      SkDebugf("jsui: Vulkan target needs COLOR_ATTACHMENT and INPUT_ATTACHMENT usage\n");
      return -1;
    }
    graphite::VulkanTextureInfo info;
    info.fSampleCount = graphite::SampleCount::k1;
    info.fMipmapped = skgpu::Mipmapped::kNo;
    info.fFormat = vf;
    info.fImageTiling = VK_IMAGE_TILING_OPTIMAL;
    info.fImageUsageFlags = usage;
    info.fSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.fAspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageLayout layout = VkImageLayout(t.layout);
    discard = layout == VK_IMAGE_LAYOUT_UNDEFINED;
    backendTex = graphite::BackendTextures::MakeVulkan(size, info, layout, g->queueFamily,
                                                       static_cast<VkImage>(t.texture), skgpu::VulkanAlloc());
    VkImageLayout finalLayout =
        t.final_layout ? VkImageLayout(t.final_layout) : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    needsTransition = finalLayout != layout;
    finalState = std::make_unique<skgpu::MutableTextureState>(
        skgpu::MutableTextureStates::MakeVulkan(finalLayout, g->queueFamily));
#else
    return -1;
#endif
  } else if (g->api == Api::Dawn) {
#if defined(JSUI_GRAPHITE_DAWN)
    wgpu::Texture texture(static_cast<WGPUTexture>(t.texture));
    if (!dawnColorSpace(t.format ? wgpu::TextureFormat(t.format) : texture.GetFormat(), &cs)) return -1;
    // Graphite renders into RenderAttachment textures; it reads the
    // destination back (some blends) through TextureBinding or CopySrc.
    if ((texture.GetUsage() & wgpu::TextureUsage::RenderAttachment) == wgpu::TextureUsage::None) {
      SkDebugf("jsui: Dawn target needs RenderAttachment usage\n");
      return -1;
    }
    backendTex = graphite::BackendTextures::MakeDawn(texture.Get());
#else
    return -1;
#endif
  } else {
#if defined(JSUI_GRAPHITE_METAL)
    if (!mtlColorSpace(t.format, &cs)) return -1;
    backendTex = graphite::BackendTextures::MakeMetal(size, static_cast<CFTypeRef>(t.texture));
#else
    return -1;
#endif
  }
  if (!surface) {
    if (!backendTex.isValid()) return -1;
    surface = SkSurfaces::WrapBackendTexture(g->recorder.get(), backendTex, cs, &props);
    if (!surface) return -1;
    g->keepTarget(t, surface);
  }
  if (rt.rt.shaders) g->warmShaders(*rt.rt.shaders, surface->imageInfo().colorType(), props);
  // The first frame on a target format hands the common pipelines to the
  // pool (pipeline_cache.h); this frame's own go ahead of them.
  g->precompiler->precompileCommon(surface->imageInfo().colorType(), surface->imageInfo().refColorSpace());

  // The target is the retained frame when the host says it still holds our
  // last one; the key is the texture handle (the host drops the flag after
  // recreating a texture, which may reuse a handle).
  const void* key;
  bool clear, discardTarget;
  retainedTarget(g->api, t, &key, &clear, &discardTarget);
  (void)discardTarget;  // == discard
  const bool incremental = key != nullptr;
  jsui::TextureScope textureScope(&g->textures);
  // Layers drawn from (and rendered into) textures of this context; a frame
  // recorded elsewhere (record() above) was painted without them.
  jsui::GpuLayerFrame layerFrame(g->layers.of(&rt));
  SkIRect painted;
  if (frame && frame->key == (clear ? t.texture : nullptr) && frame->width == t.width && frame->height == t.height &&
      !discard) {
    // Recorded for this target (jsui_record_gpu_frame): replay it.
    painted = rt.replayFrame(*frame, surface->getCanvas());
  } else {
    if (frame) rt.discardFrame(*frame);  // recorded for another target: paint afresh
    painted = rt.renderCanvasTracked(key, t.width, t.height, [&](const SkIRect&, bool) {
      return targetChoice(surface->getCanvas(), t, clear);
    });
  }
  // Nothing changed and the target is retained: no GPU work at all (unless
  // the image must still move to another layout).
  if (painted.isEmpty() && incremental && !needsTransition) {
    // Pipelines the compile pool finished since the last frame (shader
    // warm-ups) count here, not in the next frame that paints.
    rt.doc.perf.counters.pipelineCompiles += g->precompiler->takeCompiles();
    return 0;
  }

#if defined(JSUI_VULKAN)
  if (g->api == Api::Vulkan && !g->submitBarrier()) return -1;
#endif
  bool ok;
  {
    Perf::Scope gpuScope(rt.doc.perf, Phase::Gpu);
    ok = g->flush(surface.get(), finalState.get());
  }
  surface.reset();
#if defined(JSUI_VULKAN)
  if (g->api == Api::Vulkan && !g->submitBarrier()) return -1;
#endif
  if (!ok || g->ctx->isDeviceLost()) return -1;
  rt.doc.perf.counters.pipelineCompiles += g->precompiler->takeCompiles();
  g->precompiler->afterFrame();
  if (dirty) *dirty = JsuiRect{painted.x(), painted.y(), painted.width(), painted.height()};
  return 0;
}

int copyTexture(JsuiGpuContext* gpu, void* src, void* dst, const JsuiRect& r) {
  GraphiteGpu* g = asGraphite(gpu);
  if (!g || g->api != Api::Dawn || !src || !dst || r.width <= 0 || r.height <= 0) return -1;
#if defined(JSUI_GRAPHITE_DAWN)
  wgpu::CommandEncoder enc = g->dawnDevice.CreateCommandEncoder();
  wgpu::TexelCopyTextureInfo from{}, to{};
  from.texture = wgpu::Texture(static_cast<WGPUTexture>(src));
  to.texture = wgpu::Texture(static_cast<WGPUTexture>(dst));
  from.origin = to.origin = wgpu::Origin3D{uint32_t(r.x), uint32_t(r.y), 0};
  wgpu::Extent3D size{uint32_t(r.width), uint32_t(r.height), 1};
  enc.CopyTextureToTexture(&from, &to, &size);
  wgpu::CommandBuffer cmd = enc.Finish();
  g->dawnQueue.Submit(1, &cmd);
  return 0;
#else
  return -1;
#endif
}

int prepareTarget(JsuiGpuContext* gpu, unsigned format) {
  GraphiteGpu* g = asGraphite(gpu);
  if (!g || !g->compileExecutor) return -1;
  // The colour type and space a render into a target of `format` draws with
  // (render() above).
  sk_sp<SkColorSpace> cs;
  bool bgra = false;
  if (g->api == Api::Vulkan) {
#if defined(JSUI_VULKAN)
    VkFormat vf;
    if (!vkColorType(format, &vf, &cs)) return -1;
    bgra = vf == VK_FORMAT_B8G8R8A8_UNORM || vf == VK_FORMAT_B8G8R8A8_SRGB;
#endif
  } else if (g->api == Api::Dawn) {
#if defined(JSUI_GRAPHITE_DAWN)
    const wgpu::TextureFormat f = format ? wgpu::TextureFormat(format) : wgpu::TextureFormat::RGBA8Unorm;
    if (!dawnColorSpace(f, &cs)) return -1;
    bgra = f == wgpu::TextureFormat::BGRA8Unorm || f == wgpu::TextureFormat::BGRA8UnormSrgb;
#endif
  } else {
#if defined(JSUI_GRAPHITE_METAL)
    if (!mtlColorSpace(format, &cs)) return -1;
    bgra = format == kMTLPixelFormatBGRA8Unorm || format == kMTLPixelFormatBGRA8Unorm_sRGB;
#endif
  }
  g->precompiler->precompileCommon(bgra ? kBGRA_8888_SkColorType : kRGBA_8888_SkColorType, cs);
  return 0;
}

}  // namespace graphite_gpu
}  // namespace jsui

#else  // none of JSUI_VULKAN, JSUI_GRAPHITE_METAL, JSUI_GRAPHITE_DAWN

namespace jsui {
namespace graphite_gpu {
bool available() { return false; }
JsuiGpuContext* createVulkan(const JsuiVulkanDevice&) { return nullptr; }
JsuiGpuContext* createMetal(void*, void*) { return nullptr; }
JsuiGpuContext* createDawn(void*, void*, void*) { return nullptr; }
JsuiGpuContext* createDawnProcs(void*, void*, void*, void*) { return nullptr; }
const char* dawnRevision() { return nullptr; }
bool owns(JsuiGpuContext* gpu) { return gpu && gpu->kind == JsuiGpuContext::Kind::Graphite; }
void destroy(JsuiGpuContext*) {}
void purge(JsuiGpuContext*, size_t) {}
int record(Surface&, JsuiGpuContext*, const JsuiGpuTarget&, std::unique_ptr<Surface::RecordedFrame>* out) {
  out->reset();
  return -1;
}
int render(Surface&, JsuiGpuContext*, const JsuiGpuTarget&, JsuiRect* dirty, Surface::RecordedFrame*) {
  if (dirty) *dirty = JsuiRect{0, 0, 0, 0};
  return -1;
}
int prepareTarget(JsuiGpuContext*, unsigned) { return -1; }
int copyTexture(JsuiGpuContext*, void*, void*, const JsuiRect&) { return -1; }
}  // namespace graphite_gpu
}  // namespace jsui

#endif
