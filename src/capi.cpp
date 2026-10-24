// C API: thin wrappers over jsui::Runtime.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkStream.h"
#include "include/encode/SkPngEncoder.h"
#include "dev_session.h"
#include "input/input.h"
#include "js/bytecode.h"
#include "js/engine.h"
#include "jsui/jsui.h"
#include "render/gpu.h"
#include "render/gpu_metal.h"
#include "render/pipeline_cache.h"
#include "render/shader_view.h"
#include "runtime.h"

// A runtime handle (jsui_create: owns the runtime, shows its main surface)
// or a surface handle (jsui_surface_create). Runtime-wide calls act on the
// shared runtime through either; per-surface calls (viewport, rendering,
// input, cursor) act on the handle's surface.
struct JsuiRuntime {
  std::unique_ptr<jsui::Runtime> owned;
  jsui::Runtime* rt = nullptr;
  jsui::Surface* surface = nullptr;
};

jsui::Runtime* jsui::internalRuntime(JsuiRuntime* h) { return h ? h->rt : nullptr; }

namespace {

// The person's input, reported to the dev session (not the moves).
jsui::DevSession* reportTo(JsuiRuntime* rt) {
  jsui::DevSession* s = rt->rt->session.get();
  return s && s->reporting() ? s : nullptr;
}
jsui::Value rounded(float v) { return jsui::Value(double(std::lround(v))); }
std::string modText(int mods) {
  static const char* names[] = {"shift+", "ctrl+", "alt+", "meta+"};
  std::string out;
  for (int i = 0; i < 4; i++)
    if (mods & (1 << i)) out += names[i];
  return out;
}
const char* const kPadNames[] = {"A",    "B",     "X",  "Y",  "LB", "RB",   "LT",    "RT",  "Back",
                                 "Start", "LS",   "RS", "Up", "Down", "Left", "Right", "Home"};

}  // namespace

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
  h->owned = std::make_unique<jsui::Runtime>(c, h);
  h->rt = h->owned.get();
  h->surface = &h->rt->main();
  return h;
}

void jsui_destroy(JsuiRuntime* rt) {
  if (rt && !rt->owned) {
    jsui_surface_destroy(rt);
    return;
  }
  if (rt) {
    // Surface handles die with the runtime.
    std::vector<JsuiRuntime*> handles;
    for (auto& s : rt->rt->surfaces())
      if (s->handle != rt) handles.push_back(s->handle);
    rt->owned.reset();
    for (JsuiRuntime* h : handles) delete h;
  }
  delete rt;
}

JsuiRuntime* jsui_surface_create(JsuiRuntime* rt, const char* appKey, const char* propsJson, int width, int height,
                                 float pixelRatio) {
  if (!rt) return nullptr;
  jsui::Surface* s = rt->rt->createSurface(appKey ? appKey : "", propsJson ? propsJson : "", float(width),
                                           float(height), pixelRatio);
  if (!s) return nullptr;
  auto* h = new JsuiRuntime;
  h->rt = rt->rt;
  h->surface = s;
  s->handle = h;
  return h;
}

void jsui_surface_destroy(JsuiRuntime* surface) {
  if (!surface || surface->owned) return;  // the main surface lives as long as the runtime
  surface->rt->destroySurface(surface->surface);
  delete surface;
}

int jsui_surface_run(JsuiRuntime* surface, const char* appKey, const char* propsJson) {
  if (!surface) return -1;
  return surface->rt->runSurface(surface->surface, appKey ? appKey : "", propsJson ? propsJson : "") ? 0 : -1;
}

int jsui_surface_set_props(JsuiRuntime* surface, const char* propsJson) {
  return jsui_surface_run(surface, nullptr, propsJson);
}

int jsui_surface_tag(JsuiRuntime* surface) { return surface ? int(surface->surface->tag()) : 0; }

JsuiRuntime* jsui_surface_runtime(JsuiRuntime* surface) { return surface ? surface->rt->main().handle : nullptr; }

int jsui_surface_get_size(JsuiRuntime* surface, float* width, float* height) {
  if (!surface) return -1;
  jsui::Document& doc = surface->surface->doc;
  doc.layout();
  if (width) *width = doc.width();
  if (height) *height = doc.height();
  return 0;
}

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

