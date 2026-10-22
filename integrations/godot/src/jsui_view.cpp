#include "jsui_view.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_joypad_button.hpp>
#include <godot_cpp/classes/input_event_joypad_motion.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/input_event_pan_gesture.hpp>
#include <godot_cpp/classes/input_event_screen_drag.hpp>
#include <godot_cpp/classes/input_event_screen_touch.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "keymap.h"

using namespace godot;

namespace jsui_godot {

namespace {

constexpr int kMaxTexture = 16384;
constexpr float kWheelStep = 40.f;  // px per notch, like browsers
constexpr int64_t kEmulatedDevice = -1;  // InputEvent::DEVICE_ID_EMULATION

std::string toStd(const String& s) {
  CharString u = s.utf8();
  return std::string(u.get_data(), size_t(u.length()));
}

int modsOf(const InputEventWithModifiers* e) {
  int m = 0;
  if (e->is_shift_pressed()) m |= JSUI_MOD_SHIFT;
  if (e->is_ctrl_pressed()) m |= JSUI_MOD_CTRL;
  if (e->is_alt_pressed()) m |= JSUI_MOD_ALT;
  if (e->is_meta_pressed()) m |= JSUI_MOD_META;
  return m;
}

const char* contentTypeFor(const String& path) {
  String ext = path.get_extension().to_lower();
  if (ext == "js" || ext == "mjs") return "text/javascript";
  if (ext == "json" || ext == "map") return "application/json";
  if (ext == "png") return "image/png";
  if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
  if (ext == "webp") return "image/webp";
  if (ext == "gif") return "image/gif";
  if (ext == "svg") return "image/svg+xml";
  if (ext == "ttf") return "font/ttf";
  if (ext == "otf") return "font/otf";
  if (ext == "woff") return "font/woff";
  if (ext == "woff2") return "font/woff2";
  if (ext == "txt") return "text/plain";
  return "application/octet-stream";
}

bool isGodotPath(const String& url) { return url.begins_with("res://") || url.begins_with("user://"); }

Control::CursorShape cursorShape(const std::string& c) {
  if (c == "pointer") return Control::CURSOR_POINTING_HAND;
  if (c == "text" || c == "vertical-text") return Control::CURSOR_IBEAM;
  if (c == "crosshair") return Control::CURSOR_CROSS;
  if (c == "wait") return Control::CURSOR_WAIT;
  if (c == "progress") return Control::CURSOR_BUSY;
  if (c == "grab" || c == "grabbing") return Control::CURSOR_DRAG;
  if (c == "copy" || c == "alias") return Control::CURSOR_CAN_DROP;
  if (c == "not-allowed" || c == "no-drop") return Control::CURSOR_FORBIDDEN;
  if (c == "ns-resize" || c == "n-resize" || c == "s-resize" || c == "row-resize") return Control::CURSOR_VSIZE;
  if (c == "ew-resize" || c == "e-resize" || c == "w-resize" || c == "col-resize") return Control::CURSOR_HSIZE;
  if (c == "nesw-resize" || c == "ne-resize" || c == "sw-resize") return Control::CURSOR_BDIAGSIZE;
  if (c == "nwse-resize" || c == "nw-resize" || c == "se-resize") return Control::CURSOR_FDIAGSIZE;
  if (c == "move" || c == "all-scroll") return Control::CURSOR_MOVE;
  if (c == "help") return Control::CURSOR_HELP;
  return Control::CURSOR_ARROW;
}

}  // namespace

// ------------------------------------------------------------------ binding

void JsuiView::_bind_methods() {
  ClassDB::bind_method(D_METHOD("set_bundle_url", "url"), &JsuiView::set_bundle_url);
  ClassDB::bind_method(D_METHOD("get_bundle_url"), &JsuiView::get_bundle_url);
  ClassDB::bind_method(D_METHOD("set_dev_mode", "enabled"), &JsuiView::set_dev_mode);
  ClassDB::bind_method(D_METHOD("get_dev_mode"), &JsuiView::get_dev_mode);
  ClassDB::bind_method(D_METHOD("set_pixel_ratio", "ratio"), &JsuiView::set_pixel_ratio);
  ClassDB::bind_method(D_METHOD("get_pixel_ratio"), &JsuiView::get_pixel_ratio);
  ClassDB::bind_method(D_METHOD("set_autostart", "enabled"), &JsuiView::set_autostart);
  ClassDB::bind_method(D_METHOD("get_autostart"), &JsuiView::get_autostart);
  ClassDB::bind_method(D_METHOD("set_forward_gamepad", "enabled"), &JsuiView::set_forward_gamepad);
  ClassDB::bind_method(D_METHOD("get_forward_gamepad"), &JsuiView::get_forward_gamepad);

  ClassDB::bind_method(D_METHOD("start"), &JsuiView::start);
  ClassDB::bind_method(D_METHOD("stop"), &JsuiView::stop);
  ClassDB::bind_method(D_METHOD("reload"), &JsuiView::reload);
  ClassDB::bind_method(D_METHOD("is_running"), &JsuiView::is_running);
  ClassDB::bind_method(D_METHOD("post_message", "name", "data"), &JsuiView::post_message, DEFVAL(Variant()));
  ClassDB::bind_method(D_METHOD("post_message_json", "name", "json"), &JsuiView::post_message_json);
  ClassDB::bind_method(D_METHOD("get_texture"), &JsuiView::get_texture);
  ClassDB::bind_method(D_METHOD("get_frame_image"), &JsuiView::get_frame_image);
  ClassDB::bind_method(D_METHOD("get_pending_loads"), &JsuiView::get_pending_loads);
  ClassDB::bind_method(D_METHOD("get_last_error"), &JsuiView::get_last_error);
  ClassDB::bind_method(D_METHOD("get_effective_pixel_ratio"), &JsuiView::get_effective_pixel_ratio);
  ClassDB::bind_method(D_METHOD("_on_joy_connection_changed", "device", "connected"),
                       &JsuiView::onJoyConnectionChanged);

  ADD_PROPERTY(PropertyInfo(Variant::STRING, "bundle_url", PROPERTY_HINT_FILE, "*.js"), "set_bundle_url",
               "get_bundle_url");
  ADD_PROPERTY(PropertyInfo(Variant::BOOL, "dev_mode"), "set_dev_mode", "get_dev_mode");
  ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "pixel_ratio", PROPERTY_HINT_RANGE, "0,4,0.05"), "set_pixel_ratio",
               "get_pixel_ratio");
  ADD_PROPERTY(PropertyInfo(Variant::BOOL, "autostart"), "set_autostart", "get_autostart");
  ADD_PROPERTY(PropertyInfo(Variant::BOOL, "forward_gamepad"), "set_forward_gamepad", "get_forward_gamepad");

  ADD_SIGNAL(MethodInfo("message_received", PropertyInfo(Variant::STRING, "name"),
                        PropertyInfo(Variant::NIL, "data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NIL_IS_VARIANT),
                        PropertyInfo(Variant::STRING, "json")));
  ADD_SIGNAL(MethodInfo("js_error", PropertyInfo(Variant::STRING, "message")));
  ADD_SIGNAL(MethodInfo("frame_rendered", PropertyInfo(Variant::RECT2I, "dirty_rect")));
}

