// Dev mode: the runtime joins the shared session of `jsui dev --shared`.
//
// Every dev-mode runtime with an http:// bundle (native viewer, Godot
// JsuiView, the web host, ...) offers itself to the dev server, which can
// make it the live app of the session: agents attached to the server
// (`jsui run --attach`) then drive this runtime, and the person's input to
// it shows in the session's activity feed. Two HTTP channels to the origin:
//
//   POST /__jsui/session/poll   {runtime, info} -> {commands: [...]}
//        long-polled: the server holds it until it has commands.
//   POST /__jsui/session/push   {runtime, results: [...], events: [...]}
//        command results and what happened here (the person's input, UI
//        messages, calls, state writes, logs, errors), sent as they occur.
//
// A command is {id, op, ...}; it runs at the end of a tick through the C API
// (the same calls the host makes), and its result is {id, ok, value} or
// {id, ok: false, error}. Ops: mouseMove, mouseButton, wheel, mouseLeave,
// touch, key, text, padConnect, padButton, padAxis, setState, patchState,
// getState, setBuffer, postMessage, callUi (answered when the UI handler
// settles), run, hitTest, screenshot (PNG, base64), status, reload.
//
// A server without a shared session answers 404 to the poll: the runtime
// then stops asking until its bundle changes (a restarted server may have one).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/value.h"

namespace jsui {

class Runtime;

class DevSession {
 public:
  explicit DevSession(Runtime& rt);

  // A host-chosen name shown in the session (jsui_set_dev_name).
  std::string name;

  // jsui_set_dev_session(..., enabled).
  void setEnabled(bool on) { wanted_ = on; }

  // Called at the end of every tick: starts polls / pushes, runs commands.
  void tick(const std::string& origin);
  // The runtime cancelled its loads (reload): forget in-flight requests.
  void reset();
  // The dev server published a new build: try joining again if it said no.
  void serverChanged();
  // The runtime is executing a session command (its input is not reported
  // as the person's).
  bool injecting() const { return injecting_ > 0; }

  // Joined, and not running a command: the host's input is the person's.
  bool reporting() const { return joined_ && !injecting_; }
  bool joined() const { return joined_; }

  // What happened in this runtime; dropped unless the session is joined.
  void event(Value::Object ev);
  // The UI's messages, Host.calls, state writes, logs and errors.
  void uiEvent(const char* kind, Value::Object data);
  // The person's input (ignored while a command runs).
  void userInput(const char* kind, Value::Object data);
  // Answers command `id` (async ops answer later).
  void result(uint64_t id, bool ok, Value value, const std::string& error = std::string());

 private:
  void poll();
  void push();
  void run(const Value& cmd);
  Value::Object info() const;

  Runtime& rt_;
  std::string id_;
  std::string origin_;
  bool wanted_ = true;       // the host did not opt out
  bool enabled_ = true;      // false after a 404 (no shared session)
  bool joined_ = false;      // the server accepted a poll
  bool polling_ = false;
  bool pushing_ = false;
  double nextPoll_ = 0;
  int injecting_ = 0;
  std::vector<Value> commands_;  // received, run at the end of the tick
  std::vector<Value> results_;
  std::vector<Value> events_;
};

}  // namespace jsui
