// C++ helpers for native components (jsui.h "Native components"):
// header-only wrappers over the C API, so a game's component is a class.
//
//   struct Radar : jsui::Component {
//     void draw(jsui::Canvas& c, const JsuiDrawInfo& info) override {
//       float r = std::min(info.width, info.height) / 2;
//       c.circle(r, r, r - 1, jsui::Paint(0xff33ff66).stroke(2));
//       float sweep = float(c.number("sweep", info.time_ms / 1000));
//       c.arc(0, 0, 2 * r, 2 * r, sweep * 360 - 40, 40, true, jsui::Paint(0x6633ff66));
//       for (const Contact& e : game.contacts()) c.circle(r + e.x, r + e.y, 3, jsui::Paint(0xffff4444));
//     }
//     void command(const char* name, const char* args) override { if (!strcmp(name, "ping")) ping(); }
//   };
//   jsui::registerComponent<Radar>(rt, "Radar", JSUI_COMPONENT_CLOCKED);
//
// One instance per mounted view (default-constructed; rt() and view() are
// set before props()). Everything runs on the runtime's thread.
#pragma once

#include <string>

#include "jsui/jsui.h"

namespace jsui {

// A paint with chainable setters (JsuiPaint with jsui_paint_init's defaults).
struct Paint : JsuiPaint {
  explicit Paint(uint32_t argb = 0xff000000u) {
    jsui_paint_init(this);
    color = argb;
  }
  Paint& fill() { style = JSUI_PAINT_FILL; return *this; }
  Paint& stroke(float width) {
    style = JSUI_PAINT_STROKE;
    stroke_width = width;
    return *this;
  }
  Paint& round() {
    cap = JSUI_CAP_ROUND;
    join = JSUI_JOIN_ROUND;
    return *this;
  }
  Paint& additive() { blend = JSUI_BLEND_PLUS; return *this; }
  Paint& glow(float sigma) { blur = sigma; return *this; }
  Paint& shader(const JsuiGradient* g) { gradient = g; return *this; }
  Paint& dashed(const float* intervals, int count, float phase = 0) {
    dash = intervals;
    dash_count = count;
    dash_phase = phase;
    return *this;
  }
};

// A font (JsuiFont, 0 fields default to 14 px / 400).
struct Font : JsuiFont {
  explicit Font(float px = 14, int w = 400, const char* fam = nullptr) : JsuiFont{} {
    size = px;
    weight = w;
    family = fam;
  }
  Font& centered() { align = JSUI_TEXT_CENTER; return *this; }
  Font& right() { align = JSUI_TEXT_RIGHT; return *this; }
};

// An owned path (move-only).
class Path {
 public:
  Path() : p_(jsui_path_create()) {}
  explicit Path(const char* svg) : p_(jsui_path_from_svg(svg)) {}
  ~Path() { if (p_) jsui_path_destroy(p_); }
  Path(Path&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
  Path& operator=(Path&& o) noexcept {
    if (this != &o) {
      if (p_) jsui_path_destroy(p_);
      p_ = o.p_;
      o.p_ = nullptr;
    }
    return *this;
  }
  Path(const Path&) = delete;
  Path& operator=(const Path&) = delete;
  explicit operator bool() const { return p_ != nullptr; }
  JsuiPath* get() const { return p_; }
  Path& reset() { jsui_path_reset(p_); return *this; }
  Path& moveTo(float x, float y) { jsui_path_move_to(p_, x, y); return *this; }
  Path& lineTo(float x, float y) { jsui_path_line_to(p_, x, y); return *this; }
  Path& quadTo(float cx, float cy, float x, float y) { jsui_path_quad_to(p_, cx, cy, x, y); return *this; }
  Path& cubicTo(float a, float b, float c, float d, float x, float y) {
    jsui_path_cubic_to(p_, a, b, c, d, x, y);
    return *this;
  }
  Path& close() { jsui_path_close(p_); return *this; }
  Path& rect(float x, float y, float w, float h) { jsui_path_add_rect(p_, x, y, w, h); return *this; }
  Path& circle(float cx, float cy, float r) { jsui_path_add_circle(p_, cx, cy, r); return *this; }
  Path& arc(float x, float y, float w, float h, float start, float sweep) {
    jsui_path_add_arc(p_, x, y, w, h, start, sweep);
    return *this;
  }
  Path& evenOdd(bool on = true) { jsui_path_set_even_odd(p_, on); return *this; }

 private:
  JsuiPath* p_;
};

// The canvas `draw` gets (valid during draw only).
class Canvas {
 public:
  explicit Canvas(JsuiCanvas* c) : c_(c) {}
  JsuiCanvas* get() const { return c_; }
  // The SkCanvas* (only for code built against the library's own Skia).
  void* skia() const { return jsui_canvas_skia(c_); }

  // Props (Animated ones at their current value).
  double number(const char* name, double fallback = 0) const { return jsui_prop_number(c_, name, fallback); }
  bool flag(const char* name, bool fallback = false) const { return jsui_prop_bool(c_, name, fallback) != 0; }
  uint32_t color(const char* name, uint32_t fallback = 0xff000000u) const { return jsui_prop_color(c_, name, fallback); }
  const char* string(const char* name) const { return jsui_prop_string(c_, name); }
  int numbers(const char* name, float* out, int max) const { return jsui_prop_numbers(c_, name, out, max); }
  const char* json(const char* name) const { return jsui_prop_json(c_, name); }