// --------------------------------------------------------------- lifecycle

JsuiView::JsuiView() {
  set_focus_mode(FOCUS_ALL);
  // Unconsumed clicks bubble to parent controls and on to the game.
  set_mouse_filter(MOUSE_FILTER_PASS);
  set_clip_contents(true);
}

JsuiView::~JsuiView() {
  stop();
  if (canvas_item_.is_valid()) RenderingServer::get_singleton()->free_rid(canvas_item_);
}

void JsuiView::_notification(int what) {
  const bool editor = Engine::get_singleton()->is_editor_hint();
  switch (what) {
    case NOTIFICATION_READY:
      if (editor) break;
      if (Input* in = Input::get_singleton()) {
        Callable cb(this, "_on_joy_connection_changed");
        if (!in->is_connected("joy_connection_changed", cb)) in->connect("joy_connection_changed", cb);
      }
      set_process(true);
      if (autostart_ && !bundle_url_.is_empty()) start();
      break;
    case NOTIFICATION_EXIT_TREE:
      if (Input* in = Input::get_singleton()) {
        Callable cb(this, "_on_joy_connection_changed");
        if (in->is_connected("joy_connection_changed", cb)) in->disconnect("joy_connection_changed", cb);
      }
      break;
    case NOTIFICATION_PROCESS: process(); break;
    case NOTIFICATION_RESIZED:
    case NOTIFICATION_VISIBILITY_CHANGED: render_pending_ = true; break;
    case NOTIFICATION_MOUSE_EXIT:
      if (rt_) jsui_send_mouse_leave(rt_);
      break;
    case NOTIFICATION_FOCUS_EXIT: releaseHeldInput(); break;
    case NOTIFICATION_DRAW:
      if (editor) {
        // Placeholder: the runtime does not run inside the editor.
        draw_rect(Rect2(Vector2(), get_size()), Color(0.1f, 0.1f, 0.12f, 0.6f));
        draw_rect(Rect2(Vector2(), get_size()), Color(0.4f, 0.6f, 1.f, 0.8f), false, 2.f);
        Ref<Font> font = get_theme_default_font();
        if (font.is_valid())
          draw_string(font, Vector2(12, 24), "JsuiView  " + (bundle_url_.is_empty() ? String("(no bundle_url)") : bundle_url_),
                      HORIZONTAL_ALIGNMENT_LEFT, -1, 14, Color(0.8f, 0.85f, 1.f));
      }
      break;
    default: break;
  }
}

