// JsuiView's GPU renderer: Skia Graphite on Godot's own RenderingDevice
// (Vulkan on Linux / Windows, Metal on macOS, and Dawn under the WebGPU
// rendering driver of the godot-aw3 fork), or Skia Ganesh on its Direct3D 12
// device (Windows; Graphite has no D3D backend), see jsui_view.h.
//
// The views of one runtime (the runtime view and its surface views, see
// jsui_view.h) share one Skia context, the runtime view's: one image cache and
// one Recorder for the runtime, created when the first of them renders on the
// GPU. It goes with the runtime view (or when that leaves the GPU path while
// no surface view is attached), after every render queued on the runtime.
//
// Per view there are two RD textures of the view's pixel size:
//
//   src  COLOR_ATTACHMENT | INPUT_ATTACHMENT | SAMPLING | CAN_COPY_FROM |
//        CAN_COPY_TO | CAN_UPDATE (Graphite renders into colour + input attachments)
//        Skia's render target. It keeps the last frame, so a frame repaints
//        only its damage bounds (jsui_render_gpu_target with
//        JSUI_GPU_INCREMENTAL). Godot only ever uses it as a copy source, so
//        its RD tracker stays in COPY_FROM: on Vulkan the image is in
//        TRANSFER_SRC_OPTIMAL between frames (D3D12: COPY_SOURCE), and JSUI
//        leaves it there.
//   dst  SAMPLING | CAN_COPY_TO | CAN_UPDATE | CAN_COPY_FROM
//        What the canvas item draws (a Texture2DRD). Godot copies the damaged
//        rect src -> dst with RenderingDevice.texture_copy in its frame, so
//        Godot's render graph does all of dst's layout transitions and
//        barriers itself, whatever else samples it.
//
// Ordering: JSUI submits its work straight to Godot's main queue (the one
// RenderingDevice records the frame for) from the render thread, during
// process (or, with a separate render thread, in a job queued during process,
// which runs before that frame's draw: the render thread's queue is FIFO), i.e.
// before Godot submits the frame that contains the copy. On
// Vulkan the render is bracketed by full pipeline barriers (gpu_graphite.cpp),
// which also order it after the previous frame that may still be copying from
// src. On Metal both use the same MTLCommandQueue and tracked resources. On
// WebGPU (godot-aw3) both use Godot's WGPUQueue, whose submissions run in
// order, and Dawn tracks the textures' usage. On D3D12 both use Godot's
// DIRECT ID3D12CommandQueue, whose command lists run in order, and Ganesh's
// own command lists move src COPY_SOURCE -> RENDER_TARGET -> COPY_SOURCE.
//
// Backend choice (chooseRenderer), at run time, so one binary serves stock
// Godot and the fork:
//   1. The fork's WebGPU driver registers the Engine singleton
//      "WebGPUInterop" (see kWebGPUInterop below and the README for the
//      contract). If it is there and its get_dawn_revision() is the Dawn
//      jsui_dawn_revision() was built against, JSUI resolves every wgpu*
//      entry point through its get_proc_address() (jsui_gpu_create_dawn_procs;
//      JSUI links no Dawn of its own) and renders into the WGPUTexture
//      get_driver_resource(DRIVER_RESOURCE_TEXTURE) returns. A different
//      revision has a different webgpu.h ABI: one warning, CPU renderer.
//   2. Otherwise Vulkan, Metal or D3D12 through get_driver_resource, as above.
//   3. Otherwise (Compatibility, headless, ...) the CPU renderer.
//
// New textures (first frame, resize) are created with zeroed initial data and
// primed with one copy, and first rendered into on the next frame: by then
// Godot has submitted the upload (possibly on a transfer queue it waits for)
// and moved src into COPY_FROM. Until then the previous textures stay on
// screen.
//
// Hand-off to the render thread. With Godot's default thread model the
// RenderingServer runs on the main thread and every job below runs inline.
// With a separate render thread (--render-thread separate) the main thread
// waits on it only where it must:
//   * Steady state (the textures fit, none pending): a view's frame (its
//     jsui_render_gpu_target into its src, the copy into its dst) joins the
//     runtime view's frame job (batchFrame). Once every view of the runtime
//     has painted, the runtime view's process queues that one job with
//     call_on_render_thread (queueFrameJob) and returns: one render-thread
//     job per frame for all of a runtime's views, however many surfaces it
//     has. It runs before this frame's draw, so the latency is unchanged. The
//     job uses the runtime (every view's frame paints a surface of it), so
//     the main thread must not touch the runtime until it has run: reading
//     rt_ on any view of the runtime waits for it (waitRender, on the runtime
//     view's fence, which the job signals; jobs run in order, so
//     done == queued means all have run). That is why the runtime view's
//     process does every view's main-thread work on the runtime (tick, auto
//     size, cursor, CPU renders) before the GPU frames are batched, and why
//     every other kind of render queues the batch first (render(),
//     renderGpu()): what it does may run the game's code (frame_rendered,
//     the texture's `changed`), which must not see views in an unqueued
//     batch, and it keeps the render thread's order. Godot's main
//     loop calls RenderingServer.sync() after process every frame, so by the
//     next frame's input and process the job has run and nothing waits; only
//     calls into the runtime's views later in the same frame (after the
//     runtime view's _process) can block, until the job has run. The views'
//     results (frame_rendered, stats, a failure) are collected at the start
//     of the runtime view's next process.
//   * Blocking, per view, as before: choosing the renderer (creating the
//     runtime's context), creating / promoting a texture pair (start, resize;
//     with the Texture2DRD wait below), reading the frame back
//     (get_frame_image) and teardown (releaseGpu).
// The main thread holds no lock the job needs while it waits (the fence's
// mutex is released while waiting), and the job never waits for the main
// thread (waitRender returns at once on the render thread). Lifetimes: a
// surface view waits for the runtime's job before its surface goes (detach,
// free, the runtime stopping), and a runtime view before its runtime, its
// fence and the runtime's context go, so a job never outlives a view,
// surface, fence or context it uses.
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <chrono>
#include <cstdlib>
#include <condition_variable>
#include <mutex>

