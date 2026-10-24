// The bridge's command buffer (js/command_buffer.h) without React: mutations
// are recorded and applied when JS returns to native (not while JS runs),
// in call order, with ids taken when JS calls createNode; reads (measure,
// getProps), host callbacks and imperative methods (focus, scrollToEnd) see
// everything recorded before them; nodes created in one task are attached
// in a later one; unknown ids are ignored; updateProps' keepStyle keeps the
// node's current style.
#include <cstring>
#include <string>
#include <vector>

#include "core/document.h"
#include "input/input.h"
#include "jsui/jsui.h"
#include "runtime.h"
#include "test.h"

namespace {

struct Host {
  JsuiRuntime* rt = nullptr;
  jsui::Runtime* internal = nullptr;
  std::vector<std::string> messages;
  // What the main root had when each console line was logged (JS on the
  // stack, before any sync point).
  std::vector<size_t> rootChildrenAtLog;
  std::vector<size_t> rootChildrenAtMessage;
  double t = 0;

  Host() {
    JsuiConfig cfg;
    jsui_config_init(&cfg);
    cfg.width = 100;
    cfg.height = 100;
    cfg.on_message = [](void* u, const char* name, const char* json) {
      auto* h = static_cast<Host*>(u);
      h->messages.push_back(std::string(name) + "=" + json);
      h->rootChildrenAtMessage.push_back(h->internal->doc.root()->children().size());
    };
    cfg.message_user = this;
    cfg.log = [](void* u, JsuiLogLevel, const char*) {
      auto* h = static_cast<Host*>(u);
      if (h->internal) h->rootChildrenAtLog.push_back(h->internal->doc.root()->children().size());
    };
    cfg.log_user = this;
    rt = jsui_create(&cfg);
    internal = jsui::internalRuntime(rt);
  }
  ~Host() { jsui_destroy(rt); }
  int run(const char* src) { return jsui_load_bundle_source(rt, src, std::strlen(src), "test.js"); }
  void tick() { jsui_tick(rt, t += 16); }
  jsui::Node* root() { return internal->doc.root(); }
};

}  // namespace

TEST(cmdbuf_applied_when_js_returns) {
  Host h;
  CHECK_EQ(h.run(R"JS(
const n = __jsui_native;
const v = n.createNode('View');
n.setProps(v, { style: { width: 10, height: 10 } });
n.appendChild(n.rootId, v);
console.log('recorded');           // not a sync point: nothing applied yet
n.postMessage('applied', v);      // the embedder sees the tree JS built
globalThis.v = v;
)JS"),
           0);
  CHECK_EQ(h.rootChildrenAtLog.size(), size_t(1));
  CHECK_EQ(h.rootChildrenAtLog[0], size_t(0));
  CHECK_EQ(h.rootChildrenAtMessage.size(), size_t(1));
  CHECK_EQ(h.rootChildrenAtMessage[0], size_t(1));
  CHECK_EQ(h.root()->children().size(), size_t(1));
  CHECK(jsui_last_error(h.rt) == nullptr);
}

