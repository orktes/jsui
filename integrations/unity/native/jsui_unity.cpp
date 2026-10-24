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
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "jsui/jsui.h"

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
  std::deque<Message> messages;
  std::deque<Log> logs;
  Message current;  // what jsuiu_next_message handed out last
  Log currentLog;
  std::vector<uint8_t> pixels;
  int width = 0, height = 0;
  bool fresh = true;
};

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
  return u;
}

JSUI_API JsuiRuntime* jsuiu_runtime(JsuiUnity* u) { return u ? u->rt : nullptr; }

JSUI_API void jsuiu_destroy(JsuiUnity* u) {
  if (!u) return;
  jsui_destroy(u->rt);
  delete u;
}

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
