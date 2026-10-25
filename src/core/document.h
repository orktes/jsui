// Owns all nodes of one runtime, the root container and the layout pass.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/node.h"
#include "core/perf.h"

class SkMatrix;

namespace jsui {

class FontManager;
class ImageCache;
class HostTextures;
class LottieCache;
class ShaderEffects;
class ShaderEffects;

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
  // `sharedNextId`: the node id counter of the runtime when several
  // documents (surfaces) share one JS context, so ids stay unique across
  // them; nullptr: the document numbers its own nodes.
  // Surfaces of one runtime share the node id counter and the perf stats.
  // The counter is atomic: the JS bridge takes ids for the nodes its
  // commands create later (js/command_buffer.h).
  explicit Document(std::atomic<uint32_t>* sharedNextId = nullptr, Perf* sharedPerf = nullptr);
  ~Document();

  // The root container (id 1). The reconciler appends its top level here.
  Node* root() const { return root_; }

  Node* createNode(NodeType type);
  // With an id taken from the counter earlier (the bridge's Create command).
  // nullptr when the id is 0 or already in use.
  Node* createNode(NodeType type, uint32_t id);
  Node* get(uint32_t id) const;
  // Destroys a detached node and its subtree. The nodes leave the document
  // at once; their memory is released by freeDestroyed().
  void destroyNode(Node* node);
  // Frees up to `max` destroyed nodes; true while more are waiting.
  bool freeDestroyed(size_t max);
  bool hasDestroyed() const { return !graveyard_.empty(); }
  // Destroys everything except the root (used on reload).
  void clear();

  // A width or height <= 0 sizes that axis to the content: layout leaves it
  // unconstrained and width() / height() report the laid-out size.
  void setViewport(float width, float height, float pixelRatio);
  float width() const { return width_; }
  float height() const { return height_; }
  bool autoWidth() const { return autoWidth_; }
  bool autoHeight() const { return autoHeight_; }
  float pixelRatio() const { return pixelRatio_; }

  // Runs Yoga if anything is dirty. Returns true if layout changed.
  bool layout();
  void markLayoutDirty() { layoutDirty_ = true; paintDirty_ = true; }
  void markPaintDirty() { paintDirty_ = true; }
  bool paintDirty() const { return paintDirty_; }
  void clearPaintDirty() {
    paintDirty_ = false;
    exactRepaint_ = false;
  }
  // The painter drew something approximately while it moved or scaled (a
  // cached layer at the nearest whole pixel, a shadow from the mask of a
  // nearby size): it repaints it exactly in the next frame that renders,
  // which Runtime::needsRedraw asks for once nothing animates any more (not
  // in every frame while something does, so the UI still goes idle).
  void markExactRepaint() { exactRepaint_ = true; }
  bool exactRepaintPending() const { return exactRepaint_; }

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
  HostTextures* textures = nullptr;  // host textures (HostTexture views)
  LottieCache* lotties = nullptr;    // Lottie sources and playing views (LottieView)
  ShaderEffects* shaders = nullptr;  // compiled SkSL and ShaderViews (render/shader_view.h)

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
  // Nodes with autoFocus / hasTVPreferredFocus / modal props (attached or
  // not), so focus bookkeeping does not walk the whole tree.
  const std::vector<Node*>& focusHints() const { return focusHints_; }
  void setFocusHint(Node* n, bool on);
  // Ids of Text nodes created since they were last looked at: shaped ahead
  // of their first layout in spare frame time (Runtime::prepareText).
  std::vector<uint32_t> newTexts;
  // Nodes with state styles, `group` or `interaction` (Node::stateStyles),
  // walked by InputManager::tickStates; dead ids are dropped there.
  std::vector<uint32_t> stateNodes;
  // Bumped when a node's state props change (set, parsed again or removed):
  // InputManager::tickStates then derives every target again.
  uint64_t stateVersion = 0;
  // `interaction` key -> the node that carries it (Animated.interaction).
  std::unordered_map<std::string, uint32_t> interactionNodes;
  // A state transition started outside InputManager::tickStates (Node::setHidden).
  bool statesKick = false;
  // A weight of an element with an `interaction` key changed: native
  // Animated re-reads its interaction nodes (instead of polling them).
  bool interactionsMoved = false;
  // Live nodes with an onLayout listener (Node::kListensLayout). Without
  // any, layout passes skip the onLayout walk.
  int layoutListeners = 0;

  // Overlay layer (core/overlay.h). setOverlay: `config` is the node's
  // `overlay` prop, null when it is no longer an overlay.
  void setOverlay(Node* n, const Value* config);
  void setOverlayArrow(Node* n, bool on);
  Node* overlayLayer() const { return overlayLayer_; }
  // Overlay nodes, in logical tree order as of the last layout().
  const std::vector<Node*>& overlays() const { return overlays_; }
  // Bumped by every logical insert / remove (overlay order).
  uint64_t treeVersion = 0;

  // Work counters and (when profiling) phase times; see core/perf.h.
  // Surfaces report into the main surface's.
  Perf ownPerf_;
  Perf& perf;

  // Ticks run so far (Runtime::tick); image events wait for a later tick
  // than the one that set the source, so they are always asynchronous.
  uint64_t frame = 0;

 private:
  void destroyRecursive(Node* node);
  void runYoga();
  void syncOverlays();
  bool placeOverlays();
  // layerInv: the overlay layer's viewport matrix, inverted.
  bool placeOverlay(Node* n, const SkMatrix& layerInv);
  void placeArrows(Node* n);
  void dropOverlay(Node* n);
  void afterLayout(Node* node, bool report);
  void snapshotLayout(Node* node, std::vector<std::pair<Node*, Rect>>* out);
  void startLayoutAnimations(const std::vector<std::pair<Node*, Rect>>& before);
  void startAnim(Node* n, Node::LayoutAnim::Kind kind, const LayoutAnimSpec& spec, const Rect& from);
  void finishAnim(Node* n);
  void applyAnim(Node* n, double nowMs, bool* done);

  LayoutAnimConfig armed_;
  bool armedSet_ = false;
  std::vector<uint32_t> animating_;  // nodes with a running layout animation

  std::vector<Node*> focusHints_;
  std::unordered_map<uint32_t, std::unique_ptr<Node>> nodes_;
  std::vector<std::unique_ptr<Node>> graveyard_;  // destroyed, not yet freed
  std::unordered_map<uint32_t, Rect> reportedLayout_;
  uint64_t yogaPass_ = 0;  // the last layout() pass (see afterLayout)
  Node* root_ = nullptr;
  std::atomic<uint32_t> nextId_{1};
  std::atomic<uint32_t>* idCounter_ = &nextId_;
  float width_ = 0, height_ = 0, pixelRatio_ = 1;
  bool autoWidth_ = false, autoHeight_ = false;
  bool layoutDirty_ = true, paintDirty_ = true, exactRepaint_ = false;
  uint32_t fontGeneration_ = 0;  // FontManager::generation the text caches were built with
  YGConfigRef yogaConfig_ = nullptr;
  Node* overlayLayer_ = nullptr;
  std::vector<Node*> overlays_;
  std::vector<Node*> arrows_;
  uint64_t overlayTreeVersion_ = ~uint64_t(0);
  bool overlaysChanged_ = false;
  int layerZ_ = 0;
};

}  // namespace jsui
