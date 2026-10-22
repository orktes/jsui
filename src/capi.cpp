// C API: thin wrappers over jsui::Runtime.
#include <cstdlib>
#include <cstring>
#include <vector>

#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkStream.h"
#include "include/encode/SkPngEncoder.h"
#include "input/input.h"
#include "js/engine.h"
#include "jsui/jsui.h"
#include "render/gpu.h"
#include "render/gpu_metal.h"
#include "runtime.h"

struct JsuiRuntime {
  std::unique_ptr<jsui::Runtime> rt;
};

extern "C" {

void jsui_config_init(JsuiConfig* cfg) {
  std::memset(cfg, 0, sizeof *cfg);
  cfg->width = 800;
  cfg->height = 600;
  cfg->pixel_ratio = 1.0f;
}

JsuiRuntime* jsui_create(const JsuiConfig* cfg) {
  JsuiConfig c;
  if (cfg) c = *cfg;
  else jsui_config_init(&c);
  auto* h = new JsuiRuntime;
  h->rt = std::make_unique<jsui::Runtime>(c, h);
  return h;
}

void jsui_destroy(JsuiRuntime* rt) { delete rt; }

void jsui_resource_complete(JsuiRuntime* rt, uint64_t id, int status, const char* contentType, const void* data,
                            size_t size) {
  jsui::Resource res;
  res.status = status;
  if (contentType) res.contentType = contentType;
  if (data && size) res.data.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
  rt->rt->loader.complete(id, std::move(res));
}

void jsui_post_message(JsuiRuntime* rt, const char* name, const char* json) {
  rt->rt->js->hostMessage(name ? name : "", json ? json : "null");
}

int jsui_set_state(JsuiRuntime* rt, const char* key, const char* json) { return rt->rt->setState(key, json, false); }

int jsui_patch_state(JsuiRuntime* rt, const char* key, const char* json) {
  if (!json) return rt->rt->setState(key, nullptr, false);
  return rt->rt->setState(key, json, true);
}

const char* jsui_get_state(JsuiRuntime* rt, const char* key) {
  if (!key) return nullptr;
  const std::string* json = rt->rt->state.json(key);
  return json ? json->c_str() : nullptr;
}

int jsui_resolve_call(JsuiRuntime* rt, uint64_t id, const char* json) { return rt->rt->resolveCall(id, json); }
int jsui_reject_call(JsuiRuntime* rt, uint64_t id, const char* message) { return rt->rt->rejectCall(id, message); }

uint64_t jsui_call_ui(JsuiRuntime* rt, const char* name, const char* argsJson, JsuiCallResultFn done, void* user) {
  return rt->rt->callUi(name, argsJson, done, user);
}

int jsui_load_bundle_source(JsuiRuntime* rt, const char* source, size_t size, const char* name) {
  return rt->rt->loadSource(std::string(source, size), name ? name : "bundle.js") ? 0 : -1;
}

int jsui_load_bundle_url(JsuiRuntime* rt, const char* url) {
  rt->rt->loadUrl(url);
  return 0;
}

void jsui_reload(JsuiRuntime* rt) { rt->rt->reload(); }

int jsui_register_font(JsuiRuntime* rt, const char* family, const void* data, size_t size, int weight, int italic) {
  bool ok = rt->rt->fonts.registerFont(family, data, size, weight, italic != 0);
  if (ok) rt->rt->doc.markLayoutDirty();
  return ok ? 0 : -1;
}

void jsui_set_viewport(JsuiRuntime* rt, int width, int height, float pixelRatio) {
  jsui::Document& doc = rt->rt->doc;
  float scale = pixelRatio > 0 ? pixelRatio : 1.f;
  bool changed = doc.width() != float(width) || doc.height() != float(height) || doc.pixelRatio() != scale;
  doc.setViewport(float(width), float(height), scale);
  // Dimensions / useWindowDimensions in @jsui/react listen for this.
  if (changed && rt->rt->js) {
    jsui::Value::Object ev{{"type", jsui::Value("resize")},
                           {"width", jsui::Value(double(width))},
                           {"height", jsui::Value(double(height))},
                           {"scale", jsui::Value(double(scale))}};
    rt->rt->js->dispatchEvent(jsui::Value(std::move(ev)));
  }
}

void jsui_tick(JsuiRuntime* rt, double nowMs) { rt->rt->tick(nowMs); }
void jsui_tick_native(JsuiRuntime* rt, double nowMs) { rt->rt->tickNative(nowMs); }
int jsui_pending_loads(JsuiRuntime* rt) { return int(rt->rt->loader.pending()); }
int jsui_needs_redraw(JsuiRuntime* rt) { return rt->rt->needsRedraw() ? 1 : 0; }
int jsui_wants_frame(JsuiRuntime* rt) { return rt->rt->wantsFrame() ? 1 : 0; }

void jsui_render_rgba(JsuiRuntime* rt, void* pixels, int width, int height, size_t rowBytes) {
  rt->rt->render(pixels, width, height, rowBytes);
}

int jsui_render_rgba_incremental(JsuiRuntime* rt, void* pixels, int width, int height, size_t rowBytes, JsuiRect* dirty) {
  SkIRect r = rt->rt->renderIncremental(pixels, width, height, rowBytes);
  if (dirty) *dirty = JsuiRect{r.x(), r.y(), r.width(), r.height()};
  return r.isEmpty() ? 0 : 1;
}

JsuiGpuContext* jsui_gpu_create_gl(JsuiGLGetProcFn getProc, void* user) { return jsui::gpu::createGL(getProc, user); }
JsuiGpuContext* jsui_gpu_create_webgpu(void* instance, void* device, void* queue) {
  return jsui::webgpu::create(instance, device, queue);
}

namespace {
bool isWebGPU(JsuiGpuContext* gpu) { return gpu && gpu->kind == JsuiGpuContext::Kind::WebGPU; }
}  // namespace

void jsui_gpu_destroy(JsuiGpuContext* gpu) {
  if (isWebGPU(gpu)) jsui::webgpu::destroy(gpu);
  else jsui::gpu::destroy(gpu);
}
void jsui_gpu_reset(JsuiGpuContext* gpu) {
  if (!isWebGPU(gpu)) jsui::gpu::reset(gpu);
}
void jsui_gpu_context_lost(JsuiGpuContext* gpu) {
  if (isWebGPU(gpu)) jsui::webgpu::contextLost(gpu);
  else jsui::gpu::contextLost(gpu);
}
void jsui_gpu_purge(JsuiGpuContext* gpu, size_t cacheBytes) {
  if (isWebGPU(gpu)) jsui::webgpu::purge(gpu, cacheBytes);
  else jsui::gpu::purge(gpu, cacheBytes);
}

int jsui_render_webgpu(JsuiRuntime* rt, JsuiGpuContext* gpu, void* texture, int width, int height, int flags) {
  JsuiWebGPUTarget t{texture, width, height, flags};
  return jsui::webgpu::render(*rt->rt, gpu, t);
}

int jsui_render_webgpu_target(JsuiRuntime* rt, JsuiGpuContext* gpu, const JsuiWebGPUTarget* target) {
  return target ? jsui::webgpu::render(*rt->rt, gpu, *target) : -1;
}

int jsui_render_gl(JsuiRuntime* rt, JsuiGpuContext* gpu, unsigned int fbo, int width, int height, int flags) {
  JsuiGLTarget t{};
  t.fbo = fbo;
  t.width = width;
  t.height = height;
  t.flags = flags;
  return jsui::gpu::render(*rt->rt, gpu, t);
}

int jsui_render_gl_target(JsuiRuntime* rt, JsuiGpuContext* gpu, const JsuiGLTarget* target) {
  return target ? jsui::gpu::render(*rt->rt, gpu, *target) : -1;
}

JsuiMetalContext* jsui_metal_create(void* device, void* queue) { return jsui::metal::create(device, queue); }
void jsui_metal_destroy(JsuiMetalContext* mtl) { jsui::metal::destroy(mtl); }
void jsui_metal_purge(JsuiMetalContext* mtl, size_t cacheBytes) { jsui::metal::purge(mtl, cacheBytes); }
int jsui_render_metal(JsuiRuntime* rt, JsuiMetalContext* mtl, void* texture, unsigned int format, int width,
                      int height, int flags) {
  return rt ? jsui::metal::render(*rt->rt, mtl, texture, format, width, height, flags) : -1;
}

int jsui_render_png(JsuiRuntime* rt, void** outData, size_t* outSize) {
  auto& doc = rt->rt->doc;
  int w = int(doc.width() * doc.pixelRatio() + 0.5f), h = int(doc.height() * doc.pixelRatio() + 0.5f);
  std::vector<uint8_t> px(size_t(w) * h * 4);
  rt->rt->render(px.data(), w, h, size_t(w) * 4);
  SkPixmap pm(SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kPremul_SkAlphaType), px.data(), size_t(w) * 4);
  SkDynamicMemoryWStream out;
  if (!SkPngEncoder::Encode(&out, pm, {})) return -1;
  sk_sp<SkData> data = out.detachAsData();
  *outSize = data->size();
  *outData = std::malloc(data->size());
  std::memcpy(*outData, data->data(), data->size());
  return 0;
}

