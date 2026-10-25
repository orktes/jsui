#include "frame_snapshot.h"

#include <algorithm>

#include "animated/native_animated.h"
#include "runtime.h"

namespace jsui {

namespace {

// Layouts of `n` and its subtree; (originX, originY) is where the parent's
// content box starts on the page (its page rect minus its scroll offset).
void addLayouts(const Node* n, float originX, float originY, std::vector<std::pair<uint32_t, FrameSnapshot::Layout>>* out) {
  FrameSnapshot::Layout l;
  l.local = n->layoutRect();
  l.page = l.local;
  l.page.x += originX;
  l.page.y += originY;
  out->emplace_back(n->id(), l);
  float x = l.page.x - n->scrollX, y = l.page.y - n->scrollY;
  for (const Node* c : n->children()) addLayouts(c, x, y, out);
}

// `prev` when `rows` are the rows collected for it (`last`), else a new
// sorted table of them; `last` then holds them for the next frame.
template <class T, class Row>
std::shared_ptr<const T> shareOrBuild(const std::shared_ptr<const T>& prev, std::vector<Row>& rows,
                                      std::vector<Row>& last, bool* changed) {
  if (prev && rows == last) return prev;
  *changed = true;
  last.swap(rows);
  auto t = std::make_shared<T>();
  t->rows = last;
  std::sort(t->rows.begin(), t->rows.end(), [](const Row& a, const Row& b) { return a.first < b.first; });
  return t;
}

}  // namespace

const FrameSnapshot::SurfaceInfo* FrameSnapshot::surface(uint32_t tag) const {
  for (const SurfaceInfo& s : *surfaces)
    if (s.tag == tag) return &s;
  return nullptr;
}

std::shared_ptr<FrameSnapshot> FrameSnapshot::take(Runtime& rt, const FrameSnapshot* prev, Scratch& scratch) {
  uint64_t batches = rt.appliedBatches.load(std::memory_order_acquire);
  bool changed = !prev || prev->appliedBatches != batches;
  std::shared_ptr<const Surfaces> surfaces;
  if (prev && !surfacesChanged(rt, prev)) {
    surfaces = prev->surfaces;
  } else {
    scratch.surfaces.clear();
    for (const auto& surface : rt.surfaces()) {
      const Document& d = surface->doc;
      scratch.surfaces.push_back({surface->tag(), d.width(), d.height(), d.pixelRatio(), surface->appKey, surface->propsJson});
    }
    surfaces = std::make_shared<const Surfaces>(scratch.surfaces);
    changed = true;
  }
  scratch.layouts.clear();
  if (rt.snapshotLayouts.load(std::memory_order_relaxed))
    for (const auto& surface : rt.surfaces())
      if (const Node* root = surface->doc.root()) addLayouts(root, 0, 0, &scratch.layouts);
  rt.animated->snapshotValues(&scratch.values);
  rt.animated->snapshotAnimations(&scratch.animations);
  auto layouts = shareOrBuild(prev ? prev->layouts : nullptr, scratch.layouts, scratch.lastLayouts, &changed);
  auto values = shareOrBuild(prev ? prev->values : nullptr, scratch.values, scratch.lastValues, &changed);
  auto animations = shareOrBuild(prev ? prev->animations : nullptr, scratch.animations, scratch.lastAnimations, &changed);
  if (!changed) return nullptr;
  auto s = std::make_shared<FrameSnapshot>();
  s->frame = rt.doc.frame;
  s->appliedBatches = batches;
  s->surfaces = std::move(surfaces);
  s->layouts = std::move(layouts);
  s->values = std::move(values);
  s->animations = std::move(animations);
  return s;
}

bool FrameSnapshot::surfacesChanged(const Runtime& rt, const FrameSnapshot* s) {
  if (!s || s->surfaces->size() != rt.surfaces().size()) return true;
  for (size_t i = 0; i < s->surfaces->size(); i++) {
    const Surface& live = *rt.surfaces()[i];
    const SurfaceInfo& was = (*s->surfaces)[i];
    if (was.tag != live.tag() || was.width != live.doc.width() || was.height != live.doc.height() ||
        was.scale != live.doc.pixelRatio() || was.appKey != live.appKey || was.propsJson != live.propsJson)
      return true;
  }
  return false;
}

}  // namespace jsui
