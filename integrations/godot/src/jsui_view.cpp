#include "jsui_view.h"

#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/font_file.hpp>
#include <godot_cpp/classes/font_variation.hpp>
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
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "jsui_audio.h"
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
  if (ext == "ttc" || ext == "otc") return "font/collection";
  if (ext == "txt") return "text/plain";
  return "application/octet-stream";
}

bool isGodotPath(const String& url) { return url.begins_with("res://") || url.begins_with("user://"); }

bool isFontFile(const String& path) {
  String ext = path.get_extension().to_lower();
  return ext == "ttf" || ext == "otf" || ext == "ttc" || ext == "otc" || ext == "woff" || ext == "woff2";
}

// The font file bytes of a Font resource: a FontFile's data (the file it was
// imported from), or that of a FontVariation's base font. Empty (why set)
// for anything else (SystemFont, a bitmap FontFile).
PackedByteArray fontResourceData(Object* o, String& why) {
  for (int depth = 0; o && depth < 8; depth++) {
    if (auto* file = Object::cast_to<FontFile>(o)) {
      PackedByteArray data = file->get_data();
      if (data.is_empty()) why = "the FontFile has no font data (a bitmap font?)";
      return data;
    }
    auto* variation = Object::cast_to<FontVariation>(o);
    if (!variation) break;
    Ref<Font> base = variation->get_base_font();  // the variation keeps it alive
    o = base.ptr();
  }
  why = o ? "expected a FontFile, got " + o->get_class() : String("no font");
  return {};
}

// A res:// font file Godot imported: the data of its FontFile. Exported
// projects ship only the imported copy unless the file is in the export
// preset's non-resource filter.
PackedByteArray importedFontData(const String& path, String& why) {
  ResourceLoader* loader = ResourceLoader::get_singleton();
  if (!loader || !loader->exists(path, "FontFile")) return {};
  Ref<Resource> res = loader->load(path, "FontFile");
  return res.is_valid() ? fontResourceData(res.ptr(), why) : PackedByteArray();
}

// register_font's `font` argument as bytes: a PackedByteArray, a Font
// resource, or the path of a font file (FileAccess, else the imported FontFile).
PackedByteArray fontBytes(const Variant& font, String& why) {
  switch (font.get_type()) {
    case Variant::PACKED_BYTE_ARRAY: {
      PackedByteArray data = font;
      if (data.is_empty()) why = "empty PackedByteArray";
      return data;
    }
    case Variant::STRING:
    case Variant::STRING_NAME: {
      String path = font;
      if (FileAccess::file_exists(path)) {
        PackedByteArray data = FileAccess::get_file_as_bytes(path);
        if (!data.is_empty()) return data;
      }
      PackedByteArray data = isGodotPath(path) ? importedFontData(path, why) : PackedByteArray();
      if (data.is_empty() && why.is_empty()) why = "cannot read " + path;
      return data;
    }
    case Variant::OBJECT: return fontResourceData(static_cast<Object*>(font), why);
    default:
      why = "expected a FontFile, a PackedByteArray or a path, got " + Variant::get_type_name(font.get_type());
      return {};
  }
}

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
  ClassDB::bind_method(D_METHOD("set_js_frame_budget_ms", "ms"), &JsuiView::set_js_frame_budget_ms);
  ClassDB::bind_method(D_METHOD("get_js_frame_budget_ms"), &JsuiView::get_js_frame_budget_ms);
  ClassDB::bind_method(D_METHOD("set_input_pass_through", "enabled"), &JsuiView::set_input_pass_through);
  ClassDB::bind_method(D_METHOD("get_input_pass_through"), &JsuiView::get_input_pass_through);
  ClassDB::bind_method(D_METHOD("hit_test", "position"), &JsuiView::hit_test);
  BIND_CONSTANT(HIT_POINTER);
  BIND_CONSTANT(HIT_SCROLL);
  ClassDB::bind_method(D_METHOD("set_force_cpu", "enabled"), &JsuiView::set_force_cpu);
  ClassDB::bind_method(D_METHOD("get_force_cpu"), &JsuiView::get_force_cpu);
  ClassDB::bind_method(D_METHOD("set_js_thread", "mode"), &JsuiView::set_js_thread);
  ClassDB::bind_method(D_METHOD("get_js_thread"), &JsuiView::get_js_thread);
  ClassDB::bind_method(D_METHOD("sync_js", "timeout_ms"), &JsuiView::sync_js, DEFVAL(250.0));
  ClassDB::bind_method(D_METHOD("sync_js_next_frame"), &JsuiView::sync_js_next_frame);
  ClassDB::bind_method(D_METHOD("debug_stall_js", "ms"), &JsuiView::debug_stall_js);
  ClassDB::bind_method(D_METHOD("get_active_js_thread"), &JsuiView::get_active_js_thread);
  BIND_CONSTANT(JS_THREAD_OFF);
  BIND_CONSTANT(JS_THREAD_ON);
  BIND_CONSTANT(JS_THREAD_LOCKSTEP);
  ClassDB::bind_method(D_METHOD("set_runtime_view", "path"), &JsuiView::set_runtime_view);
  ClassDB::bind_method(D_METHOD("get_runtime_view"), &JsuiView::get_runtime_view);
  ClassDB::bind_method(D_METHOD("set_component", "app_key"), &JsuiView::set_component);
  ClassDB::bind_method(D_METHOD("get_component"), &JsuiView::get_component);
  ClassDB::bind_method(D_METHOD("set_initial_props", "props"), &JsuiView::set_initial_props);
  ClassDB::bind_method(D_METHOD("get_initial_props"), &JsuiView::get_initial_props);
  ClassDB::bind_method(D_METHOD("set_auto_size", "enabled"), &JsuiView::set_auto_size);
  ClassDB::bind_method(D_METHOD("get_auto_size"), &JsuiView::get_auto_size);
  ClassDB::bind_method(D_METHOD("set_pipeline_cache_path", "path"), &JsuiView::set_pipeline_cache_path);
  ClassDB::bind_method(D_METHOD("get_pipeline_cache_path"), &JsuiView::get_pipeline_cache_path);
  ClassDB::bind_method(D_METHOD("set_fixed_fps", "fps"), &JsuiView::set_fixed_fps);
  ClassDB::bind_method(D_METHOD("get_fixed_fps"), &JsuiView::get_fixed_fps);
  ClassDB::bind_method(D_METHOD("get_root_tag"), &JsuiView::get_root_tag);
  ClassDB::bind_method(D_METHOD("set_audio_enabled", "enabled"), &JsuiView::set_audio_enabled);
  ClassDB::bind_method(D_METHOD("get_audio_enabled"), &JsuiView::get_audio_enabled);
  ClassDB::bind_method(D_METHOD("set_audio_bus", "bus"), &JsuiView::set_audio_bus);
  ClassDB::bind_method(D_METHOD("get_audio_bus"), &JsuiView::get_audio_bus);
  ClassDB::bind_method(D_METHOD("set_audio_bus_map", "map"), &JsuiView::set_audio_bus_map);
  ClassDB::bind_method(D_METHOD("get_audio_bus_map"), &JsuiView::get_audio_bus_map);
  ClassDB::bind_method(D_METHOD("stop_all_sounds", "bus", "fade_ms"), &JsuiView::stop_all_sounds, DEFVAL(String()),
                       DEFVAL(0.0));
  ClassDB::bind_method(D_METHOD("get_audio_voices"), &JsuiView::get_audio_voices);
  ClassDB::bind_method(D_METHOD("resolve_audio_bus", "bus"), &JsuiView::resolve_audio_bus);

  ClassDB::bind_method(D_METHOD("start"), &JsuiView::start);
  ClassDB::bind_method(D_METHOD("stop"), &JsuiView::stop);
  ClassDB::bind_method(D_METHOD("reload"), &JsuiView::reload);
  ClassDB::bind_method(D_METHOD("is_running"), &JsuiView::is_running);
  ClassDB::bind_method(D_METHOD("post_message", "name", "data"), &JsuiView::post_message, DEFVAL(Variant()));
  ClassDB::bind_method(D_METHOD("post_message_json", "name", "json"), &JsuiView::post_message_json);
  ClassDB::bind_method(D_METHOD("set_state", "key", "value"), &JsuiView::set_state);
  ClassDB::bind_method(D_METHOD("set_state_json", "key", "json"), &JsuiView::set_state_json);
  ClassDB::bind_method(D_METHOD("patch_state", "key", "patch"), &JsuiView::patch_state);
  ClassDB::bind_method(D_METHOD("patch_state_json", "key", "json"), &JsuiView::patch_state_json);
  ClassDB::bind_method(D_METHOD("get_state", "key"), &JsuiView::get_state);
  ClassDB::bind_method(D_METHOD("set_buffer", "key", "data"), &JsuiView::set_buffer);
  ClassDB::bind_method(D_METHOD("remove_buffer", "key"), &JsuiView::remove_buffer);
  ClassDB::bind_method(D_METHOD("set_texture", "name", "texture", "live"), &JsuiView::set_texture, DEFVAL(false));
  ClassDB::bind_method(D_METHOD("remove_texture", "name"), &JsuiView::remove_texture);
  ClassDB::bind_method(D_METHOD("update_texture", "name"), &JsuiView::update_texture);
  ClassDB::bind_method(D_METHOD("register_font", "family", "font", "weight", "italic"), &JsuiView::register_font,
                       DEFVAL(0), DEFVAL(-1));
  ClassDB::bind_method(D_METHOD("unregister_font", "family"), &JsuiView::unregister_font);
  ClassDB::bind_method(D_METHOD("set_font_fallbacks", "families"), &JsuiView::set_font_fallbacks);
  ClassDB::bind_method(D_METHOD("set_default_font_family", "family"), &JsuiView::set_default_font_family);
  ClassDB::bind_method(D_METHOD("set_fonts", "fonts"), &JsuiView::set_fonts);
  ClassDB::bind_method(D_METHOD("get_fonts"), &JsuiView::get_fonts);
  ClassDB::bind_method(D_METHOD("get_state_json", "key"), &JsuiView::get_state_json);
  ClassDB::bind_method(D_METHOD("resolve_call", "id", "result"), &JsuiView::resolve_call, DEFVAL(Variant()));
  ClassDB::bind_method(D_METHOD("resolve_call_json", "id", "json"), &JsuiView::resolve_call_json);
  ClassDB::bind_method(D_METHOD("reject_call", "id", "message"), &JsuiView::reject_call);
  ClassDB::bind_method(D_METHOD("call_ui", "name", "args", "callback"), &JsuiView::call_ui, DEFVAL(Variant()),
                       DEFVAL(Callable()));
  ClassDB::bind_method(D_METHOD("get_texture"), &JsuiView::get_texture);
  ClassDB::bind_method(D_METHOD("get_frame_image"), &JsuiView::get_frame_image);
  ClassDB::bind_method(D_METHOD("get_cpu_frame_image"), &JsuiView::get_cpu_frame_image);
  ClassDB::bind_method(D_METHOD("get_renderer"), &JsuiView::get_renderer);
  ClassDB::bind_method(D_METHOD("get_render_stats"), &JsuiView::get_render_stats);
  ClassDB::bind_method(D_METHOD("set_profiling", "enabled"), &JsuiView::set_profiling);
  ClassDB::bind_method(D_METHOD("get_profiling"), &JsuiView::get_profiling);
  ClassDB::bind_method(D_METHOD("get_perf", "reset"), &JsuiView::get_perf, DEFVAL(true));
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
  // ms of JS per frame before React's scheduler yields: transitions render in
  // slices across frames (jsui_set_js_frame_budget; 0 = no limit).
  ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "js_frame_budget_ms", PROPERTY_HINT_RANGE, "0,33,0.5"),
               "set_js_frame_budget_ms", "get_js_frame_budget_ms");
  ADD_PROPERTY(PropertyInfo(Variant::BOOL, "input_pass_through"), "set_input_pass_through", "get_input_pass_through");
  ADD_PROPERTY(PropertyInfo(Variant::BOOL, "force_cpu"), "set_force_cpu", "get_force_cpu");
  // Where the bundle's JS runs (see jsui_view.h); applies on start().
  ADD_PROPERTY(PropertyInfo(Variant::INT, "js_thread", PROPERTY_HINT_ENUM, "Off,On,Lockstep"), "set_js_thread",
               "get_js_thread");
  ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "runtime_view", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "JsuiView"),
               "set_runtime_view", "get_runtime_view");
  ADD_PROPERTY(PropertyInfo(Variant::STRING, "component"), "set_component", "get_component");
  ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "initial_props"), "set_initial_props", "get_initial_props");
  ADD_PROPERTY(PropertyInfo(Variant::BOOL, "auto_size"), "set_auto_size", "get_auto_size");
  ADD_PROPERTY(PropertyInfo(Variant::BOOL, "profiling"), "set_profiling", "get_profiling");
  ADD_PROPERTY(PropertyInfo(Variant::STRING, "pipeline_cache_path", PROPERTY_HINT_SAVE_FILE, "*.bin"),
               "set_pipeline_cache_path", "get_pipeline_cache_path");
  // Array of Font resources in the inspector (MAKE_RESOURCE_TYPE_HINT("Font")).
  ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "fonts", PROPERTY_HINT_TYPE_STRING,
                            String::num_int64(Variant::OBJECT) + "/" + String::num_int64(PROPERTY_HINT_RESOURCE_TYPE) +
                                ":Font"),
               "set_fonts", "get_fonts");
  ADD_PROPERTY(PropertyInfo(Variant::INT, "fixed_fps", PROPERTY_HINT_RANGE, "0,240"), "set_fixed_fps",
               "get_fixed_fps");
  ADD_GROUP("Audio", "audio_");
  ADD_PROPERTY(PropertyInfo(Variant::BOOL, "audio_enabled"), "set_audio_enabled", "get_audio_enabled");
  ADD_PROPERTY(PropertyInfo(Variant::STRING, "audio_bus"), "set_audio_bus", "get_audio_bus");
  ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "audio_bus_map"), "set_audio_bus_map", "get_audio_bus_map");

  ADD_SIGNAL(MethodInfo("message_received", PropertyInfo(Variant::STRING, "name"),
                        PropertyInfo(Variant::NIL, "data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NIL_IS_VARIANT),
                        PropertyInfo(Variant::STRING, "json")));
  const PropertyInfo anyValue(Variant::NIL, "value", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NIL_IS_VARIANT);
  ADD_SIGNAL(MethodInfo("state_changed", PropertyInfo(Variant::STRING, "key"), anyValue));
  PropertyInfo args = anyValue;
  args.name = "args";
  ADD_SIGNAL(MethodInfo("call_requested", PropertyInfo(Variant::INT, "id"), PropertyInfo(Variant::STRING, "name"), args));
  ADD_SIGNAL(MethodInfo("call_cancelled", PropertyInfo(Variant::INT, "id")));
  PropertyInfo result = anyValue;
  result.name = "result";
  ADD_SIGNAL(MethodInfo("ui_call_completed", PropertyInfo(Variant::INT, "id"), PropertyInfo(Variant::BOOL, "ok"), result));
  ADD_SIGNAL(MethodInfo("js_error", PropertyInfo(Variant::STRING, "message")));
  ADD_SIGNAL(MethodInfo("frame_rendered", PropertyInfo(Variant::RECT2I, "dirty_rect")));
  ADD_SIGNAL(MethodInfo("sound_played", PropertyInfo(Variant::INT, "voice"), PropertyInfo(Variant::STRING, "url"),
                        PropertyInfo(Variant::STRING, "bus")));
  ADD_SIGNAL(MethodInfo("sound_ended", PropertyInfo(Variant::INT, "voice"), PropertyInfo(Variant::INT, "status")));
  BIND_CONSTANT(SOUND_FINISHED);
  BIND_CONSTANT(SOUND_STOPPED);
  BIND_CONSTANT(SOUND_FAILED);
}

