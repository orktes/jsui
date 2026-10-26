// Occlusion culling (painter.cpp, Occlusion): what a later opaque node
// covers is not painted. Every frame, full or damage-only into a retained
// buffer, must equal the same paint without culling
// (setOcclusionCulling(false)) to the bit.
#include <string>
#include <vector>

#include "render_util.h"
#include "test.h"

using namespace jsui;
using rtest::Scene;

namespace {

void patch(Node* n, const std::string& json) {
  std::string err;
  Value p = Value::parseJson(json, &err);
  Value::Object style = n->props()["style"].asObject();
  for (auto& [k, v] : p.asObject()) style[k] = v;
  Value::Object props = n->props().asObject();
  props["style"] = Value(style);
  n->setProps(Value(props));
}

// A HUD of rows (rounded, shadowed, with text), under whatever the test
// adds above it.
std::string hud() {
  std::string rows;
  for (int i = 0; i < 8; i++) {
    if (i) rows += ",";
    rows += R"J({"style":{"flexDirection":"row","alignItems":"center","height":24,"margin":4,"borderRadius":6,
      "backgroundColor":"#1d2442","boxShadow":"0 2px 6px rgba(0,0,0,0.5)"},"children":[
      {"style":{"width":12,"height":12,"marginHorizontal":6,"borderRadius":6,"backgroundColor":"#f2c35b"}},
      {"type":"Text","style":{"color":"#fff","fontSize":12},"text":"Row )J" + std::to_string(i) + R"J("}]})J";
  }
  return R"J({"style":{"position":"absolute","left":0,"top":0,"width":320,"height":240,"backgroundColor":"#0b1020",
    "experimental_backgroundImage":"linear-gradient(180deg, #0b1020, #1a2350)"},"children":[)J" +
         rows + "]}";
}

// Paints each frame four ways, from one updateDamage: into two retained
// buffers, only the damage, with and without culling, and fully with and
// without culling. With culling must equal without, to the bit, on every
// platform: the same draws under the same clip, less those whose pixels
// the occluder replaces. The retained buffers are never repainted fully,
// so damage lost under a cover that moved away would stay in `incOn`.
// (Damage-only frames against full ones are render_incremental_*'s
// subject; on macOS arm64 they differ by a level on edge pixels.)
struct Checker {
  Scene& s;
  std::vector<uint32_t> incOn, incOff, on, off;
  int step = 0;
  PaintStats lastInc;   // the incremental frame with culling
  PaintStats lastFull;  // the full repaint with culling
  explicit Checker(Scene& sc) : s(sc) {
    size_t n = size_t(s.dw()) * s.dh();
    incOn.assign(n, 0);
    incOff.assign(n, 0);
    on.assign(n, 0);
    off.assign(n, 0);
    // The retained buffers start from a full frame (the scene was rendered).
    paint(incOn, nullptr, true);
    paint(incOff, nullptr, false);
  }
  static size_t diff(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    size_t d = 0;
    for (size_t i = 0; i < a.size(); i++) d += a[i] != b[i];
    return d;
  }
  // As Surface's retained-buffer path: clip to the damage, clear, paint.
  PaintStats paint(std::vector<uint32_t>& px, const SkRegion* damage, bool cull) {
    const int w = s.dw(), h = s.dh();
    SkImageInfo info = SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    SkSurfaceProps props = surfaceProps();
    auto surface = SkSurfaces::WrapPixels(info, px.data(), size_t(w) * 4, &props);
    SkCanvas* c = surface->getCanvas();
    setOcclusionCulling(cull);
    c->save();
    if (damage) c->clipRegion(*damage);
    c->clear(SK_ColorTRANSPARENT);
    paintDocument(s.doc(), c, damage);
    c->restore();
    setOcclusionCulling(true);
    return lastPaintStats(s.doc());
  }
  void frame(const char* what) {
    s.doc().layout();
    SkRegion damage = updateDamage(s.doc(), s.dw(), s.dh());
    lastInc = PaintStats{};
    if (!damage.isEmpty()) {
      lastInc = paint(incOn, &damage, true);
      CHECK_EQ(paint(incOff, &damage, false).nodesOccluded, 0);
    }
    lastFull = paint(on, nullptr, true);
    CHECK_EQ(paint(off, nullptr, false).nodesOccluded, 0);
    size_t dFull = diff(on, off), dInc = diff(incOn, incOff);
    if (dFull || dInc) {
      std::fprintf(stderr, "  step %d (%s): %zu pixels differ in the full repaint, %zu in the retained buffer\n", step,
                   what, dFull, dInc);
      const int w = s.dw();
      for (size_t i = 0, shown = 0; i < on.size() && shown < 4; i++)
        if (on[i] != off[i] || incOn[i] != incOff[i]) {
          std::fprintf(stderr, "    (%d,%d) full %08x / %08x, retained %08x / %08x (with / without culling)\n",
                       int(i % w), int(i / w), on[i], off[i], incOn[i], incOff[i]);
          shown++;
        }
    }
    CHECK_EQ(dFull, size_t(0));
    CHECK_EQ(dInc, size_t(0));
    step++;
  }
};

}  // namespace