void JsuiView::set_bundle_url(const String& url) {
  if (url == bundle_url_) return;
  bundle_url_ = url;
  if (is_node_ready() && rt_) start();
  queue_redraw();
}

void JsuiView::set_dev_mode(bool on) {
  if (on == dev_mode_) return;
  dev_mode_ = on;
  if (is_node_ready() && rt_) start();
}

void JsuiView::set_pixel_ratio(float ratio) {
  pixel_ratio_ = std::clamp(ratio, 0.f, 4.f);
  render_pending_ = true;
}

void JsuiView::start() {
  stop();
  if (Engine::get_singleton()->is_editor_hint()) return;
  if (bundle_url_.is_empty()) {
    UtilityFunctions::push_warning("JsuiView: bundle_url is empty");
    return;
  }
  JsuiConfig cfg;
  jsui_config_init(&cfg);
  Vector2 size = get_size();
  cfg.width = std::max(1, int(std::lround(size.x)));
  cfg.height = std::max(1, int(std::lround(size.y)));
  cfg.pixel_ratio = computeRatio();
  cfg.dev_mode = dev_mode_ ? 1 : 0;
  cfg.on_message = &JsuiView::onMessage;
  cfg.message_user = this;
  cfg.log = &JsuiView::onLog;
  cfg.log_user = this;
  // res:// and user:// live in Godot's virtual file system (and in .pck
  // files once exported), so those bundles load through FileAccess. Other
  // URLs (http:// dev servers, absolute file paths) use the built-in loader.
  if (isGodotPath(bundle_url_)) {
    cfg.load = &JsuiView::onLoad;
    cfg.load_user = this;
  }
  rt_ = jsui_create(&cfg);
  if (!rt_) {
    UtilityFunctions::push_error("JsuiView: jsui_create failed");
    return;
  }
  start_usec_ = Time::get_singleton()->get_ticks_usec();
  logical_w_ = logical_h_ = pixel_w_ = pixel_h_ = 0;
  last_error_.clear();
  cursor_.clear();
  render_pending_ = true;
  updateViewport();

  // Announce pads that are already connected.
  if (Input* in = Input::get_singleton()) {
    TypedArray<int> pads = in->get_connected_joypads();
    for (int64_t i = 0; i < pads.size(); i++) onJoyConnectionChanged(int64_t(pads[i]), true);
  }
  std::string url = toStd(bundle_url_);
  jsui_load_bundle_url(rt_, url.c_str());
}

void JsuiView::stop() {
  if (!rt_) return;
  jsui_destroy(rt_);
  rt_ = nullptr;
  messages_.clear();
  held_keys_.clear();
  pads_.clear();
}

void JsuiView::reload() {
  if (rt_) jsui_reload(rt_);
}

// --------------------------------------------------------------- callbacks

void JsuiView::onMessage(void* user, const char* name, const char* json) {
  // Called from inside jsui_* calls: queue, emit once the runtime returned.
  auto* self = static_cast<JsuiView*>(user);
  self->messages_.emplace_back(name ? name : "", json ? json : "");
}

void JsuiView::onLog(void*, JsuiLogLevel level, const char* message) {
  String m = "[jsui] " + String::utf8(message ? message : "");
  switch (level) {
    case JSUI_LOG_ERROR: UtilityFunctions::push_error(m); break;
    case JSUI_LOG_WARN: UtilityFunctions::push_warning(m); break;
    case JSUI_LOG_DEBUG: UtilityFunctions::print_verbose(m); break;
    default: UtilityFunctions::print(m); break;
  }
}

