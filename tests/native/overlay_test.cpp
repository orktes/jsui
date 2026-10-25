// Overlay layer (core/overlay.h): placement against the anchor, flips and
// shifts, escaping clips and z-order, hit testing and hover chains through
// the logical parent, inherited transforms / opacity, activity, arrows,
// cleanup, and incremental repaint matching a full one.
#include <cstdio>
#include <string>
#include <vector>

#include "core/document.h"
#include "core/overlay.h"
#include "core/value.h"
#include "input/input.h"
#include "render/painter.h"
#include "render_util.h"
#include "runtime.h"
#include "test.h"

using namespace jsui;

namespace {

Node* child(Node* n, size_t i) { return n->logicalChildren()[i]; }

void patchStyle(Node* n, const std::string& json) { n->patchStyle(Value::parseJson(json).asObject()); }

// The overlay's box in the viewport (transforms included).
Rect visual(rtest::Scene& s, Node* n) { return s.rt->input->visualRect(n); }

void frame(rtest::Scene& s) {
  s.rt->tick(0);
  s.doc().layout();
}

// A 40x20 anchor at (x, y) with an overlay child of 60x30.
std::string anchorWithOverlay(float x, float y, const std::string& overlay, const std::string& extra = "") {
  return R"J({"style":{"position":"absolute","left":)J" + std::to_string(x) + R"J(,"top":)J" + std::to_string(y) +
         R"J(,"width":40,"height":20,"backgroundColor":"#202020"},"children":[
           {"props":{"overlay":)J" + overlay + R"J(},"style":{"width":60,"height":30,"backgroundColor":"#ff0000")J" + extra +
         R"J(}}]})J";
}

}  // namespace

TEST(overlay_places_against_parent_anchor) {
  rtest::Scene s(400, 300);
  Node* a = s.add(anchorWithOverlay(100, 100, R"({"placement":"bottom","offset":4})"));
  Node* o = child(a, 0);
  frame(s);
  CHECK(o->overlay != nullptr);
  CHECK_EQ(o->parent(), a);
  CHECK(o->renderParent() != a);  // rendered by its host in the layer
  CHECK(std::find(a->children().begin(), a->children().end(), o) == a->children().end());
  CHECK(s.doc().overlayLayer() != nullptr);
  CHECK_EQ(s.doc().root()->children().back(), s.doc().overlayLayer());
  Rect r = visual(s, o);
  CHECK_NEAR(r.x, 100 + 20 - 30, 0.01);  // centred under the anchor
  CHECK_NEAR(r.y, 124, 0.01);
  CHECK_NEAR(r.width, 60, 0.01);
  CHECK_NEAR(r.height, 30, 0.01);
  // absoluteRect (measure's pageX / pageY) agrees.
  Rect ab = o->absoluteRect();
  CHECK_NEAR(ab.x, 90, 0.01);
  CHECK_NEAR(ab.y, 124, 0.01);

  // Sides and alignments.
  struct Case {
    const char* placement;
    float x, y;
  } cases[] = {{"top", 90, 100 - 4 - 30}, {"top-start", 100, 66}, {"bottom-end", 80, 124},
               {"left", 100 - 4 - 60, 95}, {"right", 144, 95}, {"right-start", 144, 100}, {"center", 90, 99}};
  for (const Case& c : cases) {
    o->setProps(Value::parseJson(std::string(R"({"overlay":{"offset":4,"placement":")") + c.placement +
                                 R"("},"style":{"width":60,"height":30,"backgroundColor":"#ff0000"}})"));
    frame(s);
    Rect rr = visual(s, o);
    if (std::fabs(rr.x - c.x) > 0.01 || std::fabs(rr.y - c.y) > 0.01)
      std::fprintf(stderr, "  %s: %g,%g (want %g,%g)\n", c.placement, rr.x, rr.y, c.x, c.y);
    CHECK_NEAR(rr.x, c.x, 0.01);
    CHECK_NEAR(rr.y, c.y, 0.01);
  }
}