// A modal card over a live HUD behind a translucent scrim: the rows under
// the card are not painted, in full and in incremental frames, also while
// they change and while the card moves away or closes.
TEST(occlusion_modal_over_content) {
  Scene s(320, 240);
  Node* bg = s.add(hud());
  s.add(R"J({"style":{"position":"absolute","left":0,"top":0,"width":320,"height":240,"backgroundColor":"rgba(0,0,0,0.5)"}})J");
  Node* modal = s.add(R"J({"style":{"position":"absolute","left":8,"top":20,"width":240,"height":200,"borderRadius":16,
    "backgroundColor":"#26304f","boxShadow":"0 8px 24px rgba(0,0,0,0.7)","borderWidth":1,"borderColor":"rgba(255,255,255,0.2)"},
    "children":[{"type":"Text","style":{"color":"#fff","fontSize":16,"margin":16},"text":"Paused"}]})J");
  s.render();
  Checker c(s);
  c.frame("first frame");
  CHECK(c.lastFull.nodesOccluded > 0);
  int occludedPainted = c.lastFull.nodesPainted;
  setOcclusionCulling(false);
  s.render();
  int allPainted = lastPaintStats(s.doc()).nodesPainted;
  setOcclusionCulling(true);
  std::printf("  modal: %d of %d nodes painted, %d skipped as covered\n", occludedPainted, allPainted,
              c.lastFull.nodesOccluded);
  CHECK(occludedPainted < allPainted);
  // A row under the card changes: damaged, but covered.
  Node* row = bg->children()[2];
  patch(row->children()[0], R"J({"backgroundColor":"#ff0000"})J");
  c.frame("change under the card");
  CHECK(c.lastInc.nodesOccluded > 0);
  row->children()[1]->children()[0]->setText("Row 2 changed");
  c.frame("text under the card");
  // The card moves partly off the rows, then away: what it covered repaints.
  for (int x : {60, 120, 200, 260}) {
    patch(modal, R"J({"left":)J" + std::to_string(x) + "}");
    c.frame("card moves");
  }
  patch(modal, R"J({"display":"none"})J");
  c.frame("card closes");
  patch(row->children()[0], R"J({"backgroundColor":"#00ff00"})J");
  c.frame("change, uncovered");
  patch(modal, R"J({"display":"flex","left":8})J");
  c.frame("card opens");
  CHECK(c.lastFull.nodesOccluded > 0);
}

// A full-screen opaque menu: the HUD under it is skipped as one subtree.
TEST(occlusion_full_screen_menu) {
  for (float dpr : {1.f, 1.5f, 2.f}) {
    Scene s(320, 240, dpr);
    s.add(hud());
    Node* menu = s.add(R"J({"style":{"position":"absolute","left":0,"top":0,"width":320,"height":240,"backgroundColor":"#101426"},
      "children":[{"type":"Text","style":{"color":"#fff","fontSize":20,"margin":20},"text":"Menu"}]})J");
    s.render();
    Checker c(s);
    c.frame("menu");
    // Root (own drawing: none) and the menu with its text; the HUD culled.
    CHECK(c.lastFull.nodesOccluded >= 1);
    CHECK(c.lastFull.nodesPainted <= 4);
    patch(menu, R"J({"top":120})J");
    c.frame("menu slides down");
    CHECK(c.lastFull.nodesPainted > 4);
  }
}