// --------------------------------------------------------------- lifecycle

JsuiView::JsuiView() {
  set_focus_mode(FOCUS_ALL);
  // Unconsumed events bubble to parent controls and on to the game; with
  // input_pass_through, points the UI doesn't capture aren't even inside the
  // view (_has_point), so controls behind it get them too.
  set_mouse_filter(MOUSE_FILTER_PASS);
  set_clip_contents(true);
}

JsuiView::~JsuiView() {
  stop();
  detachFromRuntimeView();
  // Surface views of our runtime (stop() freed their surfaces) look for a
  // runtime view again.
  for (JsuiView* v : surfaces_) v->host_ = nullptr;
  surfaces_.clear();
  for (HostTexture& e : textures_)
    if (e.texture.is_valid() && e.texture->is_connected("changed", e.changed)) e.texture->disconnect("changed", e.changed);
  releaseTextureGpu();
  textures_.clear();
  releaseGpu();
  if (canvas_item_.is_valid() && RenderingServer::get_singleton())
    RenderingServer::get_singleton()->free_rid(canvas_item_);
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
      if (!runtime_view_.is_empty()) {
        if (autostart_) attachToRuntimeView();
      } else if (autostart_ && !bundle_url_.is_empty()) {
        start();
      }
      break;
    case NOTIFICATION_ENTER_TREE:
      if (!editor && is_node_ready() && attach_wanted_) attachToRuntimeView();
      break;
    case NOTIFICATION_EXIT_TREE:
      // A surface view leaves its surface (re-entering the tree attaches again).
      if (host_) {
        bool wanted = attach_wanted_;
        detachFromRuntimeView();
        attach_wanted_ = wanted;
      }
      if (Input* in = Input::get_singleton()) {
        Callable cb(this, "_on_joy_connection_changed");
        if (in->is_connected("joy_connection_changed", cb)) in->disconnect("joy_connection_changed", cb);
      }
      break;
    case NOTIFICATION_PROCESS: process(); break;
    case NOTIFICATION_PREDELETE:
      // Our children (the audio players) go before our destructor runs.
      if (audio_) audio_->dropPlayers();
      break;
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

void JsuiView::set_force_cpu(bool on) {
  if (on == force_cpu_) return;
  force_cpu_ = on;
  // Choose again on the next frame (the GPU resources go now).
  releaseGpu();
  renderer_ = Renderer::Unknown;
  texture_.unref();
  pixels_w_ = pixels_h_ = 0;
  render_pending_ = true;
}

void JsuiView::set_pixel_ratio(float ratio) {
  pixel_ratio_ = std::clamp(ratio, 0.f, 4.f);
  render_pending_ = true;
}

void JsuiView::set_runtime_view(const NodePath& path) {
  if (path == runtime_view_) return;
  bool running = rt_ != nullptr || attach_wanted_;
  stop();
  runtime_view_ = path;
  if (is_node_ready() && (running || autostart_) && (!path.is_empty() || !bundle_url_.is_empty())) start();
}

void JsuiView::set_component(const String& key) {
  if (key == component_) return;
  component_ = key;
  if (!rt_ || key.is_empty()) return;  // an empty key applies on the next start
  std::string k = toStd(key), props = propsJson();
  jsui_surface_run(rt_, k.c_str(), props.c_str());
}

void JsuiView::set_initial_props(const Dictionary& props) {
  initial_props_ = props;
  // New props re-render the running component (React Native appProperties).
  if (!rt_ || component_.is_empty()) return;
  std::string json = propsJson();
  jsui_surface_set_props(rt_, json.c_str());
}

void JsuiView::set_auto_size(bool on) {
  if (on == auto_size_) return;
  auto_size_ = on;
  viewport_auto_ = false;
  logical_w_ = logical_h_ = pixel_w_ = pixel_h_ = 0;
  if (!on) set_custom_minimum_size(Vector2());
  render_pending_ = true;
}

std::string JsuiView::propsJson() const { return toStd(JSON::stringify(initial_props_, "", false)); }

// ---------------------------------------------------------------- surfaces

JsuiView* JsuiView::resolveRuntimeView() const {
  if (runtime_view_.is_empty() || !is_inside_tree()) return nullptr;
  auto* v = Object::cast_to<JsuiView>(get_node_or_null(runtime_view_));
  if (v == this) return nullptr;
  // A runtime view that is itself a surface view: share its runtime.
  if (v && !v->runtime_view_.is_empty()) v = v->host_;
  return v;
}

void JsuiView::attachToRuntimeView() {
  attach_wanted_ = true;
  if (host_) return;
  JsuiView* host = resolveRuntimeView();
  if (!host) return;  // retried every frame (process) until the node is there
  host_ = host;
  host->surfaces_.push_back(this);
  forwardFonts();  // before the surface lays out text
  // Textures set while this view had no runtime view belong to the runtime.
  if (!textures_.empty()) {
    std::vector<HostTexture> mine;
    mine.swap(textures_);
    for (HostTexture& e : mine) {
      if (e.texture.is_valid() && e.texture->is_connected("changed", e.changed)) e.texture->disconnect("changed", e.changed);
      host->set_texture(e.name, e.texture, e.live);
      if (e.cur.valid()) retired_mirrors_.push_back(e.cur);
      if (e.next.valid()) retired_mirrors_.push_back(e.next);
    }
    releaseTextureGpu();
  }
  if (host->rt_) createSurface();
}

void JsuiView::detachFromRuntimeView() {
  attach_wanted_ = false;
  if (!host_) return;
  destroySurface();
  auto& list = host_->surfaces_;
  list.erase(std::remove(list.begin(), list.end(), this), list.end());
  host_ = nullptr;
}

void JsuiView::createSurface() {
  if (rt_ || !host_ || !host_->rt_) return;
  if (component_.is_empty()) {
    UtilityFunctions::push_warning("JsuiView: a view with runtime_view needs a component");
    return;
  }
  Vector2 size = get_size();
  int w = auto_size_ ? 0 : std::max(1, int(std::lround(size.x)));
  int h = auto_size_ ? 0 : std::max(1, int(std::lround(size.y)));
  std::string k = toStd(component_), props = propsJson();
  rt_ = jsui_surface_create(host_->rt_, k.c_str(), props.c_str(), w, h, computeRatio());
  if (!rt_) return;
  if (profiling_) jsui_set_profiling(rt_, 1);
  jsui_set_js_frame_budget(rt_, js_frame_budget_ms_);
  start_usec_ = host_->start_usec_;
  logical_w_ = logical_h_ = pixel_w_ = pixel_h_ = 0;
  viewport_auto_ = false;
  cursor_.clear();
  render_pending_ = true;
  gpu_retained_ = false;
  updateViewport();
  if (Input* in = Input::get_singleton()) {
    TypedArray<int> pads = in->get_connected_joypads();
    for (int64_t i = 0; i < pads.size(); i++) onJoyConnectionChanged(int64_t(pads[i]), true);
  }
}

void JsuiView::destroySurface() {
  if (!rt_ || !host_) return;
  JsuiRuntime* s = rt_;
  surfaceGone();
  mouse_buttons_ = 0;
  jsui_surface_destroy(s);
  if (host_->is_inside_tree()) host_->flushMessages();  // unmount effects may have posted messages
}

void JsuiView::surfaceGone() {
  rt_ = nullptr;  // waits for the renders queued on the runtime (ours among them)
  // Our queued frame has run; its result goes with the surface.
  gpu_job_pending_ = false;
  held_keys_.clear();
  pads_.clear();
  gpu_retained_ = false;
}

void JsuiView::start() {
  if (!runtime_view_.is_empty()) {
    // A surface view: (re)creates its surface on the runtime view's runtime.
    detachFromRuntimeView();
    if (!Engine::get_singleton()->is_editor_hint()) attachToRuntimeView();
    return;
  }
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
  cfg.on_state = &JsuiView::onState;
  cfg.state_user = this;
  cfg.on_call = &JsuiView::onCall;
  cfg.on_call_cancel = &JsuiView::onCallCancel;
  cfg.call_user = this;
  cfg.log = &JsuiView::onLog;
  cfg.log_user = this;
  cfg.js_thread = js_thread_;
  // res:// and user:// live in Godot's virtual file system (and in .pck
  // files once exported), so those bundles load through FileAccess. Other
  // URLs (http:// dev servers, absolute file paths) use the built-in loader.
  if (isGodotPath(bundle_url_)) {
    cfg.load = &JsuiView::onLoad;
    cfg.load_user = this;
  }
  // The UI's sounds play through Godot's audio (jsui_audio.h).
  if (!audio_) {
    audio_ = std::make_unique<ViewAudio>(*this);
    audio_->setEnabled(audio_enabled_);
  }
  cfg.on_audio = &ViewAudio::onAudio;
  cfg.audio_user = audio_.get();
  rt_ = jsui_create(&cfg);
  if (!rt_) {
    UtilityFunctions::push_error("JsuiView: jsui_create failed");
    return;
  }
  audio_->attach(rt_.unsynced());
  if (profiling_) jsui_set_profiling(rt_, 1);
  start_usec_ = Time::get_singleton()->get_ticks_usec();
  clock_ms_ = clock_wall_ms_ = 0;
  logical_w_ = logical_h_ = pixel_w_ = pixel_h_ = 0;
  last_error_.clear();
  cursor_.clear();
  render_pending_ = true;
  updateViewport();
  // Shared state set before (or across) restarts: there before the bundle runs.
  for (auto& [key, json] : state_) jsui_set_state(rt_, key.c_str(), json.c_str());
  for (auto& [key, b] : pendingBuffers_) jsui_set_buffer(rt_, key.c_str(), b.type, b.bytes.data(), b.count);
  pendingBuffers_.clear();
  startFonts();
  // Host textures: the new runtime gets them in the first frame (syncTextures,
  // before the first tick), GPU copies already made included.
  for (HostTexture& e : textures_) {
    e.gpu_set = e.pixels_set = false;
    e.dirty = true;
  }
  // The main surface's component, and surface views waiting for a runtime
  // (their components start once the bundle has registered them).
  if (!component_.is_empty()) {
    std::string k = toStd(component_), props = propsJson();
    jsui_surface_run(rt_, k.c_str(), props.c_str());
  }
  for (JsuiView* v : surfaces_) v->createSurface();

  // Announce pads that are already connected.
  if (Input* in = Input::get_singleton()) {
    TypedArray<int> pads = in->get_connected_joypads();
    for (int64_t i = 0; i < pads.size(); i++) onJoyConnectionChanged(int64_t(pads[i]), true);
  }
  // Dev mode: shown in `jsui dev`'s shared session as "Godot: <node name>".
  std::string devName = toStd("Godot: " + String(get_name()));
  jsui_set_dev_session(rt_, devName.c_str(), 1);
  std::string url = toStd(bundle_url_);
  jsui_load_bundle_url(rt_, url.c_str());
}

void JsuiView::stop() {
  if (host_ || !runtime_view_.is_empty()) {
    detachFromRuntimeView();
    return;
  }
  if (!rt_) return;
  JsuiRuntime* rt = rt_;
  rt_ = nullptr;
  // jsui_destroy frees the surface handles of our surface views too.
  for (JsuiView* v : surfaces_) v->surfaceGone();
  // Open Host.calls are cancelled and open call_ui calls fail here; their
  // signals / callbacks still run (below), so the game can drop its work.
  jsui_destroy(rt);
  if (audio_) audio_->attach(nullptr);  // its voices stopped with the runtime
  held_keys_.clear();
  mouse_buttons_ = 0;
  pads_.clear();
  std::vector<Event> pending;
  pending.swap(messages_);
  for (auto& e : pending)
    if (e.kind == Event::CallCancel || e.kind == Event::UiResult) messages_.push_back(std::move(e));
  if (is_inside_tree()) flushMessages();
  else messages_.clear();
  ui_callbacks_.clear();
  for (HostTexture& e : textures_) e.gpu_set = e.pixels_set = e.shown_pending = false;
  // The GPU target held this runtime's frames; a new runtime repaints fully.
  gpu_retained_ = false;
}

// -------------------------------------------------------------------- audio

void JsuiView::set_audio_enabled(bool on) {
  audio_enabled_ = on;
  if (audio_) audio_->setEnabled(on);
}

void JsuiView::stop_all_sounds(const String& bus, float fade_ms) {
  if (host_) return host_->stop_all_sounds(bus, fade_ms);
  if (audio_) audio_->stopAll(toStd(bus), fade_ms);
}

Array JsuiView::get_audio_voices() const {
  if (host_) return host_->get_audio_voices();
  return audio_ ? audio_->voices() : Array();
}

String JsuiView::resolve_audio_bus(const String& bus) const {
  if (host_) return host_->resolve_audio_bus(bus);
  if (audio_bus_map_.has(bus)) return String(audio_bus_map_[bus]);
  AudioServer* as = AudioServer::get_singleton();
  if (as->get_bus_index(bus) >= 0) return bus;
  // "music" plays on a bus named "Music".
  for (int32_t i = 0, n = as->get_bus_count(); i < n; i++)
    if (as->get_bus_name(i).nocasecmp_to(bus) == 0) return as->get_bus_name(i);
  return audio_bus_.is_empty() || as->get_bus_index(audio_bus_) < 0 ? String("Master") : audio_bus_;
}

void JsuiView::reload() {
  if (host_) host_->reload();
  else if (rt_) jsui_reload(rt_);
}

// --------------------------------------------------------------- callbacks

void JsuiView::onMessage(void* user, const char* name, const char* json) {
  // Called from inside jsui_* calls: queue, emit once the runtime returned.
  auto* self = static_cast<JsuiView*>(user);
  Event e{Event::Message};
  e.name = name ? name : "";
  e.json = json ? json : "";
  self->messages_.push_back(std::move(e));
}

void JsuiView::onState(void* user, const char* key, const char* json) {
  Event e{Event::State};
  e.name = key ? key : "";
  e.null = json == nullptr;
  e.json = json ? json : "";
  static_cast<JsuiView*>(user)->messages_.push_back(std::move(e));
}

void JsuiView::onCall(void* user, uint64_t id, const char* name, const char* args) {
  Event e{Event::Call};
  e.id = id;
  e.name = name ? name : "";
  e.json = args ? args : "null";
  static_cast<JsuiView*>(user)->messages_.push_back(std::move(e));
}

void JsuiView::onCallCancel(void* user, uint64_t id) {
  Event e{Event::CallCancel};
  e.id = id;
  static_cast<JsuiView*>(user)->messages_.push_back(std::move(e));
}

void JsuiView::onCallResult(void* user, uint64_t id, int ok, const char* result) {
  Event e{Event::UiResult};
  e.id = id;
  e.ok = ok != 0;
  e.json = result ? result : "";
  static_cast<JsuiView*>(user)->messages_.push_back(std::move(e));
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
    // A font file Godot imported, in an export that ships only the imported
    // FontFile: its data is the file.
    String why;
    PackedByteArray font = isFontFile(path) && isGodotPath(path) ? importedFontData(path, why) : PackedByteArray();
    if (!font.is_empty()) {
      jsui_resource_complete(rt, id, 200, contentTypeFor(path), font.ptr(), size_t(font.size()));
      return;
    }
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

// The runtime's clock: the wall clock since start(), or with fixed_fps one
// frame's time per tick (switching keeps it monotonic).
double JsuiView::clockMs() {
  const double wall = nowMs();
  clock_ms_ += fixed_fps_ > 0 ? 1000.0 / fixed_fps_ : wall - clock_wall_ms_;
  clock_wall_ms_ = wall;
  return clock_ms_;
}

float JsuiView::computeRatio() const {
  if (pixel_ratio_ > 0.f) return pixel_ratio_;
  // Control -> window pixels: parent / CanvasLayer / Camera2D transforms
  // (get_global_transform_with_canvas) and the viewport's final transform,
  // which holds the stretch (content scale) transform. On a HiDPI display
  // (macOS Retina, screen_get_scale() == 2) the window is in physical
  // pixels, so a 1280x720 canvas_items project in a 2560x1440 window
  // rasterises at 2x while layout and input stay in the control's px.
  // Not get_screen_transform(): for a root window that embeds subwindows
  // (the default) it leaves out the stretch transform and reports 1.
  Transform2D xf = get_global_transform_with_canvas();
  if (Viewport* vp = get_viewport()) xf = vp->get_final_transform() * xf;
  Vector2 s = xf.get_scale();
  float r = std::max(std::abs(s.x), std::abs(s.y));
  if (!std::isfinite(r) || r <= 0.f) r = 1.f;
  return std::clamp(r, 0.25f, 4.f);
}

void JsuiView::updateViewport() {
  if (!rt_) return;
  if (auto_size_) {
    // The content decides the size (updateAutoSize, after the tick).
    float ratio = computeRatio();
    if (viewport_auto_ && std::abs(ratio - ratio_) < 1e-3f) return;
    viewport_auto_ = true;
    ratio_ = ratio;
    jsui_set_viewport(rt_, 0, 0, ratio);
    logical_w_ = logical_h_ = 0;
    render_pending_ = true;
    return;
  }
  viewport_auto_ = false;
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
  pixel_w_ = std::max(1, int(std::lround(lw * ratio)));
  pixel_h_ = std::max(1, int(std::lround(lh * ratio)));
  render_pending_ = true;
}

void JsuiView::updateAutoSize() {
  if (!auto_size_ || !rt_) return;
  float w = 0, h = 0;
  jsui_surface_get_size(rt_, &w, &h);
  int lw = std::max(1, int(std::ceil(w - 1e-3f)));
  int lh = std::max(1, int(std::ceil(h - 1e-3f)));
  if (lw == logical_w_ && lh == logical_h_ && pixel_w_ > 0) return;
  logical_w_ = lw;
  logical_h_ = lh;
  float ratio = std::min(ratio_, float(kMaxTexture) / float(std::max(lw, lh)));
  pixel_w_ = std::max(1, int(std::lround(lw * ratio)));
  pixel_h_ = std::max(1, int(std::lround(lh * ratio)));
  render_pending_ = true;
  set_custom_minimum_size(Vector2(float(lw), float(lh)));
}

void JsuiView::set_js_frame_budget_ms(double ms) {
  js_frame_budget_ms_ = ms;
  if (rt_) jsui_set_js_frame_budget(rt_, ms);
}

bool JsuiView::sync_js(double timeout_ms) {
  JsuiView* rv = runtimeView();
  if (!rv->rt_) return true;
  bool idle = jsui_sync(rv->rt_, timeout_ms) != 0;
  rv->flushMessages();
  if (!rv->rt_) return idle;  // a signal handler stopped it
  rv->reportErrors();
  std::vector<JsuiView*> views{rv};  // signal handlers may detach views
  views.insert(views.end(), rv->surfaces_.begin(), rv->surfaces_.end());
  for (JsuiView* v : views)
    if (rv->showsOurRuntime(v)) v->afterTick();
  return idle;
}

void JsuiView::sync_js_next_frame() { runtimeView()->sync_js_next_frame_ = true; }

void JsuiView::debug_stall_js(double ms) {
  JsuiView* rv = runtimeView();
  if (rv->rt_) jsui_debug_stall_js(rv->rt_, ms);
}

int JsuiView::get_active_js_thread() const {
  const JsuiView* rv = host_ ? host_ : this;
  return rv->rt_.unsynced() ? jsui_js_thread(rv->rt_.unsynced()) : JSUI_JS_THREAD_OFF;
}

void JsuiView::process() {
  if (attach_wanted_ && !host_) attachToRuntimeView();  // the runtime view was not there yet
  if (host_) return;  // the runtime view ticks the runtime and paints us
  // The frames queued on a separate render thread last frame, ours and our
  // surface views': they have run (Godot syncs with that thread every
  // frame), take their results.
  collectFrames();
  if (!rt_) return;
  updateViewport();
  for (JsuiView* v : surfaces_) v->updateViewport();
  // Host textures before the tick: JS sees new and resized ones in it, and
  // the views showing new contents repaint this frame.
  syncTextures();
  if (!rt_) return;
  // Sounds: fades, voices that ended, reports for this tick (and the
  // sound_played / sound_ended signals, which may stop the view).
  if (audio_) {
    audio_->update(rt_);
    if (!rt_) return;
  }
  const uint64_t tick0 = Time::get_singleton()->get_ticks_usec();
  // One tick for every surface of the runtime, then each paints.
  jsui_tick_ex(rt_, clockMs(), sync_js_next_frame_ ? JSUI_TICK_SYNC_JS : 0);
  sync_js_next_frame_ = false;
  tick_usec_last_ = Time::get_singleton()->get_ticks_usec() - tick0;
  flushMessages();
  if (!rt_) return;  // a message handler stopped the view
  reportErrors();
  std::vector<JsuiView*> views{this};  // signal handlers may detach views
  views.insert(views.end(), surfaces_.begin(), surfaces_.end());
  for (JsuiView* v : views)
    if (showsOurRuntime(v)) v->afterTick();
  // Then the views whose frame changed paint. With a separate render thread
  // the GPU frames of all of them go there as one job (queueFrameJob), which
  // uses the runtime until it has run; the main thread waits for that before
  // it touches the runtime again. So every view's work on the runtime above
  // comes first, and the CPU renderers (which paint on the main thread)
  // paint before the GPU frames are queued.
  batching_ = true;
  for (bool cpu : {true, false})
    for (JsuiView* v : views)
      if (showsOurRuntime(v) && (v->renderer_ == Renderer::Cpu) == cpu && v->rt_.unsynced() && v->render_pending_ &&
          v->is_visible_in_tree())
        v->render();
  batching_ = false;
  queueFrameJob();
  queueTextureJob();  // this frame's texture copies, if the frame job did not take them
}

bool JsuiView::showsOurRuntime(const JsuiView* v) const {
  return v == this || std::find(surfaces_.begin(), surfaces_.end(), v) != surfaces_.end();
}

void JsuiView::collectFrames() {
  std::vector<JsuiView*> views{this};  // frame_rendered handlers may detach views
  views.insert(views.end(), surfaces_.begin(), surfaces_.end());
  for (JsuiView* v : views) {
    if (!showsOurRuntime(v) || v->collectGpuRender()) continue;
    v->gpuFailed();
    v->render_pending_ = true;
  }
}

void JsuiView::afterTick() {
  if (!rt_) return;
  updateAutoSize();
  updateCursor();
  if (jsui_needs_redraw(rt_)) render_pending_ = true;
}

void JsuiView::setupPipelineCache(const String& renderer) {
  pipeline_file_ = "";
  String path = pipeline_cache_path_;
  if (!gpu_ || path.is_empty()) return;
  // One file per backend: the renderer's name before the extension.
  String ext = path.get_extension();
  path = ext.is_empty() ? path + "." + renderer : path.get_basename() + "." + renderer + "." + ext;
  // A list shipped with the game at the same path under res://.
  if (path.begins_with("user://")) {
    String shipped = "res://" + path.substr(7);
    if (FileAccess::file_exists(shipped)) {
      PackedByteArray list = FileAccess::get_file_as_bytes(shipped);
      if (jsui_gpu_precompile_pipelines(gpu_, list.ptr(), size_t(list.size())) < 0)
        UtilityFunctions::push_warning("JsuiView: " + shipped + " is not a pipeline list of this JSUI build and renderer");
    }
  }
  String file = isGodotPath(path) ? ProjectSettings::get_singleton()->globalize_path(path) : path;
  DirAccess::make_dir_recursive_absolute(file.get_base_dir());
  if (jsui_gpu_set_pipeline_cache(gpu_, file.utf8().get_data()) >= 0) pipeline_file_ = file;
}

void JsuiView::render() {
  if (pixel_w_ <= 0 || pixel_h_ <= 0) return;
  render_pending_ = false;
  const bool gpuRenderer = renderer_ == Renderer::Vulkan || renderer_ == Renderer::Metal ||
                           renderer_ == Renderer::WebGPU || renderer_ == Renderer::D3D12;
  // Also when the runtime's Skia context is not there (yet): a surface view
  // that moved to another runtime view, or one that dropped its context.
  if (renderer_ == Renderer::Unknown || (gpuRenderer && !runtimeView()->gpu_)) chooseRenderer();
  const uint64_t t0 = Time::get_singleton()->get_ticks_usec();
  // Only a steady-state GPU frame joins the runtime's frame job (renderGpu);
  // every other render queues the frames batched so far first, since it may
  // run the game's code (frame_rendered, the texture's `changed`) and that
  // must not see views in an unqueued batch (they could go meanwhile).
  if (gpuRenderer) {
    if (!renderGpu()) {
      gpuFailed();
      runtimeView()->queueFrameJob();
      renderCpu();
    }
  } else {
    runtimeView()->queueFrameJob();
    renderCpu();
  }
  const uint64_t dt = Time::get_singleton()->get_ticks_usec() - t0;
  render_usec_last_ = dt;
  render_usec_total_ += dt;
  render_usec_max_ = std::max(render_usec_max_, dt);
}

void JsuiView::showTexture(const RID& texture) {
  RenderingServer* rs = RenderingServer::get_singleton();
  if (!canvas_item_.is_valid()) {
    canvas_item_ = rs->canvas_item_create();
    rs->canvas_item_set_parent(canvas_item_, get_canvas_item());
    premul_.instantiate();
    premul_->set_blend_mode(CanvasItemMaterial::BLEND_MODE_PREMULT_ALPHA);
    rs->canvas_item_set_material(canvas_item_, premul_->get_rid());
  }
  rs->canvas_item_clear(canvas_item_);
  rs->canvas_item_add_texture_rect(canvas_item_, Rect2(0, 0, float(logical_w_), float(logical_h_)), texture);
}

void JsuiView::renderCpu() {
  if (pixels_w_ != pixel_w_ || pixels_h_ != pixel_h_) {
    pixels_w_ = pixel_w_;
    pixels_h_ = pixel_h_;
    pixels_.assign(size_t(pixel_w_) * size_t(pixel_h_) * 4, 0);
    upload_.resize(int64_t(pixels_.size()));
    std::memset(upload_.ptrw(), 0, pixels_.size());
  }
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
  if (resized) {
    if (texture_.is_null()) texture_.instantiate();
    texture_->set_image(img);
  } else {
    texture_->update(img);
  }
  showTexture(texture_->get_rid());
  renders_++;
  dirty_px_total_ += int64_t(x1 - x0) * (y1 - y0);
  emit_signal("frame_rendered", Rect2i(x0, y0, x1 - x0, y1 - y0));
}

namespace {

// Premultiplied RGBA8 -> an Image with straight alpha.
Ref<Image> unpremultipliedImage(const uint8_t* px, int w, int h) {
  const size_t n = size_t(w) * size_t(h) * 4;
  PackedByteArray out;
  out.resize(int64_t(n));
  uint8_t* d = out.ptrw();
  for (size_t i = 0; i < n; i += 4) {
    uint8_t a = px[i + 3];
    for (int c = 0; c < 3; c++)
      d[i + c] = a == 0 ? 0 : a == 255 ? px[i + c] : uint8_t(std::min(255, (px[i + c] * 255 + a / 2) / a));
    d[i + 3] = a;
  }
  return Image::create_from_data(w, h, false, Image::FORMAT_RGBA8, out);
}

}  // namespace

Ref<Texture2D> JsuiView::get_texture() const {
  if (gpu_cur_.valid() && gpu_texture_.is_valid()) return gpu_texture_;
  return texture_;
}

Ref<Image> JsuiView::get_frame_image() {
  // Copy of the last frame, un-premultiplied (screenshots / tests). The CPU
  // renderer has it in memory (so this works with --headless); the GPU one
  // reads the texture back (a GPU sync: not for every frame).
  if (gpu_cur_.valid()) {
#if defined(__EMSCRIPTEN__)
    // Browser WebGPU cannot read a texture back synchronously (the fork's
    // texture_get_data returns nothing there): the CPU raster of the frame.
    return get_cpu_frame_image();
#endif
    PackedByteArray px = readGpuFrame();
    if (px.size() < int64_t(gpu_cur_.w) * gpu_cur_.h * 4) return Ref<Image>();
    return unpremultipliedImage(px.ptr(), gpu_cur_.w, gpu_cur_.h);
  }
  if (pixels_.empty()) return Ref<Image>();
  return unpremultipliedImage(pixels_.data(), pixels_w_, pixels_h_);
}

Ref<Image> JsuiView::get_cpu_frame_image() {
  if (!rt_ || pixel_w_ <= 0 || pixel_h_ <= 0) return Ref<Image>();
  // Bring the shown frame up to date first, so both describe the same state.
  if ((render_pending_ || jsui_needs_redraw(rt_)) && is_visible_in_tree()) render();
  std::vector<uint8_t> px(size_t(pixel_w_) * size_t(pixel_h_) * 4);
  jsui_render_rgba(rt_, px.data(), pixel_w_, pixel_h_, size_t(pixel_w_) * 4);
  // That was a full repaint outside the renderers' own targets: their next
  // frame repaints fully.
  gpu_retained_ = false;
  return unpremultipliedImage(px.data(), pixel_w_, pixel_h_);
}

String JsuiView::get_renderer() const {
  switch (renderer_) {
    case Renderer::Vulkan: return "vulkan";
    case Renderer::Metal: return "metal";
    case Renderer::WebGPU: return "webgpu";
    case Renderer::D3D12: return "d3d12";
    case Renderer::Cpu: return "cpu";
    default: return "";
  }
}

Dictionary JsuiView::get_render_stats() const {
  Dictionary d;
  d["renderer"] = get_renderer();
  d["renders"] = int64_t(renders_);
  d["render_usec_total"] = int64_t(render_usec_total_);
  d["render_usec_last"] = int64_t(render_usec_last_);
  d["render_usec_max"] = int64_t(render_usec_max_);
  d["dirty_pixels_total"] = dirty_px_total_;
  // The render-thread hand-off (jsui_gpu_render.cpp): GPU render jobs, the
  // main thread's waits on the render thread (count and time), and the
  // render-thread time of the jobs and of jsui_render_gpu_target in them.
  // gpu_jobs: this view's frames handed over; render_thread_jobs: the
  // runtime's render-thread jobs for them (one per frame for all its views;
  // counted by the runtime view). gpu_frame_in_flight: the view's last
  // frame handed over has not been painted yet.
  d["gpu_jobs"] = int64_t(gpu_jobs_);
  d["render_thread_jobs"] = int64_t((host_ ? host_ : this)->render_thread_jobs_);
  d["gpu_frame_in_flight"] =
      gpu_job_pending_ && gpu_job_.fence && gpu_job_.fence->done.load(std::memory_order_acquire) < gpu_job_.id;
  d["blocking_waits"] = int64_t(blocking_waits_);
  d["main_wait_usec_total"] = int64_t(main_wait_usec_total_);
  d["gpu_job_usec_total"] = int64_t(gpu_job_usec_total_.load(std::memory_order_relaxed));
  d["jsui_render_usec_total"] = int64_t(jsui_render_usec_total_.load(std::memory_order_relaxed));
  d["tick_usec_last"] = int64_t(tick_usec_last_);
  // The runtime's pipeline cache (its Skia context is the runtime view's).
  const JsuiView* rv = host_ ? host_ : this;
  if (renderer_ != Renderer::Cpu && renderer_ != Renderer::Unknown && rv->gpu_) {
    JsuiPipelineStats s;
    jsui_gpu_get_pipeline_stats(rv->gpu_, &s);
    Dictionary p;
    p["mode"] = s.mode == JSUI_PRECOMPILE_THREAD ? "thread" : s.mode == JSUI_PRECOMPILE_SLICED ? "sliced" : "none";
    p["queued"] = s.queued;
    p["precompiled"] = int64_t(s.precompiled);
    p["failed"] = int64_t(s.failed);
    p["compiled"] = int64_t(s.compiled);
    p["added"] = int64_t(s.added);
    p["unserializable"] = int64_t(s.unserializable);
    p["known"] = int64_t(s.known);
    p["file"] = rv->pipeline_file_;
    d["pipelines"] = p;
  }
  return d;
}

void JsuiView::set_profiling(bool on) {
  profiling_ = on;
  if (rt_) jsui_set_profiling(rt_, on ? 1 : 0);
}

Dictionary JsuiView::get_perf(bool reset) {
  Dictionary d;
  if (!rt_) return d;
  JsuiPerf p;
  jsui_get_perf(rt_, &p, reset ? 1 : 0);
  d["js_us"] = p.js_us;
  d["commit_us"] = p.commit_us;
  d["animated_us"] = p.animated_us;
  d["style_us"] = p.style_us;
  d["layout_us"] = p.layout_us;
  d["input_us"] = p.input_us;
  d["paint_us"] = p.paint_us;
  d["other_us"] = p.other_us;
  d["gpu_submit_us"] = p.gpu_submit_us;
  d["image_decode_us"] = p.image_decode_us;
  d["image_decodes"] = int64_t(p.image_decodes);
  d["pipeline_compiles"] = int64_t(p.pipeline_compiles);
  d["paragraph_builds"] = int64_t(p.paragraph_builds);
  d["text_measures"] = int64_t(p.text_measures);
  d["layout_passes"] = int64_t(p.layout_passes);
  d["bridge_calls"] = int64_t(p.bridge_calls);
  d["gc_collections"] = int64_t(p.gc_collections);
  d["gc_full_collections"] = int64_t(p.gc_full_collections);
  d["gc_us"] = p.gc_us;
  d["gc_max_us"] = p.gc_max_us;
  d["js_busy_ms"] = p.js_busy_ms;
  d["js_batches_applied"] = int64_t(p.js_batches_applied);
  d["js_apply_ms"] = p.js_apply_ms;
  d["js_lag_frames"] = int64_t(p.js_lag_frames);
  d["js_sync_ms"] = p.js_sync_ms;
  return d;
}

void JsuiView::updateCursor() {
  const char* c = jsui_get_cursor(rt_);
  std::string name = c ? c : "default";
  if (name == cursor_) return;
  cursor_ = name;
  set_default_cursor_shape(cursorShape(name));
}

void JsuiView::flushMessages() {
  // A surface view's runtime callbacks queue on the runtime view.
  if (host_) {
    host_->flushMessages();
    return;
  }
  if (messages_.empty()) return;
  auto batch = std::move(messages_);
  messages_.clear();
  for (auto& e : batch) {
    String j = String::utf8(e.json.c_str());
    switch (e.kind) {
      case Event::Message: {
        Variant data = j.is_empty() ? Variant() : JSON::parse_string(j);
        emit_signal("message_received", String::utf8(e.name.c_str()), data, j);
        break;
      }
      case Event::State:
        if (rt_) rememberState(e.name);
        emit_signal("state_changed", String::utf8(e.name.c_str()), e.null ? Variant() : JSON::parse_string(j));
        break;
      case Event::Call:
        emit_signal("call_requested", int64_t(e.id), String::utf8(e.name.c_str()), JSON::parse_string(j));
        break;
      case Event::CallCancel: emit_signal("call_cancelled", int64_t(e.id)); break;
      case Event::UiResult: {
        Variant result = e.ok ? JSON::parse_string(j) : Variant(j);
        auto it = ui_callbacks_.find(e.id);
        if (it != ui_callbacks_.end()) {
          Callable cb = it->second;
          ui_callbacks_.erase(it);
          if (cb.is_valid()) cb.call(e.ok, result);
        }
        emit_signal("ui_call_completed", int64_t(e.id), e.ok, result);
        break;
      }
    }
  }
}

// ------------------------------------------------------------ game data

namespace {

// RFC 7386 on Variants, for patch_state while no runtime exists.
Variant mergePatch(const Variant& target, const Variant& patch) {
  if (patch.get_type() != Variant::DICTIONARY) return patch;
  Dictionary out = target.get_type() == Variant::DICTIONARY ? Dictionary(target).duplicate() : Dictionary();
  Dictionary p = patch;
  Array keys = p.keys();
  for (int64_t i = 0; i < keys.size(); i++) {
    Variant k = keys[i];
    Variant v = p[k];
    if (v.get_type() == Variant::NIL) out.erase(k);
    else out[k] = mergePatch(out.get(k, Variant()), v);
  }
  return out;
}

bool validJson(const String& json) {
  Ref<JSON> parser;
  parser.instantiate();
  return parser->parse(json) == OK;
}

}  // namespace

void JsuiView::rememberState(const std::string& key) {
  const char* json = jsui_get_state(rt_, key.c_str());
  auto it = std::find_if(state_.begin(), state_.end(), [&](auto& kv) { return kv.first == key; });
  if (!json) {
    if (it != state_.end()) state_.erase(it);
  } else if (it != state_.end()) {
    it->second = json;
  } else {
    state_.emplace_back(key, json);
  }
}

int JsuiView::set_state(const String& key, const Variant& value) {
  return set_state_json(key, value.get_type() == Variant::NIL ? String() : JSON::stringify(value, "", false));
}

int JsuiView::set_state_json(const String& key, const String& json) {
  if (host_) return host_->set_state_json(key, json);
  std::string k = toStd(key), j = toStd(json);
  if (rt_) {
    int r = jsui_set_state(rt_, k.c_str(), json.is_empty() ? nullptr : j.c_str());
    if (r > 0) rememberState(k);
    return r;
  }
  auto it = std::find_if(state_.begin(), state_.end(), [&](auto& kv) { return kv.first == k; });
  if (json.is_empty()) {
    if (it == state_.end()) return 0;
    state_.erase(it);
    return 1;
  }
  if (!validJson(json)) {
    UtilityFunctions::push_error("JsuiView.set_state(\"" + key + "\"): invalid JSON");
    return -1;
  }
  if (it != state_.end()) {
    if (it->second == j) return 0;
    it->second = j;
  } else {
    state_.emplace_back(k, j);
  }
  return 1;
}

int JsuiView::patch_state(const String& key, const Variant& patch) {
  return patch_state_json(key, JSON::stringify(patch, "", false));
}

int JsuiView::patch_state_json(const String& key, const String& json) {
  if (host_) return host_->patch_state_json(key, json);
  std::string k = toStd(key), j = toStd(json);
  if (rt_) {
    int r = jsui_patch_state(rt_, k.c_str(), j.c_str());
    if (r > 0) rememberState(k);
    return r;
  }
  if (!validJson(json)) {
    UtilityFunctions::push_error("JsuiView.patch_state(\"" + key + "\"): invalid JSON");
    return -1;
  }
  Variant merged = mergePatch(get_state(key), JSON::parse_string(json));
  return set_state(key, merged);
}

Variant JsuiView::get_state(const String& key) const {
  String json = get_state_json(key);
  return json.is_empty() ? Variant() : JSON::parse_string(json);
}

String JsuiView::get_state_json(const String& key) const {
  if (host_) return host_->get_state_json(key);
  std::string k = toStd(key);
  if (rt_) {
    const char* json = jsui_get_state(rt_, k.c_str());
    return json ? String::utf8(json) : String();
  }
  for (auto& [sk, sj] : state_)
    if (sk == k) return String::utf8(sj.c_str());
  return String();
}

int JsuiView::resolve_call(int64_t id, const Variant& result) {
  return resolve_call_json(id, JSON::stringify(result, "", false));
}

int JsuiView::resolve_call_json(int64_t id, const String& json) {
  if (host_) return host_->resolve_call_json(id, json);
  if (!rt_ || id <= 0) return -1;
  std::string j = toStd(json);
  int r = jsui_resolve_call(rt_, uint64_t(id), json.is_empty() ? nullptr : j.c_str());
  flushMessages();
  return r;
}

int JsuiView::reject_call(int64_t id, const String& message) {
  if (host_) return host_->reject_call(id, message);
  if (!rt_ || id <= 0) return -1;
  std::string m = toStd(message);
  int r = jsui_reject_call(rt_, uint64_t(id), m.c_str());
  flushMessages();
  return r;
}

int64_t JsuiView::call_ui(const String& name, const Variant& args, const Callable& callback) {
  if (host_) return host_->call_ui(name, args, callback);
  if (!rt_) {
    UtilityFunctions::push_error("JsuiView.call_ui: the view is not running");
    return 0;
  }
  std::string n = toStd(name), a = toStd(JSON::stringify(args, "", false));
  uint64_t id = jsui_call_ui(rt_, n.c_str(), a.c_str(), &JsuiView::onCallResult, this);
  if (callback.is_valid()) ui_callbacks_[id] = callback;
  return int64_t(id);
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

int JsuiView::get_pending_loads() const {
  if (host_) return host_->get_pending_loads();
  return rt_ ? jsui_pending_loads(rt_) : 0;
}

String JsuiView::get_last_error() const {
  if (host_) return host_->get_last_error();
  if (!rt_) return String();
  const char* e = jsui_last_error(rt_);
  return e ? String::utf8(e) : String();
}

void JsuiView::post_message(const String& name, const Variant& data) {
  post_message_json(name, data.get_type() == Variant::NIL ? String() : JSON::stringify(data));
}

void JsuiView::post_message_json(const String& name, const String& json) {
  if (host_) {
    host_->post_message_json(name, json);
    return;
  }
  if (!rt_) return;
  std::string n = toStd(name), j = toStd(json);
  jsui_post_message(rt_, n.c_str(), j.c_str());
  flushMessages();
}

// -------------------------------------------------------------- frame data

int JsuiView::set_buffer(const String& key, const Variant& data) {
  if (host_) return host_->set_buffer(key, data);
  const void* ptr = nullptr;
  size_t count = 0;
  JsuiBufferType type = JSUI_BUFFER_F32;
  constexpr JsuiBufferType kReal = sizeof(real_t) == 8 ? JSUI_BUFFER_F64 : JSUI_BUFFER_F32;
  // The packed arrays are copied into locals (copy-on-write: no data copy).
  PackedFloat32Array f32;
  PackedFloat64Array f64;
  PackedInt32Array i32;
  PackedByteArray u8;
  PackedVector2Array v2;
  PackedVector3Array v3;
  PackedColorArray col;
  switch (data.get_type()) {
    case Variant::PACKED_FLOAT32_ARRAY: f32 = data; ptr = f32.ptr(); count = size_t(f32.size()); break;
    case Variant::PACKED_FLOAT64_ARRAY: f64 = data; ptr = f64.ptr(); count = size_t(f64.size()); type = JSUI_BUFFER_F64; break;
    case Variant::PACKED_INT32_ARRAY: i32 = data; ptr = i32.ptr(); count = size_t(i32.size()); type = JSUI_BUFFER_I32; break;
    case Variant::PACKED_BYTE_ARRAY: u8 = data; ptr = u8.ptr(); count = size_t(u8.size()); type = JSUI_BUFFER_U8; break;
    case Variant::PACKED_VECTOR2_ARRAY: v2 = data; ptr = v2.ptr(); count = size_t(v2.size()) * 2; type = kReal; break;
    case Variant::PACKED_VECTOR3_ARRAY: v3 = data; ptr = v3.ptr(); count = size_t(v3.size()) * 3; type = kReal; break;
    case Variant::PACKED_COLOR_ARRAY: col = data; ptr = col.ptr(); count = size_t(col.size()) * 4; break;
    default:
      UtilityFunctions::push_error("JsuiView.set_buffer(\"" + key + "\"): expected a packed array, got " +
                                   Variant::get_type_name(data.get_type()));
      return -1;
  }
  std::string k = toStd(key);
  if (rt_) return jsui_set_buffer(rt_, k.c_str(), type, ptr, count);
  PendingBuffer b{type, {}, count};
  if (ptr) b.bytes.assign(static_cast<const uint8_t*>(ptr), static_cast<const uint8_t*>(ptr) + count * (type == JSUI_BUFFER_F64 ? 8 : type == JSUI_BUFFER_U8 ? 1 : 4));
  auto it = std::find_if(pendingBuffers_.begin(), pendingBuffers_.end(), [&](auto& kv) { return kv.first == k; });
  if (it != pendingBuffers_.end()) it->second = std::move(b);
  else pendingBuffers_.emplace_back(k, std::move(b));
  return 1;
}

int JsuiView::remove_buffer(const String& key) {
  if (host_) return host_->remove_buffer(key);
  std::string k = toStd(key);
  if (rt_) return jsui_remove_buffer(rt_, k.c_str());
  auto it = std::find_if(pendingBuffers_.begin(), pendingBuffers_.end(), [&](auto& kv) { return kv.first == k; });
  if (it == pendingBuffers_.end()) return 0;
  pendingBuffers_.erase(it);
  return 1;
}

// ----------------------------------------------------------- host textures

namespace {

// Godot textures hold straight alpha, but a viewport's: its 2D / 3D renders
// into a transparent background are effectively premultiplied (what
// CanvasItemMaterial's BLEND_MODE_PREMULT_ALPHA is for).
int textureFlagsFor(const Ref<Texture2D>& t) {
  return Object::cast_to<ViewportTexture>(t.ptr()) ? 0 : JSUI_TEXTURE_UNPREMULTIPLIED;
}

}  // namespace

// -------------------------------------------------------------------- fonts

bool JsuiView::register_font(const String& family, const Variant& font, int weight, int italic) {
  String why;
  PackedByteArray data = fontBytes(font, why);
  if (data.is_empty()) {
    UtilityFunctions::push_error("JsuiView.register_font(\"" + family + "\"): " + why);
    return false;
  }
  FontOp op;
  op.family = toStd(family);
  op.data = data;
  op.weight = weight > 0 ? std::min(weight, 1000) : 0;
  op.italic = italic < 0 ? -1 : italic > 0 ? 1 : 0;
  return applyFontOp(std::move(op));
}

bool JsuiView::unregister_font(const String& family) {
  FontOp op;
  op.unregister = true;
  op.family = toStd(family);
  return applyFontOp(std::move(op));
}

bool JsuiView::applyFontOp(FontOp op) {
  if (host_) return host_->applyFontOp(std::move(op));
  // A surface view that is not attached has no rt_: the call waits in its log.
  const bool live = rt_ != nullptr;
  const bool ok = live ? runFontOp(op) : true;
  if (!op.unregister) {
    if (ok) font_ops_.push_back(std::move(op));  // with a runtime, only fonts it took
    return ok;
  }
  // Drop the logged registrations (and removals) of the family. The removal
  // itself is kept only while faces whose family the log does not name (the
  // file's own: family "", the `fonts` property) may have it.
  const String family = String::utf8(op.family.c_str());
  bool dropped = false, unnamed = !fonts_.is_empty();
  std::vector<FontOp> keep;
  for (FontOp& o : font_ops_) {
    if (String::utf8(o.family.c_str()).nocasecmp_to(family) == 0) {
      dropped = dropped || !o.unregister;
      continue;
    }
    if (!o.unregister && o.family.empty()) unnamed = true;
    keep.push_back(std::move(o));
  }
  font_ops_.swap(keep);
  if (unnamed) font_ops_.push_back(std::move(op));
  return live ? ok : dropped;
}

bool JsuiView::runFontOp(const FontOp& op) {
  if (op.unregister) return jsui_unregister_font(rt_, op.family.c_str()) == 0;
  if (jsui_register_font(rt_, op.family.c_str(), op.data.ptr(), size_t(op.data.size()), op.weight, op.italic) == 0)
    return true;
  UtilityFunctions::push_error("JsuiView.register_font(\"" + String::utf8(op.family.c_str()) +
                               "\"): not a font (TTF, OTF, TTC, WOFF or WOFF2)");
  return false;
}

void JsuiView::set_font_fallbacks(const PackedStringArray& families) {
  if (host_) return host_->set_font_fallbacks(families);
  PackedStringArray list;
  for (int64_t i = 0; i < families.size(); i++) {
    String f = families[i].strip_edges();
    if (!f.is_empty()) list.push_back(f);
  }
  font_fallbacks_ = toStd(String(",").join(list));
  font_fallbacks_set_ = true;
  if (rt_) jsui_set_font_fallbacks(rt_, font_fallbacks_.c_str());
}

void JsuiView::set_default_font_family(const String& family) {
  if (host_) return host_->set_default_font_family(family);
  default_font_family_ = toStd(family.strip_edges());
  default_font_set_ = true;
  if (rt_) jsui_set_default_font_family(rt_, default_font_family_.c_str());
}

void JsuiView::applyFontSettings() {
  if (font_fallbacks_set_) jsui_set_font_fallbacks(rt_, font_fallbacks_.c_str());
  if (default_font_set_) jsui_set_default_font_family(rt_, default_font_family_.c_str());
}

void JsuiView::set_fonts(const Array& fonts) {
  // startFonts() passes fonts_ itself, and godot-cpp's Array self-assignment
  // drops the array before taking the new reference.
  if (&fonts != &fonts_) fonts_ = fonts;
  if (host_) {
    forwardFonts();
    return;
  }
  if (!rt_ || !runtime_view_.is_empty()) return;  // registered by start() / on attach
  // A running runtime view: the fonts the array adds.
  for (int64_t i = 0; i < fonts_.size(); i++) {
    Variant v = fonts_[i];
    if (v.get_type() == Variant::NIL || fonts_applied_.has(v)) continue;
    fonts_applied_.append(v);
    String why;
    PackedByteArray data = fontBytes(v, why);
    if (data.is_empty()) {
      UtilityFunctions::push_error("JsuiView.fonts[" + String::num_int64(i) + "]: " + why);
      continue;
    }
    if (jsui_register_font(rt_, "", data.ptr(), size_t(data.size()), 0, -1) != 0)
      UtilityFunctions::push_error("JsuiView.fonts[" + String::num_int64(i) + "]: not a font (TTF, OTF, TTC, WOFF or WOFF2)");
  }
}

void JsuiView::startFonts() {
  fonts_applied_.clear();
  set_fonts(fonts_);
  // The logged calls, in order; registrations the new runtime refuses are
  // dropped (reported once).
  std::vector<FontOp> ops;
  ops.swap(font_ops_);
  for (FontOp& op : ops)
    if (runFontOp(op) || op.unregister) font_ops_.push_back(std::move(op));
  applyFontSettings();
}

void JsuiView::forwardFonts() {
  if (!host_) return;
  if (fonts_host_ != host_) {
    fonts_applied_.clear();
    fonts_host_ = host_;
  }
  // The runtime view keeps them (its log), so they outlive its restarts.
  for (int64_t i = 0; i < fonts_.size(); i++) {
    Variant v = fonts_[i];
    if (v.get_type() == Variant::NIL || fonts_applied_.has(v)) continue;
    fonts_applied_.append(v);
    host_->register_font("", v, 0, -1);
  }
  std::vector<FontOp> ops;
  ops.swap(font_ops_);
  for (FontOp& op : ops) host_->applyFontOp(std::move(op));
  if (font_fallbacks_set_) {
    host_->font_fallbacks_ = font_fallbacks_;
    host_->font_fallbacks_set_ = true;
    if (host_->rt_) jsui_set_font_fallbacks(host_->rt_, font_fallbacks_.c_str());
  }
  if (default_font_set_) {
    host_->default_font_family_ = default_font_family_;
    host_->default_font_set_ = true;
    if (host_->rt_) jsui_set_default_font_family(host_->rt_, default_font_family_.c_str());
  }
  font_fallbacks_set_ = default_font_set_ = false;
}

int JsuiView::set_texture(const String& name, const Ref<Texture2D>& texture, bool live) {
  if (host_) return host_->set_texture(name, texture, live);
  if (texture.is_null()) return remove_texture(name);
  waitRender();  // a queued render-thread job reads textures_
  const std::string key = toStd(name);
  auto it = std::find_if(textures_.begin(), textures_.end(), [&](const HostTexture& e) { return e.key == key; });
  if (it != textures_.end() && it->texture == texture) {
    if (it->live == live) return 0;
    it->live = live;
    it->dirty = true;
    return 1;
  }
  if (it == textures_.end()) {
    textures_.emplace_back();
    it = textures_.end() - 1;
    it->key = key;
    it->name = name;
    it->changed = callable_mp(this, &JsuiView::onTextureChanged).bind(name);
  } else if (it->texture.is_valid() && it->texture->is_connected("changed", it->changed)) {
    it->texture->disconnect("changed", it->changed);
  }
  // Another texture under the same name: its copy goes into the same mirror
  // when the size and format match (no gap on screen), into a new one if not.
  it->texture = texture;
  it->live = live;
  it->dirty = true;
  it->flags = textureFlagsFor(texture);
  it->src = RID();
  it->src_ok = false;
  it->cpu_only = false;
  it->warned = false;
  // ImageTexture.update() / set_image() emit `changed`: new contents.
  if (!texture->is_connected("changed", it->changed)) texture->connect("changed", it->changed);
  return 1;
}

int JsuiView::remove_texture(const String& name) {
  if (host_) return host_->remove_texture(name);
  waitRender();
  const std::string key = toStd(name);
  auto it = std::find_if(textures_.begin(), textures_.end(), [&](const HostTexture& e) { return e.key == key; });
  if (it == textures_.end()) return 0;
  // The runtime lets go first; the mirrors are freed by the next texture job,
  // after any frame that sampled them (Godot defers the free past them).
  if (rt_ && (it->gpu_set || it->pixels_set)) jsui_remove_texture(rt_, key.c_str());
  if (it->texture.is_valid() && it->texture->is_connected("changed", it->changed)) it->texture->disconnect("changed", it->changed);
  if (it->cur.valid()) retired_mirrors_.push_back(it->cur);
  if (it->next.valid()) retired_mirrors_.push_back(it->next);
  textures_.erase(it);
  return 1;
}

int JsuiView::update_texture(const String& name) {
  if (host_) return host_->update_texture(name);
  const std::string key = toStd(name);
  for (HostTexture& e : textures_)
    if (e.key == key) {
      e.dirty = true;  // only the main thread reads it
      return 1;
    }
  return -1;
}

void JsuiView::onTextureChanged(const String& name) {
  const std::string key = toStd(name);
  for (HostTexture& e : textures_)
    if (e.key == key) e.dirty = true;
}

bool JsuiView::uploadTexturePixels(HostTexture& e) {
  // ImageTexture: a copy of what the RenderingServer holds (a GPU readback on
  // Forward+ / Mobile); a ViewportTexture: the viewport's last frame.
  Ref<Image> img = e.texture->get_image();
  if (img.is_null() || img->is_empty()) return false;
  if (img->is_compressed() || img->get_format() != Image::FORMAT_RGBA8) {
    Ref<Image> copy;
    copy.instantiate();
    copy->copy_from(img);  // the original may be the server's own (headless)
    if (copy->is_compressed() && copy->decompress() != OK) return false;
    copy->convert(Image::FORMAT_RGBA8);
    img = copy;
  }
  const int w = img->get_width(), h = img->get_height();
  PackedByteArray data = img->get_data();  // mipmaps follow level 0: not read
  if (w <= 0 || h <= 0 || data.size() < int64_t(w) * h * 4) return false;
  return jsui_set_texture_pixels(rt_, e.key.c_str(), data.ptr(), w, h, size_t(w) * 4, e.flags) > 0;
}

void JsuiView::syncTextures() {
  if (textures_.empty() && retired_mirrors_.empty()) return;
  waitRender();  // the last texture work has run (in steady state it has)
  JsuiRuntime* rt = rt_;
  if (!rt) return;
  // GPU copies when the runtime renders on the GPU (one Skia context for all
  // its views); CPU pixels when any of its views paints with the CPU raster,
  // which cannot sample GPU textures (but every renderer draws pixels).
  bool anyCpu = false, anyChosen = false;
  for (JsuiView* v : surfaces_) {
    anyCpu = anyCpu || v->renderer_ == Renderer::Cpu;
    anyChosen = anyChosen || v->renderer_ != Renderer::Unknown;
  }
  anyCpu = anyCpu || renderer_ == Renderer::Cpu;
  anyChosen = anyChosen || renderer_ != Renderer::Unknown;
  if (!anyChosen && !textures_.empty()) chooseRenderer();  // what the first render would do
  const TextureMode mode = !anyCpu && renderer_ != Renderer::Cpu && gpu_ ? TextureMode::Gpu : TextureMode::Pixels;
  if (mode != texture_mode_) {
    for (HostTexture& e : textures_) {
      e.pixels_set = false;
      e.cpu_only = false;
      e.src = RID();
      e.src_ok = false;
      e.dirty = true;
    }
    texture_mode_ = mode;
  }
  texture_frame_ = Engine::get_singleton()->get_frames_drawn();
  bool work = !retired_mirrors_.empty();
  for (HostTexture& e : textures_) {
    if (e.texture.is_null()) continue;
    if (mode == TextureMode::Gpu && e.cpu_only && !e.warned) {
      e.warned = true;
      e.pixels_set = false;
      UtilityFunctions::push_warning("JsuiView: texture \"" + e.name + "\" " + String::utf8(e.why.c_str()) +
                                     ": copying it through the CPU instead (get_image(), slow for live textures)");
    }
    if (mode == TextureMode::Pixels || e.cpu_only) {
      if (e.live || e.dirty || !e.pixels_set) {
        if (mode == TextureMode::Gpu) {
          // doTextureWork checks whether the GPU path can take a new source.
          e.rs = e.texture->get_rid();
          e.copy = true;
          work = true;
        }
        e.dirty = false;
        e.pixels_set = uploadTexturePixels(e);
      }
      // Pixels replace a GPU copy in the runtime (or nothing does: then it
      // lets go of it now); the copies go with this frame's texture work.
      if (e.cur.valid() || e.next.valid()) {
        if (e.gpu_set && !e.pixels_set) jsui_remove_texture(rt, e.key.c_str());
        if (e.cur.valid()) retired_mirrors_.push_back(e.cur);
        if (e.next.valid()) retired_mirrors_.push_back(e.next);
        e.cur = e.next = TextureMirror{};
        e.gpu_set = e.shown_pending = false;
        work = true;
      }
      continue;
    }
    // A mirror made in an earlier frame: Godot has submitted that frame (its
    // first copy and layout pass), so the runtime can sample it now. The one
    // it replaces is freed by this frame's texture work, after every frame
    // that sampled it.
    if (e.next.valid() && texture_frame_ > e.next.created_frame) {
      if (e.cur.valid()) retired_mirrors_.push_back(e.cur);
      e.cur = e.next;
      e.next = TextureMirror{};
      e.gpu_set = false;
      e.shown_pending = false;
      work = true;
    }
    if (e.cur.valid() && (!e.gpu_set || e.gpu_flags != e.flags)) {
      JsuiTexture t{};
      t.kind = gpu_api_ == Renderer::Vulkan  ? JSUI_TEXTURE_VULKAN
               : gpu_api_ == Renderer::Metal ? JSUI_TEXTURE_METAL
               : gpu_api_ == Renderer::D3D12 ? JSUI_TEXTURE_D3D12
                                             : JSUI_TEXTURE_WEBGPU;
      t.handle = e.cur.handle;
      t.format = e.cur.format;
      t.width = e.cur.w;
      t.height = e.cur.h;
      t.flags = e.flags;
      // Vulkan: sampled + transfer src / dst, as the mirror is created. D3D12:
      // the state the mirror is in when JSUI renders, COPY_DEST (Godot's
      // tracker leaves it there after the copy; JSUI transitions it back).
      t.usage = gpu_api_ == Renderer::D3D12 ? 0x400u : 0u;
      e.gpu_set = jsui_set_texture(rt, e.key.c_str(), &t) >= 0;
      e.gpu_flags = e.flags;
      e.pixels_set = false;
    } else if (e.gpu_set && e.shown_pending) {
      jsui_texture_updated(rt, e.key.c_str());  // last frame's copy: repaint the views showing it
    }
    e.shown_pending = false;
    // This frame's copy (recorded into Godot's frame by doTextureWork).
    e.rs = e.texture->get_rid();
    e.copy = e.live || e.dirty || (!e.cur.valid() && !e.next.valid());
    e.dirty = false;
    work = work || e.copy;
  }
  if (!work) return;
  RenderingServer* rs = RenderingServer::get_singleton();
  if (!rs || rs->is_on_render_thread()) {
    doTextureWork();  // RenderingServer on this thread: now
  } else {
    texture_work_ = true;  // with the frame job, or a job of its own (queueTextureJob)
  }
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

int JsuiView::hit_test(const Vector2& position) const {
  JsuiRuntime* rt = rt_;
  return rt ? jsui_hit_test(rt, float(position.x), float(position.y)) : 0;
}

bool JsuiView::_has_point(const Vector2& point) const {
  if (!input_pass_through_ || Engine::get_singleton()->is_editor_hint())
    return Rect2(Vector2(), get_size()).has_point(point);
  // A press that reached the UI keeps the pointer until it is released.
  if (mouse_buttons_ && int64_t(Input::get_singleton()->get_mouse_button_mask()) != 0) return true;
  return hit_test(point) != 0;
}

void JsuiView::_gui_input(const Ref<InputEvent>& event) {
  if (!rt_ || event.is_null()) return;
  bool consumed = false;

  if (const auto* mm = Object::cast_to<InputEventMouseMotion>(event.ptr())) {
    if (mm->get_device() == kEmulatedDevice) return;  // touch already forwarded as touch
    Vector2 p = mm->get_position();
    consumed = jsui_send_mouse_move(rt_, p.x, p.y) != 0;
    // A drag whose press went to the game (or a control behind the view)
    // stays theirs while it passes over the UI.
    if (int64_t(mm->get_button_mask()) != 0 && !mouse_buttons_) consumed = false;
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
      int bit = 1 << int(b);
      if (!mb->is_pressed() && !(mouse_buttons_ & bit)) {
        // The press went elsewhere: the UI never saw it.
        flushMessages();
        return;
      }
      if (mb->is_pressed()) mouse_buttons_ |= bit;
      else mouse_buttons_ &= ~bit;
      consumed = jsui_send_mouse_button(rt_, p.x, p.y, b, mb->is_pressed() ? 1 : 0, modsOf(mb)) != 0;
      // Clicks the UI passes on don't take the keyboard away from the game.
      if (consumed && mb->is_pressed() && get_focus_mode() != FOCUS_NONE && !has_focus()) grab_focus();
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
