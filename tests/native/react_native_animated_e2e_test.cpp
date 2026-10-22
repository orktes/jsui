// End-to-end native driver (useNativeDriver: true): a real @jsui/react bundle
// (packages/cli/test/e2e/native_animated.jsx) driven through the C API.
// - parity: timing (several easings, delay), spring (origami, bounciness /
//   speed, stiffness / damping / mass + velocity, overshoot clamping), decay,
//   color / 'deg' / clamped interpolations, add / multiply / modulo /
//   diffClamp: the native-driven view's style equals the JS-driven twin's
//   every frame (within the sampled-easing tolerance) and both end on the
//   same frame;
// - JS not ticked (jsui_tick_native): the native animation advances exactly
//   frame by frame while the JS-driven one is frozen; only the animated
//   box is repainted and incremental == full repaint; the end callback
//   ({finished, value}) waits for the next jsui_tick;
// - stop / setValue / listeners sync values back to JS; JS-driven animations
//   on a native value throw; native loops; layout props throw like RN;
// - Animated.event: ScrollView offsets drive a header natively.
// Built and run by the @jsui/cli suite with JSUI_REACT_NATIVE_ANIMATED_BUNDLE.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "animated/native_animated.h"
#include "core/document.h"
#include "core/value.h"
#include "jsui/jsui.h"
#include "runtime.h"
#include "test.h"

using jsui::Value;

namespace {

constexpr int W = 400, H = 300;

struct NativeAnimatedE2E {
  std::vector<std::pair<std::string, std::string>> messages;
  JsuiRuntime* rt = nullptr;
  double now = 0;
  std::map<std::string, uint32_t> ids;

  bool load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    JsuiConfig cfg;
    jsui_config_init(&cfg);
    cfg.width = W;
    cfg.height = H;
    cfg.on_message = [](void* u, const char* name, const char* json) {
      static_cast<NativeAnimatedE2E*>(u)->messages.emplace_back(name, json ? json : "");
    };
    cfg.message_user = this;
    rt = jsui_create(&cfg);
    if (jsui_load_bundle_source(rt, src.data(), src.size(), path.c_str()) != 0) return false;
    tick(32);
    for (auto& [n, j] : messages)
      if (n == "ids") {
        Value m = Value::parseJson(j);
        for (auto& [k, v] : m.asObject()) ids[k] = uint32_t(v.asNumber());
      }
    messages.clear();
    return true;
  }
  ~NativeAnimatedE2E() {
    if (rt) jsui_destroy(rt);
  }
  jsui::Runtime& runtime() { return *reinterpret_cast<std::unique_ptr<jsui::Runtime>*>(rt)->get(); }
  void tick(double ms) {
    for (double end = now + ms; now < end;) {
      now += 16;
      jsui_tick(rt, now);
    }
  }
  void post(const char* name) { jsui_post_message(rt, name, "null"); }
  std::vector<Value> take(const std::string& name) {
    std::vector<Value> out;
    for (auto it = messages.begin(); it != messages.end();) {
      if (it->first == name) {
        out.push_back(Value::parseJson(it->second));
        it = messages.erase(it);
      } else {
        ++it;
      }
    }
    return out;
  }
  const Value& style(const std::string& name) {
    static const Value null;
    jsui::Node* n = runtime().doc.get(ids[name]);
    return n ? n->props()["style"] : null;
  }
};

// Numbers in a string ("rgba(1, 2, 3, 0.5)", "45deg").
std::vector<double> numbersIn(const std::string& s) {
  std::vector<double> out;
  const char* p = s.c_str();
  while (*p) {
    if ((*p >= '0' && *p <= '9') || ((*p == '-' || *p == '.') && p[1] >= '0' && p[1] <= '9')) {
      char* end = nullptr;
      out.push_back(std::strtod(p, &end));
      p = end;
    } else {
      p++;
    }
  }
  return out;
}