void JsuiView::onLoad(void*, JsuiRuntime* rt, uint64_t id, const char* url_c) {
  String url = String::utf8(url_c);
  if (url.begins_with("http://") || url.begins_with("https://")) {
    // A res:// bundle uses FileAccess for everything; fetch() to the network
    // is not available to it (load the bundle by URL for that).
    const char* msg = "network loads are not available to res:// bundles";
    jsui_resource_complete(rt, id, 0, "text/plain", msg, std::strlen(msg));
    return;
  }
  String path = url.begins_with("file://") ? url.substr(7) : url;
  int cut = path.find("?");
  if (cut < 0) cut = path.find("#");
  if (cut >= 0) path = path.substr(0, cut);
  Ref<FileAccess> f = FileAccess::open(path, FileAccess::READ);
  if (f.is_null()) {
    jsui_resource_complete(rt, id, 404, "text/plain", nullptr, 0);
    return;
  }
  PackedByteArray data = f->get_buffer(int64_t(f->get_length()));
  jsui_resource_complete(rt, id, 200, contentTypeFor(path), data.ptr(), size_t(data.size()));
}

void JsuiView::onJoyConnectionChanged(int64_t device, bool connected) {
  if (!rt_ || !forward_gamepad_ || device < 0) return;
  std::string name;
  if (connected) {
    name = toStd(Input::get_singleton()->get_joy_name(int32_t(device)));
    if (name.empty()) name = "Godot Joypad";
    pads_[device];
  } else {
    pads_.erase(device);
  }
  jsui_set_gamepad_connected(rt_, int(device), connected ? 1 : 0, connected ? name.c_str() : nullptr);
}

// -------------------------------------------------------------- frame loop

double JsuiView::nowMs() const { return double(Time::get_singleton()->get_ticks_usec() - start_usec_) / 1000.0; }

float JsuiView::computeRatio() const {
  if (pixel_ratio_ > 0.f) return pixel_ratio_;
  // Canvas -> screen scale: stretch mode, Camera2D zoom, CanvasLayer and
  // parent scales. Rasterising at that density keeps the UI sharp.
  Vector2 s = get_screen_transform().get_scale();
  float r = std::max(std::abs(s.x), std::abs(s.y));
  if (!std::isfinite(r) || r <= 0.f) r = 1.f;
  return std::clamp(r, 0.25f, 4.f);
}

void JsuiView::updateViewport() {
  if (!rt_) return;
  Vector2 size = get_size();
  int lw = std::max(1, int(std::lround(size.x)));
  int lh = std::max(1, int(std::lround(size.y)));
  float ratio = computeRatio();
  // Never exceed the GPU texture limit.
  ratio = std::min(ratio, float(kMaxTexture) / float(std::max(lw, lh)));
  if (lw == logical_w_ && lh == logical_h_ && std::abs(ratio - ratio_) < 1e-3f && pixel_w_ > 0) return;
  logical_w_ = lw;
  logical_h_ = lh;
  ratio_ = ratio;
  jsui_set_viewport(rt_, lw, lh, ratio);
  int pw = std::max(1, int(std::lround(lw * ratio)));
  int ph = std::max(1, int(std::lround(lh * ratio)));
  if (pw != pixel_w_ || ph != pixel_h_) {
    pixel_w_ = pw;
    pixel_h_ = ph;
    pixels_.assign(size_t(pw) * size_t(ph) * 4, 0);
    upload_.resize(int64_t(pixels_.size()));
    std::memset(upload_.ptrw(), 0, pixels_.size());
  }
  render_pending_ = true;
}

void JsuiView::process() {
  if (!rt_) return;
  updateViewport();
  jsui_tick(rt_, nowMs());
  flushMessages();
  if (!rt_) return;  // a message handler stopped the view
  updateCursor();
  reportErrors();
  if (jsui_needs_redraw(rt_)) render_pending_ = true;
  if (render_pending_ && is_visible_in_tree()) render();
}

