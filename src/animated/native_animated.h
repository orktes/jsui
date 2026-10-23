// Native Animated driver (`useNativeDriver: true`), modelled on React
// Native's NativeAnimatedModule. @orktes/jsui-react serializes the Animated graph
// (values, interpolations, math nodes, transforms, styles, props) and its
// animations (timing as a sampled easing curve, spring, decay, iterations)
// through __jsui_native.animated.*; the runtime evaluates them on its own
// frame clock in Runtime::tick() (or jsui_tick_native(), without JS) and
// writes the resulting paint-only styles straight into the connected nodes.
// JS is only involved again for end callbacks and value listeners.
#pragma once

#include <array>
#include <functional>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/value.h"

namespace jsui {

class Document;
class Node;

class NativeAnimated {
 public:
  explicit NativeAnimated(Document& doc) : doc_(doc) {}

  // Resolves view ids when views live in several documents (surfaces of one
  // runtime); unset: the document given to the constructor.
  std::function<Node*(uint32_t)> findView;

  // ---- graph (tags are chosen by JS) ---------------------------------
  // config.type: value {value, offset} | interpolation {inputRange,
  // outputType: number|color|string, outputs: [[...] per output channel],
  // parts (string template), extrapolateLeft/Right, easing: [samples]} |
  // addition | subtraction | multiplication | division {input: [a, b]} |
  // modulus {input, modulus} | diffclamp {input, min, max} |
  // color {r, g, b, a} | transform {transforms: [{property, nodeTag} |
  // {property, value}]} | style {style: {key: tag}} | props {props: {style: tag}}.
  // Returns false (and sets *error) for an unknown type.
  bool createNode(int tag, const Value& config, std::string* error = nullptr);
  void connectNodes(int parent, int child);
  void disconnectNodes(int parent, int child);
  void connectToView(int propsTag, uint32_t viewId);
  void disconnectFromView(int propsTag, uint32_t viewId);
  void dropNode(int tag);
  bool hasNode(int tag) const { return nodes_.count(tag) > 0; }

  // ---- values ------------------------------------------------------------
  // setValue stops animations on the node and applies the change to views
  // right away (like RN's setAnimatedNodeValue).
  void setValue(int tag, double v);
  void setOffset(int tag, double offset);
  void flattenOffset(int tag);
  void extractOffset(int tag);
  // Raw value (without offset) of a value node; 0 for unknown tags.
  double getValue(int tag) const;
  // Evaluated numeric output of any node (value + offset for values).
  double nodeOutput(int tag);
  // Evaluated output as it would be written into a style (number / string /
  // transform array). For tests and diagnostics.
  Value nodeStyleValue(int tag);
  void startListening(int tag) { listening_.insert(tag); }
  void stopListening(int tag) { listening_.erase(tag); }

  // ---- animations --------------------------------------------------------
  // config.type: frames {frames: [eased progress samples], duration, toValue,
  // delay} | spring {stiffness, damping, mass, initialVelocity, toValue,
  // overshootClamping, restDisplacementThreshold, restSpeedThreshold, delay} |
  // decay {velocity, deceleration, delay}; all take iterations (-1: forever).
  // Starts at `nowMs` (the runtime clock when JS started it). A running
  // animation on the same node is stopped (its end is reported unfinished).
  bool startAnimation(int id, int tag, const Value& config, double nowMs, std::string* error = nullptr);
  // Stops an animation (no end event: the caller reports it). Returns false
  // when it was not running. *lastVelocity: a spring's velocity (units/s),
  // which a spring started next on the same value continues with.
  bool stopAnimation(int id, double* lastVelocity = nullptr);
  bool animating() const { return !animations_.empty(); }
  size_t animationCount() const { return animations_.size(); }

  // ---- events (Animated.event with useNativeDriver) ------------------------
  // `eventType` is the native event type ("scroll", ...), `path` the keys
  // below the event object (e.g. ["contentOffset", "y"]).
  void addEventMapping(uint32_t viewId, const std::string& eventType, std::vector<std::string> path, int tag);
  void removeEventMapping(uint32_t viewId, const std::string& eventType, int tag);
  // Applies the mappings for a native event before it reaches JS.
  void handleEvent(const Value& event);

