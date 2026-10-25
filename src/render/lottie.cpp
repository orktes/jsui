#include "render/lottie.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "core/document.h"
#include "core/style.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRect.h"
#include "platform/loader.h"
#include "runtime.h"

#if JSUI_LOTTIE
// The aseprite prebuilt Skia ships only modules/*/include, but Skottie.h
// pulls in SlotManager.h, which includes private Skottie headers. Nothing
// here uses slots: skip it (the Builder only holds an sk_sp to one).
#if !__has_include("modules/skottie/src/SkottieValue.h")
#define SlotManager_DEFINED
namespace skottie {
class SlotManager;
}
#endif
#include "modules/skottie/include/Skottie.h"
#include "modules/skottie/include/SkottieProperty.h"
#include "modules/skresources/include/SkResources.h"
// m126+: text layers shape through a factory given to the builder.
#if __has_include("modules/skshaper/utils/FactoryHelpers.h")
#include "modules/skshaper/utils/FactoryHelpers.h"
#define JSUI_SKOTTIE_SHAPING_FACTORY 1
#endif
#endif

namespace jsui {

// One source: its JSON and the compositions parsed from it.
struct LottieCache::Entry {
  State state = State::Loading;
  bool inline_ = false;  // addData (survives clear())
  std::string json;
  std::string error;
  std::map<std::string, std::unique_ptr<Composition>> variants;  // by colorFilters JSON
  // Of the plain composition (what views measure and count frames by).
  float width = 0, height = 0;
  double inPoint = 0, outPoint = 0, fps = 0;
};

#if JSUI_LOTTIE

struct LottieCache::Composition {
  sk_sp<skottie::Animation> anim;
  // colorFilters: color properties under a matching keypath, set again
  // after every seek (an animated color would overwrite the override).
  struct Override {
    std::unique_ptr<skottie::ColorPropertyHandle> handle;
    SkColor color;
  };
  std::vector<Override> overrides;
};

namespace {

class Logger final : public skottie::Logger {
 public:
  void log(Level level, const char message[], const char* = nullptr) override {
    if (level == Level::kError && first.empty()) first = message;
  }
  std::string first;
};

// colorFilters entry: keypath segments ("*" one name, "**" any number) and
// the color. A keypath matches a color property when its segments match
// names along the property's path (compositions, layers, effects, the
// property's own node) in order; names in between may be skipped, so
// "Layer" recolors everything in that layer and "Layer.Fill 1" its fills.
struct ColorFilter {
  std::vector<std::string> keypath;
  SkColor color;
};

bool matchPath(const std::vector<std::string>& key, size_t k, const std::vector<std::string>& path, size_t p) {
  if (k == key.size()) return true;
  if (key[k] == "**") {
    for (size_t i = p; i <= path.size(); i++)
      if (matchPath(key, k + 1, path, i)) return true;
    return false;
  }
  for (size_t i = p; i < path.size(); i++)
    if ((key[k] == "*" || key[k] == path[i]) && matchPath(key, k + 1, path, i + 1)) return true;
  return false;
}

class ColorObserver final : public skottie::PropertyObserver {
 public:
  explicit ColorObserver(std::vector<ColorFilter> filters) : filters_(std::move(filters)) {}
  void onEnterNode(const char name[], NodeType) override { path_.push_back(name ? name : ""); }
  void onLeavingNode(const char*, NodeType) override {
    if (!path_.empty()) path_.pop_back();
  }
  void onColorProperty(const char name[], const LazyHandle<skottie::ColorPropertyHandle>& lazy) override {
    std::vector<std::string> path = path_;
    path.push_back(name ? name : "");
    // The last matching filter wins (like later lottie-react-native entries).
    const ColorFilter* hit = nullptr;
    for (const ColorFilter& f : filters_)
      if (matchPath(f.keypath, 0, path, 0)) hit = &f;
    if (!hit) return;
    std::unique_ptr<skottie::ColorPropertyHandle> h = lazy();
    if (!h) return;
    h->set(hit->color);
    overrides.push_back({std::move(h), hit->color});
  }
  std::vector<LottieCache::Composition::Override> overrides;

