// Overlay layer: Document's side (see core/overlay.h).
#include "core/overlay.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "core/document.h"
#include "core/node.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkRect.h"
#include "render/painter.h"

namespace jsui {

OverlayConfig parseOverlayConfig(const Value& v) {
  OverlayConfig c;
  auto num = [&](const char* k, float def) {
    const Value& x = v[k];
    return x.isNumber() && std::isfinite(x.asNumber()) ? float(x.asNumber()) : def;
  };
  // A ref that is not attached yet comes as -1: no such node (hidden).
  if (v["anchor"].isNumber()) c.anchor = v["anchor"].asNumber() > 0 ? uint32_t(v["anchor"].asNumber()) : UINT32_MAX;
  if (v["boundary"].isNumber() && v["boundary"].asNumber() > 0) c.boundary = uint32_t(v["boundary"].asNumber());
  const std::string& pl = v["placement"].asString();
  std::string side = pl, align;
  if (size_t dash = pl.find('-'); dash != std::string::npos) {
    side = pl.substr(0, dash);
    align = pl.substr(dash + 1);
  }
  if (side == "top") c.side = OverlaySide::Top;
  else if (side == "left") c.side = OverlaySide::Left;
  else if (side == "right") c.side = OverlaySide::Right;
  else if (side == "center") c.side = OverlaySide::Center;
  else if (side == "auto") c.autoSide = true;
  if (align == "start") c.align = OverlayAlign::Start;
  else if (align == "end") c.align = OverlayAlign::End;
  c.offset = num("offset", 0);
  c.crossOffset = num("crossOffset", 0);
  c.padding = std::max(0.f, num("collisionPadding", 0));
  c.flip = v["flip"].asBool(true);
  c.shift = v["collision"].asString() != "none";
  const Value& mw = v["matchWidth"];
  c.matchWidth = mw.asString() == "min" ? 2 : mw.asBool(false) ? 1 : 0;
  c.hideWhenClipped = v["hideWhenClipped"].asBool(true);
  return c;
}

namespace {

constexpr float kEps = 1e-4f;

bool clips(const Node* n) {
  return n->style().overflow != Overflow::Visible || n->type() == NodeType::ScrollView ||
         n->type() == NodeType::Image || n->type() == NodeType::Lottie || n->type() == NodeType::Shader ||
         n->type() == NodeType::Vector;
}

// The node's border box to its render parent's content space, as the
// painter places it (without pixel snapping).
SkMatrix localMatrix(const Node* n) {
  Rect r = n->visualRect();
  SkMatrix m = SkMatrix::Translate(r.x, r.y);
  if (n->layoutAnim && (n->layoutAnim->scaleX != 1 || n->layoutAnim->scaleY != 1)) {
    m.preTranslate(r.width / 2, r.height / 2);
    m.preScale(n->layoutAnim->scaleX, n->layoutAnim->scaleY);
    m.preTranslate(-r.width / 2, -r.height / 2);
  }
  if (!n->style().transform.empty()) m.preConcat(localTransform(*n));
  return m;
}

// The node's border box to the viewport through its render ancestors
// (layout, scroll offsets, transforms). With `clip`: the intersection of
// the clips of those ancestors, in the viewport.
SkMatrix viewMatrix(const Node* n, SkRect* clip) {
  thread_local std::vector<const Node*> chain;
  chain.clear();
  for (const Node* p = n; p; p = p->renderParent()) chain.push_back(p);
  SkMatrix m;
  SkRect c = SkRect::MakeLTRB(-1e9f, -1e9f, 1e9f, 1e9f);
  for (size_t i = chain.size(); i-- > 0;) {
    const Node* p = chain[i];
    m.preConcat(localMatrix(p));
    if (i == 0) break;
    if (clip && clips(p)) {
      Rect r = p->visualRect();
      if (!c.intersect(m.mapRect(SkRect::MakeWH(r.width, r.height)))) c.setEmpty();
    }
    m.preTranslate(-p->scrollX, -p->scrollY);
  }
  if (clip) *clip = c;
  return m;
}

SkRect boxOf(const Node* n) {
  Rect r = n->visualRect();
  return SkRect::MakeWH(r.width, r.height);
}

bool finiteRect(const Rect& r) {
  return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.width) && std::isfinite(r.height);
}

// Attached to `root` with every logical ancestor (and itself) displayed.
bool shown(const Node* n, const Node* root) {
  for (; n; n = n->parent()) {
    if (!n->yoga() || !n->style().display) return false;
    if (n == root) return true;
  }
  return false;
}

float clampRange(float v, float lo, float hi) { return hi < lo ? lo : std::clamp(v, lo, hi); }

OverlaySide opposite(OverlaySide s) {
  switch (s) {
    case OverlaySide::Top: return OverlaySide::Bottom;
    case OverlaySide::Bottom: return OverlaySide::Top;
    case OverlaySide::Left: return OverlaySide::Right;
    case OverlaySide::Right: return OverlaySide::Left;
    default: return s;
  }
}

Value hostProps() {
  Value::Object style;
  style["position"] = Value("absolute");
  style["left"] = Value(0);
  style["top"] = Value(0);
  style["transformOrigin"] = Value("left top");
  style["pointerEvents"] = Value("box-none");
  style["display"] = Value("none");
  Value::Object props;
  props["style"] = Value(std::move(style));
  return Value(std::move(props));
}

Value layerProps() {
  Value::Object style;
  style["position"] = Value("absolute");
  style["left"] = Value(0);
  style["top"] = Value(0);
  style["right"] = Value(0);
  style["bottom"] = Value(0);
  style["pointerEvents"] = Value("box-none");
  Value::Object props;
  props["style"] = Value(std::move(style));
  return Value(std::move(props));
}

void patch(Node* n, const char* key, Value v) {
  Value::Object o;
  o[key] = std::move(v);
  n->patchStyle(o);
}

}  // namespace

