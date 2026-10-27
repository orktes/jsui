// The host JS engine (WASM builds with JSUI_HOST_JS): the bundle runs on the
// JS engine of the page / worker / Node process that loaded jsui.wasm (V8,
// JavaScriptCore, SpiderMonkey) instead of QuickJS compiled to WASM.
//
// The JS half is web/native/jsui_hostjs.js (linked into jsui.js as a
// --pre-js): it keeps the timers, requestAnimationFrame callbacks, the
// microtask queue and the pending Host.call settle functions, evaluates the
// prelude (prelude.cpp, the same sources QuickJS runs) and the bundle in a
// scope whose globals are the runtime's (globalThis, setTimeout,
// performance, ... on the runtime clock), and implements __jsui_native over
// the jsui_hjs_* exports below. This file is the native half: JsEngine calls
// into the JS half through EM_JS, and the exports do what the QuickJS
// bridge functions (bridge*.cpp, host_api.cpp) do, with plain numbers,
// UTF-8 strings and JSON texts as arguments.
//
// Boundary encoding: props, style patches, events, Animated configs and
// call arguments cross as JSON (Value::parseJson / toJson); numbers and ids
// as numbers; strings as (pointer, length) pairs into WASM memory, which the
// JS half writes into a scratch arena. Strings coming back are returned as a
// pointer with the length in the scratch block (jsui_hjs_scratch). See
// docs/ARCHITECTURE.md "Host JS engine".
#if defined(__EMSCRIPTEN__) && defined(JSUI_HOST_JS)

#include <emscripten/emscripten.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "animated/native_animated.h"
#include "core/document.h"
#include "dev_session.h"
#include "include/effects/SkRuntimeEffect.h"
#include "input/input.h"
#include "js/bytecode.h"
#include "js/engine.h"
#include "js/engine_impl.h"
#include "platform/loader.h"
#include "render/container_view.h"
#include "render/lottie.h"
#include "render/native_component.h"
#include "render/shader_view.h"
#include "runtime.h"

// ------------------------------------------------- native -> JS (EM_JS)
// `jsuiHostJs` is defined in web/native/jsui_hostjs.js (pre-js, same scope).
// Strings go as (pointer, length); the JS side decodes them before anything
// can move WASM memory.

EM_JS(void, hjs_create, (int e, int rootId, int devMode, int lottie), {
  jsuiHostJs.create(e, rootId, !!devMode, !!lottie);
});
EM_JS(void, hjs_prelude, (int e, const char* name, const char* src, int len), {
  jsuiHostJs.prelude(e, UTF8ToString(name), jsuiHostJs.str(src, len));
});
EM_JS(void, hjs_destroy, (int e), { jsuiHostJs.destroy(e); });
EM_JS(int, hjs_eval, (int e, const char* src, int len, const char* name, int nameLen), {
  return jsuiHostJs.engine(e).evalBundle(jsuiHostJs.str(src, len), jsuiHostJs.str(name, nameLen)) ? 1 : 0;
});
EM_JS(void, hjs_run_timers, (int e, double now), { jsuiHostJs.engine(e).runTimers(now); });
EM_JS(void, hjs_run_raf, (int e, double now), { jsuiHostJs.engine(e).runAnimationFrame(now); });
EM_JS(void, hjs_run_jobs, (int e), { jsuiHostJs.engine(e).runJobs(); });
EM_JS(int, hjs_pending, (int e, double now), { return jsuiHostJs.engine(e).hasPendingWork(now) ? 1 : 0; });
EM_JS(double, hjs_next_timer, (int e), { return jsuiHostJs.engine(e).earliestDue; });
EM_JS(int, hjs_dispatch, (int e, const char* json, int len), {
  return jsuiHostJs.engine(e).dispatchEvent(jsuiHostJs.str(json, len));
});
EM_JS(void, hjs_host_message, (int e, const char* name, int nameLen, const char* json, int len), {
  jsuiHostJs.engine(e).hostMessage(jsuiHostJs.str(name, nameLen), jsuiHostJs.str(json, len));
});
EM_JS(void, hjs_layout, (int e, int id, double x, double y, double w, double h), {
  jsuiHostJs.engine(e).layoutEvent(id >>> 0, x, y, w, h);
});
EM_JS(int, hjs_call_global, (int e, const char* name, const char* json, int len), {
  return jsuiHostJs.engine(e).callGlobal(UTF8ToString(name), jsuiHostJs.str(json, len)) ? 1 : 0;
});
EM_JS(void, hjs_settle, (int e, double id, int ok, const char* payload, int len), {
  jsuiHostJs.engine(e).settleCall(id, !!ok, jsuiHostJs.str(payload, len));
});
EM_JS(int, hjs_in_call, (int e), { return jsuiHostJs.engine(e).depth > 0 ? 1 : 0; });
EM_JS(void, hjs_enter, (int e), { jsuiHostJs.engine(e).enter(); });
EM_JS(void, hjs_leave, (int e), { jsuiHostJs.engine(e).leave(); });
EM_JS(void, hjs_release_buffer, (int e, const char* key, int len), {
  jsuiHostJs.engine(e).releaseBuffer(jsuiHostJs.str(key, len));
});
// fetch() events (the prelude's host.fetch callback): the head (status,
// statusText, url, headers [[k, v]] as JSON, error), a body piece, the end.
EM_JS(void, hjs_fetch_head, (int e, double id, int status, const char* statusText, int stLen, const char* url,
                             int urlLen, const char* headers, int hLen, const char* error, int errLen), {
  var s = jsuiHostJs;
  s.engine(e).fetchEvent(id, 0, status, s.str(statusText, stLen), s.str(url, urlLen), JSON.parse(s.str(headers, hLen)),
                         s.str(error, errLen));
});
EM_JS(void, hjs_fetch_data, (int e, double id, const uint8_t* data, int len), {
  jsuiHostJs.engine(e).fetchEvent(id, 1, HEAPU8.slice(data, data + len).buffer);
});
EM_JS(void, hjs_fetch_end, (int e, double id, const char* error, int len), {
  jsuiHostJs.engine(e).fetchEvent(id, 2, jsuiHostJs.str(error, len));
});
// A loadFont finished: ok, or the error message.
EM_JS(void, hjs_font_done, (int e, double id, int ok, const char* error, int len), {
  jsuiHostJs.engine(e).fontDone(id, !!ok, jsuiHostJs.str(error, len));
});

