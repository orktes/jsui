// End-to-end Image events: a real @orktes/jsui-react bundle (packages/cli/test/e2e/
// images.jsx) driven through the C API. React Native's onLoadStart,
// onProgress, onLoad ({nativeEvent: {source: {uri, width, height}}}), onError
// and onLoadEnd for data URIs, require()'d bundled assets, URLs answered by
// the embedder's loader (jsui_resource_complete, possibly frames later),
// failed loads, undecodable data and images already cached at mount (still
// asynchronous: after jsui_post_message returns and after passive effects),
// with the embedder loader and with the built-in one. Built and run by the
// @orktes/jsui-cli test suite with JSUI_REACT_IMAGES_BUNDLE=<path>; skipped without.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/value.h"
#include "jsui/jsui.h"
#include "test.h"

using jsui::Value;

namespace {

// The 2 x 1 PNG of the fixture's data URI, served for game:// URLs.
const char kDotBase64[] =
    "iVBORw0KGgoAAAANSUhEUgAAAAIAAAABCAYAAAD0In+KAAAADklEQVR4nGP4z8AAQv8BD/kD/YURmXYAAAAASUVORK5CYII=";

std::string base64Decode(const std::string& in) {
  std::string out;
  uint32_t acc = 0;
  int bits = 0;
  for (char c : in) {
    int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
            : c == '+' ? 62 : c == '/' ? 63 : -1;
    if (v < 0) continue;
    acc = (acc << 6) | uint32_t(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(char((acc >> bits) & 0xff));
    }
  }
  return out;
}

struct ImagesE2E {
  std::vector<std::pair<std::string, std::string>> messages;
  struct Request {
    uint64_t id;
    std::string url;
  };
  std::vector<Request> requests;  // embedder loader: not answered yet
  std::vector<std::string> served;
  JsuiRuntime* rt = nullptr;
  double now = 0;
  bool hold = false;  // keep game://slow* requests pending

  bool load(const std::string& path, bool embedderLoader) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    JsuiConfig cfg;
    jsui_config_init(&cfg);
    cfg.width = 400;
    cfg.height = 300;
    cfg.on_message = [](void* u, const char* name, const char* json) {
      static_cast<ImagesE2E*>(u)->messages.emplace_back(name, json ? json : "");
    };
    cfg.message_user = this;
    cfg.log = [](void*, JsuiLogLevel, const char*) {};  // failed loads warn on purpose
    if (embedderLoader) {
      cfg.load = [](void* u, JsuiRuntime*, uint64_t id, const char* url) {
        static_cast<ImagesE2E*>(u)->requests.push_back({id, url ? url : ""});
      };
      cfg.load_user = this;
    }
    rt = jsui_create(&cfg);
    if (jsui_load_bundle_source(rt, src.data(), src.size(), path.c_str()) != 0) return false;
    tick(32);
    return true;
  }
  ~ImagesE2E() {
    if (rt) jsui_destroy(rt);
  }
  // Answers the embedder's pending requests (a frame after they were made).
  void serve() {
    std::vector<Request> later;
    for (auto& r : requests) {
      if (hold && r.url.rfind("game://slow", 0) == 0) {
        later.push_back(r);
        continue;
      }
      served.push_back(r.url);
      std::string body;
      int status = 200;
      if (r.url.rfind("game://missing", 0) == 0) {
        status = 404;
      } else if (r.url.rfind("game://", 0) == 0) {
        body = base64Decode(kDotBase64);
      } else {
        std::string file = r.url.rfind("file://", 0) == 0 ? r.url.substr(7) : r.url;
        std::ifstream in(file, std::ios::binary);
        if (in) body.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        else status = 404;
      }
      jsui_resource_complete(rt, r.id, status, "image/png", body.data(), body.size());
    }
    requests = std::move(later);
  }
  void tick(double ms) {
    for (double end = now + ms; now < end;) {
      now += 16;
      jsui_tick(rt, now);
      serve();
    }
  }
  void show(const char* json) { jsui_post_message(rt, "show", json); }
  // The `what` of every img message for probe `name`, in order ("start
  // effect load end"), and all messages taken.
  std::vector<Value> imgs;
  void collect() {
    for (auto& [n, j] : messages)
      if (n == "img") imgs.push_back(Value::parseJson(j));
    messages.clear();
  }
  std::string seq(const std::string& name) {
    collect();
    std::string s;
    for (auto& v : imgs)
      if (v["name"].asString() == name) s += (s.empty() ? "" : " ") + v["what"].asString();
    return s;
  }
  Value event(const std::string& name, const std::string& what) {
    collect();
    for (auto& v : imgs)
      if (v["name"].asString() == name && v["what"].asString() == what) return v;
    return Value();
  }
  void reset() {
    collect();
    imgs.clear();
  }
};

void checkLoaded(ImagesE2E& t, const char* name, double w, double h, const std::string& uriPrefix) {
  Value l = t.event(name, "load");
  CHECK_EQ(l["width"].asNumber(), w);
  CHECK_EQ(l["height"].asNumber(), h);
  CHECK(l["uri"].asString().rfind(uriPrefix, 0) == 0);
  if (l["uri"].asString().rfind(uriPrefix, 0) != 0)
    std::fprintf(stderr, "  %s: uri %s, expected %s...\n", name, l["uri"].asString().c_str(), uriPrefix.c_str());
}

}  // namespace

