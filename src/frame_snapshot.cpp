#include "frame_snapshot.h"

#include "animated/native_animated.h"
#include "runtime.h"

namespace jsui {

namespace {

// Layouts of `n` and its subtree; (originX, originY) is where the parent's
// content box starts on the page (its page rect minus its scroll offset).
void addLayouts(const Node* n, float originX, float originY, std::unordered_map<uint32_t, FrameSnapshot::Layout>* out) {
  FrameSnapshot::Layout l;
  l.local = n->layoutRect();
  l.page = l.local;
  l.page.x += originX;
  l.page.y += originY;
  out->emplace(n->id(), l);
  float x = l.page.x - n->scrollX, y = l.page.y - n->scrollY;
  for (const Node* c : n->children()) addLayouts(c, x, y, out);
}

}  // namespace

const FrameSnapshot::SurfaceInfo* FrameSnapshot::surface(uint32_t tag) const {
  for (const SurfaceInfo& s : surfaces)
    if (s.tag == tag) return &s;
  return nullptr;
}

std::shared_ptr<FrameSnapshot> FrameSnapshot::take(Runtime& rt) {
  auto s = std::make_shared<FrameSnapshot>();
  s->frame = rt.doc.frame;
  s->appliedBatches = rt.appliedBatches.load(std::memory_order_acquire);
  s->surfaces.reserve(rt.surfaces().size());
  for (const auto& surface : rt.surfaces()) {
    const Document& d = surface->doc;
    s->surfaces.push_back({surface->tag(), d.width(), d.height(), d.pixelRatio(), surface->appKey, surface->propsJson});
  }
  for (const auto& surface : rt.surfaces())
    if (const Node* root = surface->doc.root()) addLayouts(root, 0, 0, &s->layouts);
  rt.animated->snapshotValues(&s->values);
  rt.animated->snapshotAnimations(&s->animations);
  return s;
}

bool FrameSnapshot::surfacesChanged(const Runtime& rt, const FrameSnapshot* s) {
  if (!s || s->surfaces.size() != rt.surfaces().size()) return true;
  for (size_t i = 0; i < s->surfaces.size(); i++) {
    const Surface& live = *rt.surfaces()[i];
    const SurfaceInfo& was = s->surfaces[i];
    if (was.tag != live.tag() || was.width != live.doc.width() || was.height != live.doc.height() ||
        was.scale != live.doc.pixelRatio() || was.appKey != live.appKey || was.propsJson != live.propsJson)
      return true;
  }
  return false;
}

}  // namespace jsui