 private:
  std::vector<ColorFilter> filters_;
  std::vector<std::string> path_;
};

std::vector<ColorFilter> parseFilters(const Value& v) {
  std::vector<ColorFilter> out;
  for (const Value& e : v.asArray()) {
    Color c;
    if (!e["keypath"].isString() || !parseColor(e["color"], &c)) continue;
    ColorFilter f;
    const std::string& kp = e["keypath"].asString();
    size_t start = 0;
    while (start <= kp.size()) {
      size_t dot = kp.find('.', start);
      if (dot == std::string::npos) dot = kp.size();
      if (dot > start) f.keypath.push_back(kp.substr(start, dot - start));
      start = dot + 1;
    }
    if (f.keypath.empty()) continue;
    auto u8 = [](float x) { return unsigned(std::lround(std::clamp(x, 0.f, 1.f) * 255)); };
    f.color = SkColorSetARGB(u8(c.a), u8(c.r), u8(c.g), u8(c.b));
    out.push_back(std::move(f));
  }
  return out;
}

}  // namespace

bool LottieCache::available() { return true; }

LottieCache::Composition* LottieCache::get(const std::string& url, const Value& filters) {
  Entry* e = find(url);
  if (!e || e->state != State::Ready) return nullptr;
  std::string key = filters.isArray() && !filters.asArray().empty() ? filters.toJson() : std::string();
  auto it = e->variants.find(key);
  if (it != e->variants.end()) return it->second.get();
  auto comp = std::make_unique<Composition>();
  auto logger = sk_make_sp<Logger>();
  skottie::Animation::Builder builder;
  builder.setLogger(logger);
  builder.setFontManager(rt_.fonts.assetFontMgr());
  // Images embedded as data: URIs (the usual for UI animations), decoded
  // once at build time.
  builder.setResourceProvider(
      skresources::DataURIResourceProviderProxy::Make(nullptr, skresources::ImageDecodeStrategy::kPreDecode));
#if JSUI_SKOTTIE_SHAPING_FACTORY && JSUI_SKUNICODE_REFCNT
  builder.setTextShapingFactory(sk_make_sp<SkShapers::HarfbuzzFactory>(rt_.fonts.unicodeRef()));
#endif
  sk_sp<ColorObserver> observer;
  if (!key.empty()) {
    observer = sk_make_sp<ColorObserver>(parseFilters(filters));
    builder.setPropertyObserver(observer);
  }
  comp->anim = builder.make(e->json.data(), e->json.size());
  if (observer) comp->overrides = std::move(observer->overrides);
  if (!comp->anim) {
    // Parses as JSON but not as a composition: fail the source.
    e->state = State::Failed;
    e->error = "invalid Lottie composition" + (logger->first.empty() ? std::string() : ": " + logger->first);
    rt_.log(JSUI_LOG_WARN, "LottieView: " + e->error + " (" + (e->inline_ ? std::string("inline source") : url) + ")");
    e->variants.clear();
    generation++;
    return nullptr;
  }
  if (key.empty()) {
    e->width = comp->anim->size().width();
    e->height = comp->anim->size().height();
    e->inPoint = comp->anim->inPoint();
    e->outPoint = comp->anim->outPoint();
    e->fps = comp->anim->fps();
  }
  Composition* raw = comp.get();
  e->variants.emplace(key, std::move(comp));
  return raw;
}

#else  // !JSUI_LOTTIE

struct LottieCache::Composition {};
bool LottieCache::available() { return false; }
LottieCache::Composition* LottieCache::get(const std::string&, const Value&) { return nullptr; }

#endif

LottieCache::LottieCache(Runtime& rt) : rt_(rt) {}
LottieCache::~LottieCache() = default;

LottieCache::Entry* LottieCache::find(const std::string& url) {
  auto it = entries_.find(url);
  if (it != entries_.end()) return it->second.get();
  if (url.rfind("lottie:", 0) != 0) return nullptr;
  // Inline data JS handed over (addData) becomes an entry on first use.
  std::string json;
  {
    std::lock_guard<std::mutex> lock(inlineMutex_);
    auto in = inline_.find(url);
    if (in == inline_.end() || !in->second) return nullptr;
    json = std::move(*in->second);
    in->second.reset();  // the key stays known; the entry has the JSON now
  }
  auto e = std::make_unique<Entry>();
  e->inline_ = true;
  e->json = std::move(json);
  e->state = available() ? State::Ready : State::Failed;
  if (!available()) e->error = "Lottie is not available in this build (JSUI_LOTTIE off)";
  return entries_.emplace(url, std::move(e)).first->second.get();
}

const LottieCache::Entry* LottieCache::find(const std::string& url) const {
  auto it = entries_.find(url);
  return it == entries_.end() ? nullptr : it->second.get();
}

std::string LottieCache::addData(std::string json) {
  // FNV-1a over the contents: the same JSON (a second view, a re-render,
  // a reload) gets the same key and composition.
  uint64_t h = 1469598103934665603ull;
  for (unsigned char ch : json) h = (h ^ ch) * 1099511628211ull;
  char buf[48];
  std::snprintf(buf, sizeof buf, "lottie:%016llx-%zx", static_cast<unsigned long long>(h), json.size());
  std::string key = buf;
  // Kept aside under a lock (JS may call this on its own thread); the owner
  // thread makes the entry when a view asks for the key (find).
  std::lock_guard<std::mutex> lock(inlineMutex_);
  if (!inline_.count(key)) inline_.emplace(key, std::make_unique<std::string>(std::move(json)));
  return key;
}

std::string LottieCache::resolve(const std::string& uri) const {
  if (uri.empty() || uri.rfind("data:", 0) == 0 || uri.rfind("lottie:", 0) == 0) return uri;
  return ResourceLoader::resolve(rt_.baseUrl, uri);
}

LottieCache::State LottieCache::request(const std::string& url) {
  if (url.empty()) return State::Failed;
  Entry* e = find(url);
  // A Loading entry with nothing in flight lost its request (a reload
  // cancels every load): ask again.
  bool retry = e && e->state == State::Loading && !rt_.loader.busy();
  if (e && !retry) return e->state;
  if (!e) {
    auto fresh = std::make_unique<Entry>();
    e = fresh.get();
    entries_.emplace(url, std::move(fresh));
  }
  if (!available()) {
    e->state = State::Failed;
    e->error = "Lottie is not available in this build (JSUI_LOTTIE off)";
    return e->state;
  }
  if (url.rfind("lottie:", 0) == 0) {  // an inline key of an older JS context
    e->state = State::Failed;
    e->error = "unknown inline Lottie source";
    return e->state;
  }
  if (url.rfind("data:", 0) == 0) {
    Resource res;
    if (ResourceLoader::decodeDataUri(url, &res)) {
      e->json = res.text();
      e->state = State::Ready;
    } else {
      e->state = State::Failed;
      e->error = "malformed data URI";
    }
    return e->state;
  }
  rt_.loader.load(url, [this, url](Resource res) {
    Entry* e = find(url);
    if (!e) return;
    if (res.ok()) {
      e->json = res.text();
      e->state = State::Ready;
    } else {
      e->state = State::Failed;
      e->error = "failed to load " + url + " (status " + std::to_string(res.status) +
                 (res.error.empty() ? "" : ", " + res.error) + ")";
      rt_.log(JSUI_LOG_WARN, "LottieView: " + e->error);
    }
    loaded(url);
  });
  return e->state;
}

std::string LottieCache::error(const std::string& url) const {
  const Entry* e = find(url);
  return e ? e->error : std::string();
}

bool LottieCache::size(const std::string& url, float* width, float* height) {
  if (!get(url, Value())) return false;
  const Entry* e = find(url);
  *width = e->width;
  *height = e->height;
  return true;
}

bool LottieCache::frames(const std::string& url, double* inPoint, double* outPoint, double* fps) {
  if (!get(url, Value())) return false;
  const Entry* e = find(url);
  *inPoint = e->inPoint;
  *outPoint = e->outPoint;
  *fps = e->fps;
  return true;
}

void LottieCache::loaded(const std::string& url) {
  generation++;
  // Views of the source get their intrinsic size (and their events) in the
  // next tick.
  for (Node* n : views_) {
    auto* st = static_cast<lottie::NodeState*>(n->imageCache.get());
    if (st && st->url == url && n->yoga() && YGNodeHasMeasureFunc(n->yoga())) YGNodeMarkDirty(n->yoga());
    if (st && st->url == url) n->markPaintDirty();
  }
  for (auto& s : rt_.surfaces()) s->doc.markLayoutDirty();
}

void LottieCache::clear() {
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (it->second->inline_) ++it;
    else it = entries_.erase(it);
  }
  generation++;
}