TEST(cmdbuf_ids_and_order) {
  Host h;
  // Ids come back at once and are consecutive; nodes named before they
  // exist natively (one batch) are created, filled and ordered as called.
  CHECK_EQ(h.run(R"JS(
const n = __jsui_native;
const a = n.createNode('View'), b = n.createNode('View'), c = n.createNode('Text'), d = n.createNode('View');
const r = n.createNode('RawText');
n.setText(r, 'hello');
n.appendChild(c, r);
n.appendChild(n.rootId, a);
n.appendChild(n.rootId, c);
n.insertBefore(n.rootId, b, c);      // a b c
n.insertBefore(n.rootId, d, a);      // d a b c
n.removeChild(n.rootId, a);          // d b c
n.destroyNode(a);
n.patchStyle(b, { opacity: 0.5 });
n.setProps(b, { style: { width: 7 } });  // setProps replaces the patched style
n.patchStyle(b, { height: 3 });
n.postMessage('ids', [a, b, c, d, r]);
)JS"),
           0);
  CHECK_EQ(h.messages.size(), size_t(1));
  jsui::Node* rootNode = h.root();
  uint32_t first = rootNode->id() + 1;
  std::string want = "ids=[" + std::to_string(first) + "," + std::to_string(first + 1) + "," +
                     std::to_string(first + 2) + "," + std::to_string(first + 3) + "," + std::to_string(first + 4) + "]";
  CHECK_EQ(h.messages[0], want);
  const auto& kids = rootNode->children();
  CHECK_EQ(kids.size(), size_t(3));
  if (kids.size() == 3) {
    CHECK_EQ(kids[0]->id(), first + 3);
    CHECK_EQ(kids[1]->id(), first + 1);
    CHECK_EQ(kids[2]->id(), first + 2);
    CHECK_EQ(kids[2]->children().size(), size_t(1));
    const jsui::Value& style = kids[1]->props()["style"];
    CHECK_EQ(style["width"].asNumber(), 7.0);
    CHECK_EQ(style["height"].asNumber(), 3.0);
    CHECK(!style.has("opacity"));
  }
  CHECK(h.internal->findNode(first) == nullptr);  // destroyed
  // A native creation in between takes the next id from the same counter.
  JsuiRuntime* s = jsui_surface_create(h.rt, "S", "{}", 10, 10, 1);
  CHECK_EQ(uint32_t(jsui_surface_tag(s)), first + 5);
  CHECK_EQ(h.run("__jsui_native.postMessage('next', __jsui_native.createNode('View'))"), 0);
  CHECK_EQ(h.messages.back(), "next=" + std::to_string(first + 6));
}

TEST(cmdbuf_ids_across_surfaces) {
  Host h;
  JsuiRuntime* s = jsui_surface_create(h.rt, "S", "{}", 10, 10, 1);
  uint32_t tag = uint32_t(jsui_surface_tag(s));
  std::string src = "const n = __jsui_native; globalThis.ids = [n.createNode('View', " + std::to_string(tag) +
                    "), n.createNode('View'), n.createNode('View', " + std::to_string(tag) +
                    "), n.createNode('View', 987654)];\n"
                    "n.appendChild(" + std::to_string(tag) + ", ids[0]); n.appendChild(n.rootId, ids[1]);\n"
                    "n.appendChild(" + std::to_string(tag) + ", ids[1]);  // another surface's node: refused\n"
                    "n.postMessage('ids', ids);";
  CHECK_EQ(h.run(src.c_str()), 0);
  CHECK_EQ(h.messages.size(), size_t(1));
  jsui::Surface* surf = h.internal->surfaceByTag(tag);
  CHECK(surf != nullptr);
  if (!surf) return;
  uint32_t first = tag + 1;
  CHECK_EQ(h.messages[0], "ids=[" + std::to_string(first) + "," + std::to_string(first + 1) + "," +
                              std::to_string(first + 2) + "," + std::to_string(first + 3) + "]");
  CHECK(surf->doc.get(first) != nullptr);
  CHECK(surf->doc.get(first + 2) != nullptr);
  CHECK(h.internal->doc.get(first + 1) != nullptr);
  CHECK(h.internal->doc.get(first + 3) != nullptr);  // unknown root tag: the main surface
  CHECK_EQ(surf->doc.root()->children().size(), size_t(1));
  CHECK_EQ(h.root()->children().size(), size_t(1));
}

