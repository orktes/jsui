// HUD benchmark: a 1080p game HUD (~500 nodes, ~60 text runs) laid out and
// painted into the same RGBA buffer every frame, like a game host calling
// jsui_render_rgba. Prints ms per frame for layout and paint, steady state
// and with one animated transform. JSUI_BENCH=1 runs more frames (the default
// run is short so the test suite stays fast).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "render_util.h"
#include "test.h"

using namespace jsui;
using rtest::Scene;

namespace {

std::string num(double v) {
  char b[32];
  std::snprintf(b, sizeof b, "%g", v);
  return b;
}

// Styles are written with single quotes (no escaping noise) and converted.
std::string q(std::string s) {
  std::replace(s.begin(), s.end(), '\'', '"');
  return s;
}
std::string text(const std::string& t, const std::string& style) {
  return "{\"type\":\"Text\",\"style\":{" + q(style) + "},\"text\":\"" + t + "\"}";
}
std::string view(const std::string& style, const std::vector<std::string>& kids = {}) {
  std::string s = "{\"style\":{" + q(style) + "},\"children\":[";
  for (size_t i = 0; i < kids.size(); i++) s += (i ? "," : "") + kids[i];
  return s + "]}";
}
std::string viewId(const std::string& id, const std::string& style) {
  return "{\"props\":{\"nativeID\":\"" + id + "\"},\"style\":{" + q(style) + "}}";
}

const std::string kPanel =
    "'backgroundColor':'rgba(12,16,32,0.82)','borderRadius':12,'borderWidth':1,'borderColor':'rgba(255,255,255,0.12)',"
    "'boxShadow':'0 8px 24px rgba(0,0,0,0.5)'";
const std::string kLabel = "'color':'#cfd6ff','fontSize':13,'fontWeight':'600'";

std::string bar(double pct, const char* from, const char* to) {
  return view("'height':10,'borderRadius':5,'backgroundColor':'rgba(255,255,255,0.1)','overflow':'hidden'",
              {view("'width':'" + num(pct) + "%','height':'100%','borderRadius':5,'experimental_backgroundImage':'linear-gradient(90deg, " +
                    from + ", " + to + ")'")});
}

std::string hudJson() {
  std::vector<std::string> root;
  // Player frame: portrait, name, level, 3 bars with labels.
  root.push_back(view("'position':'absolute','left':32,'top':32,'width':380,'padding':16,'flexDirection':'row','gap':14," + kPanel,
                      {view("'width':72,'height':72,'borderRadius':36,'borderWidth':3,'borderColor':'#f2c35b',"
                            "'experimental_backgroundImage':'radial-gradient(circle, #4a3a7a, #120c1e)'"),
                       view("'flex':1,'gap':6",
                            {view("'flexDirection':'row','justifyContent':'space-between'",
                                  {text("Seren Ashvale", "'color':'#fff','fontSize':18,'fontWeight':'700'"), text("LV 42", kLabel)}),
                             bar(72, "#ff4d6a", "#ff9a6a"), text("7,240 / 10,000", "'color':'#aab','fontSize':11"),
                             bar(45, "#4fd1ff", "#4f7aff"), text("2,250 / 5,000", "'color':'#aab','fontSize':11"),
                             bar(88, "#f2c35b", "#ffe29a")})}));
  // Party frames: 4 members.
  std::vector<std::string> party;
  const char* names[] = {"Kael", "Mira", "Thorne", "Ysolde"};
  for (int i = 0; i < 4; i++)
    party.push_back(view("'flexDirection':'row','gap':10,'padding':8," + kPanel,
                         {view("'width':40,'height':40,'borderRadius':8,'backgroundColor':'#2a3150'"),
                          view("'flex':1,'gap':4", {text(names[i], "'color':'#fff','fontSize':13,'fontWeight':'600'"),
                                                     bar(30 + i * 15, "#4ade80", "#22c55e"), bar(60 - i * 10, "#4fd1ff", "#4f7aff")})}));
  root.push_back(view("'position':'absolute','left':32,'top':200,'width':240,'gap':8", party));
  // Minimap with markers and a compass needle (the animated element).
  std::vector<std::string> mm;
  for (int i = 0; i < 24; i++)
    mm.push_back(view("'position':'absolute','left':" + num(20 + (i * 37) % 180) + ",'top':" + num(20 + (i * 53) % 180) +
                      ",'width':8,'height':8,'borderRadius':4,'backgroundColor':'" + (i % 3 ? "#f2c35b" : "#ff4d6a") + "'"));
  mm.push_back(viewId("needle", "'position':'absolute','left':108,'top':40,'width':4,'height':70,'borderRadius':2,"
                                "'backgroundColor':'#ff4d6a','transformOrigin':'50% 100%'"));
  root.push_back(view("'position':'absolute','right':32,'top':32,'width':220,'height':220,'borderRadius':110,'borderWidth':3,"
                      "'borderColor':'#f2c35b','overflow':'hidden','experimental_backgroundImage':'radial-gradient(circle, #23406a, #0b1424)',"
                      "'boxShadow':'0 0 24px rgba(79,209,255,0.4)'",
                      mm));
  root.push_back(text("Emberfall Outskirts", "'position':'absolute','right':32,'top':262,'width':220,'textAlign':'center','color':'#fff',"
                                             "'fontSize':15,'fontWeight':'700','textShadow':'0 1px 2px rgba(0,0,0,0.8)'"));
  // Quest tracker: 5 quests x 3 objectives.
  std::vector<std::string> quests;
  for (int qi = 0; qi < 5; qi++) {
    std::vector<std::string> rows{text("The Cinder Gate part " + num(qi + 1), "'color':'#f2c35b','fontSize':14,'fontWeight':'700'")};
    for (int o = 0; o < 3; o++)
      rows.push_back(view("'flexDirection':'row','alignItems':'center','gap':8",
                          {view(std::string("'width':12,'height':12,'borderRadius':3,'borderWidth':1.5,'borderColor':'#cfd6ff','backgroundColor':'") +
                                (o == 0 ? "#4ade80" : "transparent") + "'"),
                           text("Find the warden " + num(o + 1) + " / 3", "'color':'#dde','fontSize':12")}));
    quests.push_back(view("'gap':4", rows));
  }
  root.push_back(view("'position':'absolute','right':32,'top':300,'width':300,'padding':14,'gap':12," + kPanel, quests));
  // Hotbar: 12 slots with icon, key, cooldown overlay and charge count.
  std::vector<std::string> slots;
  for (int i = 0; i < 12; i++)
    slots.push_back(view("'width':64,'height':64,'borderRadius':10,'borderWidth':2,'borderColor':'rgba(242,195,91,0.6)',"
                         "'experimental_backgroundImage':'linear-gradient(180deg, #2a3150, #11162a)','boxShadow':'0 4px 10px rgba(0,0,0,0.6)'",
                         {view("'position':'absolute','left':10,'top':10,'right':10,'bottom':10,'borderRadius':6,'backgroundColor':'#4a3a7a'"),
                          view("'position':'absolute','left':0,'right':0,'bottom':0,'height':" + num(i * 5) + ",'backgroundColor':'rgba(0,0,0,0.55)'"),
                          text(num(i + 1), "'position':'absolute','left':5,'top':3,'color':'#fff','fontSize':11,'fontWeight':'700'"),
                          view("'position':'absolute','right':4,'bottom':4,'paddingHorizontal':4,'borderRadius':4,'backgroundColor':'#000'",
                               {text("x" + num(i % 5 + 1), "'color':'#fff','fontSize':10")})}));
  root.push_back(view("'position':'absolute','left':0,'right':0,'bottom':40,'alignItems':'center'",
                      {view("'flexDirection':'row','gap':8,'padding':10," + kPanel, slots)}));
  // Buff row: 10 buffs with timer text.
  std::vector<std::string> buffs;
  for (int i = 0; i < 10; i++)
    buffs.push_back(view("'alignItems':'center','gap':2",
                         {view("'width':36,'height':36,'borderRadius':8,'borderWidth':1,'borderColor':'#4ade80',"
                               "'experimental_backgroundImage':'linear-gradient(135deg, #1f6f4a, #0c2a1c)'",
                               {view("'position':'absolute','left':8,'top':8,'width':18,'height':18,'borderRadius':9,'backgroundColor':'rgba(255,255,255,0.7)'")}),
                          text(num(10 + i * 7) + "s", "'color':'#fff','fontSize':10")}));
  root.push_back(view("'position':'absolute','left':440,'top':32,'flexDirection':'row','gap':6", buffs));
  // Chat: 10 lines.
  std::vector<std::string> chat;
  for (int i = 0; i < 10; i++)
    chat.push_back(text("[Guild] Player" + num(i) + ": meet at the bridge before the tide turns", "'color':'#dde','fontSize':12,'lineHeight':18"));
  root.push_back(view("'position':'absolute','left':32,'bottom':140,'width':420,'padding':10,'borderRadius':8,'backgroundColor':'rgba(0,0,0,0.45)'", chat));
  // Inventory grid (open bag): 8x6 slots, 3 nodes each.
  std::vector<std::string> cells;
  for (int i = 0; i < 48; i++)
    cells.push_back(view("'width':52,'height':52,'borderRadius':6,'borderWidth':1,'borderColor':'rgba(255,255,255,0.15)','backgroundColor':'#151b33'",
                         {view(std::string("'position':'absolute','left':8,'top':8,'width':34,'height':34,'borderRadius':4,'backgroundColor':'") +
                               (i % 4 ? "#2a3150" : "#6a4aa0") + "'"),
                          view("'position':'absolute','right':3,'bottom':3,'width':8,'height':8,'borderRadius':4,'backgroundColor':'#f2c35b'")}));
  root.push_back(view("'position':'absolute','right':360,'bottom':140,'width':492,'padding':12,'flexDirection':'row','flexWrap':'wrap','gap':8," + kPanel,
                      cells));
  return view("'width':1920,'height':1080", root);
}

int countNodes(Node* n, int* texts) {
  if (n->type() == NodeType::Text) (*texts)++;
  int c = 1;
  for (Node* k : n->children()) c += countNodes(k, texts);
  return c;
}

Node* findId(Node* n, const char* id) {
  if (n->props()["nativeID"].asString() == id) return n;
  for (Node* k : n->children())
    if (Node* f = findId(k, id)) return f;
  return nullptr;
}

struct Timing {
  double layout = 0, paint = 0, worstPaint = 0;
};

}  // namespace

