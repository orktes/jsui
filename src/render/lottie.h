// LottieView (NodeType::Lottie): Lottie (Bodymovin JSON) animations played
// natively with Skia's Skottie module.
//
// Sources are loaded through the ResourceLoader ({uri}: files, http, data:
// URIs, the embedder's loader) or handed over by JS once
// (source={require('./anim.json')}: LottieCache::addData returns a key the
// view uses as its source). A composition is parsed once per source (and
// colorFilters set) and shared by every view showing it.
//
// Playback runs on the runtime clock: LottieCache::tick() (Runtime::tick and
// jsui_tick_native) advances the playing views and repaints just those
// nodes; JS is involved only for load / error / finish events and the
// imperative play / pause / resume / reset. A `progress` (style key
// lottieProgress, which native Animated can drive) shows that frame instead.
//
// Built without Skottie (JSUI_LOTTIE off), every source fails to load with
// an error saying so and views paint nothing.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/node.h"
#include "core/value.h"

class SkCanvas;
struct SkRect;
class SkRRect;

namespace jsui {

class Runtime;

class LottieCache {
 public:
  explicit LottieCache(Runtime& rt);
  ~LottieCache();
  LottieCache(const LottieCache&) = delete;
  LottieCache& operator=(const LottieCache&) = delete;

  // True when this build plays Lottie (linked with Skottie).
  static bool available();

  // Inline composition JSON (from JS): returns the source key views use for
  // it ("lottie:<hash>"). The same JSON gives the same key.
  std::string addData(std::string json);
  // Resolves a source URI against the runtime's base URL (data: URIs and
  // addData keys are returned as they are).
  std::string resolve(const std::string& uri) const;

  enum class State { Loading, Ready, Failed };
  // Starts loading `url` on the first request.
  State request(const std::string& url);
  // Why `url` failed ("" otherwise).
  std::string error(const std::string& url) const;

  // A parsed composition (with the color overrides of one colorFilters set).
  struct Composition;
  // nullptr unless `url` is ready and parses. `filters`: the colorFilters
  // prop ([{keypath, color}], null for none).
  Composition* get(const std::string& url, const Value& filters);
  // Size of the composition in px (0 x 0 unless ready).
  bool size(const std::string& url, float* width, float* height);
  // Frames of `url`: in point, out point, frames per second.
  bool frames(const std::string& url, double* inPoint, double* outPoint, double* fps);

  // Reload: drops everything loaded from URLs (their loads were cancelled);
  // inline data stays (it is keyed by its contents).
  void clear();

  // Advances playing views to `nowMs`, starts autoPlay views whose source
  // just loaded and queues their events. Before layout (a loaded source
  // gives its views an intrinsic size).
  void tick(double nowMs);
  // Views are playing (or waiting to report a load): frames are wanted.
  bool animating() const;

  struct Event {
    enum class Kind : uint8_t { Loaded, Failed, Finished };
    uint32_t node = 0;
    Kind kind = Kind::Loaded;
    bool cancelled = false;  // Finished: stopped before its end (play / reset)
    std::string error;       // Failed
  };
  // Pending JS events (delivered by the Runtime after each tick).
  std::vector<Event> events;

  // Views (lottie::NodeState registers / unregisters).
  void addView(Node* node);
  void removeView(Node* node);

  // Bumped when a source finishes loading (or fails).
  uint32_t generation = 0;

  struct Entry;

 private:
  void loaded(const std::string& url);
  Entry* find(const std::string& url);
  const Entry* find(const std::string& url) const;

  Runtime& rt_;
  std::unordered_map<std::string, std::unique_ptr<Entry>> entries_;
  std::unordered_set<Node*> views_;
  double now_ = 0;
};

namespace lottie {

// Per-view playback state, kept in Node::imageCache.
struct NodeState : Node::Cache {
  ~NodeState() override;
  Node* node = nullptr;
  LottieCache* cache = nullptr;
  std::string url;           // resolved source ("" = none)
  bool notified = false;     // load / error reported for this source
  bool playing = false;
  bool anchored = false;     // anchorTime / anchorPos are set
  double from = 0, to = 1;   // segment being played (progress; to < from plays it backwards)
  double pos = 0;            // current progress in [0, 1]
  double anchorTime = 0, anchorPos = 0;
  float speed = 1;
  bool loop = true;
  bool autoPlay = false;
  // Imperative calls made before the source loaded, applied when it does.
  struct Pending {
    bool set = false;
    std::string cmd;
    double a = 0, b = 0;
  } pending;
};

// Installs the intrinsic-size measure function on LottieView nodes.
void installMeasure(Node* node);
// The node's state (created on first use, from its props).
NodeState* ensure(Node* node);
// Node::setProps: picks up source / playback prop changes.
void propsChanged(Node* node, const Value& old, bool* relayout);
// play (a, b: start / end frame, NaN: the whole composition), pause, resume,
// reset. False for an unknown command.
bool command(Node* node, const std::string& cmd, double a, double b);
// Progress shown now (the progress style when set, else the playhead).
double progress(Node* node);
// Paints the current frame into `box` (the padding box), clipped to `clip`.
void paint(Node* node, SkCanvas* canvas, const SkRect& box, const SkRRect& clip);

}  // namespace lottie
}  // namespace jsui
