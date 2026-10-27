// ScrollView scrolling for InputManager: offsets and scroll events, wheel
// (animated notches, pixel-precise trackpads), touch drag with velocity
// tracking, fling deceleration, paging / snap points, smooth programmatic,
// focus and keyboard scrolling, gamepad stick scrolling and the overlay
// indicator fade.
//
// Every animation is evaluated from the runtime clock in tick() (analytic
// curves, so results do not depend on the frame rate). Like the rest of
// InputManager, node pointers and ScrollState references are never held
// across a dispatch: handlers may destroy nodes or touch scrolls_.
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "animated/native_animated.h"
#include "core/document.h"
#include "core/scroll.h"
#include "input/input.h"
#include "runtime.h"

namespace jsui {

namespace {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

bool scrollEnabled(const Node* n) {
  const Value& v = n->props()["scrollEnabled"];
  return !v.isBool() || v.asBool();
}

bool isHorizontal(const Node* n) { return n->props()["horizontal"].asBool(); }

bool truthyProp(const Value& v) { return v.asBool(false) || (v.isNumber() && v.asNumber() != 0); }

Value::Object pair(float a, float b, const char* ka, const char* kb) {
  Value::Object o;
  o[ka] = Value(double(a));
  o[kb] = Value(double(b));
  return o;
}

// RN decelerationRate: 'normal' (0.998), 'fast' (0.99) or a number in (0, 1),
// the velocity factor per millisecond (iOS UIScrollView semantics).
float decelerationRate(const Node* n) {
  const Value& v = n->props()["decelerationRate"];
  if (v.isNumber()) {
    double r = v.asNumber();
    if (r > 0 && r < 1) return float(r);
  }
  if (v.asString() == "fast") return InputManager::kDecelerationFast;
  return InputManager::kDecelerationNormal;
}

float easeOutCubic(float t) {
  float u = 1 - t;
  return 1 - u * u * u;
}

float easeInOutCubic(float t) {
  if (t < 0.5f) return 4 * t * t * t;
  float u = -2 * t + 2;
  return 1 - u * u * u / 2;
}

// Programmatic / Home / End animations: longer for longer distances.
double tweenDuration(float distance) { return std::clamp(150.0 + std::fabs(distance) * 0.25, 250.0, 450.0); }

float mainOffset(const Node* sv) { return isHorizontal(sv) ? sv->scrollX : sv->scrollY; }

}  // namespace

// ------------------------------------------------------------ geometry

void InputManager::contentSize(const Node* sv, float* w, float* h) const {
  ScrollMetrics m = scrollMetrics(sv);
  *w = m.contentWidth;
  *h = m.contentHeight;
}

Node* InputManager::scrollableAncestor(Node* n, float dx, float dy) const {
  for (; n; n = n->parent()) {
    if (n->type() != NodeType::ScrollView) continue;
    if (!scrollEnabled(n)) continue;
    ScrollMetrics m = scrollMetrics(n);
    bool horizontal = isHorizontal(n);
    if (horizontal && std::fabs(dx) >= std::fabs(dy) && m.maxX() > 0) return n;
    if (!horizontal && std::fabs(dy) >= std::fabs(dx) && m.maxY() > 0) return n;
  }
  return nullptr;
}

// --------------------------------------------------------------- events

void InputManager::dispatchScrollEvent(const char* type, Node* sv, float vx, float vy, const float* target,
                                       bool toJs) {
  ScrollMetrics m = scrollMetrics(sv);
  Value::Object ev = baseEvent(type, sv);
  ev["contentOffset"] = Value(pair(sv->scrollX, sv->scrollY, "x", "y"));
  ev["contentSize"] = Value(pair(m.contentWidth, m.contentHeight, "width", "height"));
  ev["layoutMeasurement"] = Value(pair(m.viewportWidth, m.viewportHeight, "width", "height"));
  Value::Object inset;
  for (const char* k : {"top", "left", "bottom", "right"}) inset[k] = Value(0.0);
  ev["contentInset"] = Value(std::move(inset));
  ev["zoomScale"] = Value(1.0);
  ev["velocity"] = Value(pair(vx, vy, "x", "y"));
  if (target) ev["targetContentOffset"] = Value(pair(target[0], target[1], "x", "y"));
  deliver(std::move(ev), toJs);
}

namespace {

// RN scrollEventThrottle (ms): 0 / unset sends every offset change; up to
// 16 at most one per frame; more at most one per interval.
double scrollThrottle(const Node* sv) {
  double t = sv->props()["scrollEventThrottle"].asNumber(0);
  return t > 0 ? t : 0;
}

}  // namespace

// 'scroll' reaches JS only when the ScrollView listens (onScroll), at the
// rate scrollEventThrottle allows: an offset change inside the interval is
// held back and sent from tick() once it is due (with the offset then), so
// JS always gets the final position. Native Animated.event mappings see
// every change.
void InputManager::dispatchScroll(Node* sv) {
  bool js = listensForScroll(sv);
  if (!js && !rt_.animated->hasEventMapping(sv->id(), "scroll")) return;
  if (js) {
    ScrollState& st = scrollState(sv);
    double throttle = scrollThrottle(sv), t = eventTime();
    bool due = throttle == 0 || (throttle <= 16 ? st.scrollSentFrame != doc_.frame : t - st.scrollSentAt >= throttle);
    if (due) {
      st.scrollSentAt = t;
      st.scrollSentFrame = doc_.frame;
      st.scrollOwed = false;
    } else {
      st.scrollOwed = true;
      st.scrollOwedAt = t;
      js = false;
    }
  }
  dispatchScrollEvent("scroll", sv, 0, 0, nullptr, js);
}

void InputManager::flushOwedScroll(uint32_t id, double now) {
  auto it = scrolls_.find(id);
  if (it == scrolls_.end() || !it->second.scrollOwed) return;
  ScrollState& st = it->second;
  Node* sv = alive(id);
  if (!sv || !listensForScroll(sv)) {
    st.scrollOwed = false;
    return;
  }
  double throttle = scrollThrottle(sv);
  if (throttle <= 16 ? st.scrollSentFrame == doc_.frame : now - st.scrollSentAt < throttle) return;
  // Stamped with the time of the change it reports (like the event it stands for).
  st.scrollOwed = false;
  st.scrollSentAt = st.scrollOwedAt;
  st.scrollSentFrame = doc_.frame;
  double saved = eventTime_;
  eventTime_ = st.scrollOwedAt;
  dispatchScrollEvent("scroll", sv);
  eventTime_ = saved;
}

// --------------------------------------------------------------- offsets

InputManager::ScrollState& InputManager::scrollState(Node* sv) { return scrolls_[sv->id()]; }

bool InputManager::applyScroll(Node* sv, float x, float y, bool showIndicators) {
  if (!sv || !sv->yoga()) return false;
  ScrollMetrics m = scrollMetrics(sv);
  if (!std::isfinite(x)) x = sv->scrollX;
  if (!std::isfinite(y)) y = sv->scrollY;
  x = std::clamp(x, 0.f, m.maxX());
  y = std::clamp(y, 0.f, m.maxY());
  if (x == sv->scrollX && y == sv->scrollY) return false;
  sv->scrollX = x;
  sv->scrollY = y;
  sv->markBoundsDirty();
  ScrollState& st = scrollState(sv);  // tracked: re-clamped when the content shrinks
  if (showIndicators) {
    st.shownUntil = std::max(st.shownUntil, rt_.now + kIndicatorHoldMs);
    sv->scrollIndicatorOpacity = 1;
    sv->markPaintTree();
  }
  // Children move: the painter damages only what changed inside the
  // ScrollView's clip.
  doc_.markPaintDirty();
  hoverDirty_ = true;
  if (std::find(mouse_.chain.begin(), mouse_.chain.end(), sv->id()) != mouse_.chain.end()) scrolledUnderMouseAt_ = rt_.now;
  dispatchScroll(sv);
  return true;
}

bool InputManager::scrollTo(Node* sv, float x, float y) {
  if (!sv || !sv->yoga()) return false;
  uint32_t id = sv->id();
  if (!stopScrollAnimation(id, true) || !(sv = alive(id))) return false;
  return applyScroll(sv, x, y, false);
}

bool InputManager::scrollTo(Node* sv, float x, float y, bool animated) {
  if (!animated) return scrollTo(sv, x, y);
  if (!sv || !sv->yoga()) return false;
  ScrollMetrics m = scrollMetrics(sv);
  if (!std::isfinite(x)) x = sv->scrollX;
  if (!std::isfinite(y)) y = sv->scrollY;
  x = std::clamp(x, 0.f, m.maxX());
  y = std::clamp(y, 0.f, m.maxY());
  uint32_t id = sv->id();
  if (!stopScrollAnimation(id, true) || !(sv = alive(id))) return false;
  if (x == sv->scrollX && y == sv->scrollY) return false;
  float dist = std::max(std::fabs(x - sv->scrollX), std::fabs(y - sv->scrollY));
  startTween(sv, x, y, tweenDuration(dist), false, false, true);
  return true;
}

void InputManager::flashScrollIndicators(Node* sv) {
  if (!sv || sv->type() != NodeType::ScrollView) return;
  ScrollState& st = scrollState(sv);
  st.shownUntil = std::max(st.shownUntil, rt_.now + kIndicatorFlashMs);
  if (sv->scrollIndicatorOpacity != 1) {
    sv->scrollIndicatorOpacity = 1;
    sv->markPaintTree();
    doc_.markPaintDirty();
  }
}

// ------------------------------------------------------------ animation

bool InputManager::stopScrollAnimation(uint32_t id, bool notify) {
  auto it = scrolls_.find(id);
  if (it != scrolls_.end() && it->second.anim != ScrollState::Anim::None) {
    bool momentum = it->second.momentum;
    it->second.anim = ScrollState::Anim::None;
    it->second.momentum = false;
    it->second.accumulate = false;
    if (notify && momentum) {
      if (Node* sv = alive(id)) dispatchScrollEvent("momentumscrollend", sv);
    }
  }
  return alive(id) != nullptr;
}

void InputManager::startTween(Node* sv, float x, float y, double durationMs, bool momentum, bool accumulate,
                              bool easeInOut) {
  ScrollMetrics m = scrollMetrics(sv);
  ScrollState& st = scrollState(sv);
  st.anim = ScrollState::Anim::Tween;
  st.momentum = momentum;
  st.accumulate = accumulate;
  st.easeInOut = easeInOut;
  st.t0 = rt_.now;
  st.duration = std::max(1.0, durationMs);
  st.fromX = sv->scrollX;
  st.fromY = sv->scrollY;
  st.toX = std::clamp(std::isfinite(x) ? x : sv->scrollX, 0.f, m.maxX());
  st.toY = std::clamp(std::isfinite(y) ? y : sv->scrollY, 0.f, m.maxY());
  st.settleAt = -1;
}

void InputManager::scrollBase(Node* sv, float* x, float* y) {
  auto it = scrolls_.find(sv->id());
  if (it != scrolls_.end() && it->second.anim == ScrollState::Anim::Tween && it->second.accumulate) {
    *x = it->second.toX;
    *y = it->second.toY;
    return;
  }
  *x = sv->scrollX;
  *y = sv->scrollY;
}

Node* InputManager::animatingScrollAncestor(Node* n) {
  for (; n; n = n->parent()) {
    if (n->type() != NodeType::ScrollView) continue;
    auto it = scrolls_.find(n->id());
    if (it != scrolls_.end() && it->second.anim != ScrollState::Anim::None) return n;
  }
  return nullptr;
}

// ------------------------------------------------------------- snapping

bool InputManager::snaps(const Node* sv) const {
  const Value& p = sv->props();
  return p["pagingEnabled"].asBool() || p["snapToInterval"].asNumber(0) > 0 || !p["snapToOffsets"].asArray().empty();
}

// Follows React Native's iOS ScrollView: paging moves one page in the
// direction of the release velocity; snapToInterval / snapToOffsets pick
// the snap point on the velocity's side of the projected deceleration end.
float InputManager::snapTarget(Node* sv, float projected, float v, float current) {
  const Value& p = sv->props();
  bool h = isHorizontal(sv);
  ScrollMetrics m = scrollMetrics(sv);
  float view = h ? m.viewportWidth : m.viewportHeight;
  float maxOff = h ? m.maxX() : m.maxY();
  constexpr float kEps = 1e-3f;
  const float thr = kMinFlingVelocity;
  auto clampOff = [&](float o) { return std::clamp(o, 0.f, maxOff); };
  if (p["pagingEnabled"].asBool() && view > 0) {
    float f = current / view, idx;
    if (v > thr) idx = std::floor(f + kEps) + 1;
    else if (v < -thr) idx = std::ceil(f - kEps) - 1;
    else idx = std::round(projected / view);
    return clampOff(idx * view);
  }
  float interval = float(p["snapToInterval"].asNumber(0));
  if (interval > 0) {
    const std::string& align = p["snapToAlignment"].asString();
    float a = align == "center" ? (view - interval) / 2 : align == "end" ? view - interval : 0;
    float base = truthyProp(p["disableIntervalMomentum"]) ? current : projected;
    float f = (base + a) / interval, idx;
    if (v > thr) idx = std::ceil(f - kEps);
    else if (v < -thr) idx = std::floor(f + kEps);
    else idx = std::round(f);
    return clampOff(idx * interval - a);
  }
  const auto& arr = p["snapToOffsets"].asArray();
  if (arr.empty()) return kNaN;
  std::vector<float> c;
  for (const Value& o : arr)
    if (o.isNumber()) c.push_back(clampOff(float(o.asNumber())));
  if (c.empty()) return kNaN;
  std::sort(c.begin(), c.end());
  const Value& ss = p["snapToStart"];
  const Value& se = p["snapToEnd"];
  bool snapStart = !(ss.isBool() && !ss.asBool());
  bool snapEnd = !(se.isBool() && !se.asBool());
  // Without snapToStart / snapToEnd the list's ends scroll freely.
  if (!snapStart && projected < c.front()) return kNaN;
  if (!snapEnd && projected > c.back()) return kNaN;
  if (snapStart) c.insert(c.begin(), 0.f);
  if (snapEnd) c.push_back(maxOff);
  float lower = c.front(), upper = c.back();
  for (float o : c) {
    if (o <= projected + kEps) lower = o;
    if (o >= projected - kEps) {
      upper = o;
      break;
    }
  }
  if (v > thr) return upper;
  if (v < -thr) return lower;
  return projected - lower <= upper - projected ? lower : upper;
}

// ------------------------------------------------------------ touch drag
//
// The content follows the finger along the ScrollView's axis. Touch samples
// rarely line up with frames (a 120 Hz digitizer under a 60 Hz rAF, events
// a worker gets after its frame started, a sample landing just before or
// just after vsync): moving the content on every event shows two samples'
// worth of motion on one frame and none on the next, which reads as
// stutter even at a steady frame rate. When the host stamps events with
// their real times (jsui_set_event_time) the samples are only recorded
// here, and tick() moves the content to the finger position resampled at
// the frame time instead (dragResampled), like Android's and Chrome's input
// resampling. Unstamped events move the content as they arrive.

namespace {

// Release velocity (offset px/ms) from finger samples: a least-squares
// quadratic over the window, its slope at the newest sample (Android's and
// Chrome's default LSQ2 strategy). A flick speeds up until the finger
// lifts; the window's average would hand the fling a slower start than the
// content had under the finger. Two samples: their slope.
float releaseVelocity(const DragSample* s, int n) {
  if (n < 2) return 0;
  const double tn = s[n - 1].t;
  if (n == 2) {
    double dt = tn - s[0].t;
    return dt > 0 ? float((s[1].pos - s[0].pos) / dt) : 0.f;
  }
  // Normal equations for pos = a + b t + c t^2, t relative to the newest.
  double S[5] = {0, 0, 0, 0, 0}, Y[3] = {0, 0, 0};
  for (int i = 0; i < n; i++) {
    double t = s[i].t - tn, y = s[i].pos - s[n - 1].pos, tk = 1;
    for (int k = 0; k < 5; k++, tk *= t) {
      S[k] += tk;
      if (k < 3) Y[k] += tk * y;
    }
  }
  double m[3][4] = {{S[0], S[1], S[2], Y[0]}, {S[1], S[2], S[3], Y[1]}, {S[2], S[3], S[4], Y[2]}};
  for (int c = 0; c < 3; c++) {  // Gauss-Jordan with partial pivoting
    int piv = c;
    for (int r = c + 1; r < 3; r++)
      if (std::fabs(m[r][c]) > std::fabs(m[piv][c])) piv = r;
    if (std::fabs(m[piv][c]) < 1e-9) {  // samples bunched in time: the window's slope
      double dt = tn - s[0].t;
      return dt > 0 ? float((s[n - 1].pos - s[0].pos) / dt) : 0.f;
    }
    if (piv != c)
      for (int k = 0; k < 4; k++) std::swap(m[c][k], m[piv][k]);
    for (int r = 0; r < 3; r++) {
      if (r == c) continue;
      double f = m[r][c] / m[c][c];
      for (int k = c; k < 4; k++) m[r][k] -= f * m[c][k];
    }
  }
  return float(m[1][3] / m[1][1]);
}

}  // namespace

void InputManager::beginDrag(PointerState& p, Node* sv) {
  p.dragStarted = true;
  ScrollState& st = scrollState(sv);
  st.dragging = true;
  st.resample = std::isfinite(eventTime_);
  st.anim = ScrollState::Anim::None;
  st.settleAt = -1;
  st.dragOver = 0;
  st.fingerApplied = -(isHorizontal(sv) ? p.startX : p.startY);
  st.samples[0] = {eventTime(), st.fingerApplied};
  st.sampleCount = 1;
  st.frames = 0;
  dispatchScrollEvent("scrollbegindrag", sv);
}

void InputManager::dragTo(PointerState& p, Node* sv, float x, float y) {
  uint32_t id = sv->id();
  if (!p.dragStarted) {  // a caught fling starts dragging without the slop
    beginDrag(p, sv);
    if (!(sv = alive(id))) return;
  }
  ScrollState& st = scrollState(sv);
  ScrollState::Sample s{eventTime(), -(isHorizontal(sv) ? x : y)};
  if (st.sampleCount > 0 && st.samples[size_t(st.sampleCount - 1)].t == s.t) {
    st.samples[size_t(st.sampleCount - 1)] = s;  // same time: keep the latest position
  } else if (st.sampleCount < int(st.samples.size())) {
    st.samples[size_t(st.sampleCount++)] = s;
  } else {
    std::rotate(st.samples.begin(), st.samples.begin() + 1, st.samples.end());
    st.samples.back() = s;
  }
  if (!st.resample) dragApply(sv, s.pos);
}

void InputManager::dragApply(Node* sv, float finger) {
  // Past an edge the offset clamps and the overshoot is banked (up to
  // kDragEdgeSlack): without it, every jitter of a finger pulling against
  // the edge would move the content one way and never back (the clamp eats
  // the other half).
  uint32_t id = sv->id();
  bool h = isHorizontal(sv);
  ScrollMetrics m = scrollMetrics(sv);
  float maxOff = h ? m.maxX() : m.maxY();
  ScrollState& st = scrollState(sv);
  float want = mainOffset(sv) + st.dragOver + (finger - st.fingerApplied);
  float off = std::clamp(want, 0.f, maxOff);
  float over = std::clamp(want - off, -kDragEdgeSlack, kDragEdgeSlack);
  st.fingerApplied = finger;
  applyScroll(sv, h ? off : sv->scrollX, h ? sv->scrollY : off, true);
  if ((sv = alive(id))) scrollState(sv).dragOver = over;
}

float InputManager::dragResampled(ScrollState& st, double now) const {
  const auto* s = st.samples.data();
  int n = st.sampleCount;
  if (n == 0) return st.fingerApplied;
  const ScrollState::Sample& last = s[n - 1];
  // A host clock that doesn't match the frame clock: no resampling.
  if (last.t > now + kResampleMaxDeltaMs) return last.pos;
  st.sampleAges[size_t(st.frames++ % int(st.sampleAges.size()))] = float(now - last.t);
  float age = 0;
  for (int i = 0; i < std::min(st.frames, int(st.sampleAges.size())); i++) age = std::max(age, st.sampleAges[size_t(i)]);
  double want = std::clamp(double(age), kResampleLatencyMs, kResampleMaxLatencyMs);
  st.lag = st.frames == 1 ? want : std::clamp(want, st.lag - kResampleLagStepMs, st.lag + kResampleLagStepMs);
  double t = now - st.lag;
  if (last.t > t) {
    // Between the newest sample at or before t and the next one.
    int i = n - 1;
    while (i > 0 && s[i - 1].t > t) i--;
    if (i == 0) return st.fingerApplied;  // every sample is newer: stay until the frame time passes them
    const ScrollState::Sample& a = s[i - 1];
    const ScrollState::Sample& b = s[i];
    double k = (t - a.t) / (b.t - a.t);
    return float(a.pos + (b.pos - a.pos) * k);
  }
  // Past the newest: predict along the last two samples a little way.
  if (n < 2) return last.pos;
  const ScrollState::Sample& prev = s[n - 2];
  double delta = last.t - prev.t;
  if (delta < kResampleMinDeltaMs || delta > kResampleMaxDeltaMs) return last.pos;
  t = std::min(t, last.t + std::min(delta / 2, kResampleMaxPredictMs));
  return float(last.pos + (last.pos - prev.pos) * (t - last.t) / delta);
}

void InputManager::endDrag(PointerState& p, bool cancelled) {
  uint32_t id = p.scrollingId;
  bool started = p.dragStarted;
  p.dragStarted = false;
  Node* sv = alive(id);
  if (!sv) return;
  ScrollState& st = scrollState(sv);
  st.dragging = false;
  bool h = isHorizontal(sv);
  // Release velocity (offset px/ms) over the last kVelocityWindowMs; a
  // finger that rested before lifting has none.
  float v = 0;
  if (!cancelled && started && st.sampleCount >= 2) {
    const ScrollState::Sample& last = st.samples[size_t(st.sampleCount - 1)];
    if (eventTime() - last.t <= kVelocityRestMs) {
      int first = st.sampleCount - 1;
      while (first > 0 && last.t - st.samples[size_t(first - 1)].t <= kVelocityWindowMs) first--;
      v = releaseVelocity(&st.samples[size_t(first)], st.sampleCount - first);
      v = std::clamp(v, -kMaxFlingVelocity, kMaxFlingVelocity);
    }
  }
  // A resampled drag trails the finger by a few ms. Without a fling the
  // content settles where the finger lifted; a fling just carries on (a
  // catch-up step would jump).
  if (st.resample && started && st.sampleCount > 0 && std::fabs(v) < kMinFlingVelocity) {
    dragApply(sv, st.samples[size_t(st.sampleCount - 1)].pos);
    if (!(sv = alive(id))) return;
  }
  ScrollState& st2 = scrollState(sv);
  st2.resample = false;
  float cur = mainOffset(sv);
  ScrollMetrics m = scrollMetrics(sv);
  float maxOff = h ? m.maxX() : m.maxY();
  float rate = decelerationRate(sv);
  float projected = cur + v * (-1.f / std::log(rate));  // total decay distance = v / -ln(rate)
  float snap = snaps(sv) ? snapTarget(sv, projected, v, cur) : kNaN;
  bool fling = std::isnan(snap) && std::fabs(v) >= kMinFlingVelocity && ((v > 0 && cur < maxOff) || (v < 0 && cur > 0));
  float end = !std::isnan(snap) ? snap : fling ? std::clamp(projected, 0.f, maxOff) : cur;
  if (started) {
    float target[2] = {h ? end : sv->scrollX, h ? sv->scrollY : end};
    dispatchScrollEvent("scrollenddrag", sv, h ? v : 0, h ? 0 : v, target);
    if (!(sv = alive(id))) return;
  }
  if (!std::isnan(snap) && std::fabs(snap - mainOffset(sv)) >= 0.5f) {
    dispatchScrollEvent("momentumscrollbegin", sv);
    if (!(sv = alive(id))) return;
    float dist = std::fabs(snap - mainOffset(sv));
    // Start the glide at the release speed (ease-out cubic starts at 3 d / T).
    double dur = 300;
    if ((snap - mainOffset(sv)) * v > 0) dur = std::clamp(3.0 * dist / std::fabs(v), 180.0, 600.0);
    startTween(sv, h ? snap : sv->scrollX, h ? sv->scrollY : snap, dur, true, false, false);
  } else if (!std::isnan(snap)) {
    applyScroll(sv, h ? snap : sv->scrollX, h ? sv->scrollY : snap, true);
  } else if (fling) {
    dispatchScrollEvent("momentumscrollbegin", sv);
    if (!(sv = alive(id))) return;
    ScrollState& s = scrollState(sv);
    s.anim = ScrollState::Anim::Decay;
    s.momentum = true;
    s.accumulate = false;
    s.t0 = rt_.now;
    s.fromX = sv->scrollX;
    s.fromY = sv->scrollY;
    s.v0 = v;
    s.rate = rate;
    s.horizontal = h;
  }
}

// ----------------------------------------------------------------- wheel

bool InputManager::wheelScroll(Node* hit, float dx, float dy) {
  // The nearest ScrollView that can move this way. A vertical wheel over a
  // horizontal list scrolls it sideways (games UX).
  for (Node* n = hit; n; n = n->parent()) {
    if (n->type() != NodeType::ScrollView || !scrollEnabled(n)) continue;
    bool h = isHorizontal(n);
    float d = h ? (dx != 0 ? dx : dy) : dy;
    if (d == 0) continue;
    uint32_t id = n->id();
    // Mouse notches glide; trackpad deltas (fractional / small) apply
    // pixel-precisely. Notches on a snapping list go snap point to snap point.
    bool notch = smoothWheel && std::fabs(d) >= kSmoothWheelMinDelta && std::fabs(d - std::round(d)) < 1e-3f;
    if (notch) {
      if (scrollBy(n, h, d, kWheelAnimMs)) return true;
    } else {
      if (!stopScrollAnimation(id, true) || !(n = alive(id))) return true;
      bool moved = h ? applyScroll(n, n->scrollX + d, n->scrollY, true) : applyScroll(n, n->scrollX, n->scrollY + d, true);
      if (moved) {
        if ((n = alive(id)) && snaps(n)) {
          ScrollState& st = scrollState(n);
          st.settleAt = rt_.now + kSnapSettleMs;
          st.settleDir = 0;
        }
        return true;
      }
    }
    if (!(n = alive(id))) return false;
  }
  return false;
}

// ------------------------------------------------------ keyboard / gamepad

Node* InputManager::scrollTarget(bool horizontal, uint32_t preferId) {
  auto eligible = [&](Node* n) {
    if (n->type() != NodeType::ScrollView || !scrollEnabled(n) || isHorizontal(n) != horizontal) return false;
    ScrollMetrics m = scrollMetrics(n);
    return (horizontal ? m.maxX() : m.maxY()) > 0.5f;
  };
  for (Node* n = focused_; n; n = n->parent())
    if (eligible(n) && visible(n)) return n;
  for (Node* n = hovered_; n; n = n->parent())
    if (eligible(n) && visible(n)) return n;
  if (Node* p = alive(preferId); p && eligible(p) && visible(p)) return p;
  // The first one in tree order inside the active focus scope.
  Node* scope = modalScope();
  std::vector<Node*> stack{scope};
  while (!stack.empty()) {
    Node* n = stack.back();
    stack.pop_back();
    if (!n->yoga() || !n->style().display) continue;
    if (eligible(n)) return n;
    if (n->type() == NodeType::Text) continue;
    for (size_t i = n->logicalChildren().size(); i-- > 0;) stack.push_back(n->logicalChildren()[i]);
  }
  return nullptr;
}

bool InputManager::scrollBy(Node* sv, bool horizontal, float amount, double durationMs) {
  if (!sv || !scrollEnabled(sv) || amount == 0) return false;
  float bx, by;
  scrollBase(sv, &bx, &by);
  float base = horizontal ? bx : by;
  float target = base + amount;
  if (snaps(sv) && isHorizontal(sv) == horizontal) {
    float sign = amount > 0 ? 1.f : -1.f;
    float s = snapTarget(sv, base + amount, sign, base + sign);
    if (!std::isnan(s)) target = s;
  }
  ScrollMetrics m = scrollMetrics(sv);
  target = std::clamp(target, 0.f, horizontal ? m.maxX() : m.maxY());
  if (std::fabs(target - base) < 0.5f) return false;
  uint32_t id = sv->id();
  bool keep = false;  // a running accumulating tween is retargeted
  auto it = scrolls_.find(id);
  if (it != scrolls_.end()) keep = it->second.anim == ScrollState::Anim::Tween && it->second.accumulate;
  if (!keep && (!stopScrollAnimation(id, true) || !(sv = alive(id)))) return true;
  startTween(sv, horizontal ? target : bx, horizontal ? by : target, durationMs, false, true, false);
  return true;
}

bool InputManager::scrollStep(int dir) {
  if (dir < 0) return false;
  bool h = dir >= 2;
  float sign = (dir == 1 || dir == 3) ? 1.f : -1.f;
  Node* sv = scrollTarget(h);
  return sv && scrollBy(sv, h, sign * kLineStep, kKeyScrollAnimMs);
}

bool InputManager::keyboardScroll(const std::string& k, int mods) {
  bool page = k == "PageDown" || k == "PageUp";
  bool home = k == "Home", end = k == "End";
  bool space = k == " " || k == "Spacebar";  // reached only when nothing pressable is focused
  if (!page && !home && !end && !space) return false;
  bool h = false;
  Node* sv = scrollTarget(false);
  if (!sv) {
    sv = scrollTarget(true);
    h = true;
  }
  if (!sv) return false;
  ScrollMetrics m = scrollMetrics(sv);
  if (home || end) {
    float bx, by;
    scrollBase(sv, &bx, &by);
    float base = h ? bx : by, target = home ? 0 : (h ? m.maxX() : m.maxY());
    if (std::fabs(target - base) < 0.5f) return false;
    uint32_t id = sv->id();
    if (!stopScrollAnimation(id, true) || !(sv = alive(id))) return true;
    startTween(sv, h ? target : bx, h ? by : target, tweenDuration(target - mainOffset(sv)), false, true, false);
    return true;
  }
  bool back = k == "PageUp" || (space && (mods & JSUI_MOD_SHIFT));
  float view = h ? m.viewportWidth : m.viewportHeight;
  return scrollBy(sv, h, (back ? -1.f : 1.f) * view * kPageFraction, kKeyScrollAnimMs);
}

bool InputManager::hasFocusTargets() const {
  std::vector<Node*> list;
  collectFocusable(modalScope(), &list);
  return !list.empty();
}

void InputManager::tickSticks(double dt) {
  for (Pad& p : pads_) {
    float x = p.rightBlocked ? 0 : p.axes[2], y = p.rightBlocked ? 0 : p.axes[3];
    if (p.leftScrolls && p.stick.dir >= 0 && std::hypot(p.axes[0], p.axes[1]) > std::hypot(x, y)) {
      x = p.axes[0];
      y = p.axes[1];
    }
    float mag = std::hypot(x, y);
    if (!(mag > kScrollStickDeadzone)) {
      if (p.stickScrollId) {
        // Released: settle on a snap point in the direction of travel.
        if (Node* sv = alive(p.stickScrollId)) {
          ScrollState& st = scrollState(sv);
          st.settleAt = rt_.now;
          st.settleDir = p.stickScrollDir;
        }
        p.stickScrollId = 0;
      }
      continue;
    }
    if (dt <= 0) continue;
    bool horizontal = std::fabs(x) > std::fabs(y);
    Node* sv = scrollTarget(horizontal, p.stickScrollId);
    if (!sv) continue;
    // Quadratic response past the dead zone: fine control near the center.
    float k = std::min(1.f, (mag - kScrollStickDeadzone) / (1 - kScrollStickDeadzone));
    float delta = (horizontal ? x : y) / mag * kScrollStickMaxSpeed * k * k * float(dt);
    uint32_t id = sv->id();
    if (!stopScrollAnimation(id, true) || !(sv = alive(id))) continue;
    scrollState(sv).settleAt = -1;
    p.stickScrollId = id;
    p.stickScrollDir = delta > 0 ? 1.f : -1.f;
    if (horizontal) applyScroll(sv, sv->scrollX + delta, sv->scrollY, true);
    else applyScroll(sv, sv->scrollX, sv->scrollY + delta, true);
  }
}

// ------------------------------------------------------------------ tick

void InputManager::tickScrolling(double now, double /*dt*/) {
  if (scrolls_.empty()) return;
  std::vector<uint32_t> ids;
  ids.reserve(scrolls_.size());
  for (auto& e : scrolls_) ids.push_back(e.first);
  float pr = doc_.pixelRatio();
  // In-flight frames land on device pixels (crisp text while gliding); the
  // final frame is the exact target.
  auto devicePx = [pr](float v) { return std::round(v * pr) / pr; };
  for (uint32_t id : ids) {
    auto it = scrolls_.find(id);
    if (it == scrolls_.end()) continue;
    Node* sv = alive(id);
    if (!sv || !attached(sv)) {
      // Destroyed or detached: drop its animation (a detached node keeps its
      // offset; nothing is painted or dispatched for it).
      if (sv && sv->scrollIndicatorOpacity != 0) {
        sv->scrollIndicatorOpacity = 0;
        sv->markPaintTree();
      }
      scrolls_.erase(it);
      continue;
    }
    ScrollState st = it->second;  // copy: handlers below may rehash scrolls_
    if (st.anim == ScrollState::Anim::Tween) {
      double t = (now - st.t0) / st.duration;
      bool done = t >= 1;
      float e = done ? 1.f : st.easeInOut ? easeInOutCubic(float(t)) : easeOutCubic(float(t));
      float x = st.fromX + (st.toX - st.fromX) * e, y = st.fromY + (st.toY - st.fromY) * e;
      if (!done) {
        x = devicePx(x);
        y = devicePx(y);
      } else {
        it->second.anim = ScrollState::Anim::None;
        it->second.momentum = false;
        it->second.accumulate = false;
      }
      applyScroll(sv, x, y, true);
      if (done && st.momentum && (sv = alive(id))) dispatchScrollEvent("momentumscrollend", sv);
    } else if (st.anim == ScrollState::Anim::Decay) {
      double lnr = std::log(double(st.rate));
      double speed = std::fabs(st.v0);
      double tEnd = speed > kStopVelocity ? std::log(kStopVelocity / speed) / lnr : 0;
      double t = std::max(0.0, now - st.t0);
      bool done = t >= tEnd;
      if (done) t = tEnd;
      float pos = (st.horizontal ? st.fromX : st.fromY) + float(st.v0 * (std::pow(double(st.rate), t) - 1) / lnr);
      ScrollMetrics m = scrollMetrics(sv);
      float maxOff = st.horizontal ? m.maxX() : m.maxY();
      if (pos <= 0 || pos >= maxOff) {  // no overscroll: the fling stops at the edge
        pos = std::clamp(pos, 0.f, maxOff);
        done = true;
      }
      if (!done) pos = devicePx(pos);
      if (done) {
        it->second.anim = ScrollState::Anim::None;
        it->second.momentum = false;
      }
      applyScroll(sv, st.horizontal ? pos : sv->scrollX, st.horizontal ? sv->scrollY : pos, true);
      if (done && (sv = alive(id))) dispatchScrollEvent("momentumscrollend", sv);
    } else if (st.dragging && st.resample) {
      float f = devicePx(dragResampled(it->second, now));  // finger samples land on device pixels too
      if (f != st.fingerApplied) dragApply(sv, f);
    } else if (!st.dragging) {
      // Content may have shrunk under a resting ScrollView.
      ScrollMetrics m = scrollMetrics(sv);
      if (sv->scrollX > m.maxX() || sv->scrollY > m.maxY()) applyScroll(sv, sv->scrollX, sv->scrollY, false);
    }
    flushOwedScroll(id, now);
    it = scrolls_.find(id);
    if (it == scrolls_.end() || !(sv = alive(id))) continue;
    ScrollState& s = it->second;
    if (s.settleAt >= 0 && now >= s.settleAt && s.anim == ScrollState::Anim::None && !s.dragging) {
      s.settleAt = -1;
      float cur = mainOffset(sv);
      float target = snaps(sv) ? snapTarget(sv, cur, s.settleDir, cur) : kNaN;
      if (!std::isnan(target) && std::fabs(target - cur) >= 0.5f) {
        bool h = isHorizontal(sv);
        startTween(sv, h ? target : sv->scrollX, h ? sv->scrollY : target, 250, false, false, false);
      }
    }
    // Indicators: fully visible while moving and kIndicatorHoldMs after,
    // then fade out over kIndicatorFadeMs.
    if (s.anim != ScrollState::Anim::None || s.dragging) s.shownUntil = std::max(s.shownUntil, now + kIndicatorHoldMs);
    float alpha = s.shownUntil < 0 ? 0.f
                  : now <= s.shownUntil
                      ? 1.f
                      : float(std::max(0.0, 1.0 - (now - s.shownUntil) / kIndicatorFadeMs));
    if (alpha != sv->scrollIndicatorOpacity) {
      sv->scrollIndicatorOpacity = alpha;
      sv->markPaintTree();
      doc_.markPaintDirty();
    }
    if (alpha == 0) s.shownUntil = -1;
    bool idle = s.anim == ScrollState::Anim::None && !s.dragging && s.settleAt < 0 && alpha == 0 && !s.scrollOwed;
    if (idle && sv->scrollX == 0 && sv->scrollY == 0) scrolls_.erase(it);
  }
}

bool InputManager::animating() const {
  for (const auto& e : scrolls_) {
    const ScrollState& st = e.second;
    if (st.anim != ScrollState::Anim::None || st.dragging || st.settleAt >= 0 || st.shownUntil >= 0 || st.scrollOwed)
      return true;
  }
  if (mouseInside_ && hoverDirty_) return true;  // a hover refresh waits for scrolling to pause
  if (statesMoving_ || doc_.statesKick || !pressFlash_.empty()) return true;  // a state style transition
  if (!deferred_.empty()) return true;  // a default action waits for a late JS answer
  for (const PointerState& p : touches_)
    if (p.holdUntil >= 0) return true;  // a touch held for JS
  if (selPressPointer_ >= 0) return true;  // a touch may become a long-press text selection
  for (const Pad& p : pads_) {
    if (p.dpad.dir >= 0 || p.stick.dir >= 0 || p.stickScrollId) return true;
    if (!p.rightBlocked && std::hypot(p.axes[2], p.axes[3]) > kScrollStickDeadzone) return true;
  }
  return false;
}

// -------------------------------------------------------- scroll into view

void InputManager::scrollIntoView(Node* n, bool animated, float margin) {
  if (!n || !n->yoga()) return;
  uint32_t id = n->id();
  // Where the node will be once the inner ScrollViews reach their targets.
  float shiftX = 0, shiftY = 0;
  // Render ancestors: an overlay is not inside its anchor's ScrollViews.
  for (Node* p = n->renderParent(); p; p = p->renderParent()) {
    if (p->type() != NodeType::ScrollView) continue;
    Node* cur = alive(id);
    if (!cur) return;
    Rect nr = cur->absoluteRect(), pr = p->absoluteRect();
    ScrollMetrics m = scrollMetrics(p);
    float bx = p->scrollX, by = p->scrollY;
    if (animated) scrollBase(p, &bx, &by);
    // Node position in the ScrollView's content coordinates.
    float cx = nr.x - shiftX - pr.x + p->scrollX, cy = nr.y - shiftY - pr.y + p->scrollY;
    auto axis = [margin](float c, float size, float view, float base, float maxOff) {
      float mm = std::clamp((view - size) / 2, 0.f, margin);
      float s = base;
      // Align the near edge if the node is before the viewport (or larger
      // than it), the far edge if it is after.
      if (size > view || c - mm < base) s = c - mm;
      else if (c + size + mm > base + view) s = c + size + mm - view;
      return std::clamp(s, 0.f, maxOff);
    };
    float sx = axis(cx, nr.width, pr.width, bx, m.maxX());
    float sy = axis(cy, nr.height, pr.height, by, m.maxY());
    shiftX += sx - p->scrollX;
    shiftY += sy - p->scrollY;
    uint32_t pid = p->id();
    if (animated) {
      if (sx != bx || sy != by) {
        if (!stopScrollAnimation(pid, true) || !(p = alive(pid))) return;
        startTween(p, sx, sy, kFocusScrollAnimMs, false, true, false);
      }
    } else {
      scrollTo(p, sx, sy);
    }
    p = alive(pid);  // the scroll handler may have destroyed it
    if (!p) return;
  }
}

}  // namespace jsui