void JsuiView::render() {
  render_pending_ = false;
  if (pixels_.empty()) return;
  JsuiRect dirty{0, 0, 0, 0};
  const size_t stride = size_t(pixel_w_) * 4;
  int changed = jsui_render_rgba_incremental(rt_, pixels_.data(), pixel_w_, pixel_h_, stride, &dirty);
  const bool resized = texture_.is_null() || texture_->get_width() != pixel_w_ || texture_->get_height() != pixel_h_;
  if (!changed && !resized) return;
  if (resized) {
    dirty = {0, 0, pixel_w_, pixel_h_};
  }
  // Clamp defensively; then copy only the damaged rows into the staging
  // buffer. Godot 4 has no sub-rect texture upload, so the GPU upload is the
  // whole texture, but only when something changed.
  int x0 = std::clamp(dirty.x, 0, pixel_w_), y0 = std::clamp(dirty.y, 0, pixel_h_);
  int x1 = std::clamp(dirty.x + dirty.width, 0, pixel_w_), y1 = std::clamp(dirty.y + dirty.height, 0, pixel_h_);
  if (x1 <= x0 || y1 <= y0) {
    if (!resized) return;
    x0 = y0 = 0;
    x1 = pixel_w_;
    y1 = pixel_h_;
  }
  uint8_t* dst = upload_.ptrw();
  const size_t rowBytes = size_t(x1 - x0) * 4;
  for (int y = y0; y < y1; y++) {
    size_t off = size_t(y) * stride + size_t(x0) * 4;
    std::memcpy(dst + off, pixels_.data() + off, rowBytes);
  }
  Ref<Image> img = Image::create_from_data(pixel_w_, pixel_h_, false, Image::FORMAT_RGBA8, upload_);
  if (img.is_null()) return;
  RenderingServer* rs = RenderingServer::get_singleton();
  if (resized) {
    if (texture_.is_null()) texture_.instantiate();
    texture_->set_image(img);
  } else {
    texture_->update(img);
  }
  if (!canvas_item_.is_valid()) {
    canvas_item_ = rs->canvas_item_create();
    rs->canvas_item_set_parent(canvas_item_, get_canvas_item());
    premul_.instantiate();
    premul_->set_blend_mode(CanvasItemMaterial::BLEND_MODE_PREMULT_ALPHA);
    rs->canvas_item_set_material(canvas_item_, premul_->get_rid());
  }
  rs->canvas_item_clear(canvas_item_);
  rs->canvas_item_add_texture_rect(canvas_item_, Rect2(0, 0, float(logical_w_), float(logical_h_)), texture_->get_rid());
  emit_signal("frame_rendered", Rect2i(x0, y0, x1 - x0, y1 - y0));
}

Ref<Image> JsuiView::get_frame_image() const {
  // CPU copy of the last frame, un-premultiplied (for screenshots / tests;
  // independent of the rendering backend, so it also works with --headless).
  if (pixels_.empty()) return Ref<Image>();
  PackedByteArray out;
  out.resize(int64_t(pixels_.size()));
  uint8_t* d = out.ptrw();
  for (size_t i = 0; i < pixels_.size(); i += 4) {
    uint8_t a = pixels_[i + 3];
    for (int c = 0; c < 3; c++)
      d[i + c] = a == 0 ? 0 : a == 255 ? pixels_[i + c] : uint8_t(std::min(255, (pixels_[i + c] * 255 + a / 2) / a));
    d[i + 3] = a;
  }
  return Image::create_from_data(pixel_w_, pixel_h_, false, Image::FORMAT_RGBA8, out);
}

void JsuiView::updateCursor() {
  const char* c = jsui_get_cursor(rt_);
  std::string name = c ? c : "default";
  if (name == cursor_) return;
  cursor_ = name;
  set_default_cursor_shape(cursorShape(name));
}

void JsuiView::flushMessages() {
  if (messages_.empty()) return;
  auto batch = std::move(messages_);
  messages_.clear();
  for (auto& [name, json] : batch) {
    String j = String::utf8(json.c_str());
    Variant data = j.is_empty() ? Variant() : JSON::parse_string(j);
    emit_signal("message_received", String::utf8(name.c_str()), data, j);
  }
}

void JsuiView::reportErrors() {
  const char* e = jsui_last_error(rt_);
  std::string err = e ? e : "";
  if (err == last_error_) return;
  last_error_ = err;
  if (!err.empty()) {
    String m = String::utf8(err.c_str());
    UtilityFunctions::push_error("JsuiView: " + m);
    emit_signal("js_error", m);
  }
}