void Document::setOverlay(Node* n, const Value* config) {
  if (!config) {
    if (n->overlay) dropOverlay(n);
    return;
  }
  OverlayConfig cfg = parseOverlayConfig(*config);
  if (n->overlay) {
    OverlayState& st = *n->overlay;
    if (st.config.matchWidth != cfg.matchWidth && st.matchedWidth >= 0) {
      Value::Object o;
      o["width"] = Value();
      o["minWidth"] = Value();
      st.host->patchStyle(o);
      st.matchedWidth = -1;
    }
    st.config = cfg;
    markPaintDirty();
    return;
  }
  if (!overlayLayer_) {
    overlayLayer_ = createNode(NodeType::View);
    overlayLayer_->nativeOnly = true;
    overlayLayer_->setProps(layerProps());
    root_->setRenderExtras({overlayLayer_});
    layerZ_ = 0;
  }
  Node* host = createNode(NodeType::View);
  host->nativeOnly = true;
  host->setProps(hostProps());
  auto st = std::make_unique<OverlayState>();
  st->config = cfg;
  st->host = host;
  n->overlay = std::move(st);
  // Out of its parent's render list and Yoga, into the host's. The host
  // joins the layer now (the layer never lists a destroyed host) and is
  // put in tree order by the next layout.
  if (Node* p = n->parent()) p->syncRender();
  host->setRenderExtras({n});
  std::vector<Node*> hosts = overlayLayer_->renderExtras();
  hosts.push_back(host);
  overlayLayer_->setRenderExtras(std::move(hosts));
  overlays_.push_back(n);
  overlaysChanged_ = true;
  markLayoutDirty();
}

void Document::dropOverlay(Node* n) {
  Node* host = n->overlay->host;
  if (overlayLayer_) {
    std::vector<Node*> hosts = overlayLayer_->renderExtras();
    hosts.erase(std::remove(hosts.begin(), hosts.end(), host), hosts.end());
    overlayLayer_->setRenderExtras(std::move(hosts));
  }
  host->setRenderExtras({});  // n's Yoga node leaves the host's
  n->overlay.reset();
  overlays_.erase(std::remove(overlays_.begin(), overlays_.end(), n), overlays_.end());
  if (Node* p = n->parent()) p->syncRender();
  destroyRecursive(host);
  overlaysChanged_ = true;
  markLayoutDirty();
}

