// Browser glue linked into jsui.wasm (not part of the jsui library).
//
// JsuiConfig holds C function pointers, which JS cannot easily provide, so
// jsui_web_create() fills them with trampolines that forward to the JS host
// object registered under `hostId` (see web/jsui-web.js). Everything else
// the web host needs is the plain C API from include/jsui/jsui.h.
#include <emscripten/emscripten.h>

#include <cstdint>

#include "jsui/jsui.h"

#if defined(JSUI_WEBGPU)
#include <webgpu/webgpu.h>
#endif

EM_JS(void, jsui_web_js_log, (int hostId, int level, const char* msg), {
  var h = Module.__jsuiHosts && Module.__jsuiHosts.get(hostId);
  var text = UTF8ToString(msg);
  if (h && h.onLog) { h.onLog(level, text); return; }
  (level >= 3 ? console.error : level == 2 ? console.warn : level == 0 ? console.debug : console.log)("[jsui] " + text);
});

EM_JS(void, jsui_web_js_message, (int hostId, const char* name, const char* json), {
  var h = Module.__jsuiHosts && Module.__jsuiHosts.get(hostId);
  if (h && h.onMessage) h.onMessage(UTF8ToString(name), UTF8ToString(json));
});

// Game data (shared state / calls). Call ids are passed as doubles: they
// stay far below 2^53, and a double avoids BigInt in the EM_JS signatures.
EM_JS(void, jsui_web_js_state, (int hostId, const char* key, const char* json), {
  var h = Module.__jsuiHosts && Module.__jsuiHosts.get(hostId);
  if (h && h.onState) h.onState(UTF8ToString(key), json ? UTF8ToString(json) : null);
});

EM_JS(void, jsui_web_js_call, (int hostId, double id, const char* name, const char* args), {
  var h = Module.__jsuiHosts && Module.__jsuiHosts.get(hostId);
  if (h && h.onCall) h.onCall(id, UTF8ToString(name), UTF8ToString(args));
});

EM_JS(void, jsui_web_js_call_cancel, (int hostId, double id), {
  var h = Module.__jsuiHosts && Module.__jsuiHosts.get(hostId);
  if (h && h.onCallCancel) h.onCallCancel(id);
});

EM_JS(void, jsui_web_js_call_result, (int hostId, double id, int ok, const char* result), {
  var h = Module.__jsuiHosts && Module.__jsuiHosts.get(hostId);
  if (h && h.onCallResult) h.onCallResult(id, ok, UTF8ToString(result));
});

namespace {

int hostIdOf(void* user) { return int(reinterpret_cast<intptr_t>(user)); }

void stateTrampoline(void* user, const char* key, const char* json) { jsui_web_js_state(hostIdOf(user), key, json); }

void callTrampoline(void* user, uint64_t id, const char* name, const char* args) {
  jsui_web_js_call(hostIdOf(user), double(id), name ? name : "", args ? args : "null");
}

void callCancelTrampoline(void* user, uint64_t id) { jsui_web_js_call_cancel(hostIdOf(user), double(id)); }

void callResultTrampoline(void* user, uint64_t id, int ok, const char* result) {
  jsui_web_js_call_result(hostIdOf(user), double(id), ok, result ? result : "");
}

void logTrampoline(void* user, JsuiLogLevel level, const char* message) {
  jsui_web_js_log(hostIdOf(user), int(level), message ? message : "");
}

void messageTrampoline(void* user, const char* name, const char* json) {
  jsui_web_js_message(hostIdOf(user), name ? name : "", json ? json : "null");
}

}  // namespace

extern "C" {

// Creates a runtime whose log / on_message callbacks go to JS host `hostId`.
// `baseUrl` may be NULL (relative URLs then resolve against the bundle URL).
EMSCRIPTEN_KEEPALIVE JsuiRuntime* jsui_web_create(int hostId, int width, int height, float pixelRatio, int devMode,
                                                  const char* baseUrl) {
  JsuiConfig cfg;
  jsui_config_init(&cfg);
  cfg.width = width;
  cfg.height = height;
  cfg.pixel_ratio = pixelRatio;
  cfg.dev_mode = devMode;
  cfg.base_url = baseUrl && *baseUrl ? baseUrl : nullptr;
  void* user = reinterpret_cast<void*>(intptr_t(hostId));
  cfg.log = logTrampoline;
  cfg.log_user = user;
  cfg.on_message = messageTrampoline;
  cfg.message_user = user;
  cfg.on_state = stateTrampoline;
  cfg.state_user = user;
  cfg.on_call = callTrampoline;
  cfg.on_call_cancel = callCancelTrampoline;
  cfg.call_user = user;
  return jsui_create(&cfg);
}

// jsui_call_ui with the result delivered to JS host `hostId` (onCallResult).
EMSCRIPTEN_KEEPALIVE double jsui_web_call_ui(JsuiRuntime* rt, int hostId, const char* name, const char* argsJson) {
  return double(jsui_call_ui(rt, name, argsJson, callResultTrampoline, reinterpret_cast<void*>(intptr_t(hostId))));
}

// jsui_resolve_call / jsui_reject_call with a double id (no BigInt on the JS side).
EMSCRIPTEN_KEEPALIVE int jsui_web_resolve_call(JsuiRuntime* rt, double id, const char* json) {
  return jsui_resolve_call(rt, uint64_t(id), json);
}
EMSCRIPTEN_KEEPALIVE int jsui_web_reject_call(JsuiRuntime* rt, double id, const char* message) {
  return jsui_reject_call(rt, uint64_t(id), message);
}

#if defined(JSUI_WEBGPU)
// web/jsui-webgpu.js imports its GPUDevice and each frame's canvas texture
// with Emdawnwebgpu's WebGPU.importJsDevice / importJsTexture, which return
// owned references; it drops them through these.
EMSCRIPTEN_KEEPALIVE void jsui_web_wgpu_release_texture(WGPUTexture texture) { wgpuTextureRelease(texture); }
EMSCRIPTEN_KEEPALIVE void jsui_web_wgpu_release_device(WGPUDevice device) { wgpuDeviceRelease(device); }
#endif

}  // extern "C"