namespace jsui {

namespace {

// Values the exports hand back besides their return value: numbers (measure,
// viewport, buffer, texture) and the length of a returned string.
struct Scratch {
  double num[8];
  int32_t len;
};
Scratch scratch;
std::string out;  // the string an export returns (valid until the next one)

const char* retString(std::string s) {
  out = std::move(s);
  scratch.len = int32_t(out.size());
  return out.c_str();
}

std::string str(const char* p, int len) { return p && len > 0 ? std::string(p, size_t(len)) : std::string(); }

Value parse(const char* p, int len) { return len > 0 ? Value::parseJson(str(p, len)) : Value(); }

std::string keysJson(const std::vector<std::string>& keys) {
  Value::Array a;
  a.reserve(keys.size());
  for (auto& k : keys) a.emplace_back(k);
  return Value(std::move(a)).toJson();
}

class HostJsEngine;
HostJsEngine* engineOf(int e) { return reinterpret_cast<HostJsEngine*>(intptr_t(e)); }

class HostJsEngine final : public JsEngine {
 public:
  explicit HostJsEngine(Runtime& r) : rt(r), alive(std::make_shared<HostJsEngine*>(this)) {
    hjs_create(id(), int(rt.doc.root()->id()), rt.config.dev_mode != 0, LottieCache::available());
    size_t n = 0;
    const PreludePiece* pieces = preludePieces(&n);
    enterJs();
    for (size_t i = 0; i < n; i++)
      hjs_prelude(id(), pieces[i].name, pieces[i].source, int(std::strlen(pieces[i].source)));
    leaveJs();
  }

  ~HostJsEngine() override {
    alive.reset();  // loader callbacks still queued become no-ops
    for (uint64_t l : loads) rt.loader.cancel(l);
    hjs_destroy(id());
  }

  int id() const { return int(reinterpret_cast<intptr_t>(this)); }

  bool runsBytecode() const override { return false; }

  bool eval(const std::string& source, const std::string& name) override {
    Perf::Scope scope(rt.doc.perf, Phase::Js);
    // A precompiled bundle (`jsui build`): QuickJS bytecode means nothing
    // here, run its embedded source (Runtime::loadSource fetches a split one).
    std::string_view js = source;
    BundleParts parts;
    if (isBundleContainer(source)) {
      if (!parseBundleContainer(source, parts) || parts.source.empty()) {
        rt.reportError(name + ": precompiled bundle has no source for the host JS engine; build it with "
                              "`jsui build --format combo` (the default) or `--format source`");
        return false;
      }
      js = parts.source;
    }
    return hjs_eval(id(), js.data(), int(js.size()), name.data(), int(name.size())) != 0;
  }

  void runTimers(double nowMs) override {
    Perf::Scope scope(rt.doc.perf, Phase::Js);
    hjs_run_timers(id(), nowMs);
  }

  void runAnimationFrame(double nowMs) override {
    Perf::Scope scope(rt.doc.perf, Phase::Js);
    hjs_run_raf(id(), nowMs);
  }

  void runJobs() override {
    Perf::Scope scope(rt.doc.perf, Phase::Js);
    hjs_run_jobs(id());
  }

  bool hasPendingWork(double nowMs) const override { return hjs_pending(id(), nowMs) != 0; }
  double nextTimer() const override { return std::min(hjs_next_timer(id()), 1e300); }

  int dispatchEvent(const Value& event) override {
    Perf::Scope scope(rt.doc.perf, Phase::Js);
    rt.doc.perf.counters.jsCalls++;
    std::string json = event.toJson();
    return hjs_dispatch(id(), json.data(), int(json.size()));
  }

  void hostMessage(const std::string& name, const std::string& json) override {
    Perf::Scope scope(rt.doc.perf, Phase::Js);
    rt.doc.perf.counters.jsCalls++;
    hjs_host_message(id(), name.data(), int(name.size()), json.data(), int(json.size()));
  }

  void layoutEvent(uint32_t node, float x, float y, float w, float h) override {
    Perf::Scope scope(rt.doc.perf, Phase::Js);
    hjs_layout(id(), int(node), x, y, w, h);
  }

  bool callGlobal(const char* name, const Value* args, int argc) override {
    Perf::Scope scope(rt.doc.perf, Phase::Js);
    rt.doc.perf.counters.jsCalls++;
    Value::Array list(args, args + argc);
    std::string json = Value(std::move(list)).toJson();
    return hjs_call_global(id(), name, json.data(), int(json.size())) != 0;
  }

  void releaseBuffer(const std::string& key) override { hjs_release_buffer(id(), key.data(), int(key.size())); }

  bool hasCall(uint64_t call) const override { return calls.count(call) > 0; }

  bool settleCall(uint64_t call, bool ok, const std::string& payload) override {
    if (!calls.erase(call)) return false;
    Perf::Scope scope(rt.doc.perf, Phase::Js);
    rt.doc.perf.counters.jsCalls++;
    hjs_settle(id(), double(call), ok, payload.data(), int(payload.size()));
    return true;
  }

  bool dropCall(uint64_t call) override { return calls.erase(call) > 0; }

  std::vector<uint64_t> pendingCalls() const override { return {calls.begin(), calls.end()}; }

  bool inCall() const override { return hjs_in_call(id()) != 0; }
  void beginBatch() override { enterJs(); }
  void endBatch() override {
    Perf::Scope scope(rt.doc.perf, Phase::Js);
    leaveJs();
  }
  size_t memoryUsage() const override { return 0; }

  void enterJs() { hjs_enter(id()); }
  void leaveJs() { hjs_leave(id()); }

  // Starts a loader request whose completion runs `done` on the runtime
  // thread (pump), only while this engine is alive.
  uint64_t startLoad(LoadRequest req, std::function<void(HostJsEngine&, uint64_t, Resource&)> done) {
    std::weak_ptr<HostJsEngine*> weak = alive;
    auto idBox = std::make_shared<uint64_t>(0);
    uint64_t l = rt.loader.load(std::move(req), [weak, idBox, done = std::move(done)](Resource res) {
      auto strong = weak.lock();
      if (!strong) return;
      (*strong)->loads.erase(*idBox);
      done(**strong, *idBox, res);
    });
    *idBox = l;
    loads.insert(l);
    return l;
  }