void Document::setOverlayArrow(Node* n, bool on) {
  n->overlayArrow = on;
  auto it = std::find(arrows_.begin(), arrows_.end(), n);
  if (on && it == arrows_.end()) arrows_.push_back(n);
  if (!on && it != arrows_.end()) arrows_.erase(it);
  markPaintDirty();
}

void Document::syncOverlays() {
  if (overlaysChanged_ || treeVersion != overlayTreeVersion_) {
    overlaysChanged_ = false;
    overlayTreeVersion_ = treeVersion;
    // Logical tree order (the path of child indices from the root); the
    // detached ones after, as they were.
    std::vector<std::pair<std::vector<uint32_t>, Node*>> keyed;
    keyed.reserve(overlays_.size());
    for (Node* n : overlays_) {
      std::vector<uint32_t> path;
      const Node* c = n;
      for (const Node* p = n->parent(); p; c = p, p = p->parent()) {
        const auto& kids = p->logicalChildren();
        path.push_back(uint32_t(std::find(kids.begin(), kids.end(), c) - kids.begin()));
      }
      std::reverse(path.begin(), path.end());
      if (c != root_) path.insert(path.begin(), std::numeric_limits<uint32_t>::max());
      keyed.emplace_back(std::move(path), n);
    }
    std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<Node*> hosts;
    for (size_t i = 0; i < keyed.size(); i++) {
      overlays_[i] = keyed[i].second;
      hosts.push_back(overlays_[i]->overlay->host);
    }
    overlayLayer_->setRenderExtras(std::move(hosts));
  }
  for (Node* n : overlays_) {
    OverlayState& st = *n->overlay;
    bool active = true;
    float alpha = 1;
    for (Node* p = n->parent();; p = p->parent()) {
      if (!p || !p->yoga() || !p->style().display) {
        active = false;
        break;
      }
      alpha *= p->style().opacity * (p->layoutAnim ? p->layoutAnim->opacity : 1.f);
      if (p == root_) break;
    }
    st.alpha = alpha;
    if (active != st.active) {
      st.active = active;
      patch(st.host, "display", active ? Value() : Value("none"));
    }
  }
  // Above every other child of the root (a Modal is one with zIndex 10000).
  int z = 0;
  for (Node* c : root_->logicalChildren())
    if (c->style().hasZIndex && c->style().zIndex > z) z = c->style().zIndex;
  int target = z > 0 ? z + 1 : 0;
  if (target != layerZ_) {
    layerZ_ = target;
    patch(overlayLayer_, "zIndex", target ? Value(target) : Value());
  }
}

namespace {

// Nothing to draw or hit: an empty box whose children are all out of layout
// (a <Visibility> popout while hidden). Placed once that changes, by the
// layout pass that changes it.
bool emptyOverlay(const Node* n) {
  Rect box = n->layoutRect();
  if (box.width > 0 || box.height > 0) return false;
  for (const Node* c : n->children())
    if (c->style().display) return false;
  return true;
}

}  // namespace

bool Document::placeOverlays() {
  bool resized = false;
  bool layerInverted = false;
  SkMatrix layerInv;
  for (Node* n : overlays_) {
    if (!n->overlay->active || emptyOverlay(n)) continue;
    if (!layerInverted) {
      layerInverted = true;
      if (!viewMatrix(overlayLayer_, nullptr).invert(&layerInv)) layerInv.reset();
    }
    if (placeOverlay(n, layerInv)) resized = true;
  }
  return resized;
}

