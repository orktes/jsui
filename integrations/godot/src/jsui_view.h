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

  std::vector<std::pair<std::string, std::string>> messages_;
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
