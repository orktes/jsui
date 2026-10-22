// Per-runtime frame statistics: work counters (always on, a few increments
// per operation) and, when `profiling` is set, exclusive wall time per frame
// phase. Used by tools/jsui-bench and by the deterministic perf regression
// tests (tests/native/perf_test.cpp), which assert on counters, never on time.
//
// Phases nest: entering a phase pauses the enclosing one, so the times are
// exclusive and add up to the time spent inside any scope. E.g. a setProps
// from a React commit is Js -> Commit -> Style.
#pragma once

#include <chrono>
#include <cstdint>

namespace jsui {

enum class Phase : uint8_t {
  Other,     // outside every scope
  Js,        // QuickJS execution (timers, rAF, jobs, events, eval)
  Commit,    // native side of __jsui_native tree / prop mutations
  Animated,  // native-driven Animated evaluation
  Style,     // applyStyle (style objects -> ComputedStyle + Yoga)
  Layout,    // Yoga + text measurement + layout animations
  Input,     // hit testing / event routing (JS handlers count as Js)
  Paint,     // rendering
  Count
};

struct PerfCounters {
  uint64_t bridgeCalls = 0;      // __jsui_native mutation calls (create/setProps/patchStyle/setText/tree edits)
  uint64_t setProps = 0;         // Node::setProps calls
  uint64_t setPropsSkipped = 0;  // ... that changed nothing
  uint64_t styleResolves = 0;    // applyStyle runs
  uint64_t valueConversions = 0; // JS objects / arrays converted to Value
  uint64_t layoutPasses = 0;     // YGNodeCalculateLayout runs
  uint64_t textMeasures = 0;     // Yoga measure callbacks for Text / TextInput
  uint64_t paragraphBuilds = 0;  // text shaping (ParagraphBuilder::Build)
  uint64_t animatedWrites = 0;   // views written by native Animated
  uint64_t events = 0;           // events dispatched to JS
};

// Paragraph::layout runs (process-wide: the text cache has no document).
inline uint64_t gParagraphLayouts = 0;

class Perf {
 public:
  using Clock = std::chrono::steady_clock;
  PerfCounters counters;
  bool profiling = false;
  // Accumulated exclusive time per phase in microseconds (reset by the caller).
  double us[size_t(Phase::Count)] = {};

  void reset() {
    counters = PerfCounters();
    for (double& v : us) v = 0;
  }

  // RAII phase scope; a no-op unless profiling.
  class Scope {
   public:
    Scope(Perf& p, Phase ph) : p_(p.profiling ? &p : nullptr) {
      if (p_) p_->enter(ph, &prev_);
    }
    ~Scope() {
      if (p_) p_->leave(prev_);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

   private:
    Perf* p_;
    Phase prev_ = Phase::Other;
  };

 private:
  void enter(Phase ph, Phase* prev) {
    Clock::time_point now = Clock::now();
    us[size_t(current_)] += std::chrono::duration<double, std::micro>(now - t0_).count();
    *prev = current_;
    current_ = ph;
    t0_ = now;
  }
  void leave(Phase prev) {
    Clock::time_point now = Clock::now();
    us[size_t(current_)] += std::chrono::duration<double, std::micro>(now - t0_).count();
    current_ = prev;
    t0_ = now;
  }
  Phase current_ = Phase::Other;
  Clock::time_point t0_ = Clock::now();
};

}  // namespace jsui