int JsuiView::get_pending_loads() const { return rt_ ? jsui_pending_loads(rt_) : 0; }

String JsuiView::get_last_error() const {
  if (!rt_) return String();
  const char* e = jsui_last_error(rt_);
  return e ? String::utf8(e) : String();
}

void JsuiView::post_message(const String& name, const Variant& data) {
  post_message_json(name, data.get_type() == Variant::NIL ? String() : JSON::stringify(data));
}

void JsuiView::post_message_json(const String& name, const String& json) {
  if (!rt_) return;
  std::string n = toStd(name), j = toStd(json);
  jsui_post_message(rt_, n.c_str(), j.c_str());
  flushMessages();
}

// ------------------------------------------------------------------- input

void JsuiView::releaseHeldInput() {
  if (!rt_) return;
  // Losing focus mid-press would otherwise leave keys / buttons stuck down.
  auto keys = std::move(held_keys_);
  held_keys_.clear();
  for (auto& [k, held] : keys) jsui_send_key(rt_, held.key.c_str(), held.code.empty() ? nullptr : held.code.c_str(), 0, 0, 0);
  for (auto& [dev, pad] : pads_) {
    for (int b = 0; b < 17; b++)
      if (pad.buttons[b]) {
        pad.buttons[b] = false;
        jsui_send_gamepad_button(rt_, int(dev), b, 0, 0.f);
      }
    pad.trigger[0] = pad.trigger[1] = false;
  }
  flushMessages();
}

bool JsuiView::handleGamepad(const Ref<InputEvent>& event) {
  if (const auto* jb = Object::cast_to<InputEventJoypadButton>(event.ptr())) {
    int64_t dev = jb->get_device();
    if (!pads_.count(dev)) onJoyConnectionChanged(dev, true);
    int b = gamepadButton(jb->get_button_index());
    if (b < 0) return false;
    bool down = jb->is_pressed();
    pads_[dev].buttons[b] = down;
    return jsui_send_gamepad_button(rt_, int(dev), b, down, down ? 1.f : 0.f) != 0;
  }
  if (const auto* jm = Object::cast_to<InputEventJoypadMotion>(event.ptr())) {
    int64_t dev = jm->get_device();
    if (!pads_.count(dev)) onJoyConnectionChanged(dev, true);
    float v = std::clamp(float(jm->get_axis_value()), -1.f, 1.f);
    switch (jm->get_axis()) {
      case JOY_AXIS_LEFT_X: return jsui_send_gamepad_axis(rt_, int(dev), 0, v) != 0;
      case JOY_AXIS_LEFT_Y: return jsui_send_gamepad_axis(rt_, int(dev), 1, v) != 0;
      case JOY_AXIS_RIGHT_X: return jsui_send_gamepad_axis(rt_, int(dev), 2, v) != 0;
      case JOY_AXIS_RIGHT_Y: return jsui_send_gamepad_axis(rt_, int(dev), 3, v) != 0;
      case JOY_AXIS_TRIGGER_LEFT:
      case JOY_AXIS_TRIGGER_RIGHT: {
        // Standard mapping: triggers are analog buttons 6 / 7 (with hysteresis).
        int t = jm->get_axis() == JOY_AXIS_TRIGGER_LEFT ? 0 : 1;
        Pad& pad = pads_[dev];
        bool now = pad.trigger[t] ? v > 0.3f : v > 0.5f;
        if (now == pad.trigger[t]) return false;
        pad.trigger[t] = now;
        pad.buttons[6 + t] = now;
        return jsui_send_gamepad_button(rt_, int(dev), 6 + t, now, std::max(0.f, v)) != 0;
      }
      default: return false;
    }
  }
  return false;
}

