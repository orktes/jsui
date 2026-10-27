// Native components (NodeType::Component): view types the host registers
// by name and draws with its own code (jsui.h "Native components"; JS:
// requireNativeComponent / codegenNativeComponent in @orktes/jsui-react).
//
// A component view is a View plus what its class's `draw` callback paints
// into the content box. The React wrapper renders the host element
// "NativeComponentView" with the class name in the `component` prop; plain
// props cross the bridge as usual, Animated ones as paint-only style keys
// `prop:<name>` (kGroupComponent), which the prop readers (jsui_prop_*)
// prefer, so a running animation repaints the view with no React render.
//
// NativeComponents (on the Runtime, Document::components) owns the classes
// and knows every component view. A view binds to its class when both
// exist (whichever comes first: mount or jsui_register_component): `create`
// then `props` run, and `destroy` when it unmounts, its name changes or the
// class goes away. Unbound views paint only their View part.
//
// Painting calls `draw` while the painter records the view (the recording
// is reused until the view's paint version changes): props, layout,
// jsui_component_invalidate, JSUI_COMPONENT_CLOCKED (every tick) and image /
// host texture arrivals repaint it. Events from jsui_component_emit wait
// for the next tick (a draw may emit), then reach JS as `componentevent`
// ({name, data}), which the wrapper turns into the view's on<Name> prop.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
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

class NativeComponents {
 public:
  explicit NativeComponents(Runtime& rt);
  ~NativeComponents();
  NativeComponents(const NativeComponents&) = delete;
  NativeComponents& operator=(const NativeComponents&) = delete;

  struct Class {
    std::string name;
    JsuiComponentClass cls{};
  };

  // jsui_register_component / jsui_unregister_component.
  int registerClass(const std::string& name, const JsuiComponentClass& cls);
  bool unregisterClass(const std::string& name);
  // Whether `name` is registered. Any thread (JS asks from its own).
  bool has(const std::string& name) const;
  std::shared_ptr<const Class> find(const std::string& name) const;

  // jsui_component_invalidate: view != 0 that view, else every view of
  // class `name` (nullptr: all). Returns the number marked.
  int invalidate(uint32_t view, const char* name);
  // jsui_component_emit (queued until deliverEvents). -1: unknown view.
  int emit(uint32_t view, const std::string& event, const std::string& json);
  // A command from JS (ref method / dispatchCommand). False when the view
  // is unbound or its class takes no commands.
  bool command(Node* node, const std::string& name, const Value& args);

  // Repaints the views of clocked classes. Runs every tick.
  void tick(double nowMs);
  // Views of clocked classes are mounted (or events wait).
  bool animating() const;
  double now() const { return now_; }

  struct Event {
    uint32_t node = 0;
    std::string name;
    std::string json;
  };
  std::vector<Event> events;

  // Views (component::NodeState registers / unregisters).
  void addView(Node* node);
  void removeView(Node* node);
  Runtime& runtime() { return rt_; }

 private:
  Runtime& rt_;
  mutable std::mutex mutex_;  // classes_ (has() / find() from the JS thread)
  std::unordered_map<std::string, std::shared_ptr<const Class>> classes_;
  std::unordered_set<Node*> views_;
  double now_ = 0;
};

namespace component {

// Per-view state, kept in Node::imageCache.
struct NodeState : Node::Cache {
  ~NodeState() override;
  Node* node = nullptr;
  NativeComponents* owner = nullptr;
  std::string name;                        // the `component` prop
  std::shared_ptr<const NativeComponents::Class> cls;  // bound class (null: unbound)
  void* instance = nullptr;                // what create returned
  std::vector<std::string> textures;       // host textures drawn (registered users)
};

NodeState* ensure(Node* node);
// Node::setProps: binds / rebinds and calls the class's `props`.
void propsChanged(Node* node, const Value& old);
// (Re)binds the view to the class registered under its name, if any.
void bind(NodeState* st);
// Calls `destroy` and forgets the instance.
void unbind(NodeState* st);
// The view left the tree (Document::onNodeDestroyed): `destroy` now, not
// when the node's memory is freed later.
void detach(Node* node);
// The class is clocked (repaints every frame).
bool clocked(const Node* node);
// Paints the class's drawing into `box` (the content box), clipped to `clip`.
void paint(Node* node, SkCanvas* canvas, const SkRect& box, const SkRRect& clip);
// The props a class sees: no style, handlers or bridge-internal keys.
Value publicProps(const Value& props);

}  // namespace component
}  // namespace jsui