TEST(overlay_flips_and_shifts_into_the_viewport) {
  rtest::Scene s(400, 300);
  // Near the bottom-right corner: bottom does not fit (flips to top), the
  // centred box would overflow the right edge (shifts left).
  Node* a = s.add(anchorWithOverlay(350, 270, R"({"placement":"bottom","collisionPadding":5})"));
  Node* o = child(a, 0);
  frame(s);
  Rect r = visual(s, o);
  CHECK_NEAR(r.y, 270 - 30, 0.01);
  CHECK_NEAR(r.x, 400 - 5 - 60, 0.01);
  CHECK(o->overlay->placed == OverlaySide::Top);
  // Without flip / shift it stays where asked.
  o->setProps(Value::parseJson(
      R"({"overlay":{"placement":"bottom","flip":false,"collision":"none"},"style":{"width":60,"height":30}})"));
  frame(s);
  r = visual(s, o);
  CHECK_NEAR(r.y, 290, 0.01);
  CHECK_NEAR(r.x, 340, 0.01);
  // auto: the first side with room (bottom, top, right, left).
  o->setProps(Value::parseJson(R"({"overlay":{"placement":"auto"},"style":{"width":60,"height":30}})"));
  frame(s);
  CHECK(o->overlay->placed == OverlaySide::Top);
}

TEST(overlay_escapes_clips_and_zorder) {
  rtest::Scene s(200, 200);
  // A clipping 60x40 panel with the anchor inside; a later sibling with a
  // high zIndex covers where the overlay goes.
  s.add(R"J({"style":{"position":"absolute","left":0,"top":0,"width":200,"height":200,"backgroundColor":"#ffffff"},"children":[
    {"style":{"position":"absolute","left":20,"top":20,"width":60,"height":40,"overflow":"hidden","backgroundColor":"#0000ff"},"children":[
      {"style":{"position":"absolute","left":10,"top":10,"width":40,"height":20},"children":[
        {"props":{"overlay":{"placement":"bottom","offset":0}},"style":{"width":60,"height":40,"backgroundColor":"#ff0000"}}]}]},
    {"style":{"position":"absolute","left":0,"top":100,"width":200,"height":100,"zIndex":50,"backgroundColor":"#00ff00"}}]})J");
  s.render();
  s.save("overlay_escapes");
  // The overlay spans x 30..90, y 50..90: outside the panel (clip) at y 70,
  // and over the green zIndex-50 sibling at y 95 is still green (below it).
  CHECK_EQ(s.r(60, 70), 255);
  CHECK_EQ(s.g(60, 70), 0);
  CHECK_EQ(s.r(60, 55), 255);  // inside the panel too
  CHECK_EQ(s.g(60, 120), 255);
  // Moved under the green box: still on top of it.
  Node* panel = child(child(s.doc().root(), 0), 0);
  patchStyle(panel, R"({"top":70})");
  s.render();
  CHECK_EQ(s.r(60, 110), 255);
  CHECK_EQ(s.g(60, 110), 0);
}

TEST(overlay_hit_testing_and_hover_chain_follow_the_logical_parent) {
  rtest::Scene s(300, 300);
  // A group (hover listener) with an overlay below it; a sibling box under
  // the overlay's area.
  s.add(R"J({"style":{"position":"absolute","left":0,"top":0,"width":300,"height":300},"children":[
    {"props":{"group":true,"__events":["hoverIn","hoverOut"]},"style":{"position":"absolute","left":100,"top":100,"width":40,"height":40,"overflow":"hidden"},"children":[
      {"props":{"overlay":{"placement":"bottom"}},"style":{"width":80,"height":50},"children":[
        {"style":{"width":80,"height":50}}]}]},
    {"style":{"position":"absolute","left":0,"top":140,"width":300,"height":100,"zIndex":3}}]})J");
  frame(s);
  Node* top = child(s.doc().root(), 0);
  Node* group = child(top, 0);
  Node* o = child(group, 0);
  Node* inner = child(o, 0);
  Node* under = child(top, 1);
  InputManager& in = *s.rt->input;
  CHECK_EQ(in.hitTest(120, 160), inner);  // the overlay is on top
  CHECK_EQ(in.hitTest(10, 160), under);
  in.mouseMove(120, 120);  // on the anchor
  frame(s);
  CHECK(in.stateHovered(group));
  in.mouseMove(120, 170);  // onto the popout: still inside the group
  frame(s);
  CHECK(in.stateHovered(group));
  CHECK(in.stateHovered(o));
  CHECK(!in.stateHovered(under));
  in.mouseMove(10, 170);
  frame(s);
  CHECK(!in.stateHovered(group));
  CHECK(in.stateHovered(under));
  // pointerEvents none on the overlay: hits go through it.
  patchStyle(o, R"({"pointerEvents":"none"})");
  patchStyle(inner, R"({"pointerEvents":"none"})");
  frame(s);
  CHECK_EQ(in.hitTest(120, 160), under);
}