#include "jsui_view.h"
#if defined(__APPLE__)
#include "metal_texture.h"
#endif

using namespace godot;

namespace jsui_godot {

namespace {

constexpr int kVkImageLayoutTransferSrcOptimal = 6;  // VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
constexpr unsigned kMTLPixelFormatRGBA8Unorm = 70;

#if defined(__APPLE__)
// JSUI_METAL_UNTRACKED_SRC=1 keeps src a Godot-created texture (for A/B).
bool metalTrackedSrc() {
  static const bool tracked = [] {
    const char* v = std::getenv("JSUI_METAL_UNTRACKED_SRC");
    return !(v && *v && *v != '0');
  }();
  return tracked;
}
#endif

uint64_t usecNow() {
  return uint64_t(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void* driverResource(RenderingDevice* rd, RenderingDevice::DriverResource what, const RID& rid = RID()) {
  return reinterpret_cast<void*>(uintptr_t(rd->get_driver_resource(what, rid, 0)));
}

enum class TargetKind { Vulkan, WebGPU, Metal, D3D12 };

constexpr int kD3D12ResourceStateCopySource = 0x800;  // D3D12_RESOURCE_STATE_COPY_SOURCE

// What jsui_render_gpu_target needs about src, but its handle.
JsuiGpuTarget targetFor(TargetKind kind, int w, int h, int flags) {
  JsuiGpuTarget t{};
  t.width = w;
  t.height = h;
  t.flags = flags;
  if (kind == TargetKind::Vulkan) {
    t.format = 0;  // VK_FORMAT_R8G8B8A8_UNORM
    t.usage = 0;   // colour + input attachment, sampled, transfer src / dst (the bits in renderGpu)
    t.layout = t.final_layout = kVkImageLayoutTransferSrcOptimal;
  } else if (kind == TargetKind::WebGPU) {
    t.format = 0;  // the texture's own: WGPUTextureFormat_RGBA8Unorm (DATA_FORMAT_R8G8B8A8_UNORM)
  } else if (kind == TargetKind::D3D12) {
    t.format = 0;  // the resource's own: DXGI_FORMAT_R8G8B8A8_UNORM
    t.layout = t.final_layout = kD3D12ResourceStateCopySource;
  } else {
    t.format = kMTLPixelFormatRGBA8Unorm;
  }
  return t;
}

// On the render thread: Skia paints the frame into src, then Godot copies the
// repainted rect into dst (texture_copy, recorded into Godot's frame).
// false: the GPU path failed.
bool paintGpuTarget(RenderingDevice* rd, JsuiRuntime* rt, JsuiGpuContext* gpu, JsuiGpuTarget t, const RID& src,
                    const RID& dst, JsuiRect* dirty, std::atomic<uint64_t>& renderUsec) {
  *dirty = JsuiRect{0, 0, 0, 0};
  t.texture = driverResource(rd, RenderingDevice::DRIVER_RESOURCE_TEXTURE, src);
  if (!t.texture || !rt || !gpu) return false;
  const uint64_t t0 = usecNow();
  const int rc = jsui_render_gpu_target(rt, gpu, &t, dirty);
  renderUsec.fetch_add(usecNow() - t0, std::memory_order_relaxed);
  if (rc != 0) return false;
  if (dirty->width > 0 && dirty->height > 0) {
    Vector3 pos(float(dirty->x), float(dirty->y), 0);
    rd->texture_copy(src, dst, pos, pos, Vector3(float(dirty->width), float(dirty->height), 1), 0, 0, 0, 0);
  }
  return true;
}

// Which RenderingDevice driver Godot runs: "vulkan", "metal", "d3d12",
// "webgpu" (godot-aw3), or "" (Compatibility / headless: no RenderingDevice
// at all).
String renderingDriver(RenderingServer* rs) {
  if (!rs->get_rendering_device()) return String();
  // Godot 4.4+; the extension builds against the 4.3 API, so call it by name.
  if (rs->has_method("get_current_rendering_driver_name")) return rs->call("get_current_rendering_driver_name");
#if defined(__linux__)
  return "vulkan";  // 4.3 on Linux: Vulkan is the only RenderingDevice driver
#else
  return String();  // 4.3 elsewhere: cannot tell Vulkan from D3D12 / MoltenVK safely
#endif
}

// The godot-aw3 fork's interop singleton (a plain Object, registered while its
// WebGPU driver is active). Methods, all without arguments:
//   get_proc_address() -> int   address of Dawn's wgpuGetProcAddress
//   get_instance()     -> int   WGPUInstance
//   get_device()       -> int   WGPUDevice
//   get_queue()        -> int   WGPUQueue
//   get_dawn_revision() -> String  the Dawn commit the engine links
const char* const kWebGPUInterop = "WebGPUInterop";

void* interopPointer(Object* interop, const char* method) {
  if (!interop->has_method(method)) return nullptr;
  return reinterpret_cast<void*>(uintptr_t(int64_t(interop->call(method))));
}

// The interop object when it is usable with this build, else nullptr (and
// `why` says why; a revision mismatch also warns, once per process).
Object* webgpuInterop(String* why) {
  Engine* engine = Engine::get_singleton();
  if (!engine || !engine->has_singleton(kWebGPUInterop)) return nullptr;
  Object* interop = engine->get_singleton(kWebGPUInterop);
  if (!interop) return nullptr;
  const char* ours = jsui_dawn_revision();
  if (!ours) {
    *why = "this jsui_godot is built without JSUI_GRAPHITE_DAWN";
    return nullptr;
  }
  String theirs = interop->has_method("get_dawn_revision") ? String(interop->call("get_dawn_revision")) : String();
  if (theirs.strip_edges() != String(ours)) {
    *why = "Godot's Dawn is " + (theirs.is_empty() ? String("unknown") : theirs) + ", jsui_godot is built against " +
           String(ours);
    static bool warned = false;
    if (!warned) {
      warned = true;
      UtilityFunctions::push_warning("JsuiView: Dawn revision mismatch (" + *why +
                                     "): the WebGPU path needs the same revision; using the CPU renderer");
    }
    return nullptr;
  }
  return interop;
}

}  // namespace

void JsuiView::runRenderJob() {
  std::function<void()> job = std::move(render_job_);
  render_job_ = nullptr;
  if (job) job();
}

void JsuiView::runOnRenderThread(const std::function<void()>& job) {
  RenderingServer* rs = RenderingServer::get_singleton();
  // The default thread model runs the RenderingServer on the main thread:
  // call right away. With a separate render thread, queue the job there and
  // wait (the runtime must not tick while its frame is painted).
  if (!rs || rs->is_on_render_thread()) {
    job();
    return;
  }
  const uint64_t t0 = usecNow();
  std::mutex m;
  std::condition_variable cv;
  bool done = false;
  render_job_ = [&]() {
    job();
    std::lock_guard<std::mutex> lock(m);
    done = true;
    cv.notify_one();
  };
  rs->call_on_render_thread(callable_mp(this, &JsuiView::runRenderJob));
  std::unique_lock<std::mutex> lock(m);
  cv.wait(lock, [&] { return done; });
  blocking_waits_++;
  main_wait_usec_total_ += usecNow() - t0;
}

void JsuiView::waitRender() {
  // Every job on the runtime: a surface view's render uses the shared
  // runtime as much as the runtime view's own.
  Fence& f = runtimeFence();
  const uint64_t queued = f.queued.load(std::memory_order_relaxed);
  if (f.done.load(std::memory_order_acquire) == queued) return;
  // On the render thread we are inside the job (a runtime callback) or ahead
  // of it in the queue: never wait for it there.
  RenderingServer* rs = RenderingServer::get_singleton();
  if (!rs || rs->is_on_render_thread()) return;
  const uint64_t t0 = usecNow();
  std::unique_lock<std::mutex> lock(f.m);
  f.cv.wait(lock, [&] { return f.done.load(std::memory_order_acquire) == queued; });
  blocking_waits_++;
  main_wait_usec_total_ += usecNow() - t0;
}

void JsuiView::chooseRenderer() {
  renderer_ = Renderer::Cpu;
  RenderingServer* rs = RenderingServer::get_singleton();
  if (force_cpu_ || !rs) return;
  String driver = renderingDriver(rs);
  String why;
  Object* interop = webgpuInterop(&why);
  Renderer want = interop               ? Renderer::WebGPU
                  : !why.is_empty()    ? Renderer::Cpu  // WebGPUInterop is there but unusable
                  : driver == "vulkan" ? Renderer::Vulkan
                  : driver == "metal"  ? Renderer::Metal
                  : driver == "d3d12"  ? Renderer::D3D12
                                       : Renderer::Cpu;
  if (want == Renderer::Cpu) {
    if (!why.is_empty()) {
      UtilityFunctions::print("JsuiView: CPU renderer (" + why + ")");
    } else if (driver == "webgpu") {
      UtilityFunctions::print("JsuiView: CPU renderer (rendering driver 'webgpu' without the " + String(kWebGPUInterop) +
                              " singleton)");
    } else {
      UtilityFunctions::print_verbose("JsuiView: CPU renderer (rendering driver '" + driver + "')");
    }
    return;
  }
  if (want == Renderer::WebGPU) driver = "webgpu (Dawn " + String(jsui_dawn_revision()).substr(0, 12) + ")";
  // One Skia context per runtime: the runtime view's, shared by its surface
  // views (one image cache, no context switches, one job per frame).
  if (!runtimeView()->createGpuContext(want, driver, interop)) return;
  renderer_ = want;
}

bool JsuiView::createGpuContext(Renderer want, const String& driver, Object* interop) {
  if (gpu_) return true;
  RenderingServer* rs = RenderingServer::get_singleton();
  if (!rs) return false;
  runOnRenderThread([&]() {
    RenderingDevice* rd = rs->get_rendering_device();
    if (!rd) return;
    if (want == Renderer::Vulkan) {
      JsuiVulkanDevice d{};
      d.instance = driverResource(rd, RenderingDevice::DRIVER_RESOURCE_TOPMOST_OBJECT);
      d.physical_device = driverResource(rd, RenderingDevice::DRIVER_RESOURCE_PHYSICAL_DEVICE);
      d.device = driverResource(rd, RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE);
      d.queue = driverResource(rd, RenderingDevice::DRIVER_RESOURCE_COMMAND_QUEUE);
      d.queue_family = unsigned(rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_QUEUE_FAMILY, RID(), 0));
      // Godot does not expose its extension / feature lists: Skia assumes
      // none (core Vulkan 1.1), which is always valid on Godot's device.
      gpu_ = jsui_gpu_create_vulkan(&d);
    } else if (want == Renderer::WebGPU) {
      // Entry points and handles from the interop singleton: nothing has to
      // be exported by the engine binary.
      gpu_ = jsui_gpu_create_dawn_procs(interopPointer(interop, "get_proc_address"),
                                        interopPointer(interop, "get_instance"), interopPointer(interop, "get_device"),
                                        interopPointer(interop, "get_queue"));
    } else if (want == Renderer::D3D12) {
      // ID3D12Device and Godot's DIRECT ID3D12CommandQueue.
      gpu_ = jsui_gpu_create_d3d12(driverResource(rd, RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE),
                                   driverResource(rd, RenderingDevice::DRIVER_RESOURCE_COMMAND_QUEUE));
    } else {
      gpu_ = jsui_gpu_create_metal(driverResource(rd, RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE),
                                   driverResource(rd, RenderingDevice::DRIVER_RESOURCE_COMMAND_QUEUE));
    }
  });
  if (!gpu_) {
    UtilityFunctions::print("JsuiView: no Skia GPU context on '" + driver + "' (" +
                            String(want == Renderer::Vulkan   ? "built without JSUI_VULKAN or Skia rejected the device"
                                   : want == Renderer::D3D12  ? "built without JSUI_D3D or Skia rejected the device"
                                   : want == Renderer::WebGPU ? String(kWebGPUInterop) +
                                                                    " did not resolve every Dawn entry point, returned "
                                                                    "no device or Skia rejected the device"
                                                              : "built without JSUI_GRAPHITE_METAL or Skia rejected the device") +
                            "); using the CPU renderer");
    return false;
  }
  gpu_api_ = want;
  UtilityFunctions::print("JsuiView: rendering with Skia " + String(want == Renderer::D3D12 ? "Ganesh" : "Graphite") +
                          " on " + driver);
  // Ganesh (D3D12) has no pipeline cache.
  if (want != Renderer::D3D12)
    setupPipelineCache(want == Renderer::Vulkan ? "vulkan" : want == Renderer::Metal ? "metal" : "webgpu");
  return true;
}

namespace {

// `srgb_view`: the texture may also be viewed as sRGB. Texture2DRD makes such
// a view of every RGBA8 texture it wraps (Godot's texture_rd_initialize), which
// is only valid on an image created with that format in its shareable list
// (Vulkan: VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, else the validation layer
// reports VUID-VkImageViewCreateInfo-image-01762; Metal: PixelFormatView).
RID createTexture(RenderingDevice* rd, int w, int h, int64_t usage, const PackedByteArray& zeros,
                  bool srgb_view = false) {
  Ref<RDTextureFormat> fmt;
  fmt.instantiate();
  fmt->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D);
  fmt->set_format(RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
  if (srgb_view) {
    fmt->add_shareable_format(RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
    fmt->add_shareable_format(RenderingDevice::DATA_FORMAT_R8G8B8A8_SRGB);
  }
  fmt->set_width(uint32_t(w));
  fmt->set_height(uint32_t(h));
  fmt->set_usage_bits(BitField<RenderingDevice::TextureUsageBits>(usage));
  Ref<RDTextureView> view;
  view.instantiate();
  TypedArray<PackedByteArray> data;
  data.push_back(zeros);
  return rd->texture_create(fmt, view, data);
}

}  // namespace

bool JsuiView::renderGpu() {
  // The job queued last frame first (the runtime view's process() has
  // normally taken it).
  if (!collectGpuRender()) return false;
  RenderingServer* rs = RenderingServer::get_singleton();
  JsuiGpuContext* gpu = runtimeView()->gpu_;  // the runtime's
  const int w = pixel_w_, h = pixel_h_;
  const TargetKind kind = renderer_ == Renderer::Vulkan   ? TargetKind::Vulkan
                          : renderer_ == Renderer::WebGPU ? TargetKind::WebGPU
                          : renderer_ == Renderer::D3D12  ? TargetKind::D3D12
                                                          : TargetKind::Metal;
  // Steady state: the textures on screen fit and no new pair is pending.
  if (gpu_cur_.valid() && gpu_cur_.w == w && gpu_cur_.h == h && !gpu_next_.valid()) {
    GpuJob& j = gpu_job_;
    j.rt = rt_.unsynced();
    j.gpu = gpu;
    j.src = gpu_cur_.src;
    j.dst = gpu_cur_.dst;
    j.target = targetFor(kind, w, h, gpu_retained_ ? JSUI_GPU_INCREMENTAL : 0);
    j.fence = nullptr;
    j.id = 0;
    j.ok = false;
    j.dirty = JsuiRect{0, 0, 0, 0};
    // src holds this frame once the job has run (a failed job ends the GPU
    // path, and with it the retained frame).
    gpu_retained_ = true;
    gpu_job_pending_ = true;
    gpu_jobs_++;
    if (!rs || rs->is_on_render_thread()) {
      // No separate render thread: paint now and take the result
      // (frame_rendered in this render, as before).
      paintGpuJob();
      return collectGpuRender();
    }
    runtimeView()->batchFrame(this);
    return true;
  }

  // Start / resize: create, prime and promote a texture pair, blocking. The
  // frames batched so far go first (it may emit frame_rendered, see
  // render()); the job runs after them (FIFO), and the main thread waits for
  // it without touching the runtime meanwhile.
  runtimeView()->queueFrameJob();
  JsuiRuntime* rt = rt_.unsynced();
  const uint64_t frame = Engine::get_singleton()->get_frames_drawn();
  bool ok = true, promoted = false, waiting = false;
  GpuTextures retired;  // the previous pair, freed once dst is off screen
  JsuiRect dirty{0, 0, 0, 0};
  runOnRenderThread([&]() {
    const uint64_t t0 = usecNow();
    RenderingDevice* rd = rs->get_rendering_device();
    if (!rd || !gpu) {
      ok = false;
      return;
    }
    auto freeTextures = [&](GpuTextures& t) {
      if (t.src.is_valid()) rd->free_rid(t.src);
      if (t.dst.is_valid()) rd->free_rid(t.dst);
      t = GpuTextures{};
    };
    const bool curFits = gpu_cur_.valid() && gpu_cur_.w == w && gpu_cur_.h == h;
    const bool nextFits = gpu_next_.valid() && gpu_next_.w == w && gpu_next_.h == h;
    if (curFits && gpu_next_.valid()) freeTextures(gpu_next_);  // resized back
    if (!curFits && !nextFits) {
      freeTextures(gpu_next_);
      PackedByteArray zeros;
      zeros.resize(int64_t(w) * h * 4);
      zeros.fill(0);
      GpuTextures t;
      t.w = w;
      t.h = h;
      t.created_frame = frame;
      const int64_t srcUsage =
          RenderingDevice::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RenderingDevice::TEXTURE_USAGE_INPUT_ATTACHMENT_BIT |
          RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
          RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT | RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT;
#if defined(__APPLE__)
      if (kind == TargetKind::Metal && metalTrackedSrc()) {
        // Our own tracked MTLTexture (metal_texture.mm): Godot's driver may
        // create its textures untracked (godot-aw3: explicit barriers), and
        // nothing would then order Graphite's render into src before Godot's
        // copy. Godot retains the texture it imports; ours is released now.
        void* tex = newTrackedMetalTexture(driverResource(rd, RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE), w, h);
        if (tex) {
          t.src = rd->texture_create_from_extension(RenderingDevice::TEXTURE_TYPE_2D,
                                                    RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM,
                                                    RenderingDevice::TEXTURE_SAMPLES_1,
                                                    BitField<RenderingDevice::TextureUsageBits>(srcUsage),
                                                    uint64_t(uintptr_t(tex)), uint64_t(w), uint64_t(h), 1, 1);
          releaseMetalTexture(tex);
          if (t.src.is_valid()) rd->texture_clear(t.src, Color(0, 0, 0, 0), 0, 1, 0, 1);
        }
      }
      if (!t.src.is_valid())
#endif
        t.src = createTexture(rd, w, h, srcUsage, zeros);
      t.dst = createTexture(rd, w, h,
                            RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT |
                                RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT,
                            zeros, /*srgb_view=*/true);  // dst goes into a Texture2DRD
      if (!t.src.is_valid() || !t.dst.is_valid()) {
        freeTextures(t);
        ok = false;
        return;
      }
      // Godot's tracker for src goes COPY_TO (upload) -> COPY_FROM here and
      // stays there: from now on it is only ever a copy source.
      rd->texture_copy(t.src, t.dst, Vector3(), Vector3(), Vector3(float(w), float(h), 1), 0, 0, 0, 0);
      gpu_next_ = t;
    }
    if (gpu_next_.valid()) {
      if (frame <= gpu_next_.created_frame) {
        waiting = true;  // Godot has not submitted the upload + priming copy yet
        return;
      }
      retired = gpu_cur_;
      gpu_cur_ = gpu_next_;
      gpu_next_ = GpuTextures{};
      gpu_retained_ = false;
      promoted = true;
    }
    ok = paintGpuTarget(rd, rt, gpu, targetFor(kind, w, h, gpu_retained_ ? JSUI_GPU_INCREMENTAL : 0), gpu_cur_.src,
                        gpu_cur_.dst, &dirty, jsui_render_usec_total_);
    if (ok) gpu_retained_ = true;
    gpu_job_usec_total_.fetch_add(usecNow() - t0, std::memory_order_relaxed);
  });
  if (!ok) return false;
  if (waiting) {
    render_pending_ = true;  // render on the next frame
    return true;
  }
  if (promoted) {
    if (gpu_texture_.is_null()) gpu_texture_.instantiate();
    gpu_texture_->set_texture_rd_rid(gpu_cur_.dst);
    // Texture2DRD::set_texture_rd_rid only queues the RenderingServer texture's
    // creation on the render thread. With a separate render thread, get_rid()
    // right away would race it: it makes a placeholder (drawn as a magenta /
    // black checkerboard) that the queued call may never replace. Wait for the
    // queue (render-thread calls run in order) before taking the RID.
    runOnRenderThread([] {});
    // The queued call has run, so the Texture2DRD holds dst now. If Godot
    // refused dst, get_rid() would be the placeholder all the same: use the
    // CPU renderer rather than show it. (Without the wait above, a separate
    // render thread would land here every time.)
    const bool adopted = gpu_texture_->get_texture_rd_rid() == gpu_cur_.dst;
    if (adopted) {
      showTexture(gpu_texture_->get_rid());
    } else {
      UtilityFunctions::push_error("JsuiView: Texture2DRD did not take the GPU texture (set_texture_rd_rid has not run "
                                   "on the render thread or was refused)");
    }
    if (retired.valid()) {
      runOnRenderThread([&]() {
        if (RenderingDevice* rd = rs->get_rendering_device()) {
          rd->free_rid(retired.src);
          rd->free_rid(retired.dst);
        }
      });
    }
    if (!adopted) return false;
  }
  finishGpuFrame(dirty);
  return true;
}

void JsuiView::batchFrame(JsuiView* view) {
  frame_batch_.push_back(view);
  // Outside process()'s paint pass (e.g. get_cpu_frame_image): right away.
  if (!batching_) queueFrameJob();
}

void JsuiView::queueFrameJob() {
  if (frame_batch_.empty()) return;
  RenderingServer* rs = RenderingServer::get_singleton();
  // The previous frame job reads frame_job_views_ until it has run: it has
  // in steady state (one job per frame, and Godot syncs every frame).
  waitRender();
  frame_job_views_.swap(frame_batch_);
  frame_batch_.clear();
  frame_job_id_ = fence_.queued.load(std::memory_order_relaxed) + 1;
  fence_.queued.store(frame_job_id_, std::memory_order_relaxed);
  // This frame's host texture copies go first in the job (syncTextures ran
  // before any view painted).
  frame_job_textures_ = texture_work_;
  texture_work_ = false;
  for (JsuiView* v : frame_job_views_) {
    v->gpu_job_.fence = &fence_;
    v->gpu_job_.id = frame_job_id_;
  }
  render_thread_jobs_++;
  // FIFO: it runs after what the render thread has queued already (the
  // previous frame's draw) and before this frame's draw.
  rs->call_on_render_thread(callable_mp(this, &JsuiView::runFrameJob));
}

void JsuiView::runFrameJob() {
  if (frame_job_textures_) {
    frame_job_textures_ = false;
    doTextureWork();
  }
  // Each view into its own src, copied into its own dst, with the runtime's
  // context; the main thread reads the results once the fence has moved.
  for (JsuiView* v : frame_job_views_) v->paintGpuJob();
  // Signal under the lock, and touch nothing of the views from then on: once
  // done is stored a surface view (and its surface) may go. The fence is
  // ours (the runtime view's): we wait for it and take this lock before we
  // go (stop(), releaseGpu()).
  const uint64_t id = frame_job_id_;
  std::lock_guard<std::mutex> lock(fence_.m);
  fence_.done.store(id, std::memory_order_release);
  fence_.cv.notify_all();
}

void JsuiView::paintGpuJob() {
  const uint64_t t0 = usecNow();
  GpuJob& j = gpu_job_;
  RenderingServer* rs = RenderingServer::get_singleton();
  RenderingDevice* rd = rs ? rs->get_rendering_device() : nullptr;
  j.ok = rd && paintGpuTarget(rd, j.rt, j.gpu, j.target, j.src, j.dst, &j.dirty, jsui_render_usec_total_);
  gpu_job_usec_total_.fetch_add(usecNow() - t0, std::memory_order_relaxed);
}

bool JsuiView::collectGpuRender() {
  if (!gpu_job_pending_) return true;
  waitRender();
  gpu_job_pending_ = false;
  if (!gpu_job_.ok) return false;
  finishGpuFrame(gpu_job_.dirty);
  return true;
}

void JsuiView::finishGpuFrame(const JsuiRect& dirty) {
  if (dirty.width <= 0 || dirty.height <= 0) return;
  renders_++;
  dirty_px_total_ += int64_t(dirty.width) * dirty.height;
  emit_signal("frame_rendered", Rect2i(dirty.x, dirty.y, dirty.width, dirty.height));
}

void JsuiView::gpuFailed() {
  UtilityFunctions::push_warning("JsuiView: GPU rendering failed; using the CPU renderer");
  releaseGpu();
  renderer_ = Renderer::Cpu;
}

PackedByteArray JsuiView::readGpuFrame() {
  PackedByteArray px;
  if (!gpu_cur_.valid()) return px;
  // Queued after a pending render job (FIFO), so this reads the frame it rendered.
  runOnRenderThread([&]() {
    RenderingServer* rs = RenderingServer::get_singleton();
    RenderingDevice* rd = rs ? rs->get_rendering_device() : nullptr;
    if (rd) px = rd->texture_get_data(gpu_cur_.dst, 0);
  });
  return px;
}

void JsuiView::releaseGpu() {
  // A queued render job uses the textures, the context and the runtime: let
  // it finish, and let it leave the fence (a runtime view may be going away).
  waitRender();
  { std::lock_guard<std::mutex> settled(runtimeFence().m); }
  gpu_job_pending_ = false;
  RenderingServer* rs = RenderingServer::get_singleton();
  if (!rs) {
    gpu_ = nullptr;
    return;
  }
  // Our Skia context (as a runtime view) goes too unless surface views use
  // it; their renders have run (waitRender above covers the runtime).
  const bool dropContext = gpu_ && surfaces_.empty();
  if (!dropContext && !gpu_cur_.valid() && !gpu_next_.valid()) return;
  if (gpu_texture_.is_valid()) gpu_texture_->set_texture_rd_rid(RID());
  runOnRenderThread([&]() {
    RenderingDevice* rd = rs->get_rendering_device();
    for (GpuTextures* t : {&gpu_cur_, &gpu_next_}) {
      if (rd && t->src.is_valid()) rd->free_rid(t->src);
      if (rd && t->dst.is_valid()) rd->free_rid(t->dst);
      *t = GpuTextures{};
    }
    if (dropContext) {
      jsui_gpu_destroy(gpu_);
      gpu_ = nullptr;
      gpu_api_ = Renderer::Unknown;
    }
  });
  gpu_retained_ = false;
}

// ------------------------------------------------------------ host textures
//
// set_texture's textures with the GPU renderer (see HostTexture in
// jsui_view.h). syncTextures (main thread, before the tick) adopts the mirrors
// made in earlier frames and marks the textures to copy this frame; here, on
// the render thread, before any of the frame's renders:
//   * the Texture2D's RenderingDevice texture is looked up
//     (RenderingServer.texture_get_rd_texture: ImageTexture, ViewportTexture,
//     VideoStreamPlayer's texture, Texture2DRD...), every frame for a live
//     one, so a texture Godot recreated (a resize, ImageTexture.set_image) is
//     simply the next source;
//   * it is copied whole into the mirror (RenderingDevice.texture_copy, in
//     Godot's frame graph: Godot orders it after its own writes this frame
//     and does the source's barriers and layouts), a new mirror when the size
//     or format changed;
//   * Vulkan: a compute dispatch that samples one texel of the mirror follows
//     the copy, so Godot's graph leaves the mirror in
//     SHADER_READ_ONLY_OPTIMAL, the layout JSUI's render needs it in (jsui.h);
//     Godot's graph would leave a copy destination in TRANSFER_DST_OPTIMAL.
// The runtime samples the mirror from the next frame on (one frame of
// latency: JSUI renders before Godot submits the frame holding the copy).
// Metal: the mirror is our own tracked MTLTexture (as src above), so Metal
// orders Godot's copy into it and Skia's sampling on the shared queue.
// D3D12: no layout pass; the mirror stays in COPY_DEST, where Godot's tracker
// leaves it, and JSUI is told so (JsuiTexture.usage): its render transitions
// it for sampling and back within its own command lists on the shared queue.

namespace {

constexpr unsigned kVkFormatR8G8B8A8Unorm = 37, kVkFormatB8G8R8A8Unorm = 44;
constexpr unsigned kDxgiFormatR8G8B8A8Unorm = 28, kDxgiFormatB8G8R8A8Unorm = 87;
constexpr unsigned kMTLPixelFormatBGRA8Unorm = 80;

// The compute shader of the Vulkan layout pass: samples the texture, so the
// render graph moves it to SHADER_READ_ONLY_OPTIMAL. The result goes to a
// 16-byte buffer nothing reads (an unused binding could be compiled out).
const char* const kLayoutPassShader = R"(#version 450
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 0) uniform sampler2D mirror;
layout(set = 0, binding = 1, std430) restrict writeonly buffer Sink { vec4 texel; } sink;
void main() { sink.texel = texelFetch(mirror, ivec2(0), 0); }
)";

// The mirror's format for a source of RenderingDevice format `df` (false:
// the GPU path cannot show it). sRGB sources keep their bytes (sRGB-encoded,
// which is what JSUI expects) in a UNORM mirror: Vulkan and WebGPU copy
// between the two; Metal only copies identical formats.
bool mirrorFormatFor(int64_t df, bool metal, int64_t* mirror) {
  switch (df) {
    case RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM:
    case RenderingDevice::DATA_FORMAT_B8G8R8A8_UNORM: *mirror = df; return true;
    case RenderingDevice::DATA_FORMAT_R8G8B8A8_SRGB:
      *mirror = RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM;
      return !metal;
    case RenderingDevice::DATA_FORMAT_B8G8R8A8_SRGB:
      *mirror = RenderingDevice::DATA_FORMAT_B8G8R8A8_UNORM;
      return !metal;
    default: return false;
  }
}

}  // namespace

void JsuiView::queueTextureJob() {
  if (!texture_work_) return;
  RenderingServer* rs = RenderingServer::get_singleton();
  if (!rs || rs->is_on_render_thread()) {
    texture_work_ = false;
    doTextureWork();
    return;
  }
  // No frame job took it this frame (nothing painted on the GPU): a job of
  // its own on the runtime's fence, so whatever touches textures_ next waits
  // for it as for a frame job. Not counted in render_thread_jobs.
  waitRender();
  texture_work_ = false;
  texture_job_id_ = fence_.queued.load(std::memory_order_relaxed) + 1;
  fence_.queued.store(texture_job_id_, std::memory_order_relaxed);
  rs->call_on_render_thread(callable_mp(this, &JsuiView::runTextureJob));
}

void JsuiView::runTextureJob() {
  doTextureWork();
  const uint64_t id = texture_job_id_;
  std::lock_guard<std::mutex> lock(fence_.m);
  fence_.done.store(id, std::memory_order_release);
  fence_.cv.notify_all();
}

void JsuiView::doTextureWork() {
  RenderingServer* rs = RenderingServer::get_singleton();
  RenderingDevice* rd = rs ? rs->get_rendering_device() : nullptr;
  auto freeMirror = [&](TextureMirror& m) {
    if (rd && m.uniforms.is_valid() && rd->uniform_set_is_valid(m.uniforms)) rd->free_rid(m.uniforms);
    if (rd && m.rd.is_valid()) rd->free_rid(m.rd);  // RenderingDevice defers it past the frames using it
    m = TextureMirror{};
  };
  for (TextureMirror& m : retired_mirrors_) freeMirror(m);
  retired_mirrors_.clear();
  if (!rd) {
    for (HostTexture& e : textures_) e.copy = false;
    return;
  }
  const bool vulkan = gpu_api_ == Renderer::Vulkan, metal = gpu_api_ == Renderer::Metal;
  auto fail = [](HostTexture& e, const std::string& why) {
    e.cpu_only = true;
    e.why = why;
  };
  // Vulkan: the layout pass's pipeline, once per runtime view.
  auto layoutPass = [&]() -> bool {
    LayoutPass& p = layout_pass_;
    if (p.pipeline.is_valid()) return true;
    if (p.failed) return false;
    p.failed = true;
    Ref<RDShaderSource> source;
    source.instantiate();
    source->set_stage_source(RenderingDevice::SHADER_STAGE_COMPUTE, kLayoutPassShader);
    Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(source, true);
    if (spirv.is_null() || !spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_COMPUTE).is_empty()) {
      UtilityFunctions::push_warning("JsuiView: could not compile the host texture layout pass" +
                                     (spirv.is_valid() ? ": " + spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_COMPUTE)
                                                       : String()));
      return false;
    }
    p.shader = rd->shader_create_from_spirv(spirv, "JsuiView host texture layout pass");
    if (p.shader.is_valid()) p.pipeline = rd->compute_pipeline_create(p.shader);
    Ref<RDSamplerState> sampler;
    sampler.instantiate();
    p.sampler = rd->sampler_create(sampler);
    p.sink = rd->storage_buffer_create(16);
    if (!p.pipeline.is_valid() || !p.sampler.is_valid() || !p.sink.is_valid()) return false;
    p.failed = false;
    return true;
  };
  auto createMirror = [&](int w, int h, int64_t format) -> TextureMirror {
    TextureMirror m;
    const int64_t usage = RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT |
                          RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
#if defined(__APPLE__)
    if (metal && metalTrackedSrc()) {
      void* tex = newTrackedMetalTexture(driverResource(rd, RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE), w, h,
                                         format == RenderingDevice::DATA_FORMAT_B8G8R8A8_UNORM);
      if (tex) {
        m.rd = rd->texture_create_from_extension(
            RenderingDevice::TEXTURE_TYPE_2D, RenderingDevice::DataFormat(format), RenderingDevice::TEXTURE_SAMPLES_1,
            BitField<RenderingDevice::TextureUsageBits>(usage), uint64_t(uintptr_t(tex)), uint64_t(w), uint64_t(h), 1, 1);
        releaseMetalTexture(tex);
      }
    }
    if (!m.rd.is_valid())
#endif
    {
      // No initial data: the first copy writes all of it.
      Ref<RDTextureFormat> fmt;
      fmt.instantiate();
      fmt->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D);
      fmt->set_format(RenderingDevice::DataFormat(format));
      fmt->set_width(uint32_t(w));
      fmt->set_height(uint32_t(h));
      fmt->set_usage_bits(BitField<RenderingDevice::TextureUsageBits>(usage));
      Ref<RDTextureView> view;
      view.instantiate();
      m.rd = rd->texture_create(fmt, view);
    }
    if (!m.rd.is_valid()) return TextureMirror{};
    m.handle = driverResource(rd, RenderingDevice::DRIVER_RESOURCE_TEXTURE, m.rd);
    const bool bgra = format == RenderingDevice::DATA_FORMAT_B8G8R8A8_UNORM;
    m.format = vulkan  ? (bgra ? kVkFormatB8G8R8A8Unorm : kVkFormatR8G8B8A8Unorm)
               : metal ? (bgra ? kMTLPixelFormatBGRA8Unorm : kMTLPixelFormatRGBA8Unorm)
               : gpu_api_ == Renderer::D3D12 ? (bgra ? kDxgiFormatB8G8R8A8Unorm : kDxgiFormatR8G8B8A8Unorm)
                                             : 0;  // WebGPU: the texture's own
    m.data_format = format;
    m.w = w;
    m.h = h;
    m.created_frame = texture_frame_;
    if (vulkan && m.handle) {
      TypedArray<RDUniform> uniforms;
      Ref<RDUniform> tex;
      tex.instantiate();
      tex->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
      tex->set_binding(0);
      tex->add_id(layout_pass_.sampler);
      tex->add_id(m.rd);
      uniforms.push_back(tex);
      Ref<RDUniform> sink;
      sink.instantiate();
      sink->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
      sink->set_binding(1);
      sink->add_id(layout_pass_.sink);
      uniforms.push_back(sink);
      m.uniforms = rd->uniform_set_create(uniforms, layout_pass_.shader, 0);
    }
    if (!m.handle || (vulkan && !m.uniforms.is_valid())) freeMirror(m);
    return m;
  };

  std::vector<RID> touched;  // Vulkan: the mirrors copied into, for the layout pass
  for (HostTexture& e : textures_) {
    if (!e.copy) continue;
    e.copy = false;
    // Every time: Godot may have replaced the texture (resize, set_image).
    RID src = e.rs.is_valid() ? rs->texture_get_rd_texture(e.rs, false) : RID();
    if (src.is_valid() && !rd->texture_is_valid(src)) src = RID();
    if (e.cpu_only) {
      // Shown through the CPU; a new source (e.g. a ViewportTexture whose
      // viewport now exists, instead of its placeholder) is tried again.
      if (!src.is_valid() || src == e.src) continue;
      e.cpu_only = false;
      e.warned = false;
    }
    if (vulkan && !layoutPass()) {
      fail(e, "cannot be put in the layout Skia samples (no layout pass)");
      continue;
    }
    if (!src.is_valid()) {
      e.src = RID();
      fail(e, "has no RenderingDevice texture");
      continue;
    }
    if (src != e.src) {
      e.src = src;
      e.src_ok = false;
      Ref<RDTextureFormat> f = rd->texture_get_format(src);
      if (f.is_null()) {
        fail(e, "has no texture format");
        continue;
      }
      const int64_t df = f->get_format();
      if (f->get_texture_type() != RenderingDevice::TEXTURE_TYPE_2D || f->get_array_layers() != 1 ||
          f->get_samples() != RenderingDevice::TEXTURE_SAMPLES_1 || !mirrorFormatFor(df, metal, &e.mirror_format)) {
        fail(e, "is not a 2D RGBA8 / BGRA8 texture (RenderingDevice format " + std::to_string(df) +
                    "; e.g. an HDR viewport is RGBA16F)");
        continue;
      }
      if (!(int64_t(f->get_usage_bits()) & RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT)) {
        fail(e, "cannot be copied (its RenderingDevice texture lacks TEXTURE_USAGE_CAN_COPY_FROM_BIT)");
        continue;
      }
      e.src_w = int(f->get_width());
      e.src_h = int(f->get_height());
      e.src_ok = true;
    }
    if (!e.src_ok) continue;
    auto fits = [&](const TextureMirror& m) {
      return m.valid() && m.w == e.src_w && m.h == e.src_h && m.data_format == e.mirror_format;
    };
    TextureMirror* target = fits(e.cur) ? &e.cur : fits(e.next) ? &e.next : nullptr;
    if (!target) {
      freeMirror(e.next);  // never handed to the runtime
      e.next = createMirror(e.src_w, e.src_h, e.mirror_format);
      if (!e.next.valid()) {
        fail(e, "could not get a GPU copy");
        continue;
      }
      target = &e.next;
    }
    const Error err = rd->texture_copy(src, target->rd, Vector3(), Vector3(), Vector3(float(e.src_w), float(e.src_h), 1), 0,
                                       0, 0, 0);
    if (err != OK) {
      fail(e, "could not be copied on the GPU");
      continue;
    }
    if (target == &e.cur) e.shown_pending = true;  // the runtime repaints its views next frame
    if (vulkan) touched.push_back(target->uniforms);
  }
  if (!touched.empty()) {
    const int64_t list = rd->compute_list_begin();
    rd->compute_list_bind_compute_pipeline(list, layout_pass_.pipeline);
    for (const RID& u : touched) {
      rd->compute_list_bind_uniform_set(list, u, 0);
      rd->compute_list_dispatch(list, 1, 1, 1);
    }
    rd->compute_list_end();
  }
}

