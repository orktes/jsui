// ShaderView (NodeType::Shader): a view painted by a user-written SkSL
// shader (Skia runtime effects, https://skia.org/docs/user/sksl/).
//
// The source is compiled once per distinct text and shared by every view
// using it (ShaderEffects). Each frame a view's uniforms come from:
//   - the `uniform:<name>` style keys (and `uniform:<name>.<i>` for one
//     component), which are paint-only, so Animated (also the native driver
//     and Animated.hostBuffer) writes them without JS per frame;
//   - the `uniforms` prop ({name: number | number[] | color});
//   - built-ins the runtime fills in when the shader declares them and no
//     value is given: `time` (float, seconds since the view mounted, frozen
//     while `paused`), `resolution` (float2, the view's size in px) and
//     `pixelRatio` (float, device px per px).
// A shader declaring `time` repaints every frame on the runtime clock.
//
// Child shaders (`uniform shader name;`) come from the `images` prop
// ({name: {uri} | {texture} (a host texture), fit, tile}), fitted to the
// view like an Image with that resizeMode (default cover). A child named
// `content` that `images` does not give is the view's own painting (its
// background and children): the view then runs the shader as an image
// filter over itself (post-processing: CRT, ripple, dissolve...).
//
// Coordinates are px with (0, 0) at the top-left of the padding box (fill)
// or the border box (content). Compile errors are reported once per view as
// an `error` event; such a view paints nothing of its own.
//
// Preloading (precompileShaders() in JS, jsui_precompile_shader): a source
// is compiled ahead of use and drawn once off-screen in the next GPU frame,
// the way a ShaderView would draw it, so the GPU builds its pipelines then
// (Graphite on Metal / Vulkan / Dawn / WebGPU, Ganesh on GL / WebGL) rather
// than in the frame where a view first shows it: no hitch on first use.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/node.h"
#include "core/value.h"
#include "include/core/SkRefCnt.h"

class SkCanvas;
class SkImageFilter;
class SkRuntimeEffect;
struct SkRect;
class SkRRect;

namespace jsui {

class Runtime;

class ShaderEffects {
 public:
  explicit ShaderEffects(Runtime& rt);
  ~ShaderEffects();
  ShaderEffects(const ShaderEffects&) = delete;
  ShaderEffects& operator=(const ShaderEffects&) = delete;

  // A compiled source (or why it failed).
  struct Program {
    sk_sp<SkRuntimeEffect> effect;  // nullptr when it failed to compile
    std::string error;
    bool usesTime = false;          // declares `uniform float time`
    bool content = false;           // declares `uniform shader content`
  };
  // Compiles `sksl` on first use; later calls share the result.
  std::shared_ptr<const Program> get(const std::string& sksl);

  // Compiles `sksl` now and queues its warm-up draw. `images` is what the
  // view's `images` prop will be (which children are images, their fit and
  // tile mode), so the warm-up samples the same kind of child.
  std::shared_ptr<const Program> preload(const std::string& sksl, const Value& images = Value());
  // Shaders waiting for their warm-up (the next frame painted).
  size_t warmPending() const { return warm_.size(); }
  // The surfaces call it with the canvas of each frame they paint. On a GPU
  // canvas it draws every queued shader into a small off-screen surface of
  // the canvas's format (nothing shows); the frame's flush then builds the
  // pipelines. A raster canvas has nothing to warm: the queue is dropped.
  // Returns whether it drew (the frame must reach the GPU).
  bool warm(SkCanvas* canvas);

  // Repaints the views whose shader runs on the clock and queues compile
  // errors. Runs every tick.
  void tick(double nowMs);
  // Views animate on the clock (or errors wait to be reported).
  bool animating() const;
  double now() const { return now_; }

  struct Event {
    uint32_t node = 0;
    std::string error;
  };
  std::vector<Event> events;

  // Views (shader::NodeState registers / unregisters).
  void addView(Node* node);
  void removeView(Node* node);

 private:
  std::unordered_map<std::string, std::shared_ptr<const Program>> programs_;
  std::unordered_set<Node*> views_;
  double now_ = 0;
  struct Warm {
    std::shared_ptr<const Program> program;
    Value images;
  };
  std::vector<Warm> warm_;
};

namespace shader {

// Per-view state, kept in Node::imageCache.
struct NodeState : Node::Cache {
  ~NodeState() override;
  Node* node = nullptr;
  ShaderEffects* effects = nullptr;
  std::shared_ptr<const ShaderEffects::Program> program;
  bool reported = false;   // compile error delivered
  double start = 0;        // runtime ms at which time = 0
  double frozen = -1;      // paused: the time shown (s); < 0 running
  std::vector<std::string> textures;  // host textures used as children (registered users)
};

NodeState* ensure(Node* node);
// Node::setProps: picks up source / paused / images changes.
void propsChanged(Node* node, const Value& old);
// The shader takes the view's own painting as its `content` child.
bool wantsContent(Node* node);
// The shader's `time` advances with the clock (repaint every frame).
bool clocked(Node* node);
// Fill: paints the shader into `box` (the padding box), clipped to `clip`.
void paint(Node* node, SkCanvas* canvas, const SkRect& box, const SkRRect& clip);
// Content: the image filter that runs the shader over the view's painting
// (`box`: the border box, in the layer's local space). nullptr when the
// shader cannot run.
sk_sp<SkImageFilter> contentFilter(Node* node, const SkRect& box);

}  // namespace shader
}  // namespace jsui
