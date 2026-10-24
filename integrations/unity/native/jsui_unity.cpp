// jsui_unity: the native plugin behind the com.orktes.jsui Unity package.
//
// The C# side calls the JSUI C API (include/jsui/jsui.h) directly through
// P/Invoke: every jsui_* function is exported from this library, since it
// links the static jsui library. What C# cannot do well through P/Invoke
// lives here as a few jsuiu_* helpers:
//
//  - callbacks: on_message and log run inside jsui_tick on the runtime's
//    thread; instead of calling back into managed code (which IL2CPP only
//    allows for static methods) they are queued and C# drains the queue
//    after each tick with jsuiu_next_message / jsuiu_next_log.
//  - the pixel buffer: jsuiu_render keeps the CPU frame between frames so
//    jsui_render_rgba_incremental repaints only what changed, and reports
//    the damaged rectangle.
//  - the GPU path (JSUI_UNITY_GPU, Metal): Skia Graphite draws into the
//    view's RenderTexture on Unity's render thread, see below.
//
// GPU path threading. Unity renders on its own render thread and the Skia
// context must be used there (it shares Unity's MTLCommandQueue, see
// jsui_unity_metal.mm). The main thread ticks the runtime in LateUpdate, then
// jsuiu_queue_render + GL.IssuePluginEvent(jsuiu_render_event_func(), id)
// queue a render job, which runs on the render thread when Unity reaches it
// (later this frame) and paints the runtime into the target texture. The job
// uses the runtime, so the main thread must not touch it until the job has
// run: C# calls jsuiu_wait_render before every runtime call after queueing
// (JsuiView.Runtime). The job counts itself done when it finishes and never
// waits for the main thread; usually it has run long before the next frame's
// first runtime call, so the wait costs nothing.
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "jsui/jsui.h"
#if JSUI_UNITY_GPU
#include "IUnityGraphics.h"
#include "jsui_unity_gpu.h"
#else
#define UNITY_INTERFACE_API
#endif

namespace {

struct Message {
  std::string name, json;
};
struct Log {
  int level;
  std::string text;
};

}  // namespace

struct JsuiUnity {
  JsuiRuntime* rt = nullptr;
  int id = 0;  // the render event's id (GL.IssuePluginEvent passes an int)
  std::deque<Message> messages;
  std::deque<Log> logs;
  Message current;  // what jsuiu_next_message handed out last
  Log currentLog;
  std::vector<uint8_t> pixels;
  int width = 0, height = 0;
  bool fresh = true;

  // GPU path: the target (set on the main thread while no job is queued,
  // read by the job) and the job fence.
  void* texture = nullptr;
  int textureWidth = 0, textureHeight = 0;
  bool textureHolds = false;  // the texture holds this runtime's last frame
  bool gpuFailed = false;
  std::mutex mutex;
  std::condition_variable doneChanged;
  uint64_t queued = 0, done = 0;
};

namespace {

// Render event id -> plugin instance (the event carries only an int).
std::mutex g_registryMutex;
std::unordered_map<int, JsuiUnity*> g_registry;
int g_nextId = 1;

JsuiUnity* lookup(int id) {
  std::lock_guard<std::mutex> lock(g_registryMutex);
  auto it = g_registry.find(id);
  return it == g_registry.end() ? nullptr : it->second;
}

void finishJob(JsuiUnity* u) {
  std::lock_guard<std::mutex> lock(u->mutex);
  u->done++;
  u->doneChanged.notify_all();
}

// The render job, on Unity's render thread.
void UNITY_INTERFACE_API OnRenderEvent(int id) {
  JsuiUnity* u = lookup(id);
  if (!u) return;
#if JSUI_UNITY_GPU
  if (u->texture && !u->gpuFailed) {
    JsuiGpuContext* gpu = jsuiu_gpu_begin();
    if (!gpu) {
      u->gpuFailed = true;
    } else {
      JsuiGpuTarget t{};
      t.texture = u->texture;
      t.format = jsuiu_gpu_texture_format(u->texture);
      t.width = u->textureWidth;
      t.height = u->textureHeight;
      t.flags = u->textureHolds ? JSUI_GPU_INCREMENTAL : 0;
      JsuiRect dirty;
      if (jsui_render_gpu_target(u->rt, gpu, &t, &dirty) == 0) {
        u->textureHolds = true;
      } else {
        u->gpuFailed = true;
        jsuiu_gpu_fail();
      }
    }
  }
#endif
  finishJob(u);
}

}  // namespace