void JsuiView::releaseTextureGpu() {
  bool any = !retired_mirrors_.empty() || layout_pass_.shader.is_valid() || layout_pass_.sink.is_valid() ||
             layout_pass_.sampler.is_valid();
  for (HostTexture& e : textures_) {
    if (e.cur.valid()) retired_mirrors_.push_back(e.cur);
    if (e.next.valid()) retired_mirrors_.push_back(e.next);
    any = any || e.cur.valid() || e.next.valid();
    e.cur = e.next = TextureMirror{};
    e.copy = false;
  }
  if (!any) return;
  waitRender();
  runOnRenderThread([&]() {
    RenderingServer* rs = RenderingServer::get_singleton();
    RenderingDevice* rd = rs ? rs->get_rendering_device() : nullptr;
    if (!rd) return;
    for (TextureMirror& m : retired_mirrors_) {
      if (m.uniforms.is_valid() && rd->uniform_set_is_valid(m.uniforms)) rd->free_rid(m.uniforms);
      if (m.rd.is_valid()) rd->free_rid(m.rd);
    }
    LayoutPass& p = layout_pass_;
    for (RID* r : {&p.pipeline, &p.shader, &p.sampler, &p.sink})
      if (r->is_valid()) rd->free_rid(*r);
  });
  retired_mirrors_.clear();
  layout_pass_ = LayoutPass{};
}

}  // namespace jsui_godot
