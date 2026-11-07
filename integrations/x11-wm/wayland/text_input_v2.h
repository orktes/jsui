// zwp_text_input_v2 (text-input-unstable-v2, KDE / Qt): the text input
// protocol Qt 5.15's Wayland client speaks (wlroots implements only v3).
// jsui-wl does not run an input method: the shell's on-screen keyboard types
// through the virtual keyboard. What it needs from the protocol is when a text
// field in the focused app wants a keyboard: Qt sends enable +
// show_input_panel when one gets focus, hide_input_panel / disable when it
// loses it. The server sends enter / leave with the keyboard focus, and
// reports the panel as shown or hidden (input_panel_state).
#pragma once

#include <wayland-server-core.h>

#include <algorithm>
#include <functional>
#include <vector>

#include "text-input-unstable-v2-protocol.h"

class TextInputV2 {
 public:
  // `onPanel(show)`: the focused app asked for the on-screen keyboard
  // (true) or no longer needs it (false).
  bool create(wl_display* display, std::function<void(bool)> onPanel) {
    onPanel_ = std::move(onPanel);
    global_ = wl_global_create(display, &zwp_text_input_manager_v2_interface, 1, this, &TextInputV2::bindManager);
    return global_ != nullptr;
  }

  // The keyboard focus moved to `surface` (null: none).
  void focus(wl_resource* surface) {
    if (surface == focus_) return;
    if (focus_) {
      wl_list_remove(&focusGone_.link);
      wl_list_init(&focusGone_.link);
    }
    if (focus_) {
      for (Input* in : inputs_)
        if (wl_resource_get_client(in->resource) == wl_resource_get_client(focus_))
          zwp_text_input_v2_send_leave(in->resource, ++serial_, focus_);
      if (shown_) setPanel(false);
    }
    focus_ = surface;
    if (focus_) {
      focusGone_.notify = [](wl_listener* l, void*) {
        TextInputV2* self = wl_container_of(l, self, focusGone_);
        wl_list_remove(&self->focusGone_.link);
        wl_list_init(&self->focusGone_.link);
        self->focus_ = nullptr;  // the surface went: no leave to send
        if (self->shown_) self->setPanel(false);
      };
      wl_resource_add_destroy_listener(focus_, &focusGone_);
    }
    if (focus_)
      for (Input* in : inputs_)
        if (wl_resource_get_client(in->resource) == wl_resource_get_client(focus_))
          zwp_text_input_v2_send_enter(in->resource, ++serial_, focus_);
  }

  // The on-screen keyboard went away by other means (the user closed it).
  void panelClosed() {
    shown_ = false;
    for (Input* in : inputs_) zwp_text_input_v2_send_input_panel_state(in->resource, 0, 0, 0, 0, 0);
  }

 private:
  struct Input {
    TextInputV2* owner;
    wl_resource* resource;
    bool enabled = false;
  };

  static void bindManager(wl_client* client, void* data, uint32_t version, uint32_t id) {
    static const struct zwp_text_input_manager_v2_interface impl = {
        /*destroy*/ [](wl_client*, wl_resource* r) { wl_resource_destroy(r); },
        /*get_text_input*/
        [](wl_client* client, wl_resource* manager, uint32_t id, wl_resource*) {
          auto* self = static_cast<TextInputV2*>(wl_resource_get_user_data(manager));
          wl_resource* r = wl_resource_create(client, &zwp_text_input_v2_interface, 1, id);
          if (!r) {
            wl_client_post_no_memory(client);
            return;
          }
          auto* in = new Input{self, r};
          wl_resource_set_implementation(r, &TextInputV2::inputImpl(), in, [](wl_resource* res) {
            auto* in = static_cast<Input*>(wl_resource_get_user_data(res));
            auto& v = in->owner->inputs_;
            v.erase(std::remove(v.begin(), v.end(), in), v.end());
            delete in;
          });
          self->inputs_.push_back(in);
          // Already focused: it starts with an enter, as if focus had just come.
          if (self->focus_ && wl_resource_get_client(self->focus_) == client)
            zwp_text_input_v2_send_enter(r, ++self->serial_, self->focus_);
        },
    };
    wl_resource* r = wl_resource_create(client, &zwp_text_input_manager_v2_interface, std::min<int>(version, 1), id);
    if (!r) {
      wl_client_post_no_memory(client);
      return;
    }
    wl_resource_set_implementation(r, &impl, data, nullptr);
  }

  static const struct zwp_text_input_v2_interface& inputImpl() {
    static struct zwp_text_input_v2_interface impl = [] {
      struct zwp_text_input_v2_interface i{};
      i.destroy = [](wl_client*, wl_resource* r) { wl_resource_destroy(r); };
      i.enable = [](wl_client*, wl_resource* r, wl_resource*) { self(r)->enabled = true; };
      i.disable = [](wl_client*, wl_resource* r, wl_resource*) {
        Input* in = self(r);
        in->enabled = false;
        if (in->owner->shown_) in->owner->setPanel(false);
      };
      i.show_input_panel = [](wl_client*, wl_resource* r) { self(r)->owner->setPanel(true); };
      i.hide_input_panel = [](wl_client*, wl_resource* r) { self(r)->owner->setPanel(false); };
      i.set_surrounding_text = [](wl_client*, wl_resource*, const char*, int32_t, int32_t) {};
      i.set_content_type = [](wl_client*, wl_resource*, uint32_t, uint32_t) {};
      i.set_cursor_rectangle = [](wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {};
      i.set_preferred_language = [](wl_client*, wl_resource*, const char*) {};
      i.update_state = [](wl_client*, wl_resource*, uint32_t, uint32_t) {};
      return i;
    }();
    return impl;
  }

  static Input* self(wl_resource* r) { return static_cast<Input*>(wl_resource_get_user_data(r)); }

  void setPanel(bool show) {
    if (show == shown_) return;
    shown_ = show;
    for (Input* in : inputs_) zwp_text_input_v2_send_input_panel_state(in->resource, show ? 1 : 0, 0, 0, 0, 0);
    if (onPanel_) onPanel_(show);
  }

  wl_global* global_ = nullptr;
  std::function<void(bool)> onPanel_;
  std::vector<Input*> inputs_;
  wl_resource* focus_ = nullptr;
  wl_listener focusGone_{};
  uint32_t serial_ = 0;
  bool shown_ = false;
};