bool Document::placeOverlay(Node* n, const SkMatrix& layerInv) {
  OverlayState& st = *n->overlay;
  Node* host = st.host;
  Node* p = n->parent();
  const OverlayConfig& cfg = st.config;
  bool resized = false;

  // The anchor (by default the parent) in the parent's space, and the
  // clips around it in the viewport.
  Node* a = cfg.anchor ? get(cfg.anchor) : p;
  SkRect clip;
  SkMatrix pm = viewMatrix(p, a == p ? &clip : nullptr), inv;
  bool hidden = !pm.invert(&inv) || !a || (a != p && !shown(a, root_)) || !finiteRect(a->visualRect());
  SkRect anchor = SkRect::MakeEmpty();
  if (!hidden) {
    SkMatrix am = a == p ? pm : viewMatrix(a, &clip);
    SkRect dev = am.mapRect(boxOf(a));
    anchor = a == p ? boxOf(a) : inv.mapRect(dev);
    if (cfg.hideWhenClipped) {
      if (!clip.intersect(SkRect::MakeWH(width_, height_))) clip.setEmpty();
      hidden = !(dev.fLeft < clip.fRight && dev.fRight > clip.fLeft && dev.fTop < clip.fBottom && dev.fBottom > clip.fTop);
    }
  }
  if (!hidden) {
    st.anchor[0] = anchor.fLeft, st.anchor[1] = anchor.fTop, st.anchor[2] = anchor.fRight, st.anchor[3] = anchor.fBottom;
    if (cfg.matchWidth && std::fabs(st.matchedWidth - anchor.width()) > 0.01f) {
      st.matchedWidth = anchor.width();
      patch(host, cfg.matchWidth == 2 ? "minWidth" : "width", Value(double(st.matchedWidth)));
      resized = true;
    }
    // Where the overlay may go, in the parent's space.
    SkRect bounds = SkRect::MakeWH(width_, height_);
    if (Node* b = cfg.boundary ? get(cfg.boundary) : nullptr; b && shown(b, root_))
      bounds = viewMatrix(b, nullptr).mapRect(boxOf(b));
    bounds = inv.mapRect(bounds);
    bounds.inset(std::min(cfg.padding, bounds.width() / 2), std::min(cfg.padding, bounds.height() / 2));

    Rect box = n->layoutRect();  // in the host
    float w = box.width, h = box.height;
    auto room = [&](OverlaySide s) {
      switch (s) {
        case OverlaySide::Top: return anchor.fTop - cfg.offset - bounds.fTop;
        case OverlaySide::Bottom: return bounds.fBottom - anchor.fBottom - cfg.offset;
        case OverlaySide::Left: return anchor.fLeft - cfg.offset - bounds.fLeft;
        case OverlaySide::Right: return bounds.fRight - anchor.fRight - cfg.offset;
        default: return 0.f;
      }
    };
    auto need = [&](OverlaySide s) { return s == OverlaySide::Top || s == OverlaySide::Bottom ? h : w; };
    OverlaySide side = cfg.side;
    if (cfg.autoSide) {
      const OverlaySide order[4] = {OverlaySide::Bottom, OverlaySide::Top, OverlaySide::Right, OverlaySide::Left};
      side = order[0];
      float best = -1e9f;
      for (OverlaySide s : order) {
        float spare = room(s) - need(s);
        if (spare >= 0) {
          side = s;
          break;
        }
        if (spare > best) best = spare, side = s;
      }
    } else if (cfg.flip && side != OverlaySide::Center && room(side) < need(side)) {
      OverlaySide opp = opposite(side);
      if (room(opp) >= need(opp) || room(opp) > room(side)) side = opp;
    }
    auto cross = [&](float lo, float hi, float size) {
      float v = cfg.align == OverlayAlign::Start ? lo : cfg.align == OverlayAlign::End ? hi - size : (lo + hi - size) / 2;
      return v + cfg.crossOffset;
    };
    float x = 0, y = 0;
    switch (side) {
      case OverlaySide::Top:
        y = anchor.fTop - cfg.offset - h;
        x = cross(anchor.fLeft, anchor.fRight, w);
        break;
      case OverlaySide::Bottom:
        y = anchor.fBottom + cfg.offset;
        x = cross(anchor.fLeft, anchor.fRight, w);
        break;
      case OverlaySide::Left:
        x = anchor.fLeft - cfg.offset - w;
        y = cross(anchor.fTop, anchor.fBottom, h);
        break;
      case OverlaySide::Right:
        x = anchor.fRight + cfg.offset;
        y = cross(anchor.fTop, anchor.fBottom, h);
        break;
      case OverlaySide::Center:
        x = anchor.centerX() - w / 2 + cfg.crossOffset;
        y = anchor.centerY() - h / 2 + cfg.offset;
        break;
    }
    if (cfg.shift) {
      if (side != OverlaySide::Left && side != OverlaySide::Right) x = clampRange(x, bounds.fLeft, bounds.fRight - w);
      if (side != OverlaySide::Top && side != OverlaySide::Bottom) y = clampRange(y, bounds.fTop, bounds.fBottom - h);
    }
    st.placed = side;
    st.x = x;
    st.y = y;

    // The host in the layer: the parent's matrix, then the box at (x, y).
    SkMatrix hm = pm;
    hm.preTranslate(x - box.x, y - box.y);
    hm.postConcat(layerInv);
    float lin[4] = {hm.getScaleX(), hm.getSkewY(), hm.getSkewX(), hm.getScaleY()};
    bool same = true;
    for (int i = 0; i < 4; i++) same &= std::fabs(lin[i] - st.matrix[i]) < kEps;
    if (!same) {
      std::copy(lin, lin + 4, st.matrix);
      bool identity = std::fabs(lin[0] - 1) < kEps && std::fabs(lin[1]) < kEps && std::fabs(lin[2]) < kEps &&
                      std::fabs(lin[3] - 1) < kEps;
      Value t;
      if (!identity) {
        Value::Array m{Value(double(lin[0])), Value(double(lin[1])), Value(double(lin[2])),
                       Value(double(lin[3])), Value(0),              Value(0)};
        Value::Object op;
        op["matrix"] = Value(std::move(m));
        t = Value(Value::Array{Value(std::move(op))});
      }
      patch(host, "transform", std::move(t));
    }
    YGNodeRef hy = host->yoga();
    host->setOffset(hm.getTranslateX() - YGNodeLayoutGetLeft(hy), hm.getTranslateY() - YGNodeLayoutGetTop(hy));
  }
  float alpha = hidden ? 0.f : st.alpha;
  if (alpha != st.hostAlpha) {
    st.hostAlpha = alpha;
    patch(host, "opacity", alpha >= 1 ? Value() : Value(double(alpha)));
  }
  if (hidden != st.hostHidden) {
    st.hostHidden = hidden;
    patch(host, "pointerEvents", Value(hidden ? "none" : "box-none"));
  }
  st.hidden = hidden;
  if (!hidden && !arrows_.empty()) placeArrows(n);
  return resized;
}

