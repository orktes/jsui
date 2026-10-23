// JsuiView's GPU renderer: Skia Graphite on Godot's own RenderingDevice
// (Vulkan on Linux / Windows, Metal on macOS, and Dawn under the WebGPU
// rendering driver of the godot-aw3 fork), see jsui_view.h.
//
// Per view there are two RD textures of the view's pixel size:
//
//   src  COLOR_ATTACHMENT | INPUT_ATTACHMENT | SAMPLING | CAN_COPY_FROM |
//        CAN_COPY_TO | CAN_UPDATE (Graphite renders into colour + input attachments)
//        Skia's render target. It keeps the last frame, so a frame repaints
//        only its damage bounds (jsui_render_gpu_target with
//        JSUI_GPU_INCREMENTAL). Godot only ever uses it as a copy source, so
//        its RD tracker stays in COPY_FROM: on Vulkan the image is in
//        TRANSFER_SRC_OPTIMAL between frames, and JSUI leaves it there.
//   dst  SAMPLING | CAN_COPY_TO | CAN_UPDATE | CAN_COPY_FROM
//        What the canvas item draws (a Texture2DRD). Godot copies the damaged
//        rect src -> dst with RenderingDevice.texture_copy in its frame, so
//        Godot's render graph does all of dst's layout transitions and
//        barriers itself, whatever else samples it.
//
// Ordering: JSUI submits its work straight to Godot's main queue (the one
// RenderingDevice records the frame for) from the render thread, during
// process, i.e. before Godot submits the frame that contains the copy. On
// Vulkan the render is bracketed by full pipeline barriers (gpu_graphite.cpp),
// which also order it after the previous frame that may still be copying from
// src. On Metal both use the same MTLCommandQueue and tracked resources. On
// WebGPU (godot-aw3) both use Godot's WGPUQueue, whose submissions run in
// order, and Dawn tracks the textures' usage.
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
//   2. Otherwise Vulkan or Metal through get_driver_resource, as above.
//   3. Otherwise (Compatibility, headless, D3D12, ...) the CPU renderer.
//
// New textures (first frame, resize) are created with zeroed initial data and
// primed with one copy, and first rendered into on the next frame: by then
// Godot has submitted the upload (possibly on a transfer queue it waits for)
// and moved src into COPY_FROM. Until then the previous textures stay on
// screen.
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <condition_variable>
#include <mutex>

#include "jsui_view.h"

using namespace godot;

namespace jsui_godot {

namespace {

constexpr int kVkImageLayoutTransferSrcOptimal = 6;  // VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
constexpr unsigned kMTLPixelFormatRGBA8Unorm = 70;

void* driverResource(RenderingDevice* rd, RenderingDevice::DriverResource what, const RID& rid = RID()) {
  return reinterpret_cast<void*>(uintptr_t(rd->get_driver_resource(what, rid, 0)));
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
    } else {
      gpu_ = jsui_gpu_create_metal(driverResource(rd, RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE),
                                   driverResource(rd, RenderingDevice::DRIVER_RESOURCE_COMMAND_QUEUE));
    }
  });
  if (!gpu_) {
    UtilityFunctions::print("JsuiView: no Skia GPU context on '" + driver + "' (" +
                            String(want == Renderer::Vulkan   ? "built without JSUI_VULKAN or Skia rejected the device"
                                   : want == Renderer::WebGPU ? String(kWebGPUInterop) +
                                                                    " did not resolve every Dawn entry point, returned "
                                                                    "no device or Skia rejected the device"
                                                              : "built without JSUI_GRAPHITE_METAL or Skia rejected the device") +
                            "); using the CPU renderer");
    return;
  }
  renderer_ = want;
  UtilityFunctions::print("JsuiView: rendering with Skia Graphite on " + driver);
}