TEST(overlay_follows_scrolling_and_hides_when_clipped) {
  rtest::Scene s(300, 300);
  s.add(R"J({"type":"ScrollView","style":{"position":"absolute","left":0,"top":0,"width":300,"height":100},"children":[
    {"style":{"height":40}},
    {"style":{"height":20,"width":40},"children":[
      {"props":{"overlay":{"placement":"bottom"}},"style":{"width":40,"height":40}}]},
    {"style":{"height":400}}]})J");
  frame(s);
  Node* sv = child(s.doc().root(), 0);
  Node* o = child(child(sv, 1), 0);
  CHECK_NEAR(visual(s, o).y, 60, 0.01);
  // Scrolled natively: no relayout, the overlay follows.
  sv->scrollY = 30;
  sv->markBoundsDirty();
  frame(s);
  CHECK_NEAR(visual(s, o).y, 30, 0.01);
  CHECK(!o->overlay->hidden);
  // Anchor scrolled out of the ScrollView: hidden, not hit.
  sv->scrollY = 70;
  sv->markBoundsDirty();
  frame(s);
  CHECK(o->overlay->hidden);
  CHECK(o->overlay->host->style().opacity == 0);
  CHECK(s.rt->input->hitTest(20, 5) != o);
  // hideWhenClipped: false keeps it.
  o->setProps(Value::parseJson(R"({"overlay":{"placement":"bottom","hideWhenClipped":false},"style":{"width":40,"height":40}})"));
  frame(s);
  CHECK(!o->overlay->hidden);
  CHECK_NEAR(visual(s, o).y, -10, 0.01);
}

TEST(overlay_inherits_transform_and_opacity_of_its_parent) {
  rtest::Scene s(400, 400);
  // A stage scaled by 0.5 about its top-left corner, half transparent.
  s.add(R"J({"style":{"position":"absolute","left":0,"top":0,"width":400,"height":400,"opacity":0.5,"transformOrigin":"left top","transform":[{"scale":0.5}]},"children":[
    {"style":{"position":"absolute","left":100,"top":100,"width":40,"height":20},"children":[
      {"props":{"overlay":{"placement":"bottom","offset":10}},"style":{"width":60,"height":30}}]}]})J");
  frame(s);
  Node* stage = child(s.doc().root(), 0);
  Node* o = child(child(stage, 0), 0);
  Rect r = visual(s, o);
  CHECK_NEAR(r.x, (120 - 30) * 0.5, 0.01);
  CHECK_NEAR(r.y, (120 + 10) * 0.5, 0.01);
  CHECK_NEAR(r.width, 30, 0.01);
  CHECK_NEAR(r.height, 15, 0.01);
  CHECK_NEAR(o->overlay->host->style().opacity, 0.5, 1e-4);
  // An animated transform of the parent moves it without relayout.
  patchStyle(stage, R"({"transform":[{"scale":0.5},{"translateX":100}]})");
  frame(s);
  CHECK_NEAR(visual(s, o).x, (120 - 30 + 100) * 0.5, 0.01);
  patchStyle(stage, R"({"opacity":1,"transform":null})");
  frame(s);
  CHECK_NEAR(visual(s, o).width, 60, 0.01);
  CHECK(o->overlay->host->style().transform.empty());
  CHECK_NEAR(o->overlay->host->style().opacity, 1, 1e-4);
}

