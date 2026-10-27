#include "render/container_view.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

#include "core/document.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRect.h"
#include "platform/loader.h"
#include "render/painter.h"
#include "runtime.h"
#include "surface.h"

namespace jsui {

namespace container {
namespace {

NodeState* stateOf(const Node* node) {
  if (!node || node->type() != NodeType::Container) return nullptr;
  return static_cast<NodeState*>(node->imageCache.get());
}

void push(NodeState* st, Value::Object ev) {
  if (!st->cache || !st->node) return;
  st->cache->events.push_back({st->node->id(), Value(std::move(ev))});
}

Value parsed(const char* json) {
  if (!json) return Value();
  std::string err;
  Value v = Value::parseJson(json, &err);
  return err.empty() ? v : Value();
}

// The child's embedder callbacks: everything becomes an event for the
// parent's JS (delivered after the children's ticks), or goes to the parent's
// loader / log.
void onLog(void* user, JsuiLogLevel level, const char* message) {
  auto* st = static_cast<NodeState*>(user);
  std::string msg = message ? message : "";
  st->cache->runtime().log(level, "[JSUIContainer] " + msg);
  if (level == JSUI_LOG_ERROR) push(st, {{"kind", Value("error")}, {"message", Value(msg)}});
}

void onLoad(void* user, JsuiRuntime* child, uint64_t requestId, const char* url) {
  auto* st = static_cast<NodeState*>(user);
  ResourceLoader& loader = st->cache->runtime().loader;
  uint64_t id = loader.reserveId();
  st->loads.insert(id);
  LoadRequest req;
  req.url = url ? url : "";
  loader.load(id, std::move(req), [st, child, requestId, id](Resource r) {
    st->loads.erase(id);
    jsui_resource_complete(child, requestId, r.status, r.contentType.c_str(), r.data.data(), r.data.size());
  });
}

void onMessage(void* user, const char* name, const char* json) {
  push(static_cast<NodeState*>(user),
       {{"kind", Value("message")}, {"name", Value(name ? name : "")}, {"payload", parsed(json)}});
}

void onState(void* user, const char* key, const char* json) {
  Value::Object ev{{"kind", Value("state")}, {"key", Value(key ? key : "")}, {"value", parsed(json)}};
  if (!json) ev.emplace("removed", Value(true));
  push(static_cast<NodeState*>(user), std::move(ev));
}

void onCall(void* user, uint64_t callId, const char* name, const char* argsJson) {
  push(static_cast<NodeState*>(user), {{"kind", Value("call")},
                                       {"callId", Value(double(callId))},
                                       {"name", Value(name ? name : "")},
                                       {"args", parsed(argsJson)}});
}

void onCallCancel(void* user, uint64_t callId) {
  push(static_cast<NodeState*>(user), {{"kind", Value("callCancel")}, {"callId", Value(double(callId))}});
}

// A parent -> child call (jsui_call_ui) on its way back.
struct UiCall {
  Containers* cache;
  uint32_t node;
  double seq;
};

void uiCallDone(void* user, uint64_t, int ok, const char* result) {
  auto* c = static_cast<UiCall*>(user);
  Value::Object ev{{"kind", Value("callResult")}, {"seq", Value(c->seq)}, {"ok", Value(ok != 0)}};
  if (ok) ev.emplace("result", parsed(result));
  else ev.emplace("error", Value(result ? result : "failed"));
  c->cache->events.push_back({c->node, Value(std::move(ev))});
  delete c;
}

void destroyChild(NodeState* st) {
  if (!st->child) return;
  ResourceLoader& loader = st->cache->runtime().loader;
  for (uint64_t id : st->loads) loader.cancel(id);
  st->loads.clear();
  JsuiRuntime* child = st->child;
  st->child = nullptr;
  jsui_destroy(child);  // fails the child's unsettled calls (uiCallDone)
}

void createChild(NodeState* st) {
  destroyChild(st);
  if (st->src.empty() && st->data.empty()) return;
  Runtime& parent = st->cache->runtime();
  JsuiConfig cfg;
  jsui_config_init(&cfg);
  cfg.width = std::max(1, int(std::ceil(st->width)));
  cfg.height = std::max(1, int(std::ceil(st->height)));
  cfg.pixel_ratio = st->pixelRatio > 0 ? st->pixelRatio : 1;
  st->baseUrl = st->src.empty() ? parent.baseUrl : st->src;
  cfg.base_url = st->baseUrl.empty() ? nullptr : st->baseUrl.c_str();
  cfg.log = onLog;
  cfg.log_user = st;
  cfg.load = onLoad;
  cfg.load_user = st;
  cfg.on_message = onMessage;
  cfg.message_user = st;
  cfg.on_state = onState;
  cfg.state_user = st;
  cfg.on_call = onCall;
  cfg.on_call_cancel = onCallCancel;
  cfg.call_user = st;
  cfg.js_memory_limit_mb = parent.config.js_memory_limit_mb;
  cfg.js_timeout_ms = parent.config.js_timeout_ms;
  cfg.audio_disabled = parent.config.audio_disabled;
  st->child = jsui_create(&cfg);
  // The exact (fractional) viewport.
  internalRuntime(st->child)->main().setViewport(std::max(0.f, st->width), std::max(0.f, st->height),
                                                 cfg.pixel_ratio);
  if (!st->appKey.empty()) jsui_surface_run(st->child, st->appKey.c_str(), st->propsJson.c_str());
  st->runPending = false;
  if (!st->data.empty())
    jsui_load_bundle_source(st->child, st->data.data(), st->data.size(), "JSUIContainer");
  else
    jsui_load_bundle_url(st->child, st->src.c_str());
}

std::string stringOf(const Value& v) { return v.isString() ? v.asString() : v.isNull() ? "null" : v.toJson(); }

void runCommand(NodeState* st, const Value& cmd) {
  JsuiRuntime* child = st->child;
  const std::string& op = cmd["op"].asString();
  if (op == "reload") {
    createChild(st);
    return;
  }
  if (op == "call") {
    auto* c = new UiCall{st->cache, st->node->id(), cmd["seq"].asNumber()};
    if (!child) {
      uiCallDone(c, 0, 0, "the container has no running bundle");
      return;
    }
    std::string args = cmd["args"].toJson();
    jsui_call_ui(child, cmd["name"].asString().c_str(), args.c_str(), uiCallDone, c);
    return;
  }
  if (!child) return;
  if (op == "post") {
    std::string json = cmd["payload"].toJson();
    jsui_post_message(child, cmd["name"].asString().c_str(), json.c_str());
  } else if (op == "resolve") {
    std::string json = cmd["value"].toJson();
    jsui_resolve_call(child, uint64_t(cmd["callId"].asNumber()), json.c_str());
  } else if (op == "reject") {
    jsui_reject_call(child, uint64_t(cmd["callId"].asNumber()), cmd["message"].asString().c_str());
  } else if (op == "state") {
    const std::string& key = cmd["key"].asString();
    if (cmd["remove"].asBool()) {
      jsui_set_state(child, key.c_str(), nullptr);
    } else {
      std::string json = cmd["value"].toJson();
      if (cmd["patch"].asBool()) jsui_patch_state(child, key.c_str(), json.c_str());
      else jsui_set_state(child, key.c_str(), json.c_str());
    }
  }
}

// The node's content box: its layout size less padding and border.
void contentBox(const Node* node, float* x, float* y, float* w, float* h) {
  YGNodeConstRef yg = node->yoga();
  Rect r = node->layoutRect();
  float l = YGNodeLayoutGetPadding(yg, YGEdgeLeft) + YGNodeLayoutGetBorder(yg, YGEdgeLeft);
  float t = YGNodeLayoutGetPadding(yg, YGEdgeTop) + YGNodeLayoutGetBorder(yg, YGEdgeTop);
  float rr = YGNodeLayoutGetPadding(yg, YGEdgeRight) + YGNodeLayoutGetBorder(yg, YGEdgeRight);
  float b = YGNodeLayoutGetPadding(yg, YGEdgeBottom) + YGNodeLayoutGetBorder(yg, YGEdgeBottom);
  auto finite = [](float v) { return std::isfinite(v) ? v : 0.f; };
  *x = finite(l);
  *y = finite(t);
  *w = std::max(0.f, finite(r.width) - *x - finite(rr));
  *h = std::max(0.f, finite(r.height) - *y - finite(b));
}

}  // namespace

NodeState::~NodeState() {
  if (cache) {
    destroyChild(this);
    cache->removeView(node);
  }
}

NodeState* ensure(Node* node) {
  if (NodeState* st = stateOf(node)) return st;
  Document* doc = node->document();
  if (!doc || !doc->containers) return nullptr;
  auto st = std::make_unique<NodeState>();
  st->node = node;
  st->cache = doc->containers;
  st->cache->addView(node);
  NodeState* raw = st.get();
  node->imageCache = std::move(st);
  return raw;
}

void propsChanged(Node* node, const Value& old) {
  NodeState* st = ensure(node);
  if (!st) return;
  const Value& props = node->props();
  // A new source (inline data may also come as a `load` command).
  if (!old["bundleSrc"].equals(props["bundleSrc"]) || !old["bundleData"].equals(props["bundleData"])) {
    std::string src = props["bundleSrc"].asString();
    st->src = src.empty() ? src : ResourceLoader::resolve(st->cache->runtime().baseUrl, src);
    st->data = props["bundleData"].asString();
    st->stale = true;
  }
  std::string appKey = props["appKey"].asString();
  std::string propsJson = props["initialProps"].isObject() ? props["initialProps"].toJson() : "{}";
  if (appKey != st->appKey || propsJson != st->propsJson) {
    st->appKey = std::move(appKey);
    st->propsJson = std::move(propsJson);
    st->runPending = true;
  }
  // hostState: the keys that changed become state commands (null removes).
  const Value& before = old["hostState"];
  const Value& now = props["hostState"];
  for (const auto& [key, value] : now.asObject()) {
    if (before.has(key) && before[key].equals(value)) continue;
    Value::Object c{{"op", Value("state")}, {"key", Value(key)}, {"value", value}};
    if (value.isNull()) c.emplace("remove", Value(true));
    st->commands.push_back(Value(std::move(c)));
  }
  for (const auto& [key, value] : before.asObject())
    if (!now.has(key))
      st->commands.push_back(Value(Value::Object{{"op", Value("state")}, {"key", Value(key)}, {"remove", Value(true)}}));
}

bool command(Node* node, const Value& cmd) {
  NodeState* st = node && node->type() == NodeType::Container ? ensure(node) : nullptr;
  if (!st) return false;
  const std::string& op = cmd["op"].asString();
  if (op == "load") {
    // Inline bundle source (the React component sends bundleData this way,
    // not as a prop the bridge would copy on every update).
    st->src.clear();
    st->data = cmd["data"].asString();
    st->stale = true;
    return true;
  }
  if (op != "post" && op != "call" && op != "resolve" && op != "reject" && op != "state" && op != "reload")
    return false;
  st->commands.push_back(cmd);
  return true;
}

JsuiRuntime* child(const Node* node) {
  NodeState* st = stateOf(node);
  return st ? st->child : nullptr;
}

bool toChild(const Node* node, float localX, float localY, float* cx, float* cy) {
  if (!child(node)) return false;
  float x, y, w, h;
  contentBox(node, &x, &y, &w, &h);
  *cx = localX - x;
  *cy = localY - y;
  return true;
}

void paint(Node* node, SkCanvas* canvas, const SkRect& content, const SkRRect& clip) {
  NodeState* st = stateOf(node);
  if (!st || !st->child) return;
  Surface& s = internalRuntime(st->child)->main();
  float pr = s.doc.pixelRatio();
  int dw = int(std::ceil(st->width * pr)), dh = int(std::ceil(st->height * pr));
  if (dw <= 0 || dh <= 0 || !(pr > 0)) return;
  NestedPaintScope nested;
  canvas->save();
  canvas->clipRRect(clip, true);
  canvas->clipRect(content, true);
  canvas->translate(content.x(), content.y());
  // The child's paint scales by its pixel ratio (the parent's): the canvas
  // is in the parent's logical px here.
  canvas->scale(1 / pr, 1 / pr);
  s.renderCanvas(canvas, dw, dh, false);
  canvas->restore();
}

}  // namespace container

// ------------------------------------------------------------- Containers

Containers::Containers(Runtime& rt) : rt_(rt) {}

Containers::~Containers() {
  // Nodes outliving the cache (none in practice: the surfaces go first) no
  // longer reach it.
  for (Node* n : views_)
    if (auto* st = static_cast<container::NodeState*>(n->imageCache.get())) {
      container::destroyChild(st);
      st->cache = nullptr;
    }
}

void Containers::addView(Node* node) { views_.insert(node); }
void Containers::removeView(Node* node) { views_.erase(node); }

void Containers::tick(double nowMs) {
  // Callbacks only queue events (delivered after this): the set is stable.
  std::vector<Node*> list(views_.begin(), views_.end());
  for (Node* n : list) {
    auto* st = static_cast<container::NodeState*>(n->imageCache.get());
    if (!st) continue;
    float x, y, w, h;
    container::contentBox(n, &x, &y, &w, &h);
    Document* doc = n->document();
    float pr = doc ? doc->pixelRatio() : 1;
    bool resized = w != st->width || h != st->height || pr != st->pixelRatio;
    st->width = w;
    st->height = h;
    st->pixelRatio = pr;
    if (st->stale) {
      st->stale = false;
      container::createChild(st);
      resized = false;
      n->markPaintDirty();
    }
    if (!st->child) {
      // Commands without a child: calls fail, the rest has nothing to reach.
      std::vector<Value> cmds = std::move(st->commands);
      st->commands.clear();
      for (const Value& c : cmds)
        if (c["op"].asString() == "call" || c["op"].asString() == "reload") container::runCommand(st, c);
      continue;
    }
    if (resized) internalRuntime(st->child)->main().setViewport(w, h, pr);
    if (st->runPending) {
      st->runPending = false;
      jsui_surface_run(st->child, st->appKey.c_str(), st->propsJson.c_str());
    }
    std::vector<Value> cmds = std::move(st->commands);
    st->commands.clear();
    for (const Value& c : cmds) {
      container::runCommand(st, c);
      if (!st->child) break;  // a reload of a source that is gone
    }
    if (!st->child) continue;
    jsui_tick(st->child, nowMs);
    if (jsui_needs_redraw(st->child)) n->markPaintDirty();
  }
}

void Containers::tickNative(double nowMs) {
  for (Node* n : views_) {
    JsuiRuntime* child = container::child(n);
    if (!child) continue;
    jsui_tick_native(child, nowMs);
    if (jsui_needs_redraw(child)) n->markPaintDirty();
  }
}

bool Containers::animating() const {
  if (!events.empty()) return true;
  for (Node* n : views_) {
    auto* st = static_cast<const container::NodeState*>(n->imageCache.get());
    if (!st) continue;
    if (st->stale || st->runPending || !st->commands.empty()) return true;
    if (st->child && (jsui_wants_frame(st->child) || jsui_pending_loads(st->child) > 0)) return true;
  }
  return false;
}

double Containers::nextTimer() const {
  double due = std::numeric_limits<double>::infinity();
  for (Node* n : views_)
    if (JsuiRuntime* child = container::child(n)) due = std::min(due, jsui_next_timer(child));
  return due;
}

}  // namespace jsui
