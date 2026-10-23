#include "dev_session.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

#include "jsui/jsui.h"
#include "platform/loader.h"
#include "runtime.h"

namespace jsui {

namespace {

constexpr size_t kMaxEvents = 500;
constexpr int kPollTimeoutMs = 40000;  // the server holds a poll for up to ~20 s

std::string base64Encode(const uint8_t* data, size_t size) {
  static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((size + 2) / 3 * 4);
  for (size_t i = 0; i < size; i += 3) {
    uint32_t n = uint32_t(data[i]) << 16;
    if (i + 1 < size) n |= uint32_t(data[i + 1]) << 8;
    if (i + 2 < size) n |= data[i + 2];
    out += T[(n >> 18) & 63];
    out += T[(n >> 12) & 63];
    out += i + 1 < size ? T[(n >> 6) & 63] : '=';
    out += i + 2 < size ? T[n & 63] : '=';
  }
  return out;
}

bool base64Decode(const std::string& in, std::vector<uint8_t>* out) {
  out->clear();
  uint32_t acc = 0;
  int bits = 0;
  for (char c : in) {
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '+' || c == '-') v = 62;
    else if (c == '/' || c == '_') v = 63;
    else if (c == '=') break;
    else return false;
    acc = (acc << 6) | uint32_t(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out->push_back(uint8_t((acc >> bits) & 0xff));
    }
  }
  return true;
}

std::string randomId() {
  std::random_device rd;
  uint64_t a = (uint64_t(rd()) << 32) ^ rd();
  a ^= uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
  char buf[17];
  std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)a);
  return buf;
}

const char* platformName() {
#if defined(__EMSCRIPTEN__)
  return "web";
#elif defined(__APPLE__)
  return "macOS";
#elif defined(_WIN32)
  return "Windows";
#elif defined(__ANDROID__)
  return "Android";
#elif defined(__linux__)
  return "Linux";
#else
  return "native";
#endif
}

std::vector<uint8_t> bytesOf(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

// A string member, or nullptr when it is missing / null (C API "unset").
const char* cstr(const Value& v) { return v.isString() ? v.asString().c_str() : nullptr; }

struct CallUiContext {
  DevSession* session;
  uint64_t id;
};

}  // namespace

DevSession::DevSession(Runtime& rt) : rt_(rt), id_(randomId()) {}

void DevSession::serverChanged() {
  if (!enabled_) {
    enabled_ = true;
    nextPoll_ = 0;
  }
}

void DevSession::reset() {
  // The loader dropped every request (reload): ask again.
  polling_ = false;
  pushing_ = false;
}

Value::Object DevSession::info() const {
  Value::Object o;
  Document& d = rt_.doc;
  o["name"] = Value(name);
  o["platform"] = Value(platformName());
  o["bundle"] = Value(rt_.bundleUrl);
  o["width"] = Value(double(d.width()));
  o["height"] = Value(double(d.height()));
  o["scale"] = Value(double(d.pixelRatio()));
  return o;
}

void DevSession::event(Value::Object ev) {
  if (!joined_) return;
  ev["time"] = Value(std::round(rt_.now));
  if (events_.size() >= kMaxEvents) events_.erase(events_.begin());
  events_.push_back(Value(std::move(ev)));
}

void DevSession::uiEvent(const char* kind, Value::Object data) {
  if (!joined_) return;
  data["who"] = Value("ui");
  data["kind"] = Value(kind);
  event(std::move(data));
}

void DevSession::userInput(const char* kind, Value::Object data) {
  if (injecting_ || !joined_) return;
  data["who"] = Value("user");
  data["kind"] = Value(kind);
  event(std::move(data));
}

void DevSession::tick(const std::string& origin) {
  if (origin.empty() || !wanted_ || !enabled_ || rt_.config.load) return;
  origin_ = origin;
  // Commands run at the end of the tick, after this frame's JS and layout.
  if (!commands_.empty()) {
    std::vector<Value> cmds = std::move(commands_);
    commands_.clear();
    for (const Value& c : cmds) run(c);
  }
  if (!polling_ && rt_.now >= nextPoll_) poll();
  if (!pushing_ && (!results_.empty() || !events_.empty())) push();
}

