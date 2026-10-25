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

#include "core/node.h"
#include "core/value.h"
#include "host_buffers.h"
#include "host_state.h"

namespace jsui {

class Document;

class NativeAnimated {
 public:
  explicit NativeAnimated(Document& doc) : doc_(doc) {}

  // Resolves view ids when views live in several documents (surfaces of one
  // runtime); unset: the document given to the constructor.
  std::function<Node*(uint32_t)> findView;
  // Frame data read by hostBuffer nodes (the runtime's).
  const HostBuffers* buffers = nullptr;
  const HostState* state = nullptr;

  // ---- graph (tags are chosen by JS) ---------------------------------
  // config.type: value {value, offset} | interpolation {inputRange,
  // outputType: number|color|string, outputs: [[...] per output channel],
  // parts (string template), extrapolateLeft/Right, easing: [samples]} |
  // addition | subtraction | multiplication | division {input: [a, b]} |
  // modulus {input, modulus} | diffclamp {input, min, max} |
  // color {r, g, b, a} | hostBuffer {key, index} (element `index` of a
  // frame data buffer, 0 when out of range) | clock (the frame clock in
  // seconds) | hostState {key, path: [...]} (a value inside shared host
  // state: number, bool as 1 / 0, string for text) | interaction {key, instant, state: hovered|pressed|focused|
  // focusVisible|disabled} (that state's 0..1 weight on the element with
  // that `interaction` key, core/state_style.h) | format {input, decimals (-1: shortest), grouping, prefix,
  // suffix, minDigits} (a number as text) | expression {input: [tags], code: [...], slots} (a stack
  // program compiled by @orktes/jsui-react's animated/expression.js from an
  // Animated.derive function; ops below) | transform {transforms:
  // [{property, nodeTag} | {property, value}]} | style {style: {key: tag}} |
  // props {props: {style: tag, text: tag}} (text: the content of the
  // view's first RawText child, for Animated.Text's `text` prop).
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
  // decay {velocity, deceleration, delay}; all take iterations (-1: forever)
  // | clock {speed, running} (Animated.clock({...})): the value counts
  // seconds times `speed` from where it is, until stopped (never ends);
  // `running`: a node tag whose output, while 0, holds it (it then neither
  // moves nor keeps the host ticking: animating() leaves it out, as it does
  // a clock nothing reads).
  // Starts at `nowMs` (the runtime clock when JS started it). A running
  // animation on the same node is stopped (its end is reported unfinished).
  // config.type group {root: step} runs a whole composite natively (tag is
  // ignored): step = {type: leaf, tag, config} | {type: wait, duration} |
  // {type: sequence | parallel, children: [step], stopTogether} |
  // {type: loop, child: step, iterations, resets: [[tag, value]]} (the
  // values set before each iteration, resetBeforeIteration). Steps advance
  // on the frame they end, like the JS composites' end callbacks; only the
  // group's own end is reported.
  bool startAnimation(int id, int tag, const Value& config, double nowMs, std::string* error = nullptr);
  // Stops an animation (no end event: the caller reports it). Returns false
  // when it was not running. *lastVelocity: a spring's velocity (units/s),
  // which a spring started next on the same value continues with. `nowMs`
  // (the runtime clock when JS stopped it): a clock animation first counts
  // up to then, so pausing and changing speed lose no time.
  bool stopAnimation(int id, double* lastVelocity = nullptr, double nowMs = -1);
  // For the frame snapshot (frame_snapshot.h): the raw value of every
  // value node, and the velocity of every running animation by id (a
  // spring's, else 0; a composite group's: 0).
  void snapshotValues(std::vector<std::pair<int, double>>* out) const;
  void snapshotAnimations(std::vector<std::pair<int, double>>* out) const;
  // Running animations (but a held clock, or one nothing reads), or a clock
  // node something depends on.
  bool animating() const;
  size_t animationCount() const { return animations_.size(); }