// A rounded opaque card: what lies inside its corners' arcs is culled, what
// reaches into a corner (where the card does not paint) is not.
TEST(occlusion_rounded_card) {
  Scene s(320, 240);
  s.add(R"J({"style":{"position":"absolute","left":0,"top":0,"width":320,"height":240,"backgroundColor":"#203040"}})J");
  // Under the card: one well inside, one in its top-left corner.
  s.add(R"J({"style":{"position":"absolute","left":80,"top":80,"width":60,"height":60,"backgroundColor":"#ff4d6a"}})J");
  s.add(R"J({"style":{"position":"absolute","left":42,"top":42,"width":12,"height":12,"backgroundColor":"#4ade80"}})J");
  Node* card = s.add(R"J({"style":{"position":"absolute","left":40,"top":40,"width":240,"height":168,"borderRadius":24,
    "backgroundColor":"#f2c35b"}})J");
  s.render();
  Checker c(s);
  c.frame("card");
  CHECK_EQ(c.lastFull.nodesOccluded, 1);  // the inner box, not the one in the corner
  patch(card, R"J({"borderRadius":84})J");
  c.frame("rounder");
  patch(card, R"J({"borderRadius":0})J");
  c.frame("square");
  CHECK_EQ(c.lastFull.nodesOccluded, 2);  // square: the corner one too
}

// A translucent cover culls nothing; an opaque one inside a translucent
// parent, or with a blend mode, neither.
TEST(occlusion_translucent_cover_does_not_cull) {
  Scene s(320, 240);
  s.add(hud());
  Node* cover = s.add(R"J({"style":{"position":"absolute","left":0,"top":0,"width":320,"height":240,"backgroundColor":"rgba(16,20,38,0.9)"}})J");
  s.render();
  Checker c(s);
  c.frame("translucent colour");
  CHECK_EQ(c.lastFull.nodesOccluded, 0);
  patch(cover, R"J({"backgroundColor":"#101426","opacity":0.95})J");
  c.frame("opacity");
  CHECK_EQ(c.lastFull.nodesOccluded, 0);
  patch(cover, R"J({"opacity":1,"mixBlendMode":"multiply"})J");
  c.frame("blend mode");
  CHECK_EQ(c.lastFull.nodesOccluded, 0);
  patch(cover, R"J({"mixBlendMode":"normal"})J");
  c.frame("opaque");
  CHECK(c.lastFull.nodesOccluded > 0);
}

// A cover under a transform: scaled (still a rect: culls what it covers),
// rotated (no cull), and inside a blurred layer (its pixels spread: the
// layer's content is never culled by it, nor is anything by it).
TEST(occlusion_transformed_cover) {
  Scene s(320, 240);
  s.add(hud());
  Node* cover = s.add(R"J({"style":{"position":"absolute","left":40,"top":40,"width":240,"height":160,"backgroundColor":"#101426",
    "transform":[{"scale":1.25}]}})J");
  s.render();
  Checker c(s);
  c.frame("scaled");
  CHECK(c.lastFull.nodesOccluded > 0);
  for (int deg : {3, 10, 45, 90}) {
    patch(cover, R"J({"transform":[{"rotate":")J" + std::to_string(deg) + R"J(deg"}]})J");
    c.frame("rotated");
    // A quarter turn keeps it a rect.
    if (deg != 90) CHECK_EQ(c.lastFull.nodesOccluded, 0);
  }
  patch(cover, R"J({"transform":[{"translateX":0.4},{"scaleY":0.5}]})J");
  c.frame("fractional");
  patch(cover, R"J({"transform":[],"filter":"blur(6px)"})J");
  c.frame("blurred cover");
  CHECK_EQ(c.lastFull.nodesOccluded, 0);
}

// What a filter spreads (a blur, a drop shadow) around a covered node shows
// outside the cover: nothing inside the filtered layer is culled.
TEST(occlusion_filter_layers_keep_their_content) {
  Scene s(320, 240);
  s.add(R"J({"style":{"position":"absolute","left":40,"top":40,"width":200,"height":160,"filter":"blur(8px)"},"children":[
    {"style":{"position":"absolute","left":40,"top":40,"width":100,"height":80,"backgroundColor":"#ff4d6a"}}]})J");
  s.add(R"J({"style":{"position":"absolute","left":40,"top":40,"width":200,"height":160,"filter":"drop-shadow(0 0 12px #4ade80)"},
    "children":[{"style":{"position":"absolute","left":40,"top":40,"width":100,"height":80,"backgroundColor":"#4fd1ff"}}]})J");
  // Covers both inner boxes, not their layers' spread.
  Node* cover = s.add(R"J({"style":{"position":"absolute","left":75,"top":75,"width":110,"height":90,"backgroundColor":"#101426"}})J");
  s.render();
  Checker c(s);
  c.frame("over filtered layers");
  CHECK_EQ(c.lastFull.nodesOccluded, 0);
  // Covering the layers' whole bounds: culled as a whole.
  patch(cover, R"J({"left":0,"top":0,"width":320,"height":240})J");
  c.frame("over everything");
  CHECK(c.lastFull.nodesOccluded >= 2);
}