void DevSession::poll() {
  polling_ = true;
  Value::Object body;
  body["runtime"] = Value(id_);
  body["info"] = Value(info());
  LoadRequest req;
  req.url = origin_ + "/__jsui/session/poll";
  req.method = "POST";
  req.headers.emplace_back("Content-Type", "application/json");
  req.body = bytesOf(Value(std::move(body)).toJson());
  req.timeoutMs = kPollTimeoutMs;
  req.background = true;
  rt_.loader.load(std::move(req), [this](Resource res) {
    polling_ = false;
    if (res.status == 404 || res.status == 405) {
      // No shared session on this server (jsui dev without --shared, or another server).
      enabled_ = false;
      joined_ = false;
      return;
    }
    if (!res.ok()) {
      joined_ = false;
      nextPoll_ = rt_.now + 2000;
      return;
    }
    if (!joined_) {
      joined_ = true;
      rt_.log(JSUI_LOG_INFO, "joined the dev session at " + origin_);
    }
    Value v = Value::parseJson(res.text());
    for (const Value& c : v["commands"].asArray()) commands_.push_back(c);
    if (v["leave"].asBool()) {
      // The server dropped this runtime (another one took its place, or it shut down).
      joined_ = false;
      nextPoll_ = rt_.now + 2000;
    }
  });
}

void DevSession::push() {
  pushing_ = true;
  Value::Object body;
  body["runtime"] = Value(id_);
  body["results"] = Value(std::move(results_));
  body["events"] = Value(std::move(events_));
  results_.clear();
  events_.clear();
  LoadRequest req;
  req.url = origin_ + "/__jsui/session/push";
  req.method = "POST";
  req.headers.emplace_back("Content-Type", "application/json");
  req.body = bytesOf(Value(std::move(body)).toJson());
  req.timeoutMs = 10000;
  req.background = true;
  rt_.loader.load(std::move(req), [this](Resource) { pushing_ = false; });
}

void DevSession::result(uint64_t id, bool ok, Value value, const std::string& error) {
  Value::Object r;
  r["id"] = Value(double(id));
  r["ok"] = Value(ok);
  if (ok) r["value"] = std::move(value);
  else r["error"] = Value(error);
  results_.push_back(Value(std::move(r)));
}