  // A streamed load: `on` gets each event, only while this engine is alive.
  uint64_t startStream(LoadRequest req, std::function<void(HostJsEngine&, uint64_t, LoadEvent&)> on) {
    std::weak_ptr<HostJsEngine*> weak = alive;
    uint64_t l = rt.loader.reserveId();
    auto fn = std::make_shared<std::function<void(HostJsEngine&, uint64_t, LoadEvent&)>>(std::move(on));
    rt.loader.loadStream(l, std::move(req), [weak, l, fn](LoadEvent& ev) {
      auto strong = weak.lock();
      if (!strong) return;
      if (ev.kind == LoadEvent::End || (ev.kind == LoadEvent::Head && ev.res.status == 0)) (*strong)->loads.erase(l);
      (*fn)(**strong, l, ev);
    });
    loads.insert(l);
    return l;
  }

  Runtime& rt;
  std::shared_ptr<HostJsEngine*> alive;
  std::set<uint64_t> calls;            // pending Host.calls (JS keeps their settle functions)
  std::unordered_set<uint64_t> loads;  // in-flight fetch / loadFont requests
};

// ---- export helpers

Runtime& R(int e) { return engineOf(e)->rt; }

Node* nodeOf(int e, double id) {
  if (!(id >= 1 && id <= 4294967295.0)) return nullptr;
  return R(e).findNode(uint32_t(id));
}

InputManager& inputOf(Runtime& rt, Node* n) { return *rt.surfaceOf(n->document()).input; }

bool isAncestorOrSelf(Node* a, Node* n) {
  for (Node* p = n; p; p = p->parent())
    if (p == a) return true;
  return false;
}

bool sameDocument(Runtime& rt, Node* a, Node* b) {
  if (a->document() == b->document()) return true;
  rt.log(JSUI_LOG_WARN, "a node cannot move to another surface (createPortal into another root view?)");
  return false;
}

// Native side of a tree / prop mutation from the reconciler (JSUI_COMMIT_SCOPE in bridge.cpp).
#define HJS_COMMIT_SCOPE(rt)               \
  Document& commitDoc = (rt).doc;          \
  commitDoc.perf.counters.bridgeCalls++;   \
  Perf::Scope commitScope(commitDoc.perf, Phase::Commit)

LayoutAnimSpec layoutAnimSpec(const Value& v, double duration) {
  LayoutAnimSpec s;
  if (!v.isObject()) return s;
  s.enabled = true;
  s.duration = v["duration"].isNumber() ? v["duration"].asNumber() : duration;
  s.delay = v["delay"].isNumber() ? v["delay"].asNumber() : 0;
  if (v["springDamping"].isNumber()) s.springDamping = float(v["springDamping"].asNumber());
  const std::string type = v["type"].isString() ? v["type"].asString() : "easeInEaseOut";
  s.curve = type == "linear"     ? LayoutAnimCurve::Linear
            : type == "easeIn"   ? LayoutAnimCurve::EaseIn
            : type == "easeOut"  ? LayoutAnimCurve::EaseOut
            : type == "spring"   ? LayoutAnimCurve::Spring
            : type == "keyboard" ? LayoutAnimCurve::Keyboard
                                 : LayoutAnimCurve::EaseInEaseOut;
  const std::string prop = v["property"].isString() ? v["property"].asString() : "opacity";
  s.property = prop == "scaleX"    ? LayoutAnimProperty::ScaleX
               : prop == "scaleY"  ? LayoutAnimProperty::ScaleY
               : prop == "scaleXY" ? LayoutAnimProperty::ScaleXY
                                   : LayoutAnimProperty::Opacity;
  return s;
}

std::string shaderInfoJson(const ShaderEffects::Program& prog) {
  Value::Object o;
  if (!prog.effect) {
    o["error"] = Value(prog.error);
    return Value(std::move(o)).toJson();
  }
  static const char* const kTypes[] = {"float",    "float2", "float3", "float4", "float2x2", "float3x3",
                                       "float4x4", "int",    "int2",   "int3",   "int4"};
  Value::Array uniforms, children;
  for (const SkRuntimeEffect::Uniform& u : prog.effect->uniforms())
    uniforms.push_back(Value(Value::Object{{"name", Value(std::string(u.name))},
                                           {"type", Value(kTypes[int(u.type)])},
                                           {"count", Value(double(u.count))},
                                           {"color", Value(u.isColor())}}));
  for (const SkRuntimeEffect::Child& c : prog.effect->children()) children.push_back(Value(std::string(c.name)));
  o["uniforms"] = Value(std::move(uniforms));
  o["children"] = Value(std::move(children));
  return Value(std::move(o)).toJson();
}

}  // namespace

bool JsEngine::hostAvailable() { return true; }

std::unique_ptr<JsEngine> makeHostJsEngine(Runtime& rt) { return std::make_unique<HostJsEngine>(rt); }

}  // namespace jsui

using namespace jsui;