void JsuiView::_gui_input(const Ref<InputEvent>& event) {
  if (!rt_ || event.is_null()) return;
  bool consumed = false;

  if (const auto* mm = Object::cast_to<InputEventMouseMotion>(event.ptr())) {
    if (mm->get_device() == kEmulatedDevice) return;  // touch already forwarded as touch
    Vector2 p = mm->get_position();
    consumed = jsui_send_mouse_move(rt_, p.x, p.y) != 0;
  } else if (const auto* mb = Object::cast_to<InputEventMouseButton>(event.ptr())) {
    if (mb->get_device() == kEmulatedDevice) return;
    Vector2 p = mb->get_position();
    MouseButton idx = mb->get_button_index();
    if (idx == MOUSE_BUTTON_WHEEL_UP || idx == MOUSE_BUTTON_WHEEL_DOWN || idx == MOUSE_BUTTON_WHEEL_LEFT ||
        idx == MOUSE_BUTTON_WHEEL_RIGHT) {
      if (mb->is_pressed()) {
        float f = mb->get_factor() > 0 ? float(mb->get_factor()) : 1.f;
        float dx = idx == MOUSE_BUTTON_WHEEL_LEFT ? -f : idx == MOUSE_BUTTON_WHEEL_RIGHT ? f : 0.f;
        float dy = idx == MOUSE_BUTTON_WHEEL_UP ? -f : idx == MOUSE_BUTTON_WHEEL_DOWN ? f : 0.f;
        // Shift+wheel scrolls horizontally, like browsers.
        if (mb->is_shift_pressed() && dx == 0.f) std::swap(dx, dy);
        consumed = jsui_send_mouse_wheel(rt_, p.x, p.y, dx * kWheelStep, dy * kWheelStep) != 0;
      }
    } else if (idx == MOUSE_BUTTON_LEFT || idx == MOUSE_BUTTON_RIGHT || idx == MOUSE_BUTTON_MIDDLE) {
      JsuiMouseButton b = idx == MOUSE_BUTTON_RIGHT ? JSUI_MOUSE_RIGHT : idx == MOUSE_BUTTON_MIDDLE ? JSUI_MOUSE_MIDDLE
                                                                                                    : JSUI_MOUSE_LEFT;
      if (mb->is_pressed() && get_focus_mode() != FOCUS_NONE && !has_focus()) grab_focus();
      consumed = jsui_send_mouse_button(rt_, p.x, p.y, b, mb->is_pressed() ? 1 : 0, modsOf(mb)) != 0;
    }
  } else if (const auto* pg = Object::cast_to<InputEventPanGesture>(event.ptr())) {
    // Trackpad two-finger scroll (macOS): delta is in "wheel notches".
    Vector2 p = pg->get_position(), d = pg->get_delta();
    consumed = jsui_send_mouse_wheel(rt_, p.x, p.y, d.x * kWheelStep, d.y * kWheelStep) != 0;
  } else if (const auto* st = Object::cast_to<InputEventScreenTouch>(event.ptr())) {
    Vector2 p = st->get_position();
    int phase = st->is_canceled() ? 3 : st->is_pressed() ? 0 : 2;
    consumed = jsui_send_touch(rt_, int(st->get_index()), phase, p.x, p.y) != 0;
  } else if (const auto* sd = Object::cast_to<InputEventScreenDrag>(event.ptr())) {
    Vector2 p = sd->get_position();
    consumed = jsui_send_touch(rt_, int(sd->get_index()), 1, p.x, p.y) != 0;
  } else if (const auto* k = Object::cast_to<InputEventKey>(event.ptr())) {
    Key physical = k->get_physical_keycode();
    Key logical = k->get_keycode();
    if (physical == KEY_NONE) physical = logical;
    const bool down = k->is_pressed();
    std::string key, code;
    auto held = held_keys_.find(int64_t(physical));
    if (!down && held != held_keys_.end()) {
      // Releases carry no unicode: repeat what the press sent.
      key = held->second.key;
      code = held->second.code;
      held_keys_.erase(held);
    } else {
      key = domKey(logical, char32_t(k->get_unicode()), k->is_shift_pressed());
      const char* c = domCode(physical, k->get_location());
      code = c ? c : "";
      if (down) held_keys_[int64_t(physical)] = {key, code};
    }
    consumed = jsui_send_key(rt_, key.c_str(), code.empty() ? nullptr : code.c_str(), down ? 1 : 0,
                             k->is_echo() ? 1 : 0, modsOf(k)) != 0;
    // Printable characters follow as text input (not for Ctrl/Cmd chords).
    char32_t uc = char32_t(k->get_unicode());
    if (down && uc >= 0x20 && uc != 0x7F && !k->is_ctrl_pressed() && !k->is_meta_pressed()) {
      std::string text = utf8(uc);
      if (!text.empty() && jsui_send_text(rt_, text.c_str())) consumed = true;
    }
  } else if (forward_gamepad_) {
    consumed = handleGamepad(event);
  }

  flushMessages();
  if (consumed) accept_event();
}

}  // namespace jsui_godot