  // ---- frame ---------------------------------------------------------------
  // Advances animations to nowMs and writes changed props into views.
  void tick(double nowMs);
  // Writes props of nodes whose inputs changed (or whose view was re-propped
  // by React) into the views.
  void flush();

  struct EndEvent {
    int id;
    bool finished;
    double value;  // raw value of the animated node at the end
  };
  struct ValueUpdate {
    int tag;
    double value;  // raw value
  };
  // Pending JS notifications, delivered by the Runtime after each tick.
  std::vector<EndEvent> ends;
  std::vector<ValueUpdate> updates;

  // A node is going away: forget views connected to it.
  void viewDestroyed(uint32_t viewId);
  void clear();

 private:
  enum class Type : uint8_t {
    Value, Interpolation, Addition, Subtraction, Multiplication, Division, Modulus, DiffClamp,
    Color, Transform, Style, Props
  };
  enum class Extrapolate : uint8_t { Extend, Clamp, Identity };
  enum class OutKind : uint8_t { Number, Color, String };
  struct TransformEntry {
    std::string property;
    int tag = 0;          // animated input, or 0
    Value staticValue;    // when tag == 0
  };
  struct ANode {
    Type type = Type::Value;
    int tag = 0;
    std::vector<int> parents, children;
    // value
    double value = 0, offset = 0;
    // interpolation
    std::vector<double> inputRange;
    std::vector<std::vector<double>> outputs;  // [channel][range index]
    std::vector<std::string> parts;            // string template pieces
    std::vector<double> easing;                // sampled easing (empty: linear)
    OutKind outKind = OutKind::Number;
    Extrapolate left = Extrapolate::Extend, right = Extrapolate::Extend;
    // math
    std::vector<int> inputs;
    double modulus = 1, min = 0, max = 0, lastInput = 0, clamped = 0;
    bool diffInit = false;
    // color: r, g, b, a in inputs[0..3]
    // transform / style
    std::vector<TransformEntry> transforms;
    std::vector<std::pair<std::string, int>> style;
    // props
    int styleTag = 0;
    uint32_t view = 0;
    uint32_t viewPaintVersion = 0;  // the view's paintVersion after our last write
    bool viewWritten = false;
    // evaluation cache
    uint64_t evalGen = 0;
    double num = 0;
    Value out;
  };
  struct Anim {
    enum class Kind : uint8_t { Frames, Spring, Decay } kind = Kind::Frames;
    int id = 0, tag = 0;
    double start = 0, delay = 0;
    int iterations = 1, iteration = 0;
    double from = 0, to = 0;
    // frames
    std::vector<double> frames;
    double duration = 0;
    // spring
    double k = 100, c = 10, m = 1, v0 = 0, restD = 0.001, restS = 0.001;
    bool overshootClamping = false;
    double lastVelocity = 0;
    // decay
    double velocity = 0, deceleration = 0.998, last = 0;
  };
  struct Mapping {
    uint32_t view;
    std::string type;
    std::vector<std::string> path;
    int tag;
  };

  ANode* node(int tag);
  void markDirty(int tag);
  double evalNum(int tag);
  const Value& evalOut(int tag);
  void evaluate(ANode& n);
  double interpolate(ANode& n, size_t channel, double input);
  void applyProps(ANode& props);
  void beginIteration(Anim& a, double nowMs);
  // Advances one animation; returns true when it finished (all iterations).
  bool step(Anim& a, double nowMs);
  void stopAnimationsOf(int tag, bool report);

  Document& doc_;
  std::unordered_map<int, ANode> nodes_;
  std::vector<Anim> animations_;
  std::vector<Mapping> mappings_;
  std::unordered_set<int> listening_;
  std::unordered_set<int> dirtyValues_;  // value nodes changed since the last flush
  std::unordered_set<int> dirtyNotify_;  // changed natively (listener updates)
  uint64_t gen_ = 1;
};

// Shared with tests: JS-compatible number formatting for string outputs.
std::string formatAnimatedNumber(double v);

}  // namespace jsui