TEST(overlay_is_inactive_while_an_ancestor_is_hidden_or_detached) {
  rtest::Scene s(300, 300);
  Node* a = s.add(anchorWithOverlay(10, 10, R"({"placement":"bottom"})"));
  Node* o = child(a, 0);
  frame(s);
  CHECK(o->overlay->active);
  patchStyle(a, R"({"display":"none"})");
  frame(s);
  CHECK(!o->overlay->active);
  CHECK(!o->overlay->host->style().display);
  CHECK(s.rt->input->hitTest(40, 40) != o);
  patchStyle(a, R"({"display":"flex"})");
  frame(s);
  CHECK(o->overlay->active);
  // Detached with its anchor: inactive; destroyed: its host goes too.
  s.doc().root()->removeChild(a);
  frame(s);
  CHECK(!o->overlay->active);
  Node* host = o->overlay->host;
  uint32_t hostId = host->id();
  CHECK(s.doc().get(hostId) == host);
  s.doc().destroyNode(a);
  frame(s);
  CHECK(s.doc().get(hostId) == nullptr);
  CHECK(s.doc().overlays().empty());
  CHECK(s.doc().overlayLayer()->children().empty());
  s.render();
}

TEST(overlay_prop_toggles_and_tree_order) {
  rtest::Scene s(300, 300);
  Node* a = s.add(anchorWithOverlay(10, 10, R"({"placement":"bottom"})"));
  Node* b = s.add(anchorWithOverlay(100, 10, R"({"placement":"bottom"})"));
  frame(s);
  Node* oa = child(a, 0);
  Node* ob = child(b, 0);
  Node* layer = s.doc().overlayLayer();
  CHECK_EQ(layer->children().size(), size_t(2));
  CHECK_EQ(layer->children()[0], oa->overlay->host);
  // Reordered: the layer follows tree order.
  s.doc().root()->insertBefore(b, a);
  frame(s);
  CHECK_EQ(layer->children()[0], ob->overlay->host);
  // No longer an overlay: back in its parent's render list and Yoga.
  oa->setProps(Value::parseJson(R"({"style":{"width":60,"height":30}})"));
  frame(s);
  CHECK(oa->overlay == nullptr);
  CHECK_EQ(oa->renderParent(), a);
  CHECK_EQ(a->children().size(), size_t(1));
  CHECK_EQ(layer->children().size(), size_t(1));
  CHECK_NEAR(oa->absoluteRect().x, 10, 0.01);
  CHECK_NEAR(oa->absoluteRect().y, 10, 0.01);
  // And an overlay again.
  oa->setProps(Value::parseJson(R"({"overlay":{"placement":"top"},"style":{"width":60,"height":30}})"));
  frame(s);
  CHECK_EQ(layer->children().size(), size_t(2));
  CHECK(a->children().empty());
  // A sibling inserted before the overlay keeps the logical order.
  Node* sib = s.doc().createNode(NodeType::View);
  a->insertBefore(sib, oa);
  CHECK_EQ(a->logicalChildren()[0], sib);
  CHECK_EQ(a->children().size(), size_t(1));
  CHECK_EQ(a->children()[0], sib);
}

TEST(overlay_arrow_and_match_width) {
  rtest::Scene s(400, 300);
  // A 100-wide anchor at the right edge: the 160-wide overlay shifts left;
  // the arrow stays centred on the anchor.
  s.add(R"J({"style":{"position":"absolute","left":290,"top":100,"width":100,"height":20},"children":[
    {"props":{"overlay":{"placement":"top","offset":6}},"style":{"width":160,"height":40},"children":[
      {"props":{"overlayArrow":true},"style":{"position":"absolute","left":0,"top":0,"width":12,"height":12}}]}]})J");
  frame(s);
  Node* a = child(s.doc().root(), 0);
  Node* o = child(a, 0);
  Node* arrow = child(o, 0);
  Rect r = visual(s, o);
  CHECK_NEAR(r.x, 240, 0.01);
  CHECK_NEAR(r.y, 100 - 6 - 40, 0.01);
  Rect ar = visual(s, arrow);
  CHECK_NEAR(ar.x + 6, 340, 0.01);  // under the anchor's centre
  CHECK_NEAR(ar.y + 6, r.y + 40, 0.01);  // on the bottom edge
  // Flipped below: the arrow moves to the top edge.
  patchStyle(a, R"({"top":20})");
  frame(s);
  r = visual(s, o);
  CHECK(o->overlay->placed == OverlaySide::Bottom);
  CHECK_NEAR(visual(s, arrow).y + 6, r.y, 0.01);
  // matchWidth: the overlay takes the anchor's width.
  o->setProps(Value::parseJson(R"({"overlay":{"placement":"bottom","matchWidth":true},"style":{"height":40}})"));
  frame(s);
  CHECK_NEAR(visual(s, o).width, 100, 0.01);
  CHECK_NEAR(visual(s, o).x, 290, 0.01);
  patchStyle(a, R"({"width":80})");
  frame(s);
  CHECK_NEAR(visual(s, o).width, 80, 0.01);
}

