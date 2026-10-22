// JsuiView: a Godot Control that runs a JSUI bundle and shows it.
//
// The runtime ticks in NOTIFICATION_PROCESS, paints with
// jsui_render_rgba_incremental into a CPU buffer that persists between
// frames, and uploads to an ImageTexture only when pixels changed. The
// texture is drawn by an internal canvas item with premultiplied-alpha
// blending (JSUI produces premultiplied RGBA), so the node's own `material`
// stays free for the game.
#pragma once

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/canvas_item_material.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/rid.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "jsui/jsui.h"

namespace jsui_godot {

class JsuiView : public godot::Control {
  GDCLASS(JsuiView, godot::Control)

 public:
  JsuiView();
  ~JsuiView() override;

  // Properties.
  void set_bundle_url(const godot::String& url);
  godot::String get_bundle_url() const { return bundle_url_; }
  void set_dev_mode(bool on);
  bool get_dev_mode() const { return dev_mode_; }
  void set_pixel_ratio(float ratio);
  float get_pixel_ratio() const { return pixel_ratio_; }
  void set_autostart(bool on) { autostart_ = on; }
  bool get_autostart() const { return autostart_; }
  void set_forward_gamepad(bool on) { forward_gamepad_ = on; }
  bool get_forward_gamepad() const { return forward_gamepad_; }

  // Methods.
  void start();   // (re)creates the runtime and loads bundle_url
  void stop();    // destroys the runtime
  void reload();  // re-evaluates the last bundle (keeps the runtime)
  bool is_running() const { return rt_ != nullptr; }
  void post_message(const godot::String& name, const godot::Variant& data);
  void post_message_json(const godot::String& name, const godot::String& json);

  // Shared state (jsui_set_state / jsui_patch_state / jsui_get_state). Values
  // are JSON-encoded; null removes the key. Kept across start()/stop():
  // the view re-applies them to every new runtime.
  int set_state(const godot::String& key, const godot::Variant& value);
  int set_state_json(const godot::String& key, const godot::String& json);
  int patch_state(const godot::String& key, const godot::Variant& patch);
  int patch_state_json(const godot::String& key, const godot::String& json);
  godot::Variant get_state(const godot::String& key) const;
  godot::String get_state_json(const godot::String& key) const;

  // Calls: Host.call() -> call_requested signal -> resolve_call / reject_call;
  // call_ui() -> Host.handle() handler -> callback(ok, result) + ui_call_completed.
  int resolve_call(int64_t id, const godot::Variant& result);
  int resolve_call_json(int64_t id, const godot::String& json);
  int reject_call(int64_t id, const godot::String& message);
  int64_t call_ui(const godot::String& name, const godot::Variant& args, const godot::Callable& callback);
  godot::Ref<godot::ImageTexture> get_texture() const { return texture_; }
  godot::Ref<godot::Image> get_frame_image() const;
  int get_pending_loads() const;
  godot::String get_last_error() const;
  float get_effective_pixel_ratio() const { return ratio_; }

  void _gui_input(const godot::Ref<godot::InputEvent>& event) override;
  void _notification(int what);

 protected:
  static void _bind_methods();

 private:
  static void onMessage(void* user, const char* name, const char* json);
  static void onState(void* user, const char* key, const char* json);
  static void onCall(void* user, uint64_t id, const char* name, const char* args);
  static void onCallCancel(void* user, uint64_t id);
  static void onCallResult(void* user, uint64_t id, int ok, const char* result);
  static void onLog(void* user, JsuiLogLevel level, const char* message);
  static void onLoad(void* user, JsuiRuntime* rt, uint64_t id, const char* url);

  void process();
  void updateViewport();
  void render();
  void updateCursor();
  void flushMessages();
  void reportErrors();
  void releaseHeldInput();
  void onJoyConnectionChanged(int64_t device, bool connected);
  bool handleGamepad(const godot::Ref<godot::InputEvent>& event);
  float computeRatio() const;
  double nowMs() const;

  godot::String bundle_url_;
  bool dev_mode_ = false;
  float pixel_ratio_ = 0.f;  // 0: follow the canvas -> screen scale
  bool autostart_ = true;
  bool forward_gamepad_ = true;

  JsuiRuntime* rt_ = nullptr;
  uint64_t start_usec_ = 0;
  int logical_w_ = 0, logical_h_ = 0, pixel_w_ = 0, pixel_h_ = 0;
  float ratio_ = 1.f;
  bool render_pending_ = true;

  std::vector<uint8_t> pixels_;   // jsui's incremental target (stable address)
  godot::PackedByteArray upload_; // staging copy handed to the Image
  godot::Ref<godot::ImageTexture> texture_;
  godot::Ref<godot::CanvasItemMaterial> premul_;
  godot::RID canvas_item_;

  // Runtime callbacks run while JS is on the stack: they are queued in order
  // and emitted as signals once the runtime returned (flushMessages).
  struct Event {
    enum Kind { Message, State, Call, CallCancel, UiResult } kind;
    uint64_t id = 0;
    bool ok = false;
    bool null = false;  // State: the key was removed
    std::string name, json;
  };
  std::vector<Event> messages_;
  // Shared state set from Godot, re-applied when the runtime is recreated
  // (start() after stop(), bundle_url / dev_mode changes). Insertion order.
  std::vector<std::pair<std::string, std::string>> state_;
  void rememberState(const std::string& key);
  std::map<uint64_t, godot::Callable> ui_callbacks_;  // call_ui id -> callback
  std::string last_error_;
  std::string cursor_;
  struct HeldKey {
    std::string key, code;
  };
  std::map<int64_t, HeldKey> held_keys_;  // physical keycode -> what was sent on press
  struct Pad {
    bool trigger[2] = {false, false};
    bool buttons[17] = {};
  };
  std::map<int64_t, Pad> pads_;
};

}  // namespace jsui_godot