TEST(cmdbuf_render_phase_then_commit) {
  Host h;
  // Task 1 (a time-sliced render): detached nodes with props. Task 2 (the
  // commit, a timer): attaches them.
  CHECK_EQ(h.run(R"JS(
const n = __jsui_native;
const box = n.createNode('View');
n.updateProps(box, { style: [{ width: 20 }, { height: 10 }], onClick() {} }, false);
globalThis.box = box;
setTimeout(() => n.appendChild(n.rootId, box), 0);
)JS"),
           0);
  CHECK(h.root()->children().empty());
  jsui::Node* box = h.internal->findNode(h.root()->id() + 1);
  CHECK(box != nullptr);
  if (!box) return;
  CHECK(box->parent() == nullptr);
  CHECK_EQ(box->props()["style"]["width"].asNumber(), 20.0);
  CHECK_EQ(box->props()["__events"].asArray().size(), size_t(1));
  h.tick();
  CHECK_EQ(h.root()->children().size(), size_t(1));
  CHECK(box->parent() == h.root());
}

TEST(cmdbuf_unknown_ids_ignored) {
  Host h;
  CHECK_EQ(h.run(R"JS(
const n = __jsui_native;
const v = n.createNode('View');
n.destroyNode(v);
n.setProps(v, { style: { width: 1 } });
n.updateProps(424242, { style: { width: 1 } }, false);
n.patchStyle(0, { opacity: 1 });
n.setText('x', 'y');
n.appendChild(n.rootId, v);
n.appendChild(n.rootId, 424242);
n.insertBefore(n.rootId, 424242, v);
n.removeChild(n.rootId, 424242);
n.destroyNode(424242);
n.focus(424242);
n.scrollTo(424242, 0, 10, false);
n.scrollToEnd(424242, 0, false, false);
n.postMessage('lottie', n.lottieCommand(424242, 'play'));
n.postMessage('measure', n.measure(424242));
const w = n.createNode('View');
n.appendChild(n.rootId, w);
n.postMessage('w', w);
)JS"),
           0);
  CHECK(jsui_last_error(h.rt) == nullptr);
  CHECK_EQ(h.messages.size(), size_t(3));
  if (h.messages.size() == 3) {
    CHECK_EQ(h.messages[0], "lottie=false");
    CHECK_EQ(h.messages[1], "measure=null");
  }
  CHECK_EQ(h.root()->children().size(), size_t(1));
}

TEST(cmdbuf_reads_see_recorded_commands) {
  Host h;
  // measure and getProps right after the calls that changed the node; focus
  // of a node created in the same task.
  CHECK_EQ(h.run(R"JS(
const n = __jsui_native;
const v = n.createNode('View');
n.updateProps(v, { style: { width: 30, height: 12, marginLeft: 5 } }, false);
n.appendChild(n.rootId, v);
const m = n.measure(v);
n.patchStyle(v, { opacity: 0.25 });
const p = n.getProps(v);
const input = n.createNode('TextInput');
n.updateProps(input, { style: { width: 40, height: 20 } }, false);
n.appendChild(n.rootId, input);
n.focus(input);
n.postMessage('read', { x: m.x, w: m.width, h: m.height, opacity: p.style.opacity, input });
)JS"),
           0);
  CHECK_EQ(h.messages.size(), size_t(1));
  if (!h.messages.empty()) CHECK_EQ(h.messages[0], "read={\"x\":5,\"w\":30,\"h\":12,\"opacity\":0.25,\"input\":" +
                                                       std::to_string(h.root()->id() + 2) + "}");
  jsui::Node* focused = h.internal->input->focused();
  CHECK(focused != nullptr && focused->id() == h.root()->id() + 2);
}