void LottieCache::addView(Node* node) { views_.insert(node); }
void LottieCache::removeView(Node* node) { views_.erase(node); }

namespace lottie {
namespace {

void play(NodeState* st, double from, double to) {
  st->from = from;
  st->to = to;
  // Backwards (negative speed) starts at the segment's end.
  st->pos = st->speed < 0 ? to : from;
  st->playing = true;
  st->anchored = false;
}

void finish(NodeState* st, bool cancelled) {
  if (!st->cache) return;
  LottieCache::Event ev;
  ev.node = st->node->id();
  ev.kind = LottieCache::Event::Kind::Finished;
  ev.cancelled = cancelled;
  st->cache->events.push_back(std::move(ev));
}

// Stops a running, non-looping segment early: reported cancelled.
void interrupt(NodeState* st) {
  if (st->playing && !st->loop) finish(st, true);
}

double frameToProgress(LottieCache* cache, const std::string& url, double frame) {
  double ip = 0, op = 0, fps = 0;
  if (!cache->frames(url, &ip, &op, &fps) || op <= ip) return 0;
  return std::clamp((frame - ip) / (op - ip), 0.0, 1.0);
}

bool runCommand(NodeState* st, const std::string& cmd, double a, double b) {
  if (cmd == "play") {
    interrupt(st);
    double from = 0, to = 1;
    if (a == a) from = frameToProgress(st->cache, st->url, a);
    if (b == b) to = frameToProgress(st->cache, st->url, b);
    else if (a == a) to = 1;
    play(st, from, to);
  } else if (cmd == "pause") {
    st->playing = false;
  } else if (cmd == "resume") {
    if (!st->playing) {
      double end = st->speed < 0 ? st->from : st->to;
      // A finished segment starts over.
      if (std::fabs(st->pos - end) < 1e-9) st->pos = st->speed < 0 ? st->to : st->from;
      st->playing = true;
      st->anchored = false;
    }
  } else if (cmd == "reset") {
    interrupt(st);
    st->playing = false;
    st->from = 0;
    st->to = 1;
    st->pos = 0;
  } else {
    return false;
  }
  st->node->markPaintDirty();
  return true;
}

// Advances a playing view to `now`. Returns true when its frame changed.
bool advance(NodeState* st, double now) {
  if (!st->playing) return false;
  double ip = 0, op = 0, fps = 0;
  if (!st->cache->frames(st->url, &ip, &op, &fps) || op <= ip || fps <= 0) {
    st->playing = false;  // nothing to play (the source failed)
    return false;
  }
  double duration = (op - ip) / fps;
  if (!st->anchored) {
    st->anchorTime = now;
    st->anchorPos = st->pos;
    st->anchored = true;
    return false;
  }
  double start = st->speed < 0 ? st->to : st->from, end = st->speed < 0 ? st->from : st->to;
  double len = std::fabs(end - start), dir = end >= start ? 1 : -1;
  if (len <= 0 || st->speed == 0) {
    if (len <= 0 && !st->loop) {
      st->playing = false;
      finish(st, false);
    }
    return false;
  }
  double phase = std::fabs(st->anchorPos - start) + (now - st->anchorTime) / 1000.0 * std::fabs(st->speed) / duration;
  double before = st->pos;
  if (phase >= len) {
    if (st->loop) {
      phase = std::fmod(phase, len);
    } else {
      phase = len;
      st->playing = false;
      finish(st, false);
    }
  }
  st->pos = start + dir * phase;
  return st->pos != before;
}

ResizeMode resizeModeOf(Node* node) {
  const ComputedStyle& s = node->style();
  if (s.hasResizeMode) return s.resizeMode;
  const std::string& m = node->props()["resizeMode"].asString();
  if (m == "cover") return ResizeMode::Cover;
  if (m == "center") return ResizeMode::Center;
  if (m == "stretch") return ResizeMode::Stretch;
  return ResizeMode::Contain;  // lottie-react-native's default
}

YGSize measureLottie(YGNodeConstRef y, float width, YGMeasureMode wm, float height, YGMeasureMode hm) {
  Node* n = static_cast<Node*>(YGNodeGetContext(y));
  NodeState* st = n ? ensure(n) : nullptr;
  float w = 0, h = 0;
  if (st && st->cache && !st->url.empty()) st->cache->size(st->url, &w, &h);
  if (!(w > 0 && h > 0)) return {wm == YGMeasureModeExactly ? width : 0, hm == YGMeasureModeExactly ? height : 0};
  // Like an image: the composition's size, keeping its aspect ratio when
  // only one dimension is constrained.
  float ar = w / h;
  if (wm == YGMeasureModeExactly && hm == YGMeasureModeExactly) return {width, height};
  if (wm == YGMeasureModeExactly) return {width, hm == YGMeasureModeAtMost ? std::min(height, width / ar) : width / ar};
  if (hm == YGMeasureModeExactly) return {wm == YGMeasureModeAtMost ? std::min(width, height * ar) : height * ar, height};
  if (wm == YGMeasureModeAtMost && w > width) {
    w = width;
    h = w / ar;
  }
  if (hm == YGMeasureModeAtMost && h > height) {
    h = height;
    w = h * ar;
  }
  return {w, h};
}

void readPlaybackProps(NodeState* st, const Value& props) {
  float speed = props["speed"].isNumber() ? float(props["speed"].asNumber()) : 1.f;
  if (!(speed == speed)) speed = 1;
  if (speed != st->speed) {
    // Re-anchor at the current frame so the playhead does not jump.
    st->speed = speed;
    st->anchored = false;
  }
  st->loop = props["loop"].isBool() ? props["loop"].asBool() : true;
  st->autoPlay = props["autoPlay"].asBool(false);
}

}  // namespace

NodeState::~NodeState() {
  if (cache) cache->removeView(node);
}

void installMeasure(Node* node) {
  if (node->type() == NodeType::Lottie && node->yoga()) YGNodeSetMeasureFunc(node->yoga(), measureLottie);
}

NodeState* ensure(Node* node) {
  if (node->imageCache) return static_cast<NodeState*>(node->imageCache.get());
  auto st = std::make_unique<NodeState>();
  st->node = node;
  Document* doc = node->document();
  st->cache = doc ? doc->lotties : nullptr;
  const Value& src = node->props()["source"];
  std::string uri = src.isString() ? src.asString() : src["uri"].asString();
  if (st->cache) {
    st->url = st->cache->resolve(uri);
    st->cache->addView(node);
    if (!st->url.empty()) st->cache->request(st->url);
  }
  readPlaybackProps(st.get(), node->props());
  NodeState* raw = st.get();
  node->imageCache = std::move(st);
  return raw;
}

void propsChanged(Node* node, const Value& old, bool* relayout) {
  const Value& props = node->props();
  if (!styleValueEquals(old["source"], props["source"])) {
    node->imageCache.reset();
    ensure(node);
    if (node->yoga() && YGNodeHasMeasureFunc(node->yoga())) {
      YGNodeMarkDirty(node->yoga());
      *relayout = true;
    }
    return;
  }
  NodeState* st = ensure(node);
  bool wasAuto = st->autoPlay;
  readPlaybackProps(st, props);
  // autoPlay turned on after the source loaded: start now.
  if (st->autoPlay && !wasAuto && st->notified && !st->playing) play(st, 0, 1);
}

bool command(Node* node, const std::string& cmd, double a, double b) {
  if (node->type() != NodeType::Lottie) return false;
  NodeState* st = ensure(node);
  if (cmd != "play" && cmd != "pause" && cmd != "resume" && cmd != "reset") return false;
  if (!st->notified) {
    // Not loaded yet: the last call applies when it is (a play() in a
    // mount effect, before the composition arrives).
    st->pending.set = true;
    st->pending.cmd = cmd;
    st->pending.a = a;
    st->pending.b = b;
    return true;
  }
  return runCommand(st, cmd, a, b);
}

double progress(Node* node) {
  float p = node->style().lottieProgress;
  if (p >= 0) return p;
  NodeState* st = ensure(node);
  return st ? st->pos : 0;
}

void paint(Node* node, SkCanvas* canvas, const SkRect& box, const SkRRect& clip) {
#if JSUI_LOTTIE
  NodeState* st = ensure(node);
  if (!st || !st->cache || st->url.empty() || box.isEmpty()) return;
  LottieCache::Composition* comp = st->cache->get(st->url, node->props()["colorFilters"]);
  if (!comp) return;
  skottie::Animation& anim = *comp->anim;
  // Out points are exclusive (a layer ending there is gone on that frame):
  // progress 1 shows the last frame before it.
  double frames = anim.outPoint() - anim.inPoint();
  anim.seekFrame(std::clamp(progress(node) * frames, 0.0, std::max(0.0, frames - 1e-3)));
  for (auto& o : comp->overrides) o.handle->set(o.color);
  SkSize size = anim.size();
  if (size.isEmpty()) return;
  ResizeMode mode = resizeModeOf(node);
  float dw = box.width(), dh = box.height();
  if (mode != ResizeMode::Stretch) {
    float sx = box.width() / size.width(), sy = box.height() / size.height();
    float k = mode == ResizeMode::Cover ? std::max(sx, sy) : std::min(sx, sy);
    if (mode == ResizeMode::Center) k = std::min(1.f, k);  // never scales up
    dw = size.width() * k;
    dh = size.height() * k;
  }
  SkRect dst = SkRect::MakeXYWH(box.centerX() - dw / 2, box.centerY() - dh / 2, dw, dh);
  canvas->save();
  if (clip.isRect()) canvas->clipRect(clip.rect(), true);
  else canvas->clipRRect(clip, true);
  anim.render(canvas, &dst);
  canvas->restore();
#else
  (void)node, (void)canvas, (void)box, (void)clip;
#endif
}

}  // namespace lottie

