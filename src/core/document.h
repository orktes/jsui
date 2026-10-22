// Owns all nodes of one runtime, the root container and the layout pass.
#pragma once

#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/node.h"

namespace jsui {

class FontManager;
class ImageCache;

// LayoutAnimation (React Native): animates the next layout pass.
enum class LayoutAnimCurve : uint8_t { Linear, EaseInEaseOut, EaseIn, EaseOut, Spring, Keyboard };
enum class LayoutAnimProperty : uint8_t { Opacity, ScaleX, ScaleY, ScaleXY };
struct LayoutAnimSpec {
  bool enabled = false;
  LayoutAnimCurve curve = LayoutAnimCurve::EaseInEaseOut;
  LayoutAnimProperty property = LayoutAnimProperty::Opacity;
  double duration = 0, delay = 0;
  float springDamping = 0.5f;
};
struct LayoutAnimConfig {
  LayoutAnimSpec create, update, remove;
};
// Eased progress of a LayoutAnimation curve at linear time t in [0, 1]
// (springs overshoot 1 before settling).
float layoutAnimProgress(LayoutAnimCurve curve, float springDamping, float t);

class Document {
 public:
  Document();
  ~Document();

  // The root container (id 1). The reconciler appends its top level here.
  Node* root() const { return root_; }

  Node* createNode(NodeType type);
  Node* get(uint32_t id) const;
  // Destroys a detached node and its subtree.
  void destroyNode(Node* node);
  // Destroys everything except the root (used on reload).
  void clear();

  void setViewport(float width, float height, float pixelRatio);
  float width() const { return width_; }
  float height() const { return height_; }
  float pixelRatio() const { return pixelRatio_; }

  // Runs Yoga if anything is dirty. Returns true if layout changed.
  bool layout();
  void markLayoutDirty() { layoutDirty_ = true; paintDirty_ = true; }
  void markPaintDirty() { paintDirty_ = true; }
  bool paintDirty() const { return paintDirty_; }
  void clearPaintDirty() { paintDirty_ = false; }

  // LayoutAnimation. arm(): the next layout pass animates the frames that
  // change (`update`), fades / scales in nodes that appear (`create`), and
  // children removed while armed fade / scale out as ghosts (`remove`; see
  // removeChild()). The runtime disarms at the end of each tick, so a config
  // only applies to the commit that armed it.
  void armLayoutAnimation(const LayoutAnimConfig& config) { armed_ = config; armedSet_ = true; }
  void disarmLayoutAnimation() { armedSet_ = false; }
  bool layoutAnimationArmed() const { return armedSet_; }
  // JS removeChild: detaches `child`, or turns it into a fading ghost of
  // `parent` while a delete animation is armed.
  void removeChild(Node* parent, Node* child);
  // A ghost re-inserted into the tree stops fading and becomes a node again.
  void reviveGhost(Node* ghost);
  // Advances running layout animations to `nowMs` (marks paint dirty).
  void tickLayoutAnimations(double nowMs);
  bool layoutAnimating() const { return !animating_.empty(); }
  // Runtime clock (set every tick): layout animations start at this time.
  double now = 0;

  // Yoga configuration shared by this document's nodes (one per document:
  // no global state).
  YGConfigRef yogaConfig() const { return yogaConfig_; }

  // Services used by text measurement and painting. Set by the Runtime.
  FontManager* fonts = nullptr;
  ImageCache* images = nullptr;

  // Called after layout for nodes whose props contain `__events` with
  // "layout" when their layoutRect changed.
  std::function<void(Node*, const Rect&)> onLayout;
  // Called for every node right before it is destroyed.
  std::function<void(Node*)> onNodeDestroyed;

  // Image nodes whose source changed, waiting for their load / error (and
  // progress) events (Runtime::deliverImageEvents). One entry per node: a
  // newer source replaces the older entry.
  struct ImageEvent {
    uint32_t node = 0;
    uint64_t frame = 0;    // Document::frame when the source was set
    bool cached = false;   // decoded (or failed) already when requested
  };
  void queueImageEvents(uint32_t node, bool cached);
  std::vector<ImageEvent> imageEvents;
  // Ticks run so far (Runtime::tick); image events wait for a later tick
  // than the one that set the source, so they are always asynchronous.
  uint64_t frame = 0;

 private:
  void destroyRecursive(Node* node);
  void reportLayout(Node* node);
  void snapshotLayout(Node* node, std::vector<std::pair<Node*, Rect>>* out);
  void startLayoutAnimations(const std::vector<std::pair<Node*, Rect>>& before);
  void startAnim(Node* n, Node::LayoutAnim::Kind kind, const LayoutAnimSpec& spec, const Rect& from);
  void finishAnim(Node* n);
  void applyAnim(Node* n, double nowMs, bool* done);

  LayoutAnimConfig armed_;
  bool armedSet_ = false;
  std::vector<uint32_t> animating_;  // nodes with a running layout animation

  std::unordered_map<uint32_t, std::unique_ptr<Node>> nodes_;
  std::unordered_map<uint32_t, Rect> reportedLayout_;
  Node* root_ = nullptr;
  uint32_t nextId_ = 1;
  float width_ = 0, height_ = 0, pixelRatio_ = 1;
  bool layoutDirty_ = true, paintDirty_ = true;
  uint32_t fontGeneration_ = 0;  // FontManager::generation the text caches were built with
  YGConfigRef yogaConfig_ = nullptr;
};

}  // namespace jsui