TEST(overlay_ref_anchor_and_boundary) {
  rtest::Scene s(400, 300);
  s.add(R"J({"style":{"position":"absolute","left":0,"top":0,"width":400,"height":300},"children":[
    {"style":{"position":"absolute","left":50,"top":50,"width":200,"height":100}},
    {"style":{"position":"absolute","left":300,"top":200,"width":20,"height":20}},
    {"style":{"width":10,"height":10},"children":[{"style":{"width":50,"height":30}}]}]})J");
  Node* top = child(s.doc().root(), 0);
  Node* boundary = child(top, 0);
  Node* anchor = child(top, 1);
  Node* o = child(child(top, 2), 0);
  o->setProps(Value::parseJson(R"({"overlay":{"placement":"right","anchor":)" + std::to_string(anchor->id()) +
                               R"(},"style":{"width":50,"height":30}})"));
  frame(s);
  Rect r = visual(s, o);
  CHECK_NEAR(r.x, 320, 0.01);
  CHECK_NEAR(r.y, 195, 0.01);
  // Inside a boundary node: shifted into it.
  o->setProps(Value::parseJson(R"({"overlay":{"placement":"top","anchor":)" + std::to_string(anchor->id()) +
                               R"(,"boundary":)" + std::to_string(boundary->id()) + R"(},"style":{"width":50,"height":30}})"));
  frame(s);
  r = visual(s, o);
  CHECK_NEAR(r.x, 200, 0.01);  // right edge at the boundary's (250)
  CHECK_NEAR(r.y, 170, 0.01);
  // A ref not attached yet (-1): hidden.
  o->setProps(Value::parseJson(R"({"overlay":{"anchor":-1},"style":{"width":50,"height":30}})"));
  frame(s);
  CHECK(o->overlay->hidden);
}

TEST(overlay_raises_the_layer_over_root_zindex) {
  rtest::Scene s(300, 300);
  Node* a = s.add(anchorWithOverlay(10, 10, R"({"placement":"bottom"})"));
  (void)a;
  s.add(R"({"style":{"position":"absolute","left":0,"top":0,"width":300,"height":300,"zIndex":10000}})");
  frame(s);
  CHECK_EQ(s.doc().overlayLayer()->style().zIndex, 10001);
  CHECK_EQ(s.rt->input->hitTest(40, 40), child(a, 0));
}