TEST(cmdbuf_update_props_keeps_style) {
  Host h;
  // setNativeProps of non-style props: the style JS patched (and the one
  // given now is ignored) stays; hidden and pointerEvents still apply.
  CHECK_EQ(h.run(R"JS(
const n = __jsui_native;
const v = n.createNode('View');
n.updateProps(v, { style: { width: 30, height: 12 }, testID: 'a' }, false);
n.patchStyle(v, { opacity: 0.5 });
n.updateProps(v, { style: { width: 99 }, testID: 'b', pointerEvents: 'none', onClick() {} }, false, true);
globalThis.v = v;
)JS"),
           0);
  jsui::Node* v = h.internal->findNode(h.root()->id() + 1);
  CHECK(v != nullptr);
  if (!v) return;
  const jsui::Value& style = v->props()["style"];
  CHECK_EQ(style["width"].asNumber(), 30.0);
  CHECK_EQ(style["opacity"].asNumber(), 0.5);
  CHECK_EQ(style["pointerEvents"].asString(), std::string("none"));
  CHECK_EQ(v->props()["testID"].asString(), std::string("b"));
  CHECK_EQ(v->props()["__events"].asArray().size(), size_t(1));
  CHECK_EQ(h.run("__jsui_native.updateProps(v, { testID: 'c' }, true, true)"), 0);
  CHECK_EQ(v->props()["style"]["display"].asString(), std::string("none"));
  CHECK_EQ(v->props()["style"]["width"].asNumber(), 30.0);
  CHECK(!v->props().has("__events"));
}

TEST(cmdbuf_scroll_to_end) {
  Host h;
  // scrollToEnd measures the content container natively, in the same task
  // that sized it.
  CHECK_EQ(h.run(R"JS(
const n = __jsui_native;
const sv = n.createNode('ScrollView');
n.updateProps(sv, { style: { width: 50, height: 40, overflow: 'scroll' } }, false);
const content = n.createNode('View');
n.updateProps(content, { style: { width: 50, height: 130 } }, false);
n.appendChild(sv, content);
n.appendChild(n.rootId, sv);
n.scrollToEnd(sv, content, false, false);
globalThis.sv = sv;
)JS"),
           0);
  jsui::Node* sv = h.internal->findNode(h.root()->id() + 1);
  CHECK(sv != nullptr);
  if (sv) CHECK_NEAR(sv->scrollY, 90.0, 0.01);
}

TEST(cmdbuf_event_handler_commands) {
  Host h;
  // Commands recorded by an event handler are applied when it returns to
  // the native code that dispatched it.
  CHECK_EQ(h.run(R"JS(
const n = __jsui_native;
globalThis.__jsui_dispatchEvent = (e) => {
  if (e.type !== 'resize') return 0;
  const v = n.createNode('View');
  n.appendChild(n.rootId, v);
  return 0;
};
)JS"),
           0);
  jsui_set_viewport(h.rt, 80, 80, 1);
  CHECK_EQ(h.root()->children().size(), size_t(1));
  jsui_set_viewport(h.rt, 90, 80, 1);
  CHECK_EQ(h.root()->children().size(), size_t(2));
}

TEST(cmdbuf_native_animated) {
  Host h;
  // Graph commands share the order of the tree's: a view created in the
  // same task gets the animated style; a bad config still throws at once.
  CHECK_EQ(h.run(R"JS(
const n = __jsui_native, a = n.animated;
const v = n.createNode('View');
n.updateProps(v, { style: { width: 10, height: 10 } }, false);
n.appendChild(n.rootId, v);
a.createNode(1, { type: 'value', value: 0.25, offset: 0 });
a.createNode(2, { type: 'style', style: { opacity: 1 } });
a.createNode(3, { type: 'props', props: { style: 2 } });
a.connect(1, 2);
a.connect(2, 3);
a.connectToView(3, v);
let threw = false;
try { a.createNode(4, { type: 'nope' }); } catch (e) { threw = true; }
a.setValue(1, 0.75);
n.postMessage('r', { threw, value: a.getValue(1) });
globalThis.v = v;
)JS"),
           0);
  CHECK_EQ(h.messages.size(), size_t(1));
  if (!h.messages.empty()) CHECK_EQ(h.messages[0], "r={\"threw\":true,\"value\":0.75}");
  jsui::Node* v = h.internal->findNode(h.root()->id() + 1);
  CHECK(v != nullptr);
  if (v) CHECK_NEAR(v->props()["style"]["opacity"].asNumber(), 0.75, 1e-9);
}
