// The JS debugger: Chrome DevTools / VS Code debug a dev-mode runtime's
// QuickJS through `jsui dev` (builds with JSUI_DEBUGGER).
//
// The protocol work is quickjs-cdp's (third_party/quickjs-cdp: breakpoints,
// stepping, scopes, evaluation). The engine emits its statement traps only
// into code parsed while a debug handler is installed, so the agent is
// attached to a new JS context before the bundle runs, and only when the dev
// server offered debugging (X-Jsui-Inspect on the bundle response).
//
// No sockets of our own: CDP travels through the dev server over HTTP, on two
// threads of the inspector's (the runtime's thread is stopped while JS is
// paused at a breakpoint, and the pause loop still has to be answered):
//
//   POST /__jsui/inspect/poll  {runtime, name, title} -> {events: [...]}
//        long-polled; events are {type: "open"|"close", session} and
//        {type: "message", session, message: "<CDP JSON>"}
//   POST /__jsui/inspect/push  {runtime, messages: [{session|null, message}]}
//   POST /__jsui/inspect/bye   {runtime}
//
// The dev server lists the runtime in /json/list (chrome://inspect, VS Code's
// "attach") and relays each frontend's WebSocket to it. A frontend stays
// attached across reloads and hot updates: breakpoints carry over to the next
// JS context (qjs_cdp_export_state / import_state).
//
// Threads: attach / detach / poll / scriptLoaded / console / exception run on
// the runtime's thread; the rest of the work is the inspector's own threads.
#pragma once

#if JSUI_DEBUGGER

#include <quickjs.h>

#include "core/perf.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct QJSCDPAgent;

namespace jsui {

class Inspector : public PhaseSpanSink {
 public:
  // `origin`: the dev server (http://host:port). `id`: the runtime's id in
  // the shared session, stable across reloads.
  Inspector(std::string origin, std::string id, std::string name);
  ~Inspector();

  const std::string& origin() const { return origin_; }
  void setTitle(const std::string& title);

  // A new JS context, before any of the bundle is parsed / it is going away.
  // `phases`: the runtime's frame phases, reported into CPU profiles.
  void attach(JSContext* ctx, Perf* phases = nullptr);
  void detach(JSContext* ctx);
  bool attached() const { return agent_ != nullptr; }

  // Handles the frontends' requests that arrived while JS was not running
  // (once per tick).
  void poll();
  // A script was evaluated (the bundle, a hot update).
  void scriptLoaded(const std::string& url, const std::string& source);
  // console.* with the original arguments (Runtime.consoleAPICalled type).
  void console(const char* type, const JSValue* args, int count);
  // An uncaught exception (Runtime.exceptionThrown).
  void exception(JSValueConst exc, const std::string& text);
  // JS returned to the runtime (a CPU profile counts what follows as idle).
  void jsIdle();
  // Whether a frontend records a CPU profile: the runtime's phases then
  // report to this sink (span: any thread), shown in the profile.
  bool profiling() const { return sampling_; }
  void span(Phase phase, std::chrono::steady_clock::time_point start,
            std::chrono::steady_clock::time_point end) override;
  void scope(Phase phase, std::chrono::steady_clock::time_point start,
             std::chrono::steady_clock::time_point end) override;

 private:
  struct Out {
    std::string session;  // empty: every frontend
    std::string message;
  };

  static void sendFromAgent(void* opaque, void* session, const char* json, size_t len);
  void receiveLoop();
  void sendLoop();
  void handleEvent(const std::string& type, const std::string& session, const std::string& message);
  void send(const std::string& session, std::string message);
  void sendContextCreated(const std::string& session);
  void* sessionToken(const std::string& session);
  std::string sessionOf(void* token);
  bool sleepFor(int ms);  // false when stopping
  void sampleLoop();
  void syncSampler();
  void flushSpans();  // runtime thread

  const std::string origin_, id_, name_;
  std::string title_;

  // The agent and the JS context it watches (runtime thread; the receive
  // thread queues into the agent under agentMutex_).
  std::mutex agentMutex_;
  QJSCDPAgent* agent_ = nullptr;
  JSContext* ctx_ = nullptr;
  int contextId_ = 0;
  std::string state_;  // exported breakpoints, for the next context
  std::vector<std::pair<std::string, std::string>> scripts_;  // of this context
  // Requests that arrived while there was no agent (between contexts).
  std::vector<std::pair<std::string, std::string>> early_;

  // Frontends: session id -> whether it enabled Runtime (and wants
  // executionContextCreated). Session tokens handed to the agent are indices
  // into sessionNames_ (+1), never reused.
  std::mutex sessMutex_;  // sessions_, sessionNames_, title_
  std::map<std::string, bool> sessions_;
  std::vector<std::string> sessionNames_;
  std::atomic<bool> lastSessionClosed_{false};

  // Outgoing messages, sent in batches by the send thread.
  std::mutex outMutex_;
  std::condition_variable outCv_;
  std::deque<Out> out_;

  std::mutex stopMutex_;
  std::condition_variable stopCv_;
  std::atomic<bool> stopping_{false};
  std::thread receiver_, sender_;

  // While the frontend records a CPU profile (Profiler.start): arms the
  // agent's statement traps every sampling interval, so the next statement
  // takes a sample. Started and stopped on the runtime's thread.
  std::atomic<bool> sampling_{false};
  std::thread sampler_;
  struct Span {
    Phase phase;
    bool whole;          // a whole scope (scope()), else exclusive time (span())
    int64_t start, end;  // steady_clock microseconds, the agent's clock
  };
  Perf* phases_ = nullptr;  // runtime thread
  std::mutex spanMutex_;
  std::vector<Span> spans_;
};

}  // namespace jsui

#endif  // JSUI_DEBUGGER
