// The JS debugger's runtime side (see inspector.h).
#include "js/inspector.h"

#if JSUI_DEBUGGER

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "core/value.h"
#include "net/http_client.h"

extern "C" {
#include "quickjs-cdp.h"
}

namespace jsui {

namespace {

constexpr int kPollTimeoutMs = 35000;  // the server holds a poll ~20 s
constexpr int kPushTimeoutMs = 10000;

std::string emptyResult(const Value& id) { return "{\"id\":" + id.toJson() + ",\"result\":{}}"; }

int64_t steadyUs(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count();
}
int64_t profilerClock() { return steadyUs(std::chrono::steady_clock::now()); }

double monotonicSeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
double wallSeconds() {
  return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string base64(const std::string& in) {
  static const char k[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    uint32_t v = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8) | uint8_t(in[i + 2]);
    out += k[v >> 18];
    out += k[(v >> 12) & 63];
    out += k[(v >> 6) & 63];
    out += k[v & 63];
  }
  if (i < in.size()) {
    uint32_t v = uint8_t(in[i]) << 16;
    if (i + 1 < in.size()) v |= uint8_t(in[i + 1]) << 8;
    out += k[v >> 18];
    out += k[(v >> 12) & 63];
    out += i + 1 < in.size() ? k[(v >> 6) & 63] : '=';
    out += '=';
  }
  return out;
}

// The Network panel's resource type.
std::string netType(const std::string& kind, const std::string& contentType, const std::string& url) {
  if (kind == "fetch") return "Fetch";
  if (kind == "image" || contentType.rfind("image/", 0) == 0) return "Image";
  if (kind == "font" || contentType.rfind("font/", 0) == 0 || url.find(".woff") != std::string::npos ||
      url.find(".ttf") != std::string::npos || url.find(".otf") != std::string::npos)
    return "Font";
  if (kind == "script" || contentType.find("javascript") != std::string::npos) return "Script";
  if (contentType.rfind("audio/", 0) == 0 || contentType.rfind("video/", 0) == 0) return "Media";
  return "Other";
}

Value headerObject(const HeaderList& headers) {
  Value::Object o;
  for (auto& [k, v] : headers) o[k] = Value(v);
  return Value(std::move(o));
}

constexpr size_t kNetBodyMax = 16u << 20;   // one body kept for getResponseBody
constexpr size_t kNetBytesMax = 96u << 20;  // all kept bodies

const char* spanName(Phase p) {
  switch (p) {
    case Phase::Commit: return "(commit)";
    case Phase::Animated: return "(animated)";
    case Phase::Style: return "(style)";
    case Phase::Layout: return "(layout)";
    case Phase::Input: return "(input)";
    case Phase::Paint: return "(paint)";
    case Phase::Gpu: return "(gpu)";
    case Phase::Worklet: return "(worklets)";
    default: return "(native)";
  }
}

}  // namespace

Inspector::Inspector(std::string origin, std::string id, std::string name)
    : origin_(std::move(origin)), id_(std::move(id)), name_(std::move(name)) {
  receiver_ = std::thread([this] { receiveLoop(); });
  sender_ = std::thread([this] { sendLoop(); });
}

Inspector::~Inspector() {
  stopping_ = true;
  {
    std::lock_guard<std::mutex> l(stopMutex_);
    stopCv_.notify_all();
  }
  {
    std::lock_guard<std::mutex> l(outMutex_);
    outCv_.notify_all();
  }
  if (receiver_.joinable()) receiver_.join();
  if (sender_.joinable()) sender_.join();
  sampling_ = false;
  if (sampler_.joinable()) sampler_.join();
  if (phases_) phases_->spans.store(nullptr, std::memory_order_relaxed);
  if (agent_) qjs_cdp_free(agent_);
  // The frontends see the target go away now rather than after a timeout.
  net::HttpRequest req;
  req.method = "POST";
  req.url = origin_ + "/__jsui/inspect/bye";
  req.headers.emplace_back("Content-Type", "application/json");
  std::string body = Value(Value::Object{{"runtime", Value(id_)}}).toJson();
  req.body.assign(body.begin(), body.end());
  req.timeoutMs = 1000;
  net::httpRequest(req);
}

void Inspector::setTitle(const std::string& title) {
  std::lock_guard<std::mutex> l(sessMutex_);
  title_ = title;
}

// ------------------------------------------------------------ JS context

void Inspector::attach(JSContext* ctx, Perf* phases) {
  std::vector<std::pair<std::string, std::string>> early;
  {
    std::lock_guard<std::mutex> l(agentMutex_);
    if (agent_) return;
    ctx_ = ctx;
    phases_ = phases;
    agent_ = qjs_cdp_new(ctx, ++contextId_, &Inspector::sendFromAgent, this);
    qjs_cdp_profiler_set_clock(agent_, &profilerClock);
    if (!state_.empty()) qjs_cdp_import_state(agent_, state_.c_str());
    scripts_.clear();
    early.swap(early_);
  }
  std::vector<std::string> greet;
  {
    std::lock_guard<std::mutex> l(sessMutex_);
    for (auto& [s, runtimeOn] : sessions_)
      if (runtimeOn) greet.push_back(s);
  }
  for (auto& s : greet) sendContextCreated(s);
  for (auto& [s, msg] : early) qjs_cdp_send_message(agent_, sessionToken(s), msg.data(), msg.size());
}

void Inspector::detach(JSContext* ctx) {
  QJSCDPAgent* old = nullptr;
  int id = 0;
  {
    std::lock_guard<std::mutex> l(agentMutex_);
    if (!agent_ || ctx_ != ctx) return;
    char* state = qjs_cdp_export_state(agent_);
    state_ = state ? state : "";
    free(state);
    old = agent_;
    agent_ = nullptr;
    ctx_ = nullptr;
    if (phases_) phases_->spans.store(nullptr, std::memory_order_relaxed);
    phases_ = nullptr;
    id = contextId_;
    scripts_.clear();
  }
  qjs_cdp_free(old);
  send("", "{\"method\":\"Runtime.executionContextDestroyed\",\"params\":{\"executionContextId\":" +
               std::to_string(id) + "}}");
  send("", "{\"method\":\"Runtime.executionContextsCleared\",\"params\":{}}");
}

void Inspector::poll() {
  if (!agent_) return;
  flushSpans();  // before a Profiler.stop is handled
  qjs_cdp_poll(agent_);
  syncSampler();
  // Every frontend left: start over with no breakpoints and the debugger
  // off, as a fresh attach expects (the traps parsed into this context's
  // code stay, a new agent drives them).
  if (lastSessionClosed_.exchange(false)) {
    std::lock_guard<std::mutex> l(agentMutex_);
    if (!agent_) return;
    qjs_cdp_free(agent_);
    agent_ = qjs_cdp_new(ctx_, contextId_, &Inspector::sendFromAgent, this);
    qjs_cdp_profiler_set_clock(agent_, &profilerClock);
    state_.clear();
    for (auto& [url, source] : scripts_) qjs_cdp_script_loaded(agent_, url.c_str(), source.c_str());
  }
  syncSampler();
}

void Inspector::jsEnter(const char* what) {
  if (agent_ && sampling_) qjs_cdp_profiler_enter(agent_, what);
}

void Inspector::jsIdle() {
  if (agent_ && sampling_) qjs_cdp_profiler_idle(agent_);
}

void Inspector::span(Phase phase, std::chrono::steady_clock::time_point start,
                     std::chrono::steady_clock::time_point end) {
  if (!sampling_) return;
  std::lock_guard<std::mutex> l(spanMutex_);
  spans_.push_back({phase, false, steadyUs(start), steadyUs(end)});
}

void Inspector::scope(Phase phase, std::chrono::steady_clock::time_point start,
                      std::chrono::steady_clock::time_point end) {
  if (!sampling_) return;
  std::lock_guard<std::mutex> l(spanMutex_);
  spans_.push_back({phase, true, steadyUs(start), steadyUs(end)});
}

std::string netId(uint64_t id) { return "jsui-" + std::to_string(id); }

void Inspector::loadStarted(uint64_t id, const LoadRequest& req) {
  if (!network_ || req.url.find("/__jsui/") != std::string::npos) return;
  std::string type = netType(req.kind, "", req.url), document;
  {
    std::lock_guard<std::mutex> l(netMutex_);
    net_[id] = NetRequest{type, {}, false};
  }
  {
    std::lock_guard<std::mutex> l(sessMutex_);
    document = title_;
  }
  Value::Object request{{"url", Value(req.url)},
                        {"method", Value(req.method)},
                        {"headers", headerObject(req.headers)},
                        {"initialPriority", Value("High")},
                        {"referrerPolicy", Value("no-referrer")}};
  if (!req.body.empty()) request["postData"] = Value(std::string(req.body.begin(), req.body.end()));
  Value::Object params{{"requestId", Value(netId(id))},
                       {"loaderId", Value("L1")},
                       {"documentURL", Value(document)},
                       {"request", Value(std::move(request))},
                       {"timestamp", Value(monotonicSeconds())},
                       {"wallTime", Value(wallSeconds())},
                       {"initiator", Value(Value::Object{{"type", Value("other")}})},
                       {"type", Value(type)},
                       {"frameId", Value("F1")}};
  send("", Value(Value::Object{{"method", Value("Network.requestWillBeSent")}, {"params", Value(std::move(params))}}).toJson());
}

void Inspector::loadAnswered(uint64_t id, int kind, const Resource& res) {
  std::string type;
  {
    std::lock_guard<std::mutex> l(netMutex_);
    auto it = net_.find(id);
    if (it == net_.end() || it->second.done) return;
    if (kind != LoadEvent::Data && it->second.type == "Other") it->second.type = netType("", res.contentType, res.url);
    type = it->second.type;
  }
  const double now = monotonicSeconds();
  auto event = [&](const char* method, Value::Object params) {
    params["requestId"] = Value(netId(id));
    params["timestamp"] = Value(now);
    send("", Value(Value::Object{{"method", Value(method)}, {"params", Value(std::move(params))}}).toJson());
  };
  auto keep = [&](const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> l(netMutex_);
    auto it = net_.find(id);
    if (it == net_.end() || it->second.body.size() + data.size() > kNetBodyMax) return;
    it->second.body.append(data.begin(), data.end());
    netBytes_ += data.size();
  };
  auto finish = [&](const std::string& error, size_t bytes) {
    if (!error.empty())
      event("Network.loadingFailed", {{"type", Value(type)}, {"errorText", Value(error)}, {"canceled", Value(false)}});
    else
      event("Network.loadingFinished", {{"encodedDataLength", Value(double(bytes))}});
    std::lock_guard<std::mutex> l(netMutex_);
    auto it = net_.find(id);
    if (it != net_.end()) it->second.done = true;
    // The oldest finished bodies go first.
    for (auto o = net_.begin(); netBytes_ > kNetBytesMax && o != net_.end();) {
      if (!o->second.done || o->first == id) {
        ++o;
        continue;
      }
      netBytes_ -= o->second.body.size();
      o = net_.erase(o);
    }
  };
  if (kind == LoadEvent::Head || kind == 3) {
    if (res.status == 0) {
      finish(res.error.empty() ? "net::ERR_FAILED" : res.error, 0);
      return;
    }
    std::string mime = res.contentType.substr(0, res.contentType.find(';'));
    Value::Object response{{"url", Value(res.url)},
                           {"status", Value(res.status)},
                           {"statusText", Value(res.statusText)},
                           {"headers", headerObject(res.headers)},
                           {"mimeType", Value(mime)},
                           {"connectionReused", Value(false)},
                           {"connectionId", Value(0)},
                           {"encodedDataLength", Value(0)},
                           {"securityState", Value(res.url.rfind("https:", 0) == 0 ? "secure" : "neutral")}};
    event("Network.responseReceived", {{"loaderId", Value("L1")}, {"type", Value(type)}, {"response", Value(std::move(response))}, {"frameId", Value("F1")}});
    if (kind == 3) {
      if (!res.data.empty()) {
        keep(res.data);
        event("Network.dataReceived", {{"dataLength", Value(double(res.data.size()))}, {"encodedDataLength", Value(double(res.data.size()))}});
      }
      finish("", res.data.size());
    } else if (!res.data.empty()) {
      keep(res.data);
      event("Network.dataReceived", {{"dataLength", Value(double(res.data.size()))}, {"encodedDataLength", Value(double(res.data.size()))}});
    }
  } else if (kind == LoadEvent::Data) {
    keep(res.data);
    event("Network.dataReceived", {{"dataLength", Value(double(res.data.size()))}, {"encodedDataLength", Value(double(res.data.size()))}});
  } else {  // End
    size_t bytes = 0;
    {
      std::lock_guard<std::mutex> l(netMutex_);
      auto it = net_.find(id);
      if (it != net_.end()) bytes = it->second.body.size();
    }
    finish(res.error, bytes);
  }
}

// Network.* the runtime answers itself (receive thread): the requests are
// the loader's, reported as they happen.
bool Inspector::networkAnswer(const std::string& method, const Value& id, const Value& params, const std::string& session) {
  auto result = [&](Value r) {
    send(session, Value(Value::Object{{"id", id}, {"result", std::move(r)}}).toJson());
  };
  if (method == "Network.enable") {
    network_ = true;
    result(Value(Value::Object{}));
  } else if (method == "Network.disable") {
    network_ = false;
    result(Value(Value::Object{}));
  } else if (method == "Network.getResponseBody") {
    const std::string& rid = params["requestId"].asString();
    uint64_t n = rid.rfind("jsui-", 0) == 0 ? std::strtoull(rid.c_str() + 5, nullptr, 10) : 0;
    std::string body;
    bool found = false;
    {
      std::lock_guard<std::mutex> l(netMutex_);
      auto it = net_.find(n);
      if (it != net_.end() && it->second.done) {
        body = it->second.body;
        found = true;
      }
    }
    if (!found) {
      send(session, "{\"id\":" + id.toJson() + ",\"error\":{\"code\":-32000,\"message\":\"No resource with given identifier found\"}}");
      return true;
    }
    bool text = true;
    for (unsigned char c : body)
      if (c == 0 || (c < 0x09)) {
        text = false;
        break;
      }
    result(Value(Value::Object{{"body", Value(text ? body : base64(body))}, {"base64Encoded", Value(!text)}}));
  } else if (method == "Network.emulateNetworkConditionsByRule") {
    result(Value(Value::Object{{"ruleIds", Value(Value::Array{})}}));
  } else if (method == "Network.getSecurityIsolationStatus") {
    return false;  // "not found": DevTools does without
  } else {
    result(Value(Value::Object{}));  // cache, throttling, blocking, ...: nothing to do
  }
  return true;
}

void Inspector::flushSpans() {
  std::vector<Span> spans;
  {
    std::lock_guard<std::mutex> l(spanMutex_);
    spans.swap(spans_);
  }
  if (!agent_) return;
  for (const Span& sp : spans) {
    if (sp.whole) qjs_cdp_profiler_add_scope(agent_, spanName(sp.phase), sp.start, sp.end);
    else qjs_cdp_profiler_add_span(agent_, spanName(sp.phase), sp.start, sp.end);
  }
}

void Inspector::syncSampler() {
  bool want = agent_ && qjs_cdp_profiling(agent_);
  if (want == sampling_) return;
  if (sampler_.joinable()) {
    sampling_ = false;
    sampler_.join();
  }
  sampling_ = want;
  // While a CPU profile records, the runtime's phases (layout, paint, ...)
  // go into it too.
  if (phases_) phases_->spans.store(want ? this : nullptr, std::memory_order_relaxed);
  if (want) sampler_ = std::thread([this] { sampleLoop(); });
}

void Inspector::sampleLoop() {
  while (sampling_ && !stopping_) {
    int us;
    {
      std::lock_guard<std::mutex> l(agentMutex_);
      if (!agent_) break;
      qjs_cdp_profiler_tick(agent_);
      us = qjs_cdp_sampling_interval(agent_);
    }
    std::this_thread::sleep_for(std::chrono::microseconds(us));
  }
}

void Inspector::scriptLoaded(const std::string& url, const std::string& source) {
  if (!agent_) return;
  {
    std::lock_guard<std::mutex> l(agentMutex_);
    scripts_.emplace_back(url, source);
  }
  qjs_cdp_script_loaded(agent_, url.c_str(), source.c_str());
}

void Inspector::console(const char* type, const JSValue* args, int count) {
  if (!agent_) return;
  {
    std::lock_guard<std::mutex> l(sessMutex_);
    if (sessions_.empty()) return;
  }
  qjs_cdp_console_message(agent_, type, args, count);
}

void Inspector::exception(JSValueConst exc, const std::string& text) {
  if (!agent_) return;
  {
    std::lock_guard<std::mutex> l(sessMutex_);
    if (sessions_.empty()) return;
  }
  qjs_cdp_exception_thrown(agent_, exc, text.c_str());
}

// ------------------------------------------------------------ sessions

void* Inspector::sessionToken(const std::string& session) {
  std::lock_guard<std::mutex> l(sessMutex_);
  for (size_t i = 0; i < sessionNames_.size(); i++)
    if (sessionNames_[i] == session) return reinterpret_cast<void*>(uintptr_t(i + 1));
  sessionNames_.push_back(session);
  return reinterpret_cast<void*>(uintptr_t(sessionNames_.size()));
}

std::string Inspector::sessionOf(void* token) {
  uintptr_t i = reinterpret_cast<uintptr_t>(token);
  std::lock_guard<std::mutex> l(sessMutex_);
  return i > 0 && i <= sessionNames_.size() ? sessionNames_[i - 1] : std::string();
}

void Inspector::sendFromAgent(void* opaque, void* session, const char* json, size_t len) {
  auto* self = static_cast<Inspector*>(opaque);
  // The answer to our own resume when the last frontend left.
  static const char ownReply[] = "{\"id\":0,";
  if (!session && len >= sizeof(ownReply) - 1 && std::memcmp(json, ownReply, sizeof(ownReply) - 1) == 0) return;
  self->send(session ? self->sessionOf(session) : std::string(), std::string(json, len));
}

void Inspector::sendContextCreated(const std::string& session) {
  int id = contextId_;
  Value::Object context{{"id", Value(id)},
                        {"origin", Value(origin_)},
                        {"name", Value(name_)},
                        {"uniqueId", Value(id_ + "-" + std::to_string(id))},
                        {"auxData", Value(Value::Object{{"isDefault", Value(true)}})}};
  Value msg(Value::Object{{"method", Value("Runtime.executionContextCreated")},
                          {"params", Value(Value::Object{{"context", Value(std::move(context))}})}});
  send(session, msg.toJson());
}

// What the frontend sent. Runtime.enable and anything the agent does not
// implement (Log, Network, ...) are answered here, on this thread:
// a frontend waits for every reply, and some arrive while JS is paused.
void Inspector::handleEvent(const std::string& type, const std::string& session, const std::string& message) {
  if (type == "open") {
    std::lock_guard<std::mutex> l(sessMutex_);
    sessions_.emplace(session, false);
    return;
  }
  if (type == "close") {
    bool last = false;
    {
      std::lock_guard<std::mutex> l(sessMutex_);
      sessions_.erase(session);
      last = sessions_.empty();
    }
    if (last) {
      // Don't leave the program stopped with nobody to resume it.
      std::lock_guard<std::mutex> l(agentMutex_);
      static const char resume[] = "{\"id\":0,\"method\":\"Debugger.resume\"}";
      if (agent_) qjs_cdp_send_message(agent_, nullptr, resume, sizeof(resume) - 1);
      lastSessionClosed_ = true;
    }
    return;
  }
  if (type != "message") return;
  {
    std::lock_guard<std::mutex> l(sessMutex_);
    sessions_.emplace(session, false);  // a message before its "open"
  }
  Value msg = Value::parseJson(message);
  const std::string& method = msg["method"].asString();
  const Value& id = msg["id"];
  if (method == "Runtime.enable" || method == "Runtime.disable") {
    bool on = method == "Runtime.enable";
    {
      std::lock_guard<std::mutex> l(sessMutex_);
      sessions_[session] = on;
    }
    send(session, emptyResult(id));
    bool attached;
    {
      std::lock_guard<std::mutex> l(agentMutex_);
      attached = agent_ != nullptr;
    }
    if (on && attached) sendContextCreated(session);
    return;
  }
  if (method.rfind("Network.", 0) == 0 && networkAnswer(method, id, msg["params"], session)) return;
  if (!qjs_cdp_handles(method.c_str())) {
    // An empty result would tell DevTools these did something: it then reads
    // fields that aren't there or waits for data that never comes.
    if (method.rfind("Profiler.", 0) == 0 || method.rfind("HeapProfiler.", 0) == 0 ||
        method.rfind("Network.", 0) == 0) {
      send(session, "{\"id\":" + id.toJson() + ",\"error\":{\"code\":-32601,\"message\":\"'" + method +
                        "' wasn't found\"}}");
      return;
    }
    send(session, emptyResult(id));
    return;
  }
  void* token = sessionToken(session);
  std::lock_guard<std::mutex> l(agentMutex_);
  if (!agent_) {
    early_.emplace_back(session, message);
    return;
  }
  // Also stops code that never returns to the runtime's loop (an endless
  // loop), which would never get to the queued request.
  if (method == "Debugger.pause") qjs_cdp_pause(agent_);
  qjs_cdp_send_message(agent_, token, message.data(), message.size());
}

// ------------------------------------------------------------ transport

bool Inspector::sleepFor(int ms) {
  std::unique_lock<std::mutex> l(stopMutex_);
  stopCv_.wait_for(l, std::chrono::milliseconds(ms), [this] { return stopping_.load(); });
  return !stopping_;
}

void Inspector::receiveLoop() {
  int failures = 0;
  while (!stopping_) {
    net::HttpRequest req;
    req.method = "POST";
    req.url = origin_ + "/__jsui/inspect/poll";
    req.timeoutMs = kPollTimeoutMs;
    req.headers.emplace_back("Content-Type", "application/json");
    std::string title;
    {
      std::lock_guard<std::mutex> l(sessMutex_);
      title = title_;
    }
    std::string body =
        Value(Value::Object{
                  {"runtime", Value(id_)}, {"name", Value(name_)}, {"title", Value(title)}, {"profiler", Value(true)}, {"network", Value(true)}})
            .toJson();
    req.body.assign(body.begin(), body.end());
    net::HttpResponse res = net::httpRequest(req, &stopping_);
    if (stopping_) break;
    if (res.status >= 400 && res.status < 500) return;  // a dev server without the debugger
    if (res.status != 200) {
      // The dev server restarted or is gone: keep trying, more slowly.
      if (!sleepFor(failures++ < 5 ? 500 : 2000)) break;
      continue;
    }
    failures = 0;
    Value reply = Value::parseJson(std::string(res.body.begin(), res.body.end()));
    for (const Value& ev : reply["events"].asArray())
      handleEvent(ev["type"].asString(), ev["session"].asString(), ev["message"].asString());
  }
}

void Inspector::send(const std::string& session, std::string message) {
  std::lock_guard<std::mutex> l(outMutex_);
  out_.push_back({session, std::move(message)});
  outCv_.notify_all();
}

void Inspector::sendLoop() {
  for (;;) {
    std::deque<Out> batch;
    {
      std::unique_lock<std::mutex> l(outMutex_);
      outCv_.wait(l, [this] { return stopping_ || !out_.empty(); });
      if (stopping_) return;
      batch.swap(out_);
    }
    Value::Array messages;
    messages.reserve(batch.size());
    for (auto& m : batch)
      messages.push_back(Value(Value::Object{{"session", m.session.empty() ? Value() : Value(m.session)},
                                             {"message", Value(std::move(m.message))}}));
    std::string body = Value(Value::Object{{"runtime", Value(id_)}, {"messages", Value(std::move(messages))}}).toJson();
    net::HttpRequest req;
    req.method = "POST";
    req.url = origin_ + "/__jsui/inspect/push";
    req.timeoutMs = kPushTimeoutMs;
    req.headers.emplace_back("Content-Type", "application/json");
    req.body.assign(body.begin(), body.end());
    for (int attempt = 0; attempt < 3 && !stopping_; attempt++) {
      net::HttpResponse res = net::httpRequest(req, &stopping_);
      if (res.status >= 200 && res.status < 500) break;
      if (!sleepFor(300)) return;
    }
  }
}

}  // namespace jsui

#endif  // JSUI_DEBUGGER