TEST(bench_hud) {
  using Clock = std::chrono::steady_clock;
  bool full = std::getenv("JSUI_BENCH") != nullptr;
  const int frames = full ? 300 : 10;
  Scene s(1920, 1080);
  s.add(hudJson());
  int texts = 0;
  int nodes = countNodes(s.doc().root(), &texts);
  Node* needle = findId(s.doc().root(), "needle");
  CHECK(needle != nullptr);
  std::vector<uint32_t> px(size_t(1920) * 1080), ref(px.size());
  // Like Animated: the needle's transform changes every frame via setProps.
  auto animate = [&](double t) {
    if (!needle) return;
    Value::Object style = needle->props()["style"].asObject();
    Value::Object rot;
    rot["rotate"] = Value(num(t * 0.1) + "deg");
    style["transform"] = Value(Value::Array{Value(rot)});
    Value::Object props = needle->props().asObject();
    props["style"] = Value(style);
    needle->setProps(Value(props));
  };
  auto frame = [&](double t, Timing* tm, bool anim, bool incremental) {
    if (anim) animate(t);
    auto t0 = Clock::now();
    s.doc().layout();
    auto t1 = Clock::now();
    if (incremental) s.rt->renderIncremental(px.data(), 1920, 1080, 1920 * 4);
    else s.rt->render(px.data(), 1920, 1080, 1920 * 4);
    auto t2 = Clock::now();
    double p = std::chrono::duration<double, std::milli>(t2 - t1).count();
    tm->layout += std::chrono::duration<double, std::milli>(t1 - t0).count();
    tm->paint += p;
    tm->worstPaint = std::max(tm->worstPaint, p);
  };
  Timing cold, fullSteady, fullAnim, incSteady, incAnim;
  frame(0, &cold, false, false);  // shaping, first raster
  for (int i = 0; i < frames; i++) frame(i, &fullSteady, false, false);
  for (int i = 0; i < frames; i++) frame(i * 16.0, &fullAnim, true, false);
  for (int i = 0; i < frames; i++) frame(i, &incSteady, false, true);
  for (int i = 0; i < frames; i++) frame(1000 + i * 16.0, &incAnim, true, true);
  PaintStats ps = lastPaintStats(s.doc());
  auto ms = [&](double v) { return v / frames; };
  std::printf("  perf: HUD 1920x1080, %d nodes, %d text runs; cold frame: layout %.2f ms, paint %.2f ms\n", nodes, texts,
              cold.layout, cold.paint);
  std::printf("  perf: full repaint:  steady layout %.3f ms paint %.2f ms (worst %.2f); animated transform layout %.3f ms paint %.2f ms (worst %.2f)\n",
              ms(fullSteady.layout), ms(fullSteady.paint), fullSteady.worstPaint, ms(fullAnim.layout), ms(fullAnim.paint),
              fullAnim.worstPaint);
  std::printf("  perf: incremental:   steady layout %.3f ms paint %.3f ms (worst %.2f); animated transform layout %.3f ms paint %.3f ms (worst %.2f)\n",
              ms(incSteady.layout), ms(incSteady.paint), incSteady.worstPaint, ms(incAnim.layout), ms(incAnim.paint),
              incAnim.worstPaint);
  std::printf("  perf: last incremental frame: %d nodes painted, %d subtrees culled, damage %lld px\n", ps.nodesPainted,
              ps.nodesCulled, (long long)ps.damagedPixels);
  CHECK(nodes >= 450);
  CHECK(texts >= 55);
  // The incrementally updated buffer must equal a full repaint.
  s.rt->render(ref.data(), 1920, 1080, 1920 * 4);
  size_t diff = 0;
  for (size_t i = 0; i < px.size(); i++)
    if (px[i] != ref[i] && diff++ < 12)
      std::printf("    diff at %zu,%zu: %08x vs %08x\n", i % 1920, i / 1920, px[i], ref[i]);
  CHECK_EQ(diff, size_t(0));
  s.px = px;
  s.save("bench_hud");
}