  // Buffer `key` has new contents: its hostBuffer nodes are re-evaluated at
  // the next flush (tick).
  void bufferChanged(const std::string& key);
  // Host state `key` changed: its hostState nodes re-read it.
  void stateChanged(const std::string& key);

  // ---- events (Animated.event with useNativeDriver) ------------------------
  // `eventType` is the native event type ("scroll", ...), `path` the keys
  // below the event object (e.g. ["contentOffset", "y"]).
  void addEventMapping(uint32_t viewId, const std::string& eventType, std::vector<std::string> path, int tag);
  void removeEventMapping(uint32_t viewId, const std::string& eventType, int tag);
  // Applies the mappings for a native event before it reaches JS.
  void handleEvent(const Value& event);
  // A mapping listens to `eventType` events of view `viewId`.
  bool hasEventMapping(uint32_t viewId, const char* eventType) const;

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
    Color, HostBuffer, HostState, Clock, Interaction, Expression, Format, Transform, Style, Props
  };
  // Expression ops (packages/react/src/animated/expression.js OP).
  enum class Op : uint8_t {
    Const = 0, Input = 1, Load = 2, Store = 3,
    Add = 10, Sub = 11, Mul = 12, Div = 13, Mod = 14, Pow = 15, Neg = 16, Not = 17,
    Lt = 20, Le = 21, Gt = 22, Ge = 23, Eq = 24, Ne = 25, And = 26, Or = 27, Select = 28,
    Call = 40,
  };
  struct Instr {
    Op op;
    uint8_t fn = 0;  // Call: function id
    uint32_t index = 0;  // Input / Load / Store
    double k = 0;  // Const
  };
  enum class Extrapolate : uint8_t { Extend, Clamp, Identity };
  enum class OutKind : uint8_t { Number, Color, String };
  struct TransformEntry {
    std::string property;
    int tag = 0;          // animated input, or 0
    Value staticValue;    // when tag == 0
  };
  struct StyleEntry {
    std::string key;
    int tag = 0;
    uint32_t group = 0;  // paintStyleGroup(key), resolved once
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
    // hostBuffer (the entry is cached while buffers->generation() holds)
    std::string bufferKey;
    size_t bufferIndex = 0;
    const HostBuffers::Entry* bufferEntry = nullptr;
    uint64_t bufferGen = 0;
    // interaction: the element's `interaction` key and the StateSlot read
    std::string interactionKey;
    int slot = 0;
    // format
    int decimals = -1, minDigits = 0;
    std::string grouping, prefix, suffix, timePattern;
    // props: the text input (0: none)
    int textTag = 0;
    int visibleTag = 0;  // props: Visibility's `visible` (0: hidden)
    // hostState
    std::vector<std::string> statePath;
    bool instant = false;  // interaction: the state itself (0 / 1), not its eased weight
    // expression
    std::vector<Instr> code;
    uint32_t slots = 0, stackSize = 0;
    // transform / style
    std::vector<TransformEntry> transforms;
    std::vector<StyleEntry> style;
    // props
    int styleTag = 0;
    uint32_t view = 0;
    uint32_t viewPaintVersion = 0;  // the view's paintVersion after our last write
    bool viewWritten = false;
    // evaluation cache. `out` keeps its containers and string buffer from
    // frame to frame (updated in place), so evaluating allocates nothing
    // once a node has been evaluated.
    uint64_t evalGen = 0;
    double num = 0;
    Value out;
    std::string str;  // scratch for string outputs
    // flush bookkeeping (instead of hash sets, which allocate per insert)
    uint64_t visitGen = 0;     // flush walk that last visited the node
    bool queued = false;       // in dirtyValues_
    bool notifyQueued = false; // in dirtyNotify_
  };
  struct Anim {
    enum class Kind : uint8_t { Frames, Spring, Decay, Wait, Clock } kind = Kind::Frames;
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
    // clock: value = from + (t - start) / 1000 * speed while not held
    double speed = 1;
    int runningTag = 0;  // holds the clock while its output is 0 (0: none)
    bool held = false;
  };
  struct Mapping {
    uint32_t view;
    std::string type;
    std::vector<std::string> path;
    int tag;
  };