TEST(react_images_e2e) {
  const char* path = std::getenv("JSUI_REACT_IMAGES_BUNDLE");
  if (!path) {
    std::printf("  (skipped: set JSUI_REACT_IMAGES_BUNDLE, see packages/cli/test)\n");
    return;
  }
  ImagesE2E t;
  CHECK(t.load(path, true));
  const char* err = jsui_last_error(t.rt);
  if (err) std::fprintf(stderr, "  js error: %s\n", err);
  CHECK(err == nullptr);
  t.messages.clear();

  // A data URI decodes synchronously, yet onLoad is asynchronous (RN): not
  // during the render jsui_post_message ran, but on the next tick, after the
  // passive effects; then onLoadEnd.
  t.show(R"([{"name":"dot","src":"dot"}])");
  CHECK_EQ(t.seq("dot"), std::string("start effect"));  // no load yet
  CHECK(jsui_wants_frame(t.rt));
  t.tick(16);
  CHECK_EQ(t.seq("dot"), std::string("start effect load end"));
  checkLoaded(t, "dot", 2, 1, "data:");
  t.reset();
  t.tick(32);
  CHECK(!jsui_wants_frame(t.rt));

  // A bundled asset (require), a URL the embedder serves, one it answers a
  // few frames later, a 404, a missing relative file and undecodable data.
  // The dot stays mounted: no new events for it.
  t.hold = true;
  t.show(
      R"([{"name":"dot","src":"dot"},{"name":"sword","src":"sword"},{"name":"host","src":"game://tex/a.png"},)"
      R"({"name":"slow","src":"game://slow.png"},{"name":"missing","src":"game://missing.png"},)"
      R"({"name":"nofile","src":"no-such-image.png"},{"name":"bad","src":"bad"}])");
  t.tick(48);
  CHECK_EQ(t.seq("dot"), std::string(""));
  CHECK_EQ(t.seq("sword"), std::string("start effect progress load end"));
  checkLoaded(t, "sword", 128, 128, "item-sword");
  Value p = t.event("sword", "progress");
  CHECK(p["loaded"].asNumber() > 1000);
  CHECK_EQ(p["loaded"].asNumber(), p["total"].asNumber());
  CHECK_EQ(t.seq("host"), std::string("start effect progress load end"));
  checkLoaded(t, "host", 2, 1, "a.png");
  CHECK_EQ(t.seq("missing"), std::string("start effect error end"));
  CHECK(t.event("missing", "error")["error"].asBool());
  CHECK_EQ(t.seq("nofile"), std::string("start effect error end"));
  CHECK_EQ(t.seq("bad"), std::string("start effect error end"));
  CHECK_EQ(t.seq("slow"), std::string("start effect"));
  CHECK(jsui_wants_frame(t.rt));  // a load in flight
  t.hold = false;
  t.tick(32);
  CHECK_EQ(t.seq("slow"), std::string("start effect progress load end"));
  t.reset();
  t.tick(32);
  CHECK(!jsui_wants_frame(t.rt));

  // Already cached at mount (Image and ImageBackground): still onLoad, on
  // the next tick, without onProgress and without a new request.
  size_t requests = t.served.size();
  t.show(R"([{"name":"again","src":"sword"},{"name":"bg","src":"game://tex/a.png","background":true}])");
  CHECK_EQ(t.seq("again"), std::string("start effect"));
  t.tick(16);
  CHECK_EQ(t.seq("again"), std::string("start effect load end"));
  checkLoaded(t, "again", 128, 128, "item-sword");
  CHECK_EQ(t.seq("bg"), std::string("start effect load end"));
  CHECK_EQ(t.served.size(), requests);
  t.reset();
  // A new source on the same Image: a new load start and load.
  t.show(R"([{"name":"again","src":"dot"}])");
  t.tick(16);
  CHECK_EQ(t.seq("again"), std::string("start load end"));
  checkLoaded(t, "again", 2, 1, "data:");
  t.reset();
  t.tick(32);
  CHECK(!jsui_wants_frame(t.rt));
  CHECK(jsui_last_error(t.rt) == nullptr);

  // The built-in loader (bundled asset from disk, missing file).
  ImagesE2E b;
  CHECK(b.load(path, false));
  b.messages.clear();
  b.show(R"([{"name":"sword","src":"sword"},{"name":"nofile","src":"no-such-image.png"}])");
  // File reads complete on the loader thread: wait in real time (up to 5 s).
  for (int i = 0; i < 5000 && (b.seq("sword").find(" end") == std::string::npos ||
                               b.seq("nofile").find(" end") == std::string::npos);
       i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    b.tick(16);
  }
  CHECK_EQ(b.seq("sword"), std::string("start effect progress load end"));
  checkLoaded(b, "sword", 128, 128, "item-sword");
  CHECK_EQ(b.seq("nofile"), std::string("start effect error end"));
  CHECK(jsui_last_error(b.rt) == nullptr);
}
