// Native Animated driver (src/animated/native_animated.*): the evaluator on
// its own (interpolation incl. colors / strings / extrapolation, math nodes,
// timing curves, spring and decay against their closed forms, iterations,
// stop / setValue, props written into views) and through the runtime with a
// plain-JS bundle using __jsui_native.animated (frame clock, JS not ticked,
// end callbacks, damage == the animated node, incremental == full repaint).
// The parity with @orktes/jsui-react's JS driver is checked end to end in
// react_native_animated_e2e_test.cpp.
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "animated/native_animated.h"
#include "core/document.h"
#include "core/value.h"
#include "jsui/jsui.h"
#include "runtime.h"
#include "test.h"

using namespace jsui;

namespace {

Value json(const char* s) {
  std::string err;
  Value v = Value::parseJson(s, &err);
  if (!err.empty()) std::fprintf(stderr, "  bad json in test: %s\n", err.c_str());
  return v;
}

double quad(double t) { return t * t; }

// A sampled easing curve (what @orktes/jsui-react sends for timing animations).
Value frames(double (*fn)(double), int count) {
  Value::Array a;
  for (int i = 0; i < count; i++) a.emplace_back(fn(double(i) / (count - 1)));
  return Value(std::move(a));
}

Value obj(std::initializer_list<std::pair<const char*, Value>> kv) {
  Value::Object o;
  for (auto& [k, v] : kv) o[k] = v;
  return Value(std::move(o));
}

// Closed form of a damped spring from `from` to `to` (initial velocity 0).
double springAt(double k, double c, double m, double from, double to, double t) {
  double zeta = c / (2 * std::sqrt(k * m)), w0 = std::sqrt(k / m), x0 = to - from;
  if (zeta < 1) {
    double w1 = w0 * std::sqrt(1 - zeta * zeta);
    return to - std::exp(-zeta * w0 * t) * ((zeta * w0 * x0 / w1) * std::sin(w1 * t) + x0 * std::cos(w1 * t));
  }
  return to - std::exp(-w0 * t) * (x0 + w0 * x0 * t);
}

}  // namespace

