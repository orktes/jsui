// JSUIContainer (NodeType::Container): a whole JSUI runtime (its own JS
// context, document, input, fonts and image cache) running inside a node of
// another one. "JSUI inside JSUI": an app that hosts apps built with JSUI.
//
//   <JSUIContainer bundleSrc="games/solitaire.js" appKey="Solitaire"
//                  initialProps={{ seed: 7 }} hostState={{ theme: 'dark' }}
//                  onMessage={(name, payload) => ...} onCall={(name, args) => ...} />
//
// The node is the child's embedder, with everything an embedder has:
//   * Loading: `bundleSrc` is resolved against the parent's base URL and
//     loaded through the parent's ResourceLoader (the embedder's loader when
//     it has one), as are every image, font and fetch() of the child.
//     `bundleData` is bundle source evaluated as it is (a prop, or a `load`
//     command: what the React component sends).
//   * Size: the node's content box (layout size minus padding and border) is
//     the child's viewport, at the parent's pixel ratio.
//   * Frames: Containers::tick ticks each child on the parent's clock, after
//     the parent's layout; a child with something new to paint repaints its
//     node (Node::markPaintDirty), so the parent's damage covers it.
//   * Painting: container::paint draws the child's frame straight into the
//     parent's canvas, under the node's clip and transform (no texture, no
//     extra GPU surface: the same on every backend).
//   * Input: InputManager routes pointer / touch / wheel over the node to the
//     child (in its coordinates) and keys, text and gamepad input while the
//     node has focus; what the child does not consume goes on in the parent.
//   * Host messaging both ways: the child's Host.postMessage, Host.call and
//     HostState changes become `containerevent` events on the node (the
//     React component turns them into onMessage / onCall / onStateChange),
//     and the parent's commands (ContainerCommand: post / call / resolve /
//     reject / state / reload) reach the child as the C API calls an
//     embedder would make.
//
// The child is isolated: it shares nothing with the parent but the loader,
// the clock and the log. Audio uses the built-in backend and <Video> has no
// handler inside a container (their handlers answer the embedder's runtime).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/node.h"
#include "core/value.h"
#include "jsui/jsui.h"

class SkCanvas;
struct SkRect;
class SkRRect;

namespace jsui {

class Runtime;

class Containers {
 public:
  explicit Containers(Runtime& rt);
  ~Containers();
  Containers(const Containers&) = delete;
  Containers& operator=(const Containers&) = delete;

  // Sizes and ticks every child runtime (Runtime::tick, after layout) and
  // repaints the nodes of the children that have something new to paint.
  void tick(double nowMs);
  // jsui_tick_native for the children.
  void tickNative(double nowMs);
  // A child wants frames (jsui_wants_frame), or has events to deliver.
  bool animating() const;
  // The earliest timer of the children (infinity: none).
  double nextTimer() const;

  // What the children told their embedder, for the parent's JS: the payload
  // of a `containerevent` on `node` (Runtime::deliverContainerEvents).
  struct Event {
    uint32_t node = 0;
    Value payload;
  };
  std::vector<Event> events;

  // Views (container::NodeState registers / unregisters).
  void addView(Node* node);
  void removeView(Node* node);
  Runtime& runtime() const { return rt_; }

 private:
  Runtime& rt_;
  std::unordered_set<Node*> views_;
};

namespace container {

// The child runtime of a JSUIContainer, kept in Node::imageCache.
struct NodeState : Node::Cache {
  ~NodeState() override;
  Node* node = nullptr;
  Containers* cache = nullptr;
  JsuiRuntime* child = nullptr;
  // The source the child runs (bundleSrc resolved against the parent's
  // base URL, or the bundleData text), its base URL, and the AppRegistry
  // component and initial props it shows (appKey empty: the bundle decides).
  std::string src, data, baseUrl, appKey, propsJson = "{}";
  // The source changed: the next tick (re)creates the child.
  bool stale = false;
  // appKey / initialProps changed: the next tick runs them.
  bool runPending = false;
  // Commands (see command()) for the next tick, in order: the child runs JS
  // only inside the parent's tick, never nested in the parent's JS.
  std::vector<Value> commands;
  // Viewport given to the child (logical px) and its pixel ratio.
  float width = -1, height = -1, pixelRatio = 0;
  // The parent's loads made for the child (cancelled with it).
  std::unordered_set<uint64_t> loads;

  // ---- the child's services, through the parent's
  // Null once this state is gone: callbacks left in the parent's Audio /
  // Video check it. `generation` tells one child runtime from the next.
  std::shared_ptr<NodeState*> life = std::make_shared<NodeState*>(this);
  uint64_t generation = 0;
  // Child voice -> the parent's voice playing it (with what the child last
  // asked, so bus changes rescale it).
  struct Voice {
    uint64_t parent;
    std::string bus;
    float volume, pitch, pan;
  };
  std::unordered_map<uint64_t, Voice> voices;
  // The child's bus gains (volume, 0 while muted): applied to its voices
  // here, so the parent's own mix stays the parent's.
  std::unordered_map<std::string, float> busGains;
  // Child video -> the parent's video playing it, and the child texture key
  // aliased to the parent's.
  struct Video {
    uint64_t parent;
    std::string texture;
  };
  std::unordered_map<uint64_t, Video> videos;
  // The parent's font generation the child last saw.
  uint32_t parentFonts = 0;
};

// Creates / replaces the child after a props change.
void propsChanged(Node* node, const Value& old);
// Paints the child's frame into `content` (the node's content box), clipped
// to `clip` (its inner border radius rrect).
void paint(Node* node, SkCanvas* canvas, const SkRect& content, const SkRRect& clip);
// A command from the parent's JS (ContainerCommand): {op: 'load', data}
// (inline bundle source) | {op: 'post', name, payload} | {op: 'call', seq,
// name, args} | {op: 'resolve', callId, value} | {op: 'reject', callId,
// message} | {op: 'state', key, value, patch, remove} | {op: 'reload'}.
// Applied in the parent's next tick (`load` replaces the child there).
// Returns false for an unknown node or command.
bool command(Node* node, const Value& cmd);
// The child runtime of a container node (nullptr: none, or not a container).
JsuiRuntime* child(const Node* node);
// A viewport point to the child's coordinates (the node's content box).
bool toChild(const Node* node, float localX, float localY, float* cx, float* cy);

}  // namespace container
}  // namespace jsui