void DevSession::run(const Value& c) {
  JsuiRuntime* h = rt_.handle;
  uint64_t id = uint64_t(c["id"].asNumber());
  const std::string& op = c["op"].asString();
  auto num = [&](const char* k, double def = 0) { return c[k].asNumber(def); };
  auto flt = [&](const char* k) { return float(num(k)); };
  auto json = [&](const char* k) -> std::string { return c[k].isNull() ? std::string() : c[k].toJson(); };
  injecting_++;
  struct Done {
    int& n;
    ~Done() { n--; }
  } done{injecting_};

  if (op == "mouseMove") return result(id, true, Value(jsui_send_mouse_move(h, flt("x"), flt("y")) != 0));
  if (op == "mouseButton") {
    int r = jsui_send_mouse_button(h, flt("x"), flt("y"), JsuiMouseButton(int(num("button"))), c["down"].asBool() ? 1 : 0,
                                   int(num("mods")));
    return result(id, true, Value(r != 0));
  }
  if (op == "wheel") return result(id, true, Value(jsui_send_mouse_wheel(h, flt("x"), flt("y"), flt("dx"), flt("dy")) != 0));
  if (op == "mouseLeave") return result(id, true, Value(jsui_send_mouse_leave(h) != 0));
  if (op == "touch") {
    return result(id, true, Value(jsui_send_touch(h, int(num("id")), int(num("phase")), flt("x"), flt("y")) != 0));
  }
  if (op == "key") {
    int r = jsui_send_key(h, cstr(c["key"]), cstr(c["code"]), c["down"].asBool() ? 1 : 0, c["repeat"].asBool() ? 1 : 0,
                          int(num("mods")));
    return result(id, true, Value(r != 0));
  }
  if (op == "text") return result(id, true, Value(jsui_send_text(h, c["text"].asString().c_str()) != 0));
  if (op == "padConnect") {
    jsui_set_gamepad_connected(h, int(num("pad")), c["connected"].asBool() ? 1 : 0, cstr(c["name"]));
    return result(id, true, Value());
  }
  if (op == "padButton") {
    int r = jsui_send_gamepad_button(h, int(num("pad")), int(num("button")), c["down"].asBool() ? 1 : 0,
                                     float(num("value", c["down"].asBool() ? 1 : 0)));
    return result(id, true, Value(r != 0));
  }
  if (op == "padAxis") {
    return result(id, true, Value(jsui_send_gamepad_axis(h, int(num("pad")), int(num("axis")), flt("value")) != 0));
  }
  if (op == "setState" || op == "patchState") {
    std::string j = json("value");
    const char* key = c["key"].asString().c_str();
    int r = op == "setState" ? jsui_set_state(h, key, j.empty() ? nullptr : j.c_str())
                             : jsui_patch_state(h, key, j.empty() ? "null" : j.c_str());
    if (r < 0) return result(id, false, Value(), "invalid state for " + c["key"].asString());
    return result(id, true, Value());
  }
  if (op == "getState") {
    const char* j = jsui_get_state(h, c["key"].asString().c_str());
    return result(id, true, j ? Value::parseJson(j) : Value());
  }
  if (op == "setBuffer") {
    std::vector<uint8_t> bytes;
    if (!base64Decode(c["data"].asString(), &bytes)) return result(id, false, Value(), "invalid base64 buffer");
    static const size_t sizes[] = {4, 8, 4, 4, 2, 2, 1, 1};
    int type = int(num("type"));
    if (type < 0 || type > 7) return result(id, false, Value(), "invalid buffer type");
    size_t count = bytes.size() / sizes[type];
    int r = jsui_set_buffer(h, c["key"].asString().c_str(), JsuiBufferType(type), bytes.data(), count);
    return result(id, r >= 0, Value(), r >= 0 ? std::string() : "jsui_set_buffer failed");
  }
  if (op == "postMessage") {
    std::string j = json("data");
    jsui_post_message(h, c["name"].asString().c_str(), j.empty() ? "null" : j.c_str());
    return result(id, true, Value());
  }
  if (op == "callUi") {
    std::string j = json("args");
    auto* ctx = new CallUiContext{this, id};
    jsui_call_ui(h, c["name"].asString().c_str(), j.empty() ? "null" : j.c_str(),
                 [](void* user, uint64_t, int ok, const char* payload) {
                   auto* cx = static_cast<CallUiContext*>(user);
                   if (ok) cx->session->result(cx->id, true, Value::parseJson(payload ? payload : "null"));
                   else cx->session->result(cx->id, false, Value(), payload ? payload : "failed");
                   delete cx;
                 },
                 ctx);
    return;  // answered when the handler settles
  }
  if (op == "run") {
    std::string props = json("props");
    int r = jsui_surface_run(h, cstr(c["component"]), props.empty() ? "{}" : props.c_str());
    return result(id, r == 0, Value(), r == 0 ? std::string() : "invalid props");
  }
  if (op == "hitTest") return result(id, true, Value(double(jsui_hit_test(h, flt("x"), flt("y")))));
  if (op == "screenshot") {
    void* data = nullptr;
    size_t size = 0;
    if (jsui_render_png(h, &data, &size) != 0) return result(id, false, Value(), "render failed");
    std::string b64 = base64Encode(static_cast<const uint8_t*>(data), size);
    jsui_free(data);
    return result(id, true, Value(std::move(b64)));
  }
  if (op == "status") {
    Value::Object s = info();
    s["wantsFrame"] = Value(jsui_wants_frame(h) != 0);
    s["pendingLoads"] = Value(double(jsui_pending_loads(h)));
    s["cursor"] = Value(jsui_get_cursor(h));
    s["error"] = Value(rt_.lastError);
    s["time"] = Value(rt_.now);
    return result(id, true, Value(std::move(s)));
  }
  if (op == "reload") {
    rt_.requestReload();
    return result(id, true, Value());
  }
  result(id, false, Value(), "unknown op " + op);
}

}  // namespace jsui