void Document::placeArrows(Node* n) {
  const OverlayState& st = *n->overlay;
  Rect box = n->layoutRect();
  float w = box.width, h = box.height;
  // The anchor's centre in the overlay box.
  float cx = (st.anchor[0] + st.anchor[2]) / 2 - st.x, cy = (st.anchor[1] + st.anchor[3]) / 2 - st.y;
  for (Node* r : arrows_) {
    Node* owner = r->parent();
    while (owner && !owner->overlay) owner = owner->parent();
    if (owner != n || !r->yoga()) continue;
    // r's render parent's origin in the overlay box (transforms between
    // them are not followed).
    float ox = 0, oy = 0;
    Node* q = r->renderParent();
    for (; q && q != n; q = q->renderParent()) {
      Rect qr = q->layoutRect();
      ox += qr.x;
      oy += qr.y;
      if (Node* qp = q->renderParent()) ox -= qp->scrollX, oy -= qp->scrollY;
    }
    if (!q) continue;
    ox -= n->scrollX;
    oy -= n->scrollY;
    Rect rr = r->layoutRect();
    float rw = rr.width, rh = rr.height;
    float pad = std::min(8.f, std::max(0.f, std::min(w - rw, h - rh) / 2));
    float dx, dy;
    switch (st.placed) {
      case OverlaySide::Top:
        dx = clampRange(cx - rw / 2, pad, w - rw - pad);
        dy = h - rh / 2;
        break;
      case OverlaySide::Bottom:
        dx = clampRange(cx - rw / 2, pad, w - rw - pad);
        dy = -rh / 2;
        break;
      case OverlaySide::Left:
        dx = w - rw / 2;
        dy = clampRange(cy - rh / 2, pad, h - rh - pad);
        break;
      case OverlaySide::Right:
        dx = -rw / 2;
        dy = clampRange(cy - rh / 2, pad, h - rh - pad);
        break;
      default: continue;
    }
    r->setOffset(dx - ox - (rr.x - r->offsetX), dy - oy - (rr.y - r->offsetY));
  }
}

}  // namespace jsui