  void save() { jsui_canvas_save(c_); }
  void saveLayer(float alpha) { jsui_canvas_save_layer(c_, alpha); }
  void restore() { jsui_canvas_restore(c_); }
  void translate(float x, float y) { jsui_canvas_translate(c_, x, y); }
  void scale(float sx, float sy) { jsui_canvas_scale(c_, sx, sy); }
  void rotate(float degrees) { jsui_canvas_rotate(c_, degrees); }
  void clipRect(float x, float y, float w, float h) { jsui_canvas_clip_rect(c_, x, y, w, h); }
  void clipRRect(float x, float y, float w, float h, float r) { jsui_canvas_clip_rrect(c_, x, y, w, h, r); }
  void clipPath(const Path& p) { jsui_canvas_clip_path(c_, p.get()); }

  void clear(uint32_t argb) { jsui_canvas_clear(c_, argb); }
  void rect(float x, float y, float w, float h, const Paint& p) { jsui_canvas_draw_rect(c_, x, y, w, h, &p); }
  void rrect(float x, float y, float w, float h, float r, const Paint& p) {
    jsui_canvas_draw_rrect(c_, x, y, w, h, r, &p);
  }
  void circle(float cx, float cy, float r, const Paint& p) { jsui_canvas_draw_circle(c_, cx, cy, r, &p); }
  void oval(float x, float y, float w, float h, const Paint& p) { jsui_canvas_draw_oval(c_, x, y, w, h, &p); }
  void line(float x0, float y0, float x1, float y1, const Paint& p) { jsui_canvas_draw_line(c_, x0, y0, x1, y1, &p); }
  void arc(float x, float y, float w, float h, float start, float sweep, bool center, const Paint& p) {
    jsui_canvas_draw_arc(c_, x, y, w, h, start, sweep, center, &p);
  }
  void path(const Path& path, const Paint& p) { jsui_canvas_draw_path(c_, path.get(), &p); }
  float text(const char* utf8, float x, float y, const Font& f, const Paint& p) {
    return jsui_canvas_draw_text(c_, utf8, x, y, &f, &p);
  }
  float measure(const char* utf8, const Font& f) { return jsui_canvas_measure_text(c_, utf8, &f); }
  bool image(const char* uri, float x, float y, float w, float h, float alpha = 1) {
    return jsui_canvas_draw_image(c_, uri, x, y, w, h, alpha) != 0;
  }
  bool texture(const char* key, float x, float y, float w, float h, float alpha = 1) {
    return jsui_canvas_draw_texture(c_, key, x, y, w, h, alpha) != 0;
  }

 private:
  JsuiCanvas* c_;
};

// Registers class `name` whose views are each a new T (a Component).
template <class T>
int registerComponent(JsuiRuntime* rt, const char* name, int flags = 0);

// Base class of a C++ component: one instance per mounted view.
class Component {
 public:
  virtual ~Component() = default;
  virtual void draw(Canvas& canvas, const JsuiDrawInfo& info) = 0;
  // React changed the view's props (also once after mounting), as JSON.
  virtual void props(const char* json) { (void)json; }
  // A command from JS (codegenNativeCommands / dispatchCommand); args: a JSON array.
  virtual void command(const char* name, const char* args) {
    (void)name;
    (void)args;
  }

  JsuiRuntime* rt() const { return rt_; }
  int view() const { return view_; }
  // The state it draws changed: repaint this view.
  void invalidate() { jsui_component_invalidate(rt_, view_, nullptr); }
  // Calls the view's on<Event> prop with nativeEvent = json (an object).
  bool emit(const char* event, const char* json = nullptr) {
    return jsui_component_emit(rt_, view_, event, json) == 1;
  }

 private:
  template <class T>
  friend int registerComponent(JsuiRuntime*, const char*, int);
  JsuiRuntime* rt_ = nullptr;
  int view_ = 0;
};

template <class T>
int registerComponent(JsuiRuntime* rt, const char* name, int flags) {
  JsuiComponentClass cls;
  jsui_component_class_init(&cls);
  cls.flags = flags;
  cls.create = [](void*, JsuiRuntime* r, int view) -> void* {
    T* t = new T();
    Component* c = t;
    c->rt_ = r;
    c->view_ = view;
    return c;
  };
  cls.destroy = [](void* inst, int) { delete static_cast<Component*>(inst); };
  cls.props = [](void* inst, int, const char* json) { static_cast<Component*>(inst)->props(json); };
  cls.draw = [](void* inst, JsuiCanvas* c, const JsuiDrawInfo* info) {
    Canvas canvas(c);
    static_cast<Component*>(inst)->draw(canvas, *info);
  };
  cls.command = [](void* inst, int, const char* name, const char* args) {
    static_cast<Component*>(inst)->command(name, args);
  };
  return jsui_register_component(rt, name, &cls);
}

}  // namespace jsui