extern "C" {

// A runtime of width x height logical px at pixel_ratio device px per px.
// dev_mode: poll the bundle's dev server (jsui dev) and hot-update.
JSUI_API JsuiUnity* jsuiu_create(int width, int height, float pixel_ratio, int dev_mode) {
  auto* u = new JsuiUnity();
  JsuiConfig cfg;
  jsui_config_init(&cfg);
  cfg.width = width;
  cfg.height = height;
  cfg.pixel_ratio = pixel_ratio;
  cfg.dev_mode = dev_mode;
  cfg.message_user = u;
  cfg.on_message = [](void* user, const char* name, const char* json) {
    static_cast<JsuiUnity*>(user)->messages.push_back({name ? name : "", json ? json : "null"});
  };
  cfg.log_user = u;
  cfg.log = [](void* user, JsuiLogLevel level, const char* text) {
    auto* self = static_cast<JsuiUnity*>(user);
    if (self->logs.size() < 1000) self->logs.push_back({int(level), text ? text : ""});
  };
  u->rt = jsui_create(&cfg);
  if (!u->rt) {
    delete u;
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(g_registryMutex);
  u->id = g_nextId++;
  g_registry[u->id] = u;
  return u;
}

JSUI_API JsuiRuntime* jsuiu_runtime(JsuiUnity* u) { return u ? u->rt : nullptr; }

JSUI_API void jsuiu_wait_render(JsuiUnity* u);

JSUI_API void jsuiu_destroy(JsuiUnity* u) {
  if (!u) return;
  jsuiu_wait_render(u);
  {
    std::lock_guard<std::mutex> lock(g_registryMutex);
    g_registry.erase(u->id);
  }
  jsui_destroy(u->rt);
  delete u;
}

// ------------------------------------------------------------ GPU path

// Whether the GPU path can be used: the plugin was built with it and Unity
// renders with Metal. Valid once the plugin is loaded (any jsuiu_ call).
JSUI_API int jsuiu_gpu_available(void) {
#if JSUI_UNITY_GPU
  return jsuiu_gpu_supported() ? 1 : 0;
#else
  return 0;
#endif
}

// Whether a render job found the GPU path unusable (no Skia context on this
// device, a failed render): the view should switch to the CPU path.
JSUI_API int jsuiu_gpu_failed(JsuiUnity* u) {
  jsuiu_wait_render(u);
  return u && u->gpuFailed ? 1 : 0;
}

// The render target: an id<MTLTexture> (RenderTexture.GetNativeTexturePtr)
// of width x height device px. A new texture is repainted in full.
JSUI_API void jsuiu_set_target(JsuiUnity* u, void* texture, int width, int height) {
  if (!u) return;
  jsuiu_wait_render(u);
  if (texture != u->texture || width != u->textureWidth || height != u->textureHeight) u->textureHolds = false;
  u->texture = texture;
  u->textureWidth = width;
  u->textureHeight = height;
}

// Forget that the target holds the last frame (its contents were lost):
// the next render repaints it in full.
JSUI_API void jsuiu_invalidate_target(JsuiUnity* u) {
  if (!u) return;
  jsuiu_wait_render(u);
  u->textureHolds = false;
}

// Counts a render job as queued and returns the event id to pass to
// GL.IssuePluginEvent(jsuiu_render_event_func(), id), which C# must call
// right after.
JSUI_API int jsuiu_queue_render(JsuiUnity* u) {
  if (!u) return 0;
  std::lock_guard<std::mutex> lock(u->mutex);
  u->queued++;
  return u->id;
}

// Waits until every queued render job has run. A job that never runs (the
// render thread stopped, e.g. at shutdown) is given up on after 2 s.
JSUI_API void jsuiu_wait_render(JsuiUnity* u) {
  if (!u) return;
  std::unique_lock<std::mutex> lock(u->mutex);
  if (u->done >= u->queued) return;
  if (!u->doneChanged.wait_for(lock, std::chrono::seconds(2), [u] { return u->done >= u->queued; }))
    u->done = u->queued;
}

// The UnityRenderingEvent for GL.IssuePluginEvent.
JSUI_API void* jsuiu_render_event_func(void) { return reinterpret_cast<void*>(&OnRenderEvent); }

// The next queued Host.postMessage from JS: 1 and its name / JSON (UTF-8,
// valid until the next call), or 0 when the queue is empty.
JSUI_API int jsuiu_next_message(JsuiUnity* u, const char** name, const char** json) {
  if (!u || u->messages.empty()) return 0;
  u->current = std::move(u->messages.front());
  u->messages.pop_front();
  *name = u->current.name.c_str();
  *json = u->current.json.c_str();
  return 1;
}

// The next queued log line (console.log, errors): 1 and its JsuiLogLevel /
// text (valid until the next call), or 0.
JSUI_API int jsuiu_next_log(JsuiUnity* u, int* level, const char** text) {
  if (!u || u->logs.empty()) return 0;
  u->currentLog = std::move(u->logs.front());
  u->logs.pop_front();
  *level = u->currentLog.level;
  *text = u->currentLog.text.c_str();
  return 1;
}

// Paints the UI into the plugin's RGBA8 premultiplied buffer of width x
// height device px (top row first), repainting only what changed since the
// last call. Returns 1 and the repainted rectangle in `dirty` (x, y, width,
// height) when pixels changed, 0 when the frame is unchanged. `pixels`
// receives the buffer (valid until the next call with another size).
JSUI_API int jsuiu_render(JsuiUnity* u, int width, int height, int force, JsuiRect* dirty, void** pixels) {
  if (!u || width <= 0 || height <= 0) return 0;
  if (width != u->width || height != u->height) {
    u->width = width;
    u->height = height;
    u->pixels.assign(size_t(width) * height * 4, 0);
    u->fresh = true;
  }
  *pixels = u->pixels.data();
  if (u->fresh || force) {
    jsui_render_rgba(u->rt, u->pixels.data(), width, height, size_t(width) * 4);
    *dirty = {0, 0, width, height};
    u->fresh = false;
    return 1;
  }
  if (!jsui_needs_redraw(u->rt)) return 0;
  if (!jsui_render_rgba_incremental(u->rt, u->pixels.data(), width, height, size_t(width) * 4, dirty)) return 0;
  return dirty->width > 0 && dirty->height > 0;
}

}  // extern "C"
