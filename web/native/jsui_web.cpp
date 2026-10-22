// Browser glue linked into jsui.wasm (not part of the jsui library).
//
// JsuiConfig holds C function pointers, which JS cannot easily provide, so
// jsui_web_create() fills them with trampolines that forward to the JS host
// object registered under `hostId` (see web/jsui-web.js). Everything else
// the web host needs is the plain C API from include/jsui/jsui.h.
#include <emscripten/emscripten.h>

#include <cstdint>

#include "jsui/jsui.h"

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

namespace {

int hostIdOf(void* user) { return int(reinterpret_cast<intptr_t>(user)); }

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
  return jsui_create(&cfg);
}

}  // extern "C"