void jsui_free(void* p) { std::free(p); }

void jsui_set_event_time(JsuiRuntime* rt, double time_ms) { rt->rt->input->setEventTime(time_ms); }
int jsui_send_mouse_move(JsuiRuntime* rt, float x, float y) { return rt->rt->input->mouseMove(x, y); }
int jsui_send_mouse_button(JsuiRuntime* rt, float x, float y, JsuiMouseButton b, int down, int mods) {
  return rt->rt->input->mouseButton(x, y, int(b), down != 0, mods);
}
int jsui_send_mouse_wheel(JsuiRuntime* rt, float x, float y, float dx, float dy) {
  return rt->rt->input->mouseWheel(x, y, dx, dy);
}
int jsui_send_mouse_leave(JsuiRuntime* rt) { return rt->rt->input->mouseLeave(); }
int jsui_send_touch(JsuiRuntime* rt, int id, int phase, float x, float y) { return rt->rt->input->touch(id, phase, x, y); }
int jsui_send_key(JsuiRuntime* rt, const char* key, const char* code, int down, int repeat, int mods) {
  if (rt->rt->devKey(key, down != 0)) return 1;  // dev overlay (Escape)
  return rt->rt->input->key(key, code, down != 0, repeat != 0, mods);
}
int jsui_send_text(JsuiRuntime* rt, const char* utf8) { return rt->rt->input->text(utf8); }
int jsui_send_gamepad_button(JsuiRuntime* rt, int pad, int button, int down, float value) {
  return rt->rt->input->gamepadButton(pad, button, down != 0, value);
}
int jsui_send_gamepad_axis(JsuiRuntime* rt, int pad, int axis, float value) {
  return rt->rt->input->gamepadAxis(pad, axis, value);
}
void jsui_set_gamepad_connected(JsuiRuntime* rt, int pad, int connected, const char* id) {
  rt->rt->input->gamepadConnected(pad, connected != 0, id);
}
const char* jsui_get_cursor(JsuiRuntime* rt) { return rt->rt->cursor.c_str(); }
void jsui_set_clipboard_callbacks(JsuiRuntime* rt, JsuiClipboardGetFn get, JsuiClipboardSetFn set, void* user) {
  if (!rt) return;
  rt->rt->clipboardGet = get;
  rt->rt->clipboardSet = set;
  rt->rt->clipboardUser = user;
}
const char* jsui_last_error(JsuiRuntime* rt) {
  return rt->rt->lastError.empty() ? nullptr : rt->rt->lastError.c_str();
}

}  // extern "C"
