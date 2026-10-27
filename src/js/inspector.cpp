// The JS debugger's runtime side (see inspector.h).
#include "js/inspector.h"

#if JSUI_DEBUGGER

#include <chrono>
#include <cstdint>
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
                  {"runtime", Value(id_)}, {"name", Value(name_)}, {"title", Value(title)}, {"profiler", Value(true)}})
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