void LottieCache::tick(double nowMs) {
  now_ = nowMs;
  for (Node* n : views_) {
    auto* st = static_cast<lottie::NodeState*>(n->imageCache.get());
    if (!st) continue;
    if (!st->notified && !st->url.empty()) {
      State s = request(st->url);
      if (s == State::Ready && !get(st->url, Value())) s = State::Failed;  // not a composition
      if (s != State::Loading) {
        st->notified = true;
        Event ev;
        ev.node = n->id();
        ev.kind = s == State::Ready ? Event::Kind::Loaded : Event::Kind::Failed;
        if (s == State::Failed) ev.error = error(st->url);
        events.push_back(std::move(ev));
        if (s == State::Ready) {
          if (n->yoga() && YGNodeHasMeasureFunc(n->yoga())) {
            YGNodeMarkDirty(n->yoga());
            if (Document* d = n->document()) d->markLayoutDirty();
          }
          n->markPaintDirty();
          if (st->pending.set) {
            st->pending.set = false;
            lottie::runCommand(st, st->pending.cmd, st->pending.a, st->pending.b);
          } else if (st->autoPlay && n->style().lottieProgress < 0) {
            lottie::play(st, 0, 1);
          }
        }
      }
    }
    // A view shown at a given progress does not play.
    if (st->playing && n->style().lottieProgress < 0 && lottie::advance(st, nowMs)) n->markPaintDirty();
  }
}

bool LottieCache::animating() const {
  for (Node* n : views_) {
    auto* st = static_cast<const lottie::NodeState*>(n->imageCache.get());
    if (!st) continue;
    if (st->playing && n->style().lottieProgress < 0) return true;
    // Waiting to report a load that already finished (or a data source).
    if (!st->notified && !st->url.empty()) {
      const Entry* e = find(st->url);
      if (!e || e->state != State::Loading) return true;
    }
  }
  return !events.empty();
}

}  // namespace jsui