TEST(native_animated_interpolation) {
  Document doc;
  NativeAnimated na(doc);
  CHECK(na.createNode(1, json(R"({"type":"value","value":0})")));
  // Numbers, two segments, extend on both sides (default).
  CHECK(na.createNode(2, json(R"({"type":"interpolation","inputRange":[0,1,2],"outputType":"number","outputs":[[0,100,150]]})")));
  na.connectNodes(1, 2);
  auto at = [&](int tag, double v) {
    na.setValue(1, v);
    return na.nodeOutput(tag);
  };
  CHECK_NEAR(at(2, 0.5), 50, 1e-12);
  CHECK_NEAR(at(2, 1.5), 125, 1e-12);
  CHECK_NEAR(at(2, -1), -100, 1e-12);  // extend
  CHECK_NEAR(at(2, 3), 200, 1e-12);
  // clamp / identity.
  CHECK(na.createNode(3, json(R"({"type":"interpolation","inputRange":[0,10],"outputType":"number","outputs":[[0,1]],
                                  "extrapolateLeft":"identity","extrapolateRight":"clamp"})")));
  na.connectNodes(1, 3);
  CHECK_NEAR(at(3, 20), 1, 1e-12);
  CHECK_NEAR(at(3, -7), -7, 1e-12);
  CHECK_NEAR(at(3, 5), 0.5, 1e-12);
  // Colors: channels interpolated, rounded and clamped like the JS driver.
  CHECK(na.createNode(4, json(R"({"type":"interpolation","inputRange":[0,1],"outputType":"color",
                                  "outputs":[[255,0],[0,0],[0,255],[1,0.5]]})")));
  na.connectNodes(1, 4);
  na.setValue(1, 0.5);
  CHECK_EQ(na.nodeStyleValue(4).asString(), std::string("rgba(128, 0, 128, 0.75)"));
  na.setValue(1, 2);  // extended past the end, then clamped to 0..255 / 0..1
  CHECK_EQ(na.nodeStyleValue(4).asString(), std::string("rgba(0, 0, 255, 0)"));
  // Strings with numbers ('deg', several numbers): template kept, 5 decimals.
  CHECK(na.createNode(5, json(R"({"type":"interpolation","inputRange":[0,1],"outputType":"string",
                                  "parts":["","deg"],"outputs":[[0,360]]})")));
  CHECK(na.createNode(6, json(R"J({"type":"interpolation","inputRange":[0,1],"outputType":"string",
                                  "parts":["translate(","px, ","px)"],"outputs":[[0,10],[-5,5]]})J")));
  na.connectNodes(1, 5);
  na.connectNodes(1, 6);
  na.setValue(1, 1.0 / 3);
  CHECK_EQ(na.nodeStyleValue(5).asString(), std::string("120deg"));
  CHECK_EQ(na.nodeStyleValue(6).asString(), std::string("translate(3.33333px, -1.66667px)"));
  // Custom easing (sampled curve) applied to the normalized input.
  CHECK(na.createNode(7, obj({{"type", "interpolation"},
                              {"inputRange", json("[0,1]")},
                              {"outputType", "number"},
                              {"outputs", json("[[0,100]]")},
                              {"easing", frames(quad, 257)}})));
  na.connectNodes(1, 7);
  CHECK_NEAR(at(7, 0.5), 25, 1e-9);
  CHECK_NEAR(at(7, 0.3), 9, 2e-3);
  CHECK_EQ(formatAnimatedNumber(0.00001), std::string("0.00001"));
  CHECK_EQ(formatAnimatedNumber(-0.0), std::string("0"));
  CHECK_EQ(formatAnimatedNumber(1e21), std::string("1e+21"));
}

TEST(native_animated_math_nodes) {
  Document doc;
  NativeAnimated na(doc);
  na.createNode(1, json(R"({"type":"value","value":3})"));
  na.createNode(2, json(R"({"type":"value","value":4,"offset":1})"));
  na.createNode(3, json(R"({"type":"addition","input":[1,2]})"));
  na.createNode(4, json(R"({"type":"subtraction","input":[1,2]})"));
  na.createNode(5, json(R"({"type":"multiplication","input":[1,2]})"));
  na.createNode(6, json(R"({"type":"division","input":[1,2]})"));
  na.createNode(7, json(R"({"type":"modulus","input":1,"modulus":2})"));
  for (int t = 3; t <= 6; t++) {
    na.connectNodes(1, t);
    na.connectNodes(2, t);
  }
  na.connectNodes(1, 7);
  CHECK_EQ(na.nodeOutput(3), 8.0);
  CHECK_EQ(na.nodeOutput(4), -2.0);
  CHECK_EQ(na.nodeOutput(5), 15.0);
  CHECK_EQ(na.nodeOutput(6), 0.6);
  CHECK_EQ(na.nodeOutput(7), 1.0);
  na.setValue(1, -3);
  CHECK_EQ(na.nodeOutput(7), 1.0);  // ((-3 % 2) + 2) % 2, like the JS node
  na.setOffset(2, -4);              // divide by zero -> 0 natively
  CHECK_EQ(na.nodeOutput(6), 0.0);
  na.flattenOffset(2);
  CHECK_EQ(na.getValue(2), 0.0);
  na.setValue(2, 3);
  na.extractOffset(2);
  CHECK_EQ(na.getValue(2), 0.0);
  CHECK_EQ(na.nodeOutput(2), 3.0);
  // diffClamp follows the input's changes, clamped (starting from JS's state).
  na.createNode(8, json(R"({"type":"diffclamp","input":1,"min":0,"max":10,"last":-3,"value":2})"));
  na.connectNodes(1, 8);
  CHECK_EQ(na.nodeOutput(8), 2.0);
  na.setValue(1, 5);  // +8
  CHECK_EQ(na.nodeOutput(8), 10.0);
  na.setValue(1, 1);  // -4
  CHECK_EQ(na.nodeOutput(8), 6.0);
  na.setValue(1, -20);
  CHECK_EQ(na.nodeOutput(8), 0.0);
  // Color node: rgba() of four values.
  na.createNode(10, json(R"({"type":"value","value":10.4})"));
  na.createNode(11, json(R"({"type":"value","value":20.5})"));
  na.createNode(12, json(R"({"type":"value","value":30})"));
  na.createNode(13, json(R"({"type":"value","value":0.25})"));
  na.createNode(14, json(R"({"type":"color","r":10,"g":11,"b":12,"a":13})"));
  CHECK_EQ(na.nodeStyleValue(14).asString(), std::string("rgba(10, 21, 30, 0.25)"));
  std::string error;
  CHECK(!na.createNode(20, json(R"({"type":"tracking"})"), &error));
  CHECK(error.find("tracking") != std::string::npos);
}

TEST(native_animated_timing_spring_decay) {
  Document doc;
  NativeAnimated na(doc);
  na.createNode(1, json(R"({"type":"value","value":0})"));
  // timing: quad easing sampled every 2 ms over 300 ms, delay 20 ms.
  CHECK(na.startAnimation(7, 1, obj({{"type", "frames"}, {"frames", frames(quad, 151)}, {"duration", 300.0},
                                     {"toValue", 50.0}, {"delay", 20.0}}),
                          1000));
  CHECK(na.animating());
  double maxErr = 0;
  for (double t = 1000; t <= 1320; t += 5) {
    na.tick(t);
    double p = std::clamp((t - 1020) / 300, 0.0, 1.0);
    maxErr = std::max(maxErr, std::fabs(na.getValue(1) - 50 * quad(p)));
  }
  CHECK(maxErr < 1e-3);
  CHECK(!na.animating());
  CHECK_EQ(na.ends.size(), size_t(1));
  CHECK(na.ends[0].id == 7 && na.ends[0].finished && na.ends[0].value == 50);
  na.ends.clear();

  // spring (stiffness / damping / mass): the closed form, then exactly at rest.
  na.setValue(1, 0);
  na.startAnimation(8, 1, json(R"({"type":"spring","stiffness":170,"damping":9,"mass":1.2,"toValue":1})"), 0);
  double t = 0;
  maxErr = 0;
  while (na.animating() && t < 10000) {
    t += 16;
    na.tick(t);
    if (na.animating()) maxErr = std::max(maxErr, std::fabs(na.getValue(1) - springAt(170, 9, 1.2, 0, 1, t / 1000)));
  }
  CHECK(maxErr < 1e-12);
  CHECK(t > 1000 && t < 6000);
  CHECK_EQ(na.getValue(1), 1.0);
  CHECK(na.ends.size() == 1 && na.ends[0].finished);
  na.ends.clear();
  // overshootClamping ends at the first crossing.
  na.setValue(1, 0);
  na.startAnimation(9, 1, json(R"({"type":"spring","stiffness":300,"damping":5,"toValue":1,"overshootClamping":true})"), 0);
  for (t = 16; na.animating() && t < 2000; t += 16) na.tick(t);
  CHECK(t < 200);
  CHECK_EQ(na.getValue(1), 1.0);
  na.ends.clear();

  // decay: v / (1 - d) * (1 - e^{-(1 - d) t}), ends when a frame moves < 0.1.
  na.setValue(1, 10);
  na.startAnimation(10, 1, json(R"({"type":"decay","velocity":2,"deceleration":0.99})"), 0);
  na.tick(100);
  CHECK_NEAR(na.getValue(1), 10 + 2 / 0.01 * (1 - std::exp(-0.01 * 100)), 1e-9);
  for (t = 116; na.animating() && t < 5000; t += 16) na.tick(t);
  CHECK(na.getValue(1) > 205 && na.getValue(1) < 210);  // 10 + 200 asymptotically
  CHECK(na.ends.size() == 1 && na.ends[0].finished);
  na.ends.clear();

  // iterations: restarts from the start value; one end after all of them.
  na.setValue(1, 0);
  na.startAnimation(11, 1, obj({{"type", "frames"}, {"frames", json("[0,1]")}, {"duration", 100.0}, {"toValue", 1.0},
                                {"iterations", 3.0}}),
                    0);
  int restarts = 0;
  double last = 0;
  for (t = 10; na.animating(); t += 10) {
    na.tick(t);
    if (na.getValue(1) < last) restarts++;
    last = na.getValue(1);
  }
  CHECK_EQ(restarts, 2);
  CHECK_NEAR(t, 310, 1e-9);
  CHECK_EQ(na.ends.size(), size_t(1));
  na.ends.clear();

  // stop: no end event (the caller reports it), a spring's velocity is returned.
  na.setValue(1, 0);
  na.startAnimation(12, 1, json(R"({"type":"spring","stiffness":100,"damping":10,"toValue":100})"), 0);
  na.tick(50);
  double v = 0;
  CHECK(na.stopAnimation(12, &v));
  CHECK(v > 100);  // units / s, heading for 100
  CHECK(!na.stopAnimation(12));
  CHECK(na.ends.empty());
  // setValue stops the node's animations and reports them unfinished.
  na.startAnimation(13, 1, json(R"({"type":"decay","velocity":1})"), 50);
  na.setValue(1, 5);
  CHECK(!na.animating());
  CHECK(na.ends.size() == 1 && na.ends[0].id == 13 && !na.ends[0].finished);
  CHECK_EQ(na.getValue(1), 5.0);
  // Starting on an animating node stops the previous animation the same way.
  na.ends.clear();
  na.startAnimation(14, 1, json(R"({"type":"decay","velocity":1})"), 60);
  na.startAnimation(15, 1, json(R"({"type":"decay","velocity":-1})"), 60);
  CHECK(na.ends.size() == 1 && na.ends[0].id == 14 && !na.ends[0].finished);
  CHECK_EQ(na.animationCount(), size_t(1));
  // A zero-duration timing applies at once and still reports its end (a
  // sequence / loop it is part of must go on).
  na.ends.clear();
  na.startAnimation(17, 1, json(R"({"type":"frames","frames":[0,1],"duration":0,"toValue":3})"), 70);
  CHECK_EQ(na.getValue(1), 3.0);
  CHECK(na.ends.size() == 2 && na.ends[0].id == 15 && !na.ends[0].finished);
  CHECK(na.ends.size() == 2 && na.ends[1].id == 17 && na.ends[1].finished && na.ends[1].value == 3.0);
  CHECK(!na.animating());
  std::string error;
  CHECK(!na.startAnimation(16, 999, json(R"({"type":"decay"})"), 0, &error));
  CHECK(!na.startAnimation(16, 1, json(R"({"type":"bogus"})"), 0, &error));
}

TEST(native_animated_props_into_view) {
  Document doc;
  doc.setViewport(200, 100, 1);
  Node* view = doc.createNode(NodeType::View);
  doc.root()->appendChild(view);
  view->setProps(json(R"({"style":{"width":50,"height":50,"backgroundColor":"red","opacity":1}})"));
  NativeAnimated na(doc);
  na.createNode(1, json(R"({"type":"value","value":0.5})"));
  na.createNode(2, json(R"({"type":"interpolation","inputRange":[0,1],"outputType":"string","parts":["","deg"],"outputs":[[0,90]]})"));
  na.connectNodes(1, 2);
  na.createNode(3, json(R"({"type":"transform","transforms":[{"property":"translateX","value":5},{"property":"rotate","nodeTag":2}]})"));
  na.connectNodes(2, 3);
  na.createNode(4, json(R"({"type":"style","style":{"opacity":1,"transform":3}})"));
  na.connectNodes(1, 4);
  na.connectNodes(3, 4);
  na.createNode(5, json(R"({"type":"props","props":{"style":4}})"));
  na.connectNodes(4, 5);
  doc.layout();
  na.connectToView(5, view->id());
  na.flush();
  auto& style = view->props()["style"];
  CHECK_EQ(style["opacity"].asNumber(), 0.5);
  CHECK_EQ(style["transform"].toJson(), std::string(R"([{"translateX":5},{"rotate":"45deg"}])"));
  CHECK_EQ(style["backgroundColor"].asString(), std::string("red"));  // other keys kept
  // Paint-only: no relayout.
  doc.clearPaintDirty();
  na.setValue(1, 1);
  CHECK(doc.paintDirty());
  CHECK(!doc.layout());
  CHECK_EQ(view->props()["style"]["opacity"].asNumber(), 1.0);
  // React re-props the view with the value it rendered: the live value is
  // overlaid again at the next flush.
  view->setProps(json(R"({"style":{"width":50,"height":50,"backgroundColor":"blue","opacity":0.5}})"));
  na.tick(0);
  CHECK_EQ(view->props()["style"]["opacity"].asNumber(), 1.0);
  CHECK_EQ(view->props()["style"]["backgroundColor"].asString(), std::string("blue"));
  // Unchanged frames write nothing.
  uint32_t version = view->paintVersion;
  na.tick(16);
  CHECK_EQ(view->paintVersion, version);
  // Event mappings: path below the event object.
  na.addEventMapping(view->id(), "scroll", {"contentOffset", "y"}, 1);
  na.handleEvent(json((R"({"type":"scroll","target":)" + std::to_string(view->id()) + R"(,"contentOffset":{"x":0,"y":0.25}})").c_str()));
  CHECK_EQ(view->props()["style"]["opacity"].asNumber(), 0.25);
  na.handleEvent(json((R"({"type":"wheel","target":)" + std::to_string(view->id()) + R"(,"contentOffset":{"y":0.75}})").c_str()));
  CHECK_EQ(na.getValue(1), 0.25);  // other event types are not mapped
  na.removeEventMapping(view->id(), "scroll", 1);
  na.handleEvent(json((R"({"type":"scroll","target":)" + std::to_string(view->id()) + R"(,"contentOffset":{"y":0.75}})").c_str()));
  CHECK_EQ(na.getValue(1), 0.25);
  // A destroyed view is forgotten; dropping nodes unlinks them.
  na.viewDestroyed(view->id());
  na.setValue(1, 0.1);
  CHECK_EQ(view->props()["style"]["opacity"].asNumber(), 0.25);
  na.dropNode(2);
  CHECK(!na.hasNode(2));
  CHECK_EQ(na.nodeStyleValue(3).toJson(), std::string(R"([{"translateX":5},{"rotate":null}])"));
}

// Through the runtime with a bundle that talks to __jsui_native.animated
// directly: the runtime's frame clock drives it, jsui_tick_native advances
// it without JS, ends reach JS on the next tick, and each frame repaints
// only the animated node.
TEST(native_animated_runtime_frame_clock) {
  JsuiConfig cfg;
  jsui_config_init(&cfg);
  cfg.width = 200;
  cfg.height = 100;
  std::vector<std::string> messages;
  cfg.on_message = [](void* u, const char* name, const char* json) {
    static_cast<std::vector<std::string>*>(u)->push_back(std::string(name) + "=" + json);
  };
  cfg.message_user = &messages;
  JsuiRuntime* rt = jsui_create(&cfg);
  const char* src = R"JS(
    const N = __jsui_native, A = N.animated;
    const bg = N.createNode('View');
    N.setProps(bg, { style: { width: 200, height: 100, backgroundColor: '#000' } });
    N.appendChild(N.rootId, bg);
    const box = N.createNode('View');
    N.setProps(box, { style: { position: 'absolute', left: 20, top: 30, width: 40, height: 20, backgroundColor: '#fff', opacity: 0 } });
    N.appendChild(bg, box);
    A.createNode(1, { type: 'value', value: 0, offset: 0 });
    A.createNode(2, { type: 'style', style: { opacity: 1 } });
    A.connect(1, 2);
    A.createNode(3, { type: 'props', props: { style: 2 } });
    A.connect(2, 3);
    A.connectToView(3, box);
    globalThis.__jsui_onAnimatedEnd = (id, finished, value) => N.postMessage('end', { id, finished, value, at: performance.now() });
    globalThis.__jsui_onHostMessage = (name) => {
      if (name === 'start') A.startAnimation(5, 1, { type: 'frames', frames: [0, 1], duration: 160, toValue: 1, iterations: 1 });
      if (name === 'bad') { try { A.createNode(9, { type: 'nope' }); } catch (e) { N.postMessage('error', String(e.message)); } }
    };
    N.postMessage('box', box);
  )JS";
  CHECK_EQ(jsui_load_bundle_source(rt, src, std::strlen(src), "na.js"), 0);
  Runtime& r = *reinterpret_cast<std::unique_ptr<Runtime>*>(rt)->get();
  double now = 0;
  jsui_tick(rt, now += 16);
  CHECK(!messages.empty());
  uint32_t boxId = uint32_t(std::stoul(messages.back().substr(4)));
  Node* box = r.doc.get(boxId);
  CHECK(box != nullptr);
  if (!box) return;
  CHECK(!jsui_wants_frame(rt));
  std::vector<uint8_t> inc(200 * 100 * 4), full(200 * 100 * 4);
  jsui_render_rgba_incremental(rt, inc.data(), 200, 100, 800, nullptr);
  jsui_post_message(rt, "start", "null");
  double start = now;
  CHECK(jsui_wants_frame(rt));
  // JS is never ticked from here: the animation still advances every frame.
  bool damageOk = true;
  for (int k = 1; k <= 12; k++) {
    jsui_tick_native(rt, now += 16);
    CHECK_NEAR(box->props()["style"]["opacity"].asNumber(), std::min(1.0, (now - start) / 160), 1e-12);
    JsuiRect d{};
    jsui_render_rgba_incremental(rt, inc.data(), 200, 100, 800, &d);
    if (k <= 10 && !(d.x >= 19 && d.y >= 29 && d.x + d.width <= 61 && d.y + d.height <= 51 && d.width > 0)) damageOk = false;
  }
  CHECK(damageOk);
  jsui_render_rgba(rt, full.data(), 200, 100, 800);
  CHECK(std::memcmp(inc.data(), full.data(), inc.size()) == 0);
  CHECK_EQ(full[(40 * 200 + 30) * 4], 255);  // the box is fully opaque white
  messages.clear();
  CHECK(jsui_wants_frame(rt));  // the end callback is still pending
  jsui_tick(rt, now += 16);
  CHECK_EQ(messages.size(), size_t(1));
  if (!messages.empty()) CHECK(messages[0].rfind(R"(end={"id":5,"finished":true,"value":1,"at":)", 0) == 0);
  CHECK(!jsui_wants_frame(rt));
  messages.clear();
  jsui_post_message(rt, "bad", "null");
  CHECK(!messages.empty() && messages[0].find("unsupported animated node type 'nope'") != std::string::npos);
  jsui_destroy(rt);
}

// Expression nodes (Animated.derive): programs are validated when created
// and evaluated on their inputs; clock nodes follow tick().
TEST(native_animated_expression) {
  Document doc;
  NativeAnimated na(doc);
  CHECK(na.createNode(1, json(R"({"type":"value","value":3})")));
  CHECK(na.createNode(2, json(R"({"type":"value","value":4})")));
  // const s = a * a; return s + (b > 3 ? clamp(b, 0, 3.5) : 0)  -> 9 + 3.5
  CHECK(na.createNode(3, json(R"({"type":"expression","input":[1,2],"slots":1,
    "code":[1,0, 1,0, 12, 3,0, 2,0, 1,1, 0,3, 22, 1,1, 0,0, 0,3.5, 40,26, 0,0, 28, 10]})")));
  na.connectNodes(1, 3);
  na.connectNodes(2, 3);
  CHECK_NEAR(na.nodeOutput(3), 12.5, 1e-12);
  na.setValue(2, 1);
  CHECK_NEAR(na.nodeOutput(3), 9, 1e-12);
  // NaN -> 0.
  CHECK(na.createNode(4, json(R"({"type":"expression","input":[1],"slots":0,"code":[0,0,0,0,13]})")));
  CHECK_EQ(na.nodeOutput(4), 0.0);
  // Rejected: underflow, leftover values, bad indices / ops / functions.
  std::string err;
  for (const char* bad : {R"({"type":"expression","input":[1],"code":[10]})",
                          R"({"type":"expression","input":[1],"code":[0,1,0,2]})",
                          R"({"type":"expression","input":[1],"code":[1,1]})",
                          R"({"type":"expression","input":[1],"slots":0,"code":[2,0]})",
                          R"({"type":"expression","input":[1],"code":[99]})",
                          R"({"type":"expression","input":[1],"code":[0,1,40,77]})",
                          R"({"type":"expression","input":[1],"code":[0]})"}) {
    err.clear();
    CHECK(!na.createNode(9, json(bad), &err));
    CHECK(err.find("expression") != std::string::npos);
  }
  // Clock: seconds of the frame clock, dirty every tick while used.
  CHECK(na.createNode(5, json(R"({"type":"clock"})")));
  CHECK(!na.animating());
  CHECK(na.createNode(6, json(R"({"type":"expression","input":[5],"code":[1,0,0,2,12]})")));
  na.connectNodes(5, 6);
  CHECK(na.animating());
  na.tick(1500);
  CHECK_NEAR(na.nodeOutput(6), 3, 1e-12);
  na.tick(2000);
  CHECK_NEAR(na.nodeOutput(6), 4, 1e-12);
  na.dropNode(6);
  CHECK(!na.animating());
}