extern "C" {

// The scratch block: double num[8] at +0, int32 length of a returned string at +64.
EMSCRIPTEN_KEEPALIVE void* jsui_hjs_scratch() { return &scratch; }

// The string the last export returned (write_state's parse error).
EMSCRIPTEN_KEEPALIVE const char* jsui_hjs_last_string() {
  scratch.len = int32_t(out.size());
  return out.c_str();
}

// The runtime clock (performance.now, timers).
EMSCRIPTEN_KEEPALIVE double jsui_hjs_now(int e) { return R(e).now; }
// __jsui_native.shouldYield (bridge.cpp's js_shouldYield).
EMSCRIPTEN_KEEPALIVE int jsui_hjs_should_yield(int e) { return R(e).jsShouldYield() ? 1 : 0; }

// Microtask jobs a drain ran (PerfCounters::jsJobs).
EMSCRIPTEN_KEEPALIVE void jsui_hjs_count_jobs(int e, int jobs) { R(e).doc.perf.counters.jsJobs += uint64_t(jobs); }

// ------------------------------------------------------------ node tree

// createNode(type, rootTag) -> id; 0: unknown type.
EMSCRIPTEN_KEEPALIVE double jsui_hjs_create_node(int e, const char* type, int len, double rootTag) {
  Runtime& rt = R(e);
  HJS_COMMIT_SCOPE(rt);
  NodeType t;
  if (!nodeTypeFromName(str(type, len), &t)) return 0;
  Document* doc = &rt.doc;
  if (rootTag >= 1 && rootTag <= 4294967295.0)
    if (Surface* s = rt.surfaceByTag(uint32_t(rootTag))) doc = &s->doc;
  return double(doc->createNode(t)->id());
}

// setProps(id, props JSON): replaces the node's props (Node diffs them).
EMSCRIPTEN_KEEPALIVE void jsui_hjs_set_props(int e, double id, const char* json, int len) {
  Runtime& rt = R(e);
  HJS_COMMIT_SCOPE(rt);
  if (Node* n = nodeOf(e, id)) n->setProps(parse(json, len));
}

// patchStyle(id, partial JSON): null members remove the key.
EMSCRIPTEN_KEEPALIVE void jsui_hjs_patch_style(int e, double id, const char* json, int len) {
  Runtime& rt = R(e);
  HJS_COMMIT_SCOPE(rt);
  Node* n = nodeOf(e, id);
  if (!n) return;
  Value partial = parse(json, len);
  if (partial.isObject()) n->patchStyle(partial.asObject());
}

EMSCRIPTEN_KEEPALIVE void jsui_hjs_set_text(int e, double id, const char* s, int len) {
  Runtime& rt = R(e);
  HJS_COMMIT_SCOPE(rt);
  if (Node* n = nodeOf(e, id)) n->setText(str(s, len));
}

EMSCRIPTEN_KEEPALIVE void jsui_hjs_append_child(int e, double parent, double child) {
  Runtime& rt = R(e);
  HJS_COMMIT_SCOPE(rt);
  Node* p = nodeOf(e, parent);
  Node* c = nodeOf(e, child);
  if (p && c && !isAncestorOrSelf(c, p) && sameDocument(rt, p, c)) p->appendChild(c);
}

EMSCRIPTEN_KEEPALIVE void jsui_hjs_insert_before(int e, double parent, double child, double before) {
  Runtime& rt = R(e);
  HJS_COMMIT_SCOPE(rt);
  Node* p = nodeOf(e, parent);
  Node* c = nodeOf(e, child);
  Node* b = nodeOf(e, before);
  if (b && b->parent() != p) b = nullptr;
  if (p && c && c != b && !isAncestorOrSelf(c, p) && sameDocument(rt, p, c)) p->insertBefore(c, b);
}

EMSCRIPTEN_KEEPALIVE void jsui_hjs_remove_child(int e, double parent, double child) {
  Runtime& rt = R(e);
  HJS_COMMIT_SCOPE(rt);
  Node* p = nodeOf(e, parent);
  Node* c = nodeOf(e, child);
  if (p && c) p->document()->removeChild(p, c);
}

EMSCRIPTEN_KEEPALIVE void jsui_hjs_destroy_node(int e, double id) {
  Runtime& rt = R(e);
  HJS_COMMIT_SCOPE(rt);
  if (Node* n = nodeOf(e, id)) n->document()->destroyNode(n);
}

// configureLayoutAnimation(config JSON | empty: disarm).
EMSCRIPTEN_KEEPALIVE void jsui_hjs_configure_layout_animation(int e, const char* json, int len) {
  Runtime& rt = R(e);
  Value cfg = parse(json, len);
  if (!cfg.isObject()) {
    for (auto& s : rt.surfaces()) s->doc.disarmLayoutAnimation();
    return;
  }
  double duration = cfg["duration"].isNumber() ? cfg["duration"].asNumber() : 300;
  LayoutAnimConfig c;
  c.create = layoutAnimSpec(cfg["create"], duration);
  c.update = layoutAnimSpec(cfg["update"], duration);
  c.remove = layoutAnimSpec(cfg["delete"], duration);
  for (auto& s : rt.surfaces()) s->doc.armLayoutAnimation(c);
}

// measure(id) -> 1 with {x, y, width, height, pageX, pageY} in scratch.num; 0: unknown id.
EMSCRIPTEN_KEEPALIVE int jsui_hjs_measure(int e, double id) {
  Node* n = nodeOf(e, id);
  if (!n) return 0;
  n->document()->layout();
  Rect r = n->layoutRect(), a = n->absoluteRect();
  double v[6] = {r.x, r.y, r.width, r.height, a.x, a.y};
  std::copy(v, v + 6, scratch.num);
  return 1;
}

// viewport(rootTag) -> {width, height, scale} in scratch.num.
EMSCRIPTEN_KEEPALIVE void jsui_hjs_viewport(int e, double rootTag) {
  Runtime& rt = R(e);
  const Document* d = &rt.doc;
  if (rootTag >= 1 && rootTag <= 4294967295.0)
    if (Surface* s = rt.surfaceByTag(uint32_t(rootTag))) d = &s->doc;
  scratch.num[0] = d->width();
  scratch.num[1] = d->height();
  scratch.num[2] = d->pixelRatio();
}

// surfaceApp(rootTag) -> JSON {appKey, initialProps}; null pointer: none.
EMSCRIPTEN_KEEPALIVE const char* jsui_hjs_surface_app(int e, double rootTag) {
  if (!(rootTag >= 0 && rootTag <= 4294967295.0)) return nullptr;
  Surface* s = R(e).surfaceByTag(uint32_t(rootTag));
  if (!s || s->appKey.empty()) return nullptr;
  std::string json = "{\"appKey\":" + Value(s->appKey).toJson() + ",\"initialProps\":" +
                     (s->propsJson.empty() ? std::string("null") : s->propsJson) + "}";
  return retString(std::move(json));
}

EMSCRIPTEN_KEEPALIVE void jsui_hjs_focus(int e, double id, int blur) {
  if (Node* n = nodeOf(e, id)) {
    if (blur) inputOf(R(e), n).blur(n);
    else inputOf(R(e), n).focus(n);
  }
}

EMSCRIPTEN_KEEPALIVE void jsui_hjs_scroll_to(int e, double id, double x, double y, int animated) {
  Node* n = nodeOf(e, id);
  if (!n) return;
  n->document()->layout();
  inputOf(R(e), n).scrollTo(n, float(x == x ? x : 0), float(y == y ? y : 0), animated != 0);
}

EMSCRIPTEN_KEEPALIVE void jsui_hjs_flash_scroll_indicators(int e, double id) {
  Node* n = nodeOf(e, id);
  if (!n) return;
  n->document()->layout();
  inputOf(R(e), n).flashScrollIndicators(n);
}

EMSCRIPTEN_KEEPALIVE void jsui_hjs_set_text_input_selection(int e, double id, double start, double end) {
  Node* n = nodeOf(e, id);
  if (!n) return;
  auto toIndex = [](double v) { return v == v ? int(std::clamp(v, 0.0, 1e9)) : 0; };
  inputOf(R(e), n).setInputSelection(n, toIndex(start), toIndex(end));
}

EMSCRIPTEN_KEEPALIVE const char* jsui_hjs_get_clipboard(int e) { return retString(R(e).clipboardText()); }

EMSCRIPTEN_KEEPALIVE void jsui_hjs_set_clipboard(int e, const char* s, int len) { R(e).setClipboardText(str(s, len)); }

// postMessage(name, json): to the embedder's on_message (and the dev session).
EMSCRIPTEN_KEEPALIVE void jsui_hjs_post_message(int e, const char* name, int nameLen, const char* json, int len) {
  Runtime& rt = R(e);
  std::string n = str(name, nameLen), j = str(json, len);
  if (rt.session && rt.session->joined())
    rt.session->uiEvent("message", {{"name", Value(n)}, {"data", j.empty() ? Value() : Value::parseJson(j)}});
  if (rt.config.on_message) rt.config.on_message(rt.config.message_user, n.c_str(), j.c_str());
}

// ------------------------------------------------------ Lottie / shaders

EMSCRIPTEN_KEEPALIVE const char* jsui_hjs_lottie_data(int e, const char* json, int len) {
  return retString(R(e).lotties->addData(str(json, len)));
}

EMSCRIPTEN_KEEPALIVE int jsui_hjs_lottie_command(int e, double id, const char* cmd, int len, double a, double b) {
  Node* n = nodeOf(e, id);
  return n && lottie::command(n, str(cmd, len), a, b) ? 1 : 0;
}

// componentCommand(id, name, args JSON) / hasComponent(name): native
// components (render/native_component.h).
EMSCRIPTEN_KEEPALIVE int jsui_hjs_component_command(int e, double id, const char* name, int nameLen, const char* args,
                                                    int argsLen) {
  Runtime& rt = R(e);
  return rt.components->command(nodeOf(e, id), str(name, nameLen), parse(args, argsLen)) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int jsui_hjs_has_component(int e, const char* name, int len) {
  return R(e).components->has(str(name, len)) ? 1 : 0;
}

// containerCommand(id, JSON command) (render/container_view.h).
EMSCRIPTEN_KEEPALIVE int jsui_hjs_container_command(int e, double id, const char* json, int len) {
  Node* n = nodeOf(e, id);
  return n && container::command(n, Value::parseJson(str(json, len))) ? 1 : 0;
}

// shaderInfo / shaderPreload (preload 1, with the images prop as JSON) -> JSON.
EMSCRIPTEN_KEEPALIVE const char* jsui_hjs_shader_info(int e, const char* sksl, int len, int preload, const char* images,
                                                      int imagesLen) {
  Runtime& rt = R(e);
  std::string src = str(sksl, len);
  auto prog = preload ? rt.shaders->preload(src, parse(images, imagesLen)) : rt.shaders->get(src);
  return retString(shaderInfoJson(*prog));
}

EMSCRIPTEN_KEEPALIVE int jsui_hjs_shaders_warming(int e) { return int(R(e).shaders->warmPending()); }

// ------------------------------------------------------------------ fonts

// loadFont(family, url, weight, italic) -> request id; the result arrives in
// the engine's fontDone(id, ok, error).
EMSCRIPTEN_KEEPALIVE double jsui_hjs_load_font(int e, const char* family, int familyLen, const char* url, int urlLen,
                                               int weight, int italic) {
  Runtime& rt = R(e);
  LoadRequest req;
  req.url = ResourceLoader::resolve(rt.baseUrl, str(url, urlLen));
  std::string fam = str(family, familyLen);
  return double(engineOf(e)->startLoad(std::move(req), [fam, weight, italic](HostJsEngine& eng, uint64_t l,
                                                                              Resource& res) {
    Runtime& rt = eng.rt;
    std::string error;
    if (!res.ok()) {
      error = "loadFont: failed to load " + res.url + " (status " + std::to_string(res.status) +
              (res.error.empty() ? "" : ", " + res.error) + ")";
    } else if (std::string why; !rt.fonts.registerFont(fam, res.data.data(), res.data.size(), weight, italic, &why)) {
      error = "loadFont: " + res.url + ": " + why;
    }
    if (error.empty()) rt.fontsChanged();
    else rt.log(JSUI_LOG_WARN, error);
    Perf::Scope scope(rt.doc.perf, Phase::Js);
    hjs_font_done(eng.id(), double(l), error.empty(), error.data(), int(error.size()));
  }));
}

// loadFontData(family, bytes, weight, italic) -> 1, or 0 with the reason as the returned string.
EMSCRIPTEN_KEEPALIVE const char* jsui_hjs_load_font_data(int e, const char* family, int familyLen,
                                                         const uint8_t* bytes, int size, int weight, int italic) {
  Runtime& rt = R(e);
  std::string why;
  if (!rt.fonts.registerFont(str(family, familyLen), bytes, size_t(size), weight, italic, &why))
    return retString(why.empty() ? std::string("not a font") : why);
  rt.fontsChanged();
  return nullptr;
}

EMSCRIPTEN_KEEPALIVE int jsui_hjs_unload_font(int e, const char* family, int len) {
  Runtime& rt = R(e);
  bool ok = rt.fonts.unregisterFamily(str(family, len));
  if (ok) rt.fontsChanged();
  return ok;
}

EMSCRIPTEN_KEEPALIVE const char* jsui_hjs_get_font_families(int e) { return retString(keysJson(R(e).fonts.familyNames())); }

// setFontFallbacks(families JSON array of names, already split).
EMSCRIPTEN_KEEPALIVE void jsui_hjs_set_font_fallbacks(int e, const char* json, int len) {
  Runtime& rt = R(e);
  std::vector<std::string> list;
  Value families = parse(json, len);
  for (const Value& v : families.asArray())
    if (v.isString()) list.push_back(v.asString());
  uint32_t gen = rt.fonts.generation;
  rt.fonts.setFallbackFamilies(std::move(list));
  if (rt.fonts.generation != gen) rt.fontsChanged();
}

EMSCRIPTEN_KEEPALIVE void jsui_hjs_set_default_font_family(int e, const char* family, int len) {
  Runtime& rt = R(e);
  uint32_t gen = rt.fonts.generation;
  rt.fonts.setDefaultFamily(str(family, len));
  if (rt.fonts.generation != gen) rt.fontsChanged();
}

// ---------------------------------------------------- dev / reload / log

EMSCRIPTEN_KEEPALIVE void jsui_hjs_reload(int e) { R(e).requestReload(); }

// devError(text) / devError(null) (clear 1).
EMSCRIPTEN_KEEPALIVE void jsui_hjs_dev_error(int e, const char* text, int len, int clear) {
  Runtime& rt = R(e);
  if (clear) {
    if (!rt.lastError.empty()) {
      rt.lastError.clear();
      rt.redrawAll();
    }
    return;
  }
  rt.reportError(str(text, len));
}

EMSCRIPTEN_KEEPALIVE void jsui_hjs_log(int e, int level, const char* msg, int len) {
  R(e).log(JsuiLogLevel(level & 3), str(msg, len));
}

// An uncaught error / unhandled rejection, already formatted ("Name: message\nstack").
EMSCRIPTEN_KEEPALIVE void jsui_hjs_report_error(int e, const char* msg, int len) { R(e).reportError(str(msg, len)); }

EMSCRIPTEN_KEEPALIVE const char* jsui_hjs_resolve_url(int e, const char* url, int urlLen, const char* base,
                                                      int baseLen, int hasBase) {
  Runtime& rt = R(e);
  return retString(ResourceLoader::resolve(hasBase ? str(base, baseLen) : rt.baseUrl, str(url, urlLen)));
}

EMSCRIPTEN_KEEPALIVE const char* jsui_hjs_base_url(int e) { return retString(R(e).baseUrl); }

// fetch(url, method, headers JSON [[k, v]], body, timeout, redirect) -> request
// id; the response arrives in the engine's fetchEvent(id, ...).
EMSCRIPTEN_KEEPALIVE double jsui_hjs_fetch(int e, const char* url, int urlLen, const char* method, int methodLen,
                                           const char* headers, int headersLen, const uint8_t* body, int bodyLen,
                                           int timeoutMs, int redirect) {
  LoadRequest req;
  req.url = str(url, urlLen);  // already resolved by the prelude (Request)
  req.method = str(method, methodLen);
  if (req.method.empty()) req.method = "GET";
  Value headerList = parse(headers, headersLen);
  for (const Value& pair : headerList.asArray())
    req.headers.emplace_back(pair.asArray().size() > 0 ? pair.asArray()[0].asString() : std::string(),
                             pair.asArray().size() > 1 ? pair.asArray()[1].asString() : std::string());
  if (body && bodyLen > 0) req.body.assign(body, body + bodyLen);
  req.timeoutMs = timeoutMs;
  req.redirect = redirect >= JSUI_REDIRECT_FOLLOW && redirect <= JSUI_REDIRECT_MANUAL ? redirect : JSUI_REDIRECT_FOLLOW;
  return double(engineOf(e)->startStream(std::move(req), [](HostJsEngine& eng, uint64_t l, LoadEvent& ev) {
    Resource& res = ev.res;
    Perf::Scope scope(eng.rt.doc.perf, Phase::Js);
    eng.rt.doc.perf.counters.jsCalls++;
    if (ev.kind == LoadEvent::Data) {
      hjs_fetch_data(eng.id(), double(l), res.data.data(), int(res.data.size()));
      return;
    }
    if (ev.kind == LoadEvent::End) {
      hjs_fetch_end(eng.id(), double(l), res.error.data(), int(res.error.size()));
      return;
    }
    Value::Array headerList;
    for (auto& [k, v] : res.headers) headerList.push_back(Value(Value::Array{Value(k), Value(v)}));
    if (res.headers.empty() && !res.contentType.empty())  // files, data:, embedder loads
      headerList.push_back(Value(Value::Array{Value("content-type"), Value(res.contentType)}));
    std::string hdrs = Value(std::move(headerList)).toJson();
    std::string statusText = res.statusText;
    if (statusText.empty() && res.ok()) statusText = "OK";
    hjs_fetch_head(eng.id(), double(l), res.status, statusText.data(), int(statusText.size()), res.url.data(),
                   int(res.url.size()), hdrs.data(), int(hdrs.size()), res.error.data(), int(res.error.size()));
  }));
}

EMSCRIPTEN_KEEPALIVE void jsui_hjs_cancel(int e, double id) {
  if (!(id > 0)) return;
  HostJsEngine* eng = engineOf(e);
  eng->rt.loader.cancel(uint64_t(id));
  eng->loads.erase(uint64_t(id));
}

// ------------------------------------------------------------- Animated

// createNode(tag, config JSON) -> null, or the error as the returned string.
EMSCRIPTEN_KEEPALIVE const char* jsui_hjs_a_create_node(int e, int tag, const char* json, int len) {
  std::string error;
  if (!R(e).animated->createNode(tag, parse(json, len), &error)) return retString("Animated (native): " + error);
  return nullptr;
}

// op: 0 connect, 1 disconnect, 2 connectToView, 3 disconnectFromView.
EMSCRIPTEN_KEEPALIVE void jsui_hjs_a_link(int e, int op, int a, double b) {
  NativeAnimated& na = *R(e).animated;
  switch (op) {
    case 0: na.connectNodes(a, int(b)); break;
    case 1: na.disconnectNodes(a, int(b)); break;
    case 2:
      na.connectToView(a, uint32_t(b));
      na.flush();
      break;
    case 3: na.disconnectFromView(a, uint32_t(b)); break;
  }
}

// op: 0 dropNode, 1 flattenOffset, 2 extractOffset, 3 startListening, 4 stopListening.
EMSCRIPTEN_KEEPALIVE void jsui_hjs_a_node_op(int e, int op, int tag) {
  NativeAnimated& na = *R(e).animated;
  switch (op) {
    case 0: na.dropNode(tag); break;
    case 1: na.flattenOffset(tag); break;
    case 2: na.extractOffset(tag); break;
    case 3: na.startListening(tag); break;
    case 4: na.stopListening(tag); break;
  }
}

// offset 0: setValue, 1: setOffset.
EMSCRIPTEN_KEEPALIVE void jsui_hjs_a_set_value(int e, int tag, double v, int offset) {
  if (offset) R(e).animated->setOffset(tag, v == v ? v : 0);
  else R(e).animated->setValue(tag, v == v ? v : 0);
}

EMSCRIPTEN_KEEPALIVE double jsui_hjs_a_get_value(int e, int tag) { return R(e).animated->getValue(tag); }

EMSCRIPTEN_KEEPALIVE const char* jsui_hjs_a_start_animation(int e, int id, int tag, const char* json, int len) {
  Runtime& rt = R(e);
  std::string error;
  if (!rt.animated->startAnimation(id, tag, parse(json, len), rt.now, &error))
    return retString("Animated (native): " + error);
  return nullptr;
}

// stopAnimation(id) -> its velocity, NaN if it was not running.
EMSCRIPTEN_KEEPALIVE double jsui_hjs_a_stop_animation(int e, int id) {
  double velocity = 0;
  if (!R(e).animated->stopAnimation(id, &velocity, R(e).now)) return std::numeric_limits<double>::quiet_NaN();
  return velocity;
}

// addEventMapping(view, eventType, path JSON array, tag) (remove 1: removeEventMapping, no path).
EMSCRIPTEN_KEEPALIVE void jsui_hjs_a_event_mapping(int e, double view, const char* type, int typeLen,
                                                   const char* path, int pathLen, int tag, int remove) {
  NativeAnimated& na = *R(e).animated;
  if (remove) {
    na.removeEventMapping(uint32_t(view), str(type, typeLen), tag);
    return;
  }
  std::vector<std::string> keys;
  Value pathList = parse(path, pathLen);
  for (const Value& k : pathList.asArray()) keys.push_back(k.asString());
  na.addEventMapping(uint32_t(view), str(type, typeLen), std::move(keys), tag);
}

// ------------------------------------------------------------ game data

// getState(key) -> JSON text; null pointer: unset.
EMSCRIPTEN_KEEPALIVE const char* jsui_hjs_get_state(int e, const char* key, int len) {
  auto lock = R(e).state.lock();
  const std::string* json = R(e).state.json(str(key, len));
  return json ? retString(*json) : nullptr;
}

EMSCRIPTEN_KEEPALIVE double jsui_hjs_state_version(int e, const char* key, int len) {
  return double(R(e).state.version(str(key, len)));
}

EMSCRIPTEN_KEEPALIVE const char* jsui_hjs_state_keys(int e) { return retString(keysJson(R(e).state.keys())); }

// statePatches(key, since) -> JSON array of patch texts; null pointer: the log cannot cover it.
EMSCRIPTEN_KEEPALIVE const char* jsui_hjs_state_patches(int e, const char* key, int len, double since) {
  if (!(since >= 1 && since < 9007199254740992.0)) return nullptr;
  std::vector<const std::string*> patches;
  auto lock = R(e).state.lock();
  if (!R(e).state.patchesSince(str(key, len), uint64_t(since), &patches)) return nullptr;
  Value::Array a;
  a.reserve(patches.size());
  for (const std::string* p : patches) a.emplace_back(*p);
  return retString(Value(std::move(a)).toJson());
}

// setState / patchState (patch 1) / removal (remove 1) -> the key's version;
// -1 with the parse error as the returned string when the JSON is invalid.
EMSCRIPTEN_KEEPALIVE double jsui_hjs_write_state(int e, const char* key, int keyLen, const char* json, int len,
                                                 int remove, int patch) {
  Runtime& rt = R(e);
  std::string k = str(key, keyLen);
  bool changed = false;
  if (remove) {
    changed = rt.state.remove(k, true);
  } else {
    std::string error;
    std::string j = str(json, len);
    HostState::Result r = patch ? rt.state.patch(k, j, &error, true) : rt.state.set(k, j, &error, true);
    if (r == HostState::Result::Invalid) {
      retString(error);
      return -1;
    }
    changed = r == HostState::Result::Changed;
  }
  // JS notifies its own subscribers; the embedder is told here.
  if (changed) rt.uiStateChanged(k);
  return double(rt.state.version(k));
}

// buffer(key) -> 1 with [address, type, count, version] in scratch.num: JS
// views the buffer's storage in WASM memory directly; 0: unset.
EMSCRIPTEN_KEEPALIVE int jsui_hjs_buffer(int e, const char* key, int len) {
  const HostBuffers::Entry* b = R(e).buffers.find(str(key, len));
  if (!b) return 0;
  scratch.num[0] = double(reinterpret_cast<uintptr_t>(b->data()));
  scratch.num[1] = double(b->type);
  scratch.num[2] = double(b->count);
  scratch.num[3] = double(b->version);
  return 1;
}

EMSCRIPTEN_KEEPALIVE double jsui_hjs_buffer_version(int e, const char* key, int len) {
  return double(R(e).buffers.version(str(key, len)));
}

EMSCRIPTEN_KEEPALIVE const char* jsui_hjs_buffer_keys(int e) { return retString(keysJson(R(e).buffers.keys())); }

// texture(key) -> 1 with [width, height, version] in scratch.num; 0: unset.
EMSCRIPTEN_KEEPALIVE int jsui_hjs_texture(int e, const char* key, int len) {
  const HostTextures::Entry* t = R(e).textures.find(str(key, len));
  if (!t) return 0;
  scratch.num[0] = t->desc.width;
  scratch.num[1] = t->desc.height;
  scratch.num[2] = double(t->descVersion);
  return 1;
}

EMSCRIPTEN_KEEPALIVE const char* jsui_hjs_texture_keys(int e) { return retString(keysJson(R(e).textures.keys())); }

// hostCall: a call id the JS side registers its settle function under
// before jsui_hjs_host_call passes the call on (the embedder may answer from
// inside on_call); 0: nobody takes calls.
EMSCRIPTEN_KEEPALIVE double jsui_hjs_new_call_id(int e) {
  Runtime& rt = R(e);
  if (!rt.config.on_call && !(rt.session && rt.session->joined())) return 0;
  return double(rt.newCallId());
}

// -> 1: on_call has it (pending until settled); 0: no on_call (forget it).
EMSCRIPTEN_KEEPALIVE int jsui_hjs_host_call(int e, double id, const char* name, int nameLen, const char* json,
                                            int len) {
  HostJsEngine* eng = engineOf(e);
  Runtime& rt = eng->rt;
  std::string n = str(name, nameLen), j = len > 0 ? str(json, len) : std::string("null");
  if (rt.session && rt.session->joined())
    rt.session->uiEvent("call", {{"name", Value(n)}, {"args", Value::parseJson(j)}});
  if (!rt.config.on_call) return 0;
  eng->calls.insert(uint64_t(id));
  rt.config.on_call(rt.config.call_user, uint64_t(id), n.c_str(), j.c_str());
  return 1;
}

EMSCRIPTEN_KEEPALIVE int jsui_hjs_cancel_call(int e, double id) {
  HostJsEngine* eng = engineOf(e);
  if (!(id >= 1) || !eng->dropCall(uint64_t(id))) return 0;
  eng->rt.hostCallCancelled(uint64_t(id));
  return 1;
}

EMSCRIPTEN_KEEPALIVE void jsui_hjs_call_result(int e, double id, int ok, const char* payload, int len) {
  if (!(id >= 1)) return;
  R(e).uiCallSettled(uint64_t(id), ok != 0, str(payload, len));
}

// ---------------------------------------------------------------- audio

EMSCRIPTEN_KEEPALIVE double jsui_hjs_audio_play(int e, const char* url, int urlLen, const char* bus, int busLen,
                                                double volume, double pitch, double pan, int loop, double fadeMs,
                                                double offsetS) {
  Runtime& rt = R(e);
  std::string b = str(bus, busLen);
  if (b.empty()) b = "sfx";
  return double(rt.audio->play(ResourceLoader::resolve(rt.baseUrl, str(url, urlLen)), b, float(volume), float(pitch),
                               float(pan), loop != 0, float(fadeMs), offsetS));
}

EMSCRIPTEN_KEEPALIVE void jsui_hjs_audio_voice(int e, int op, double voice, double volume, double pitch, double pan,
                                               double fadeMs) {
  if (voice < 1 || (op != JSUI_AUDIO_STOP && op != JSUI_AUDIO_PAUSE && op != JSUI_AUDIO_RESUME && op != JSUI_AUDIO_SET))
    return;
  R(e).audio->voice(JsuiAudioOp(op), uint64_t(voice), float(volume), float(pitch), float(pan), float(fadeMs));
}

EMSCRIPTEN_KEEPALIVE void jsui_hjs_audio_bus(int e, const char* bus, int len, double volume, int muted, double fadeMs) {
  std::string b = str(bus, len);
  if (!b.empty()) R(e).audio->bus(b, float(volume), muted != 0, float(fadeMs));
}

EMSCRIPTEN_KEEPALIVE void jsui_hjs_audio_stop_all(int e, const char* bus, int len, double fadeMs) {
  R(e).audio->stopAll(str(bus, len), float(fadeMs));
}

// magic 0: preload, 1: unload, 2: resolve only -> the absolute URL.
EMSCRIPTEN_KEEPALIVE const char* jsui_hjs_audio_sound(int e, const char* url, int len, int magic) {
  Runtime& rt = R(e);
  std::string abs = ResourceLoader::resolve(rt.baseUrl, str(url, len));
  if (magic == 0) rt.audio->preload(abs);
  else if (magic == 1) rt.audio->unload(abs);
  return retString(abs);
}

// ---------------------------------------------------------------- video

// `sources`: "url\ntype\n" per alternative (type empty: unknown).
EMSCRIPTEN_KEEPALIVE double jsui_hjs_video_open(int e, const char* sources, int srcLen, const char* texture,
                                                int texLen, int paused, int muted, double volume, int loop,
                                                double rate, double progressMs) {
  Runtime& rt = R(e);
  std::vector<std::string> urls, types;
  std::string all = str(sources, srcLen);
  for (size_t at = 0; at < all.size();) {
    size_t a = all.find('\n', at), b = a == std::string::npos ? a : all.find('\n', a + 1);
    if (b == std::string::npos) break;
    std::string u = all.substr(at, a - at);
    if (!u.empty()) {
      urls.push_back(ResourceLoader::resolve(rt.baseUrl, u));
      types.push_back(all.substr(a + 1, b - a - 1));
    }
    at = b + 1;
  }
  uint64_t id = rt.video->reserve();
  rt.video->open(id, urls, types, str(texture, texLen), paused != 0, muted != 0, float(volume), loop != 0,
                 float(rate), progressMs);
  return double(id);
}

EMSCRIPTEN_KEEPALIVE void jsui_hjs_video_set(int e, double video, int paused, int muted, double volume, int loop,
                                             double rate) {
  if (video >= 1) R(e).video->set(uint64_t(video), paused != 0, muted != 0, float(volume), loop != 0, float(rate));
}

EMSCRIPTEN_KEEPALIVE void jsui_hjs_video_seek(int e, double video, double timeS) {
  if (video >= 1) R(e).video->seek(uint64_t(video), timeS);
}

EMSCRIPTEN_KEEPALIVE void jsui_hjs_video_close(int e, double video) {
  if (video >= 1) R(e).video->close(uint64_t(video));
}

}  // extern "C"

#endif  // __EMSCRIPTEN__ && JSUI_HOST_JS