int jsui_set_buffer(JsuiRuntime* rt, const char* key, JsuiBufferType type, const void* data, size_t count) {
  if (!key) return -1;
  int r = rt->rt->buffers.set(key, type, data, count);
  if (r < 0) rt->rt->log(JSUI_LOG_ERROR, std::string("jsui_set_buffer(\"") + key + "\"): invalid type");
  return r;
}

void* jsui_map_buffer(JsuiRuntime* rt, const char* key, JsuiBufferType type, size_t count) {
  if (!key) return nullptr;
  void* p = rt->rt->buffers.map(key, type, count);
  if (!p) rt->rt->log(JSUI_LOG_ERROR, std::string("jsui_map_buffer(\"") + key + "\"): invalid type");
  return p;
}

int jsui_commit_buffer(JsuiRuntime* rt, const char* key) { return key && rt->rt->buffers.commit(key) ? 1 : -1; }

int jsui_remove_buffer(JsuiRuntime* rt, const char* key) { return key && rt->rt->buffers.remove(key) ? 1 : 0; }

const void* jsui_get_buffer(JsuiRuntime* rt, const char* key, JsuiBufferType* type, size_t* count) {
  const jsui::HostBuffers::Entry* e = key ? rt->rt->buffers.find(key) : nullptr;
  if (type) *type = e ? e->type : JSUI_BUFFER_F32;
  if (count) *count = e ? e->count : 0;
  return e ? e->data() : nullptr;
}

int jsui_set_texture(JsuiRuntime* rt, const char* key, const JsuiTexture* texture) {
  if (!key) return -1;
  int r = rt->rt->textures.set(key, texture);
  if (r < 0) rt->rt->log(JSUI_LOG_ERROR, std::string("jsui_set_texture(\"") + key + "\"): invalid texture");
  return r;
}

int jsui_set_texture_pixels(JsuiRuntime* rt, const char* key, const void* rgba, int width, int height,
                            size_t rowBytes, int flags) {
  if (!key) return -1;
  int r = rt->rt->textures.setPixels(key, rgba, width, height, rowBytes, flags);
  if (r < 0) rt->rt->log(JSUI_LOG_ERROR, std::string("jsui_set_texture_pixels(\"") + key + "\"): invalid pixels");
  return r;
}

int jsui_texture_updated(JsuiRuntime* rt, const char* key) { return key && rt->rt->textures.updated(key) ? 1 : -1; }

int jsui_remove_texture(JsuiRuntime* rt, const char* key) { return key && rt->rt->textures.remove(key) ? 1 : 0; }