  ANode* node(int tag);
  Node* viewOf(const ANode& props);
  void markDirty(int tag);
  void markDirty(ANode& n);
  void markNotify(ANode& n);
  void setRgba(ANode& n, double r, double g, double b, double a);
  double evalNum(int tag);
  const Value& evalOut(int tag);
  void evaluate(ANode& n);
  double interpolate(ANode& n, size_t channel, double input);
  void applyProps(ANode& props);
  void beginIteration(Anim& a, double nowMs);
  // Advances one animation; returns true when it finished (all iterations).
  bool step(Anim& a, double nowMs);
  void stopAnimationsOf(int tag, bool report);
  bool compileExpression(ANode& n, const Value& config, std::string* error);
  bool parseAnim(Anim& a, const Value& config, std::string* error);
  // Composite groups. A leaf's animation has a negative id (leafDone_ holds
  // what happens when it ends).
  struct Step {
    enum class Kind : uint8_t { Leaf, Wait, Sequence, Parallel, Loop } kind = Kind::Leaf;
    int tag = 0;
    Value config;
    double wait = 0;
    bool stopTogether = true;
    int iterations = 1;
    std::vector<std::pair<int, double>> resets;
    std::vector<Step> children;
  };
  struct Group {
    int id = 0;
    Step root;
    std::vector<int> leaves;  // running leaf animation ids
    bool done = false;
    int zeroRounds = 0;       // iterations ended without time passing (runaway guard)
    double lastRound = -1;
  };
  bool parseStep(Step& s, const Value& c, std::string* error);
  using Done = std::function<void(bool finished, double nowMs)>;
  void runStep(Group& g, const Step& s, double nowMs, Done done);
  void endGroup(int id, bool finished);
  void leafEnded(int leafId, bool finished, double nowMs);
  Group* group(int id);
  void purgeGroups();
  std::vector<std::unique_ptr<Group>> groups_;
  std::unordered_map<int, Done> leafDone_;
  std::unordered_map<int, int> leafGroup_;  // leaf id -> group id
  int nextLeafId_ = -1;
  double runExpression(ANode& n);
  void formatNumber(ANode& n, double v);
  size_t liveClocks() const;

  Document& doc_;
  std::unordered_map<int, ANode> nodes_;
  std::vector<int> propsNodes_;  // tags of the Props nodes in nodes_
  // flush() scratch, kept between frames.
  std::vector<int> stack_;
  std::vector<ANode*> props_;
  std::vector<Node::StyleWrite> writes_;
  uint64_t visitGen_ = 0;
  // Views are re-checked for React re-props (paintVersion) only when some
  // Node::setProps / patchStyle ran since the last check (Perf::propsEpoch)
  // or a props node was connected.
  uint64_t propsEpoch_ = 0;
  bool recheckViews_ = true;
  std::vector<Anim> animations_;
  std::vector<Mapping> mappings_;
  std::unordered_set<int> listening_;
  std::vector<int> dirtyValues_;  // nodes changed since the last flush (ANode::queued)
  std::vector<int> dirtyNotify_;  // values changed natively (listener updates; ANode::notifyQueued)
  std::unordered_map<std::string, std::vector<int>> bufferNodes_;  // key -> hostBuffer node tags
  std::unordered_map<std::string, std::vector<int>> stateNodes_;   // key -> hostState node tags
  std::vector<int> clockNodes_;  // tags of the Clock nodes
  std::vector<int> interactionNodes_;  // tags of the Interaction nodes
  double interactionWeight(const ANode& n) const;
  double clockMs_ = 0;           // the last tick's time
  std::vector<double> exprStack_, exprSlots_;  // runExpression scratch
  uint64_t gen_ = 1;
};

// Shared with tests: JS-compatible number formatting for string outputs.
std::string formatAnimatedNumber(double v);

}  // namespace jsui
