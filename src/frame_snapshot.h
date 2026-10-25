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
// them.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/node.h"

namespace jsui {

class Runtime;

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
  std::vector<SurfaceInfo> surfaces;  // [0]: the main surface
  const SurfaceInfo* surface(uint32_t tag) const;

  // Layout of every node attached to a surface: relative to the parent
  // (measure's x / y) and to the viewport (pageX / pageY, scrolling
  // included, transforms not), as Node::layoutRect / absoluteRect.
  struct Layout {
    Rect local, page;
  };
  std::unordered_map<uint32_t, Layout> layouts;

  // Native Animated: raw values of the value nodes, and the velocity of each
  // running animation by animation id.
  std::unordered_map<int, double> values;
  std::unordered_map<int, double> animations;

  // Takes one of `rt` (owner thread, after layout).
  static std::shared_ptr<FrameSnapshot> take(Runtime& rt);
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
