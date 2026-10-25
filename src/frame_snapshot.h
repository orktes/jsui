// The frame snapshot: what JS may read synchronously of the owner thread's
// state, copied at the end of a tick (see docs/ARCHITECTURE.md "Shared
// state"). In the runtime's JS-thread mode the bridge's sync reads come from
// here instead of the live tree, which a JS thread cannot touch:
// `measure` (layout rects), `viewport` / `surfaceApp` (the surface table),
// `animated.getValue` and `stopAnimation`'s velocity (native Animated).
// The answer is one frame old at most; RN-style async measure callbacks
// (measureAsync) get the exact layout of the committed tree instead.
//
// A snapshot is immutable once published; readers keep the shared_ptr they
// got for as long as they read it, so publishing a new one never blocks on
// them. Its tables are shared between snapshots while they do not change:
// a tick that moved nothing publishes nothing, and one that only advanced
// native Animated copies the value table alone (no per-node allocations on
// the owner thread).
#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "core/node.h"

namespace jsui {

class Runtime;

// Rows sorted by key: one allocation, binary searched.
template <class K, class V>
struct SortedTable {
  std::vector<std::pair<K, V>> rows;
  const V* find(K key) const {
    auto it = std::lower_bound(rows.begin(), rows.end(), key, [](const std::pair<K, V>& r, K k) { return r.first < k; });
    return it != rows.end() && it->first == key ? &it->second : nullptr;
  }
};

struct FrameSnapshot {
  uint64_t frame = 0;
  // Command batches applied before it was taken (Runtime::appliedBatches):
  // a JS write recorded in a later batch is newer than what it shows.
  uint64_t appliedBatches = 0;

  struct SurfaceInfo {
    uint32_t tag = 0;
    float width = 0, height = 0, scale = 1;
    std::string appKey, propsJson;
  };
  using Surfaces = std::vector<SurfaceInfo>;
  std::shared_ptr<const Surfaces> surfaces;  // [0]: the main surface; never null
  const SurfaceInfo* surface(uint32_t tag) const;

  // Layout of every node attached to a surface (once JS has measured:
  // Runtime::snapshotLayouts; empty before): relative to the parent
  // (measure's x / y) and to the viewport (pageX / pageY, scrolling
  // included, transforms not), as Node::layoutRect / absoluteRect.
  struct Layout {
    Rect local, page;
    bool operator==(const Layout& o) const {
      return local.x == o.local.x && local.y == o.local.y && local.width == o.local.width &&
             local.height == o.local.height && page.x == o.page.x && page.y == o.page.y;
    }
  };
  using Layouts = SortedTable<uint32_t, Layout>;
  using Numbers = SortedTable<int, double>;
  std::shared_ptr<const Layouts> layouts;  // never null

  // Native Animated: raw values of the value nodes, and the velocity of each
  // running animation by animation id.
  std::shared_ptr<const Numbers> values, animations;  // never null

  // Reused buffers of the owner thread (Runtime::snapshotScratch): each
  // table's rows as last collected, in collection order (the tree walk, the
  // animated node map), so an unchanged frame is told apart by one compare,
  // without sorting or allocating.
  struct Scratch {
    std::vector<std::pair<uint32_t, Layout>> layouts, lastLayouts;
    std::vector<std::pair<int, double>> values, lastValues, animations, lastAnimations;
    Surfaces surfaces;
  };
  // Takes one of `rt` (owner thread, after layout), sharing the tables of
  // `prev` that did not change. nullptr when nothing did (`prev` stands).
  static std::shared_ptr<FrameSnapshot> take(Runtime& rt, const FrameSnapshot* prev, Scratch& scratch);
  // Whether the surface table (tags, sizes, apps) changed since `s`.
  static bool surfacesChanged(const Runtime& rt, const FrameSnapshot* s);
};

// The current snapshot, swapped under a lock.
class SnapshotStore {
 public:
  void publish(std::shared_ptr<const FrameSnapshot> s) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_.swap(s);
  }
  // nullptr before the first publish.
  std::shared_ptr<const FrameSnapshot> current() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_;
  }
  void clear() { publish(nullptr); }

 private:
  mutable std::mutex mutex_;
  std::shared_ptr<const FrameSnapshot> current_;
};

}  // namespace jsui