void jsui_audio_voice_ended(JsuiRuntime* rt, uint64_t voice, int status) {
  if (rt) rt->rt->audio->voiceEnded(voice, status);
}
void jsui_audio_sound_loaded(JsuiRuntime* rt, const char* sound, int ok, double duration_s) {
  if (rt && sound) rt->rt->audio->soundLoaded(sound, ok != 0, duration_s);
}
void jsui_fetch(JsuiRuntime* rt, const char* url, JsuiFetchFn done, void* user) {
  if (!rt || !url || !done) return;
  jsui::Runtime* r = rt->rt;
  r->loader.load(jsui::ResourceLoader::resolve(r->baseUrl, url), [done, user](jsui::Resource res) {
    done(user, res.status, res.contentType.c_str(), res.data.data(), res.data.size());
  });
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

int jsui_compile_bundle(const char* source, size_t size, const char* name, int keepSource, const char* sourceUrl,
                        void** outData, size_t* outSize, char** outError) {
  if (outData) *outData = nullptr;
  if (outSize) *outSize = 0;
  if (outError) *outError = nullptr;
  std::string out, error;
  if (!source || !outData || !outSize) {
    error = "jsui_compile_bundle: missing argument";
  } else if (jsui::compileBundle(std::string_view(source, size), name ? name : "bundle.js", keepSource != 0,
                                 sourceUrl ? sourceUrl : "", out, error)) {
    *outData = std::malloc(out.size() ? out.size() : 1);
    if (*outData) {
      std::memcpy(*outData, out.data(), out.size());
      *outSize = out.size();
      return 0;
    }
    error = "out of memory";
  }
  if (outError) {
    *outError = static_cast<char*>(std::malloc(error.size() + 1));
    if (*outError) std::memcpy(*outError, error.c_str(), error.size() + 1);
  }
  return -1;
}

const char* jsui_bytecode_key(void) { return jsui::bytecodeKey(); }

void jsui_reload(JsuiRuntime* rt) {
  if (jsui::DevSession* s = rt->rt->session.get(); s && s->reporting()) s->userInput("reload", {});
  rt->rt->reload();
}

void jsui_dev_update(JsuiRuntime* rt) { rt->rt->devUpdate(); }

void jsui_set_dev_session(JsuiRuntime* rt, const char* name, int enabled) {
  if (!rt || !rt->rt->session) return;
  rt->rt->session->name = name ? name : "";
  rt->rt->session->setEnabled(enabled != 0);
}

int jsui_register_font(JsuiRuntime* rt, const char* family, const void* data, size_t size, int weight, int italic) {
  std::string why;
  if (!rt->rt->fonts.registerFont(family ? family : "", data, size, weight, italic, &why)) {
    rt->rt->log(JSUI_LOG_WARN, "jsui_register_font(" + std::string(family ? family : "") + "): " + why);
    return -1;
  }
  rt->rt->fontsChanged();
  return 0;
}

int jsui_unregister_font(JsuiRuntime* rt, const char* family) {
  if (!family || !rt->rt->fonts.unregisterFamily(family)) return -1;
  rt->rt->fontsChanged();
  return 0;
}

void jsui_set_font_fallbacks(JsuiRuntime* rt, const char* families) {
  std::vector<std::string> list;
  std::string csv = families ? families : "";
  size_t i = 0;
  while (i <= csv.size()) {
    size_t c = csv.find(',', i);
    if (c == std::string::npos) c = csv.size();
    std::string f = csv.substr(i, c - i);
    i = c + 1;
    size_t a = f.find_first_not_of(" \t\"'"), b = f.find_last_not_of(" \t\"'");
    if (a != std::string::npos) list.push_back(f.substr(a, b - a + 1));
  }
  uint32_t gen = rt->rt->fonts.generation;
  rt->rt->fonts.setFallbackFamilies(std::move(list));
  if (rt->rt->fonts.generation != gen) rt->rt->fontsChanged();
}

void jsui_set_default_font_family(JsuiRuntime* rt, const char* family) {
  uint32_t gen = rt->rt->fonts.generation;
  if (family) rt->rt->fonts.setDefaultFamily(family);
  if (rt->rt->fonts.generation != gen) rt->rt->fontsChanged();
}

void jsui_set_viewport(JsuiRuntime* rt, int width, int height, float pixelRatio) {
  rt->surface->setViewport(float(width), float(height), pixelRatio);
}

void jsui_tick(JsuiRuntime* rt, double nowMs) { rt->rt->tick(nowMs); }
void jsui_tick_native(JsuiRuntime* rt, double nowMs) { rt->rt->tickNative(nowMs); }
void jsui_set_js_frame_budget(JsuiRuntime* rt, double ms) { rt->rt->jsSliceMs = ms; }
double jsui_get_js_frame_budget(JsuiRuntime* rt) { return rt->rt->jsSliceMs; }
int jsui_pending_loads(JsuiRuntime* rt) { return int(rt->rt->loader.pending()); }
int jsui_needs_redraw(JsuiRuntime* rt) { return rt->surface->needsRedraw() ? 1 : 0; }
int jsui_wants_frame(JsuiRuntime* rt) { return rt->rt->wantsFrame() ? 1 : 0; }

// Every render is the Paint phase (jsui_set_profiling); layout and image
// decodes inside it book to their own phases, Graphite's submit to Gpu.
#define JSUI_PAINT_SCOPE(rt) jsui::Perf::Scope paint_scope((rt)->surface->doc.perf, jsui::Phase::Paint)

void jsui_render_rgba(JsuiRuntime* rt, void* pixels, int width, int height, size_t rowBytes) {
  JSUI_PAINT_SCOPE(rt);
  rt->surface->render(pixels, width, height, rowBytes);
}

int jsui_render_rgba_incremental(JsuiRuntime* rt, void* pixels, int width, int height, size_t rowBytes, JsuiRect* dirty) {
  JSUI_PAINT_SCOPE(rt);
  SkIRect r = rt->surface->renderIncremental(pixels, width, height, rowBytes);
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
  if (jsui::graphite_gpu::owns(gpu)) jsui::graphite_gpu::destroy(gpu);
  else if (jsui::d3d::owns(gpu)) jsui::d3d::destroy(gpu);
  else if (isWebGPU(gpu)) jsui::webgpu::destroy(gpu);
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
  if (jsui::graphite_gpu::owns(gpu)) jsui::graphite_gpu::purge(gpu, cacheBytes);
  else if (jsui::d3d::owns(gpu)) jsui::d3d::purge(gpu, cacheBytes);
  else if (isWebGPU(gpu)) jsui::webgpu::purge(gpu, cacheBytes);
  else jsui::gpu::purge(gpu, cacheBytes);
}

int jsui_render_webgpu(JsuiRuntime* rt, JsuiGpuContext* gpu, void* texture, int width, int height, int flags) {
  JSUI_PAINT_SCOPE(rt);
  JsuiWebGPUTarget t{texture, width, height, flags};
  return jsui::webgpu::render(*rt->surface, gpu, t);
}

int jsui_render_webgpu_target(JsuiRuntime* rt, JsuiGpuContext* gpu, const JsuiWebGPUTarget* target) {
  JSUI_PAINT_SCOPE(rt);
  return target ? jsui::webgpu::render(*rt->surface, gpu, *target) : -1;
}

JsuiGpuContext* jsui_gpu_create_vulkan(const JsuiVulkanDevice* device) {
  return device ? jsui::graphite_gpu::createVulkan(*device) : nullptr;
}
JsuiGpuContext* jsui_gpu_create_metal(void* device, void* queue) { return jsui::graphite_gpu::createMetal(device, queue); }
JsuiGpuContext* jsui_gpu_create_dawn(void* instance, void* device, void* queue) {
  return jsui::graphite_gpu::createDawn(instance, device, queue);
}
JsuiGpuContext* jsui_gpu_create_dawn_procs(void* wgpu_get_proc_address, void* instance, void* device, void* queue) {
  return jsui::graphite_gpu::createDawnProcs(wgpu_get_proc_address, instance, device, queue);
}
const char* jsui_dawn_revision(void) { return jsui::graphite_gpu::dawnRevision(); }
JsuiGpuContext* jsui_gpu_create_d3d12(void* device, void* queue) { return jsui::d3d::create12(device, queue); }
JsuiGpuContext* jsui_gpu_create_d3d11(void* device) { return jsui::d3d::create11(device); }

int jsui_render_gpu_target(JsuiRuntime* rt, JsuiGpuContext* gpu, const JsuiGpuTarget* target, JsuiRect* dirty) {
  if (!rt || !target) {
    if (dirty) *dirty = JsuiRect{0, 0, 0, 0};
    return -1;
  }
  JSUI_PAINT_SCOPE(rt);
  if (jsui::d3d::owns(gpu)) return jsui::d3d::render(*rt->surface, gpu, *target, dirty);
  return jsui::graphite_gpu::render(*rt->surface, gpu, *target, dirty);
}

int jsui_render_vulkan(JsuiRuntime* rt, JsuiGpuContext* gpu, void* image, int width, int height, int flags) {
  JsuiGpuTarget t{};
  t.texture = image;
  t.width = width;
  t.height = height;
  t.flags = flags;
  t.layout = t.final_layout = 5;  // VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
  return jsui_render_gpu_target(rt, gpu, &t, nullptr);
}

int jsui_gpu_set_pipeline_cache(JsuiGpuContext* gpu, const char* path) {
  if (!gpu || !gpu->pipelines) return -1;
  return gpu->pipelines->setFile(path ? path : "");
}

int jsui_gpu_precompile_pipelines(JsuiGpuContext* gpu, const void* data, size_t size) {
  if (!gpu || !gpu->pipelines) return -1;
  return gpu->pipelines->addList(data, size);
}

int jsui_gpu_save_pipelines(JsuiGpuContext* gpu, void** outData, size_t* outSize) {
  if (!outData || !outSize) return -1;
  *outData = nullptr;
  *outSize = 0;
  if (!gpu || !gpu->pipelines) return -1;
  std::string list = gpu->pipelines->save();
  void* p = std::malloc(list.size());
  if (!p) return -1;
  std::memcpy(p, list.data(), list.size());
  *outData = p;
  *outSize = list.size();
  return 0;
}

int jsui_gpu_precompile_step(JsuiGpuContext* gpu, double budgetMs) {
  return gpu && gpu->pipelines ? gpu->pipelines->step(budgetMs) : 0;
}

int jsui_precompile_shader(JsuiRuntime* rt, const char* sksl, const char* imagesJson) {
  if (!rt || !sksl) return -1;
  jsui::Value images = imagesJson && *imagesJson ? jsui::Value::parseJson(imagesJson) : jsui::Value();
  auto prog = rt->rt->shaders->preload(sksl, images);
  if (prog->effect) return 0;
  rt->rt->log(JSUI_LOG_WARN, "jsui_precompile_shader: " + prog->error);
  return -1;
}

void jsui_gpu_get_pipeline_stats(JsuiGpuContext* gpu, JsuiPipelineStats* out) {
  if (!out) return;
  *out = JsuiPipelineStats{};
  if (!gpu || !gpu->pipelines) return;
  jsui::pipeline_cache::Stats s = gpu->pipelines->stats();
  out->mode = s.mode;
  out->queued = s.queued;
  out->precompiled = s.precompiled;
  out->failed = s.failed;
  out->compiled = s.compiled;
  out->added = s.added;
  out->unserializable = s.unserializable;
  out->known = s.known;
}

int jsui_render_gl(JsuiRuntime* rt, JsuiGpuContext* gpu, unsigned int fbo, int width, int height, int flags) {
  JsuiGLTarget t{};
  t.fbo = fbo;
  t.width = width;
  t.height = height;
  t.flags = flags;
  JSUI_PAINT_SCOPE(rt);
  return jsui::gpu::render(*rt->surface, gpu, t);
}

int jsui_render_gl_target(JsuiRuntime* rt, JsuiGpuContext* gpu, const JsuiGLTarget* target) {
  JSUI_PAINT_SCOPE(rt);
  return target ? jsui::gpu::render(*rt->surface, gpu, *target) : -1;
}

JsuiMetalContext* jsui_metal_create(void* device, void* queue) { return jsui::metal::create(device, queue); }
void jsui_metal_destroy(JsuiMetalContext* mtl) { jsui::metal::destroy(mtl); }
void jsui_metal_purge(JsuiMetalContext* mtl, size_t cacheBytes) { jsui::metal::purge(mtl, cacheBytes); }
int jsui_render_metal(JsuiRuntime* rt, JsuiMetalContext* mtl, void* texture, unsigned int format, int width,
                      int height, int flags) {
  if (!rt) return -1;
  JSUI_PAINT_SCOPE(rt);
  return jsui::metal::render(*rt->surface, mtl, texture, format, width, height, flags);
}

int jsui_render_png(JsuiRuntime* rt, void** outData, size_t* outSize) {
  auto& doc = rt->surface->doc;
  doc.layout();
  int w = int(doc.width() * doc.pixelRatio() + 0.5f), h = int(doc.height() * doc.pixelRatio() + 0.5f);
  if (w <= 0 || h <= 0) return -1;
  std::vector<uint8_t> px(size_t(w) * h * 4);
  rt->surface->render(px.data(), w, h, size_t(w) * 4);
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

void jsui_set_profiling(JsuiRuntime* rt, int on) { rt->surface->doc.perf.setProfiling(on != 0); }

void jsui_get_perf(JsuiRuntime* rt, JsuiPerf* out, int reset) {
  jsui::Perf& p = rt->surface->doc.perf;
  auto us = [&](jsui::Phase ph) { return p.us[size_t(ph)]; };
  const jsui::PerfCounters& c = p.counters;
  *out = JsuiPerf{};
  out->js_us = us(jsui::Phase::Js);
  out->commit_us = us(jsui::Phase::Commit);
  out->animated_us = us(jsui::Phase::Animated);
  out->style_us = us(jsui::Phase::Style);
  out->layout_us = us(jsui::Phase::Layout);
  out->input_us = us(jsui::Phase::Input);
  out->paint_us = us(jsui::Phase::Paint);
  out->other_us = us(jsui::Phase::Other);
  out->gpu_submit_us = us(jsui::Phase::Gpu);
  out->image_decode_us = c.imageDecodeUs;
  out->image_decodes = c.imageDecodes;
  out->pipeline_compiles = c.pipelineCompiles;
  out->paragraph_builds = c.paragraphBuilds;
  out->text_measures = c.textMeasures;
  out->layout_passes = c.layoutPasses;
  out->bridge_calls = c.bridgeCalls;
  if (reset) p.reset();
}

void jsui_set_event_time(JsuiRuntime* rt, double time_ms) { rt->surface->input->setEventTime(time_ms); }
#define JSUI_INPUT_SCOPE jsui::Perf::Scope perf_scope(rt->surface->doc.perf, jsui::Phase::Input)
int jsui_hit_test(JsuiRuntime* rt, float x, float y) {
  if (!rt || !std::isfinite(x) || !std::isfinite(y)) return 0;
  return int(rt->surface->input->capturesAt(x, y));
}
int jsui_hit_regions(JsuiRuntime* rt, float* out, int max_regions) {
  if (!rt) return 0;
  std::vector<float> regions;
  rt->surface->input->hitRegions(&regions);
  int n = int(regions.size() / 5);
  if (out && max_regions > 0) std::copy_n(regions.begin(), size_t(std::min(n, max_regions)) * 5, out);
  return n;
}
int jsui_send_mouse_move(JsuiRuntime* rt, float x, float y) {
  JSUI_INPUT_SCOPE;
  return rt->surface->input->mouseMove(x, y);
}
int jsui_send_mouse_button(JsuiRuntime* rt, float x, float y, JsuiMouseButton b, int down, int mods) {
  JSUI_INPUT_SCOPE;
  int r = rt->surface->input->mouseButton(x, y, int(b), down != 0, mods);
  if (jsui::DevSession* s = down ? reportTo(rt) : nullptr)
    s->userInput("click", {{"x", rounded(x)}, {"y", rounded(y)}, {"button", jsui::Value(int(b))}, {"consumed", jsui::Value(r != 0)}});
  return r;
}
int jsui_send_mouse_wheel(JsuiRuntime* rt, float x, float y, float dx, float dy) {
  JSUI_INPUT_SCOPE;
  return rt->surface->input->mouseWheel(x, y, dx, dy);
}
int jsui_send_mouse_leave(JsuiRuntime* rt) {
  JSUI_INPUT_SCOPE;
  return rt->surface->input->mouseLeave();
}
int jsui_send_touch(JsuiRuntime* rt, int id, int phase, float x, float y) {
  JSUI_INPUT_SCOPE;
  int r = rt->surface->input->touch(id, phase, x, y);
  if (jsui::DevSession* s = phase == 0 ? reportTo(rt) : nullptr)
    s->userInput("tap", {{"x", rounded(x)}, {"y", rounded(y)}, {"consumed", jsui::Value(r != 0)}});
  return r;
}
int jsui_send_key(JsuiRuntime* rt, const char* key, const char* code, int down, int repeat, int mods) {
  JSUI_INPUT_SCOPE;
  if (rt->rt->devKey(key, down != 0)) return 1;  // dev overlay (Escape)
  int r = rt->surface->input->key(key, code, down != 0, repeat != 0, mods);
  // Typed characters arrive as text; named keys and shortcuts here.
  bool named = key && (std::strlen(key) > 1 || (mods & ~1));
  if (jsui::DevSession* s = down && !repeat && named ? reportTo(rt) : nullptr)
    s->userInput("key", {{"key", jsui::Value(modText(mods) + key)}, {"consumed", jsui::Value(r != 0)}});
  return r;
}
int jsui_send_text(JsuiRuntime* rt, const char* utf8) {
  JSUI_INPUT_SCOPE;
  int r = rt->surface->input->text(utf8);
  if (jsui::DevSession* s = utf8 && *utf8 ? reportTo(rt) : nullptr) s->userInput("type", {{"text", jsui::Value(utf8)}});
  return r;
}
int jsui_send_gamepad_button(JsuiRuntime* rt, int pad, int button, int down, float value) {
  JSUI_INPUT_SCOPE;
  int r = rt->surface->input->gamepadButton(pad, button, down != 0, value);
  if (jsui::DevSession* s = down ? reportTo(rt) : nullptr) {
    jsui::Value name = button >= 0 && button < 17 ? jsui::Value(kPadNames[button]) : jsui::Value(button);
    s->userInput("press", {{"button", name}, {"pad", jsui::Value(pad)}, {"consumed", jsui::Value(r != 0)}});
  }
  return r;
}
int jsui_send_gamepad_axis(JsuiRuntime* rt, int pad, int axis, float value) {
  JSUI_INPUT_SCOPE;
  return rt->surface->input->gamepadAxis(pad, axis, value);
}
void jsui_set_gamepad_connected(JsuiRuntime* rt, int pad, int connected, const char* id) {
  rt->surface->input->gamepadConnected(pad, connected != 0, id);
}
const char* jsui_get_cursor(JsuiRuntime* rt) { return rt->surface->cursor.c_str(); }
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
