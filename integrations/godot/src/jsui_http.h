// ViewHttp: the network half of a JsuiView's request handler
// (JsuiConfig.on_request, include/jsui/jsui.h "Resource loading"). The UI's
// fetch() to http:// and https:// goes through Godot's HTTPClient: Godot's
// TLS (mbedTLS and its certificate bundle, or the project's), its proxy
// settings, and the browser's fetch on Godot web.
//
//   * Requests arrive on whichever thread runs the loader (the main thread,
//     JS's thread or the UI thread) and are queued; update() runs every
//     frame on the main thread before the runtime's tick and drives each one
//     without blocking: resolving, connecting, TLS, sending, then the body.
//   * The answer streams back as it arrives: jsui_request_respond() once
//     the final response's headers are in, jsui_request_body() per chunk
//     read this frame, jsui_request_end() at the end (or with the error).
//   * Redirects: HTTPClient does not follow them. "follow" reconnects to
//     Location up to 20 times (303, and 301 / 302 after a POST, continue as
//     a body-less GET, as browsers do), "error" fails the request, "manual"
//     answers with the 3xx itself.
//   * Timeouts: the request's timeout_ms over everything, and like the
//     built-in loader 30 s to the response headers and 30 s between body
//     chunks.
//   * Cancel (AbortController, reload, unmount) closes the connection.
#pragma once

#include <godot_cpp/classes/http_client.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "jsui/jsui.h"

namespace jsui_godot {

class ViewHttp {
 public:
  ViewHttp();
  ~ViewHttp();

  // The runtime the handler serves (after jsui_create), or nullptr once it is
  // gone: every request in flight is closed without an answer.
  void attach(JsuiRuntime* rt);
  // Takes an http:// or https:// request (any thread): 0, it is answered from
  // update(); nonzero for any other URL.
  int request(uint64_t id, const JsuiRequest* req);
  void cancel(uint64_t id);  // any thread
  // Every frame on the main thread before the runtime's tick.
  void update();
  int active() const;

 private:
  enum class Step { Connect, Request, Body };
  struct Op {
    uint64_t id = 0;
    std::string url;  // the current hop's
    std::string method;
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<uint8_t> body;
    int timeoutMs = 0;
    int redirect = JSUI_REDIRECT_FOLLOW;
    int hops = 0;
    Step step = Step::Connect;
    godot::Ref<godot::HTTPClient> client;
    std::string target;  // path + query of url
    uint64_t start = 0, deadline = 0, idleDeadline = 0;
    bool responded = false;
  };

  // One step of op (false: it has ended and was answered).
  bool advance(Op& op, uint64_t now);
  bool connect(Op& op, std::string* error);
  // The response's headers are in: answer, follow or fail (false: ended).
  bool head(Op& op, uint64_t now);
  void fail(Op& op, const std::string& error);

  JsuiRuntime* rt_ = nullptr;
  mutable std::mutex m_;  // incoming_, cancels_, rt_ for request()
  std::vector<std::unique_ptr<Op>> incoming_;
  std::vector<uint64_t> cancels_;
  std::vector<std::unique_ptr<Op>> ops_;  // main thread
};

}  // namespace jsui_godot