// Largest numeric difference between two style values (1e9: different shape).
double maxDiff(const Value& a, const Value& b) {
  if (a.isNumber() && b.isNumber()) return std::fabs(a.asNumber() - b.asNumber());
  if (a.isString() && b.isString()) {
    auto x = numbersIn(a.asString()), y = numbersIn(b.asString());
    if (x.size() != y.size()) return 1e9;
    double d = 0;
    for (size_t i = 0; i < x.size(); i++) d = std::max(d, std::fabs(x[i] - y[i]));
    return d;
  }
  if (a.isArray() && b.isArray()) {
    if (a.asArray().size() != b.asArray().size()) return 1e9;
    double d = 0;
    for (size_t i = 0; i < a.asArray().size(); i++) d = std::max(d, maxDiff(a.asArray()[i], b.asArray()[i]));
    return d;
  }
  if (a.isObject() && b.isObject()) {
    if (a.asObject().size() != b.asObject().size()) return 1e9;
    double d = 0;
    for (auto& [k, v] : a.asObject()) d = std::max(d, b.has(k) ? maxDiff(v, b[k]) : 1e9);
    return d;
  }
  return a.isNull() && b.isNull() ? 0 : 1e9;
}

double transformValue(const Value& style, const char* key) {
  for (const Value& t : style["transform"].asArray())
    if (t.has(key)) return t[key].asNumber(NAN);
  return NAN;
}

}  // namespace