TEST(overlay_incremental_repaint_matches_full) {
  // The same scene painted incrementally (damage only) and in full while
  // the anchor scrolls, moves and the overlay content changes.
  const char* tree = R"J({"style":{"position":"absolute","left":0,"top":0,"width":240,"height":200,"backgroundColor":"#101820"},"children":[
    {"type":"ScrollView","style":{"position":"absolute","left":20,"top":20,"width":120,"height":120,"backgroundColor":"#203040"},"children":[
      {"style":{"height":50}},
      {"style":{"height":24,"width":60,"marginLeft":20,"backgroundColor":"#4080ff"},"children":[
        {"props":{"overlay":{"placement":"right","offset":4}},"style":{"width":70,"height":40,"borderRadius":6,"backgroundColor":"#f0c040"},"children":[
          {"style":{"margin":8,"height":10,"backgroundColor":"#802020"}}]}]},
      {"style":{"height":300}}]}]})J";
  rtest::Scene a(240, 200), b(240, 200);
  a.add(tree);
  b.add(tree);
  std::vector<uint32_t> inc(size_t(240) * 200, 0);
  auto check = [&](int step) {
    a.rt->tick(0);
    a.rt->renderIncremental(inc.data(), 240, 200, 240 * 4);
    b.render();
    int diff = 0;
    for (size_t i = 0; i < inc.size(); i++) diff += inc[i] != b.px[i];
    if (diff) std::fprintf(stderr, "  step %d: %d pixels differ\n", step, diff);
    CHECK_EQ(diff, 0);
  };
  check(0);
  auto both = [&](auto&& fn) {
    fn(a);
    fn(b);
  };
  auto sv = [](rtest::Scene& s) { return child(child(s.doc().root(), 0), 0); };
  for (int i = 1; i <= 6; i++) {
    both([&](rtest::Scene& s) {
      Node* v = sv(s);
      v->scrollY = float(i * 7);
      v->markBoundsDirty();
    });
    check(i);
  }
  both([&](rtest::Scene& s) { patchStyle(sv(s), R"({"left":60})"); });
  check(7);
  both([&](rtest::Scene& s) { patchStyle(child(child(child(sv(s), 1), 0), 0), R"({"backgroundColor":"#20a020"})"); });
  check(8);
  both([&](rtest::Scene& s) { patchStyle(child(sv(s), 1), R"({"display":"none"})"); });
  check(9);
  both([&](rtest::Scene& s) { patchStyle(child(sv(s), 1), R"({"display":"flex"})"); });
  check(10);
}

TEST(overlay_incremental_repaint_of_fading_content) {
  // A hover popout: content-sized overlay above its anchor whose content
  // fades and scales out, then leaves layout (<Visibility>), and back.
  const char* tree = R"J({"style":{"position":"absolute","left":0,"top":0,"width":240,"height":200,"backgroundColor":"#101820"},"children":[
    {"style":{"position":"absolute","left":40,"top":120,"width":60,"height":40,"overflow":"hidden","backgroundColor":"#4080ff"},"children":[
      {"props":{"overlay":{"placement":"top","offset":8}},"style":{"pointerEvents":"none"},"children":[
        {"style":{"width":120,"paddingVertical":10},"children":[
          {"props":{"overlayArrow":true},"style":{"position":"absolute","width":10,"height":10,"borderWidth":1,"borderColor":"#ffffff","backgroundColor":"#302010","transform":[{"rotate":"45deg"}]}},
          {"style":{"height":40,"borderRadius":8,"borderWidth":1,"borderColor":"#f0c040","backgroundColor":"#302010","boxShadow":"0 8px 16px rgba(0,0,0,0.7)"}}]}]}]}]})J";
  rtest::Scene a(240, 200), b(240, 200);
  a.add(tree);
  b.add(tree);
  std::vector<uint32_t> inc(size_t(240) * 200, 0);
  auto check = [&](int step) {
    a.rt->tick(0);
    a.rt->renderIncremental(inc.data(), 240, 200, 240 * 4);
    b.render();
    int diff = 0;
    for (size_t i = 0; i < inc.size(); i++) diff += inc[i] != b.px[i];
    if (diff) std::fprintf(stderr, "  step %d: %d pixels differ\n", step, diff);
    CHECK_EQ(diff, 0);
  };
  check(0);
  auto content = [](rtest::Scene& s) { return child(child(child(child(s.doc().root(), 0), 0), 0), 0); };
  const char* steps[] = {R"({"opacity":0.8,"transform":[{"scale":0.98}]})", R"({"opacity":0.4,"transform":[{"scale":0.95}]})",
                         R"({"opacity":0.1,"transform":[{"scale":0.92}]})", R"({"opacity":0,"display":"none"})",
                         R"({"opacity":0.3,"display":"flex","transform":[{"scale":0.94}]})",
                         R"({"opacity":1,"transform":null})", R"({"display":"none"})"};
  int i = 1;
  for (const char* st : steps) {
    patchStyle(content(a), st);
    patchStyle(content(b), st);
    check(i++);
  }
}