// Paint order, not document order: a node raised by zIndex covers what
// paints before it, and is not covered by what it paints over; an
// ancestor's rounded clip cuts what an occluder covers.
TEST(occlusion_paint_order_and_clips) {
  Scene s(320, 240);
  s.add(R"J({"style":{"position":"absolute","left":0,"top":0,"width":320,"height":240},"children":[
    {"style":{"position":"absolute","left":20,"top":20,"width":200,"height":160,"backgroundColor":"#101426","zIndex":2}},
    {"style":{"position":"absolute","left":40,"top":40,"width":60,"height":60,"backgroundColor":"#ff4d6a"}},
    {"style":{"position":"absolute","left":150,"top":40,"width":60,"height":60,"backgroundColor":"#4ade80","zIndex":3}}]})J");
  // A clip with rounded corners around an opaque child that fills it.
  s.add(R"J({"style":{"position":"absolute","left":230,"top":20,"width":80,"height":80,"borderRadius":30,"overflow":"hidden"},
    "children":[{"style":{"width":80,"height":80,"backgroundColor":"#f2c35b"}}]})J");
  s.add(R"J({"style":{"position":"absolute","left":226,"top":16,"width":10,"height":10,"backgroundColor":"#4fd1ff","zIndex":-1}})J");
  s.render();
  Checker c(s);
  c.frame("zIndex");
  CHECK(c.lastFull.nodesOccluded >= 1);  // the red box under the raised panel
}

// The off switch: nothing occluded, the same pixels (checked by every frame
// above).
TEST(occlusion_can_be_turned_off) {
  Scene s(320, 240);
  s.add(hud());
  s.add(R"J({"style":{"position":"absolute","left":0,"top":0,"width":320,"height":240,"backgroundColor":"#101426"}})J");
  s.render();
  CHECK(lastPaintStats(s.doc()).nodesOccluded > 0);
  setOcclusionCulling(false);
  s.render();
  CHECK_EQ(lastPaintStats(s.doc()).nodesOccluded, 0);
  setOcclusionCulling(true);
}

// The occluders come from the records (updateRecords keeps each subtree's
// largest): a paint with none that could cover anything it repaints does
// no per-node work at all (PaintStats::occluders 0).
TEST(occlusion_costs_nothing_without_occluders) {
  Scene s(320, 240);
  // Translucent backdrop and panels, an opaque box inside a fading panel.
  s.add(R"J({"style":{"position":"absolute","left":0,"top":0,"width":320,"height":240,"backgroundColor":"rgba(11,16,32,0.9)"},
    "children":[{"style":{"position":"absolute","left":20,"top":20,"width":200,"height":160,"opacity":0.9},"children":[
      {"style":{"width":180,"height":100,"backgroundColor":"#1d2442"}}]},
    {"style":{"position":"absolute","left":240,"top":20,"width":60,"height":60,"backgroundColor":"rgba(255,255,255,0.5)"}}]})J");
  Node* badge = s.add(R"J({"style":{"position":"absolute","left":250,"top":200,"width":40,"height":20,"backgroundColor":"rgba(242,195,91,0.5)"}})J");
  s.render();
  Checker c(s);
  c.frame("translucent UI");
  CHECK_EQ(c.lastFull.occluders, 0);
  CHECK_EQ(c.lastFull.nodesOccluded, 0);
  // An opaque badge: an occluder for full repaints, not for a damage away
  // from it.
  patch(badge, R"J({"left":240,"backgroundColor":"#f2c35b"})J");
  c.frame("badge moves");
  CHECK_EQ(c.lastFull.occluders, 1);
  patch(s.doc().root()->children()[0]->children()[1], R"J({"backgroundColor":"rgba(255,0,0,0.5)"})J");
  c.frame("change away from the badge");
  CHECK_EQ(c.lastInc.occluders, 0);
}

// An occluder inside a subtree skipped as covered (or culled) is behind
// what paints after that subtree: it must not cull it.
TEST(occlusion_skipped_occluders_are_behind) {
  Scene s(320, 240);
  // zIndex (paint order is not document order there) keeps the inner
  // occluder in the list though the cover contains it.
  Node* box = s.add(R"J({"style":{"position":"absolute","left":0,"top":0,"width":320,"height":240},"children":[
    {"style":{"position":"absolute","left":40,"top":40,"width":140,"height":140,"backgroundColor":"#101426","zIndex":1}},
    {"style":{"position":"absolute","left":80,"top":80,"width":20,"height":20,"backgroundColor":"#ff4d6a","zIndex":2}},
    {"style":{"position":"absolute","left":50,"top":50,"width":100,"height":100},"children":[
      {"style":{"position":"absolute","left":10,"top":10,"width":80,"height":80,"backgroundColor":"#4fd1ff"}}]}]})J");
  // Painted last, inside the inner occluder (painted first).
  Node* x = box->children()[1];
  s.render();
  Checker c(s);
  c.frame("over");
  CHECK(c.lastFull.nodesOccluded >= 1);  // the first subtree
  patch(x, R"J({"backgroundColor":"#ff0000"})J");
  c.frame("change on top");
}