namespace {

RID createTexture(RenderingDevice* rd, int w, int h, int64_t usage, const PackedByteArray& zeros) {
  Ref<RDTextureFormat> fmt;
  fmt.instantiate();
  fmt->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D);
  fmt->set_format(RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
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
  RenderingServer* rs = RenderingServer::get_singleton();
  const int w = pixel_w_, h = pixel_h_;
  const uint64_t frame = Engine::get_singleton()->get_frames_drawn();
  bool ok = true, promoted = false, waiting = false;
  GpuTextures retired;  // the previous pair, freed once dst is off screen
  JsuiRect dirty{0, 0, 0, 0};
  runOnRenderThread([&]() {
    RenderingDevice* rd = rs->get_rendering_device();
    if (!rd || !gpu_) {
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
      t.src = createTexture(rd, w, h,
                            RenderingDevice::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RenderingDevice::TEXTURE_USAGE_INPUT_ATTACHMENT_BIT |
                                RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
                                RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT |
                                RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT,
                            zeros);
      t.dst = createTexture(rd, w, h,
                            RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT |
                                RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT,
                            zeros);
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

    JsuiGpuTarget t{};
    t.texture = driverResource(rd, RenderingDevice::DRIVER_RESOURCE_TEXTURE, gpu_cur_.src);
    t.width = w;
    t.height = h;
    t.flags = gpu_retained_ ? JSUI_GPU_INCREMENTAL : 0;
    if (renderer_ == Renderer::Vulkan) {
      t.format = 0;  // VK_FORMAT_R8G8B8A8_UNORM
      t.usage = 0;   // colour + input attachment, sampled, transfer src / dst (the bits above)
      t.layout = t.final_layout = kVkImageLayoutTransferSrcOptimal;
    } else if (renderer_ == Renderer::WebGPU) {
      t.format = 0;  // the texture's own: WGPUTextureFormat_RGBA8Unorm (DATA_FORMAT_R8G8B8A8_UNORM)
    } else {
      t.format = kMTLPixelFormatRGBA8Unorm;
    }
    if (!t.texture || jsui_render_gpu_target(rt_, gpu_, &t, &dirty) != 0) {
      ok = false;
      return;
    }
    gpu_retained_ = true;
    if (dirty.width > 0 && dirty.height > 0) {
      Vector3 pos(float(dirty.x), float(dirty.y), 0);
      rd->texture_copy(gpu_cur_.src, gpu_cur_.dst, pos, pos, Vector3(float(dirty.width), float(dirty.height), 1), 0, 0,
                       0, 0);
    }
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
  if (dirty.width > 0 && dirty.height > 0) {
    renders_++;
    dirty_px_total_ += int64_t(dirty.width) * dirty.height;
    emit_signal("frame_rendered", Rect2i(dirty.x, dirty.y, dirty.width, dirty.height));
  }
  return true;
}

PackedByteArray JsuiView::readGpuFrame() {
  PackedByteArray px;
  if (!gpu_cur_.valid()) return px;
  runOnRenderThread([&]() {
    RenderingServer* rs = RenderingServer::get_singleton();
    RenderingDevice* rd = rs ? rs->get_rendering_device() : nullptr;
    if (rd) px = rd->texture_get_data(gpu_cur_.dst, 0);
  });
  return px;
}

void JsuiView::releaseGpu() {
  RenderingServer* rs = RenderingServer::get_singleton();
  if (!rs) {
    gpu_ = nullptr;
    return;
  }
  if (!gpu_ && !gpu_cur_.valid() && !gpu_next_.valid()) return;
  if (gpu_texture_.is_valid()) gpu_texture_->set_texture_rd_rid(RID());
  runOnRenderThread([&]() {
    RenderingDevice* rd = rs->get_rendering_device();
    for (GpuTextures* t : {&gpu_cur_, &gpu_next_}) {
      if (rd && t->src.is_valid()) rd->free_rid(t->src);
      if (rd && t->dst.is_valid()) rd->free_rid(t->dst);
      *t = GpuTextures{};
    }
    if (gpu_) jsui_gpu_destroy(gpu_);
    gpu_ = nullptr;
  });
  gpu_retained_ = false;
}

}  // namespace jsui_godot