TEST(react_native_animated_e2e) {
  const char* path = std::getenv("JSUI_REACT_NATIVE_ANIMATED_BUNDLE");
  if (!path) {
    std::printf("  (skipped: set JSUI_REACT_NATIVE_ANIMATED_BUNDLE, see packages/cli/test)\n");
    return;
  }
  NativeAnimatedE2E t;
  CHECK(t.load(path));
  const char* err = jsui_last_error(t.rt);
  if (err) std::fprintf(stderr, "  js error: %s\n", err);
  CHECK(err == nullptr);
  CHECK(t.ids.size() > 20);
  CHECK(!jsui_wants_frame(t.rt));

  // ---- parity: native vs JS driver, frame by frame.
  struct Pair {
    const char* name;
    double tol;  // sampled timing curves differ slightly; the rest is the same math
  };
  const Pair pairs[] = {{"ease", 2e-3},          {"bounce", 1.0},         {"elastic", 2e-3},
                        {"bezier", 2e-3},        {"springOrigami", 1e-6}, {"springBouncy", 1e-6},
                        {"springPhysics", 1e-6}, {"springClamped", 1e-6}, {"decay", 1e-6},
                        {"color", 1.0 + 1e-6}};
  std::map<std::string, double> worst;
  std::map<std::string, std::map<std::string, double>> endTick;
  t.post("parity");
  CHECK_EQ(t.runtime().animated->animationCount(), std::size(pairs));  // the native half runs in the runtime
  int frames = 0, wanting = 0;
  for (; frames < 400; frames++) {
    t.tick(16);
    wanting += jsui_wants_frame(t.rt) ? 1 : 0;
    for (auto& p : pairs) {
      std::string n = p.name;
      double d = maxDiff(t.style(n + "0"), t.style(n + "1"));
      worst[n] = std::max(worst[n], d);
    }
    for (const Value& e : t.take("end")) {
      CHECK(e["finished"].asBool());
      endTick[e["name"].asString()][e["driver"].asString()] = t.now;
    }
    if (endTick.size() == std::size(pairs) && std::all_of(endTick.begin(), endTick.end(), [](auto& kv) { return kv.second.size() == 2; }))
      break;
  }
  CHECK(frames < 400);
  CHECK(wanting >= frames);  // native animations keep the host ticking until the last frame
  for (auto& p : pairs) {
    std::printf("    parity %-14s max |native - js| = %.3g (tolerance %.3g), ends js %.0f native %.0f ms\n", p.name,
                worst[p.name], p.tol, endTick[p.name]["js"], endTick[p.name]["native"]);
    CHECK(worst[p.name] <= p.tol);
    CHECK_EQ(endTick[p.name]["js"], endTick[p.name]["native"]);
  }
  // Final values agree exactly.
  for (auto& p : pairs) CHECK(maxDiff(t.style(std::string(p.name) + "0"), t.style(std::string(p.name) + "1")) <= p.tol);
  t.tick(32);
  CHECK(!jsui_wants_frame(t.rt));

  // ---- JS not ticked: native keeps animating, frame by frame.
  std::vector<uint8_t> inc(size_t(W) * H * 4), full(size_t(W) * H * 4);
  jsui_render_rgba_incremental(t.rt, inc.data(), W, H, W * 4, nullptr);
  t.post("blocked");
  double start = t.now;
  CHECK(jsui_wants_frame(t.rt));
  double maxErr = 0;
  bool damageOk = true, pixelsOk = true;
  for (int k = 1; k <= 21; k++) {  // 336 ms: past the 320 ms end
    t.now += 16;
    jsui_tick_native(t.rt, t.now);
    double expected = std::min(1.0, (t.now - start) / 320.0);
    maxErr = std::max(maxErr, std::fabs(t.style("blockedNative")["opacity"].asNumber(-1) - expected));
    CHECK_EQ(t.style("blockedJs")["opacity"].asNumber(-1), 0.0);  // JS driver frozen
    // Only the native box (50 x 50 at 260, 0) is repainted, and the
    // incremental frame equals a full repaint.
    JsuiRect dirty{};
    jsui_render_rgba_incremental(t.rt, inc.data(), W, H, W * 4, &dirty);
    if (k <= 20 && !(dirty.x >= 259 && dirty.y >= 0 && dirty.x + dirty.width <= 311 && dirty.height <= 51 && dirty.width > 0))
      damageOk = false;
  }
  jsui_render_rgba(t.rt, full.data(), W, H, W * 4);
  pixelsOk = std::memcmp(inc.data(), full.data(), inc.size()) == 0;
  // The incremental buffer for the next checks must be ours again.
  jsui_render_rgba_incremental(t.rt, inc.data(), W, H, W * 4, nullptr);
  std::printf("    js not ticked: native opacity max error %.2g over 21 frames, js opacity stayed 0\n", maxErr);
  CHECK(maxErr < 1e-9);
  CHECK(damageOk);
  CHECK(pixelsOk);
  CHECK(t.take("blockedEnd").empty());  // ends are delivered by the next jsui_tick
  t.tick(16);
  auto ends = t.take("blockedEnd");
  CHECK_EQ(ends.size(), size_t(2));  // native's (queued) and JS's (it caught up: 352 ms > 320)
  bool sawNative = false;
  for (auto& e : ends) {
    CHECK(e["finished"].asBool());
    if (e["driver"].asString() == "native") {
      sawNative = true;
      CHECK_EQ(e["value"].asNumber(), 1.0);
    }
  }
  CHECK(sawNative);
  CHECK_EQ(t.style("blockedJs")["opacity"].asNumber(), 1.0);

  // ---- stop / listeners / setValue sync back to JS.
  t.post("stopStart");
  double s0 = t.now;
  t.tick(304);
  t.post("stopRead");
  auto read = t.take("stopRead");
  CHECK_EQ(read.size(), size_t(1));
  if (!read.empty()) {
    double nativeX = transformValue(t.style("stop"), "translateX");
    CHECK_NEAR(nativeX, (t.now - s0) / 10.0, 1e-9);
    CHECK_NEAR(read[0]["value"].asNumber(), nativeX, 1e-9);  // JS reads the native value
    CHECK(read[0]["listenerUpdates"].asNumber() >= 18);      // one per native frame
    CHECK_NEAR(read[0]["listenerLast"].asNumber(), nativeX, 1e-9);
  }
  t.post("stop");
  auto stopped = t.take("stopValue");
  auto stopEnd = t.take("stopEnd");
  CHECK_EQ(stopped.size(), size_t(1));
  CHECK_EQ(stopEnd.size(), size_t(1));
  double atStop = transformValue(t.style("stop"), "translateX");
  if (!stopped.empty()) CHECK_NEAR(stopped[0].asNumber(), atStop, 1e-9);
  if (!stopEnd.empty()) {
    CHECK(!stopEnd[0]["finished"].asBool(true));
    CHECK_NEAR(stopEnd[0]["value"].asNumber(), atStop, 1e-9);
  }
  t.tick(64);
  CHECK_EQ(transformValue(t.style("stop"), "translateX"), atStop);
  CHECK(!jsui_wants_frame(t.rt));
  t.post("setValue");  // applied to the view at once, without a tick
  CHECK_EQ(transformValue(t.style("stop"), "translateX"), 42.0);
  auto afterSet = t.take("afterSet");
  CHECK(!afterSet.empty() && afterSet[0].asNumber() == 42.0);
  t.post("restart");
  auto restart = t.take("restart");
  CHECK(!restart.empty() && restart[0].asString().find("moved to \"native\"") != std::string::npos);

  // ---- native loop (iterations: 3): restarts from 0 natively, one end.
  t.post("loop");
  double l0 = t.now;
  int drops = 0;
  double last = 0, loopEndAt = 0;
  for (int i = 0; i < 40 && loopEndAt == 0; i++) {
    t.tick(16);
    double o = t.style("loop")["opacity"].asNumber();
    if (o < last) drops++;
    last = o;
    auto e = t.take("loopEnd");
    if (!e.empty()) {
      CHECK(e[0]["finished"].asBool());
      loopEndAt = t.now - l0;
    }
  }
  std::printf("    loop x3 of 96 ms ended after %.0f ms, %d restarts\n", loopEndAt, drops);
  CHECK(loopEndAt >= 288 && loopEndAt <= 304);
  CHECK_EQ(drops, 2);
  CHECK_EQ(t.style("loop")["opacity"].asNumber(), 1.0);

  // ---- Animated.loop(sequence([timing 0 -> 1 (200 ms), timing -> 0 (0 ms)])):
  // like React Native, both drivers animate 0 -> 1, snap back and repeat
  // (the zero-duration native timing reports its end, so the sequence and
  // the loop go on), and the native loop keeps jsui_wants_frame() set.
  t.post("seqLoop");
  {
    struct Track {
      double last = 0, peak = 0;
      int snaps = 0, rising = 0;
    } tn, tj;
    bool idle = false;
    auto sample = [](Track& k, double o) {
      if (o < k.last - 0.5) k.snaps++;
      else if (o > k.last) k.rising++;
      k.peak = std::max(k.peak, o);
      k.last = o;
    };
    for (int i = 0; i < 80; i++) {  // 1280 ms: six iterations of ~200 ms
      t.tick(16);
      sample(tn, t.style("seqNative")["opacity"].asNumber());
      sample(tj, t.style("seqJs")["opacity"].asNumber());
      if (!jsui_wants_frame(t.rt)) idle = true;
    }
    std::printf("    loop(sequence) native: %d snaps, %d rising frames, peak %.2f; js: %d snaps, %d rising, peak %.2f\n",
                tn.snaps, tn.rising, tn.peak, tj.snaps, tj.rising, tj.peak);
    CHECK(tn.snaps >= 4);
    CHECK(tn.rising >= 40);
    CHECK(tn.peak > 0.9);
    CHECK(tj.snaps >= 4);
    CHECK(tj.rising >= 40);
    CHECK(tj.peak > 0.9);
    CHECK(!idle);
    CHECK(t.take("seqEnd").empty());
    t.post("seqStop");
    t.tick(32);
    auto ends = t.take("seqEnd");
    CHECK_EQ(ends.size(), size_t(2));
    for (auto& e : ends) CHECK(!e.asBool());
  }

  // ---- layout props are rejected like React Native.
  t.post("layoutProp");
  auto lp = t.take("layoutProp");
  CHECK(!lp.empty() && lp[0].asString().find("Style property 'width' is not supported by native animated module") !=
                           std::string::npos);
  CHECK_EQ(t.style("width")["width"].asNumber(), 10.0);

  // ---- Animated.event: the ScrollView's offset drives the header natively.
  CHECK_EQ(transformValue(t.style("header"), "translateY"), 0.0);
  jsui_send_mouse_wheel(t.rt, 350, 250, 0, 30);
  t.tick(400);
  jsui::Node* sv = t.runtime().doc.get(t.ids["scroll"]);
  CHECK(sv != nullptr);
  if (sv) {
    CHECK(sv->scrollY > 10);
    CHECK_NEAR(transformValue(t.style("header"), "translateY"), -std::min(40.0, sv->scrollY * 0.4), 1e-4);
  }
  jsui_send_mouse_wheel(t.rt, 350, 250, 0, 400);
  t.tick(400);
  if (sv) {
    CHECK(sv->scrollY > 100);
    CHECK_EQ(transformValue(t.style("header"), "translateY"), -40.0);
  }
  CHECK(jsui_last_error(t.rt) == nullptr);
}
