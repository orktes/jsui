// zwp_text_input_v3 (text-input-unstable-v3, wlroots' wlr_text_input_v3):
// the text input protocol Chromium / Electron (--enable-wayland-ime
// --wayland-text-input-version=3) and GTK 4 speak. As with text_input_v2.h,
// jsui-wl runs no input method: the shell's on-screen keyboard types through
// the virtual keyboard, so what it needs is when a text field in the focused
// app wants a keyboard. The client enables a text input (enable + commit)
// when a field gets focus and disables it when the field loses it; the
// server sends enter / leave as the keyboard focus moves to and from the
// client's surfaces.
#pragma once

#include <wayland-server-core.h>

#include <algorithm>
#include <functional>
#include <vector>

extern "C" {
#define static
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_text_input_v3.h>
#undef static
}

class TextInputV3 {
 public:
  // `onPanel(show)`: a text field of the focused app wants the on-screen
  // keyboard (true) or no longer does (false).
  bool create(wl_display* display, std::function<void(bool)> onPanel) {
    onPanel_ = std::move(onPanel);
    manager_ = wlr_text_input_manager_v3_create(display);
    if (!manager_) return false;
    add(&manager_->events.text_input, [this](void* d) { added(static_cast<wlr_text_input_v3*>(d)); });
    return true;
  }

  // The keyboard focus moved to `surface` (null: none).
  void focus(wlr_surface* surface) {
    focus_ = surface;
    for (Input* in : inputs_) refocus(in);
  }

 private:
  struct Listener {
    wl_listener l{};
    std::function<void(void*)> fn;
  };
  struct Input {
    wlr_text_input_v3* ti;
    std::vector<Listener*> listeners;
    bool shown = false;
  };

  Listener* add(wl_signal* sig, std::function<void(void*)> fn) {
    auto* l = new Listener;
    l->fn = std::move(fn);
    l->l.notify = [](wl_listener* ll, void* data) { reinterpret_cast<Listener*>(ll)->fn(data); };
    wl_signal_add(sig, &l->l);
    return l;
  }

  void added(wlr_text_input_v3* ti) {
    auto* in = new Input{ti, {}};
    inputs_.push_back(in);
    in->listeners.push_back(add(&ti->events.enable, [this, in](void*) { panel(in, true); }));
    // A commit while enabled: the field (or what it wants) changed; the panel stays.
    in->listeners.push_back(add(&ti->events.commit, [this, in](void*) {
      if (in->ti->current_enabled) panel(in, true);
    }));
    in->listeners.push_back(add(&ti->events.disable, [this, in](void*) { panel(in, false); }));
    in->listeners.push_back(add(&ti->events.destroy, [this, in](void*) {
      panel(in, false);
      for (Listener* l : in->listeners) {
        wl_list_remove(&l->l.link);
        delete l;
      }
      inputs_.erase(std::remove(inputs_.begin(), inputs_.end(), in), inputs_.end());
      delete in;
    }));
    refocus(in);
  }

  // Enter the focused surface if it belongs to this text input's client;
  // leave whatever it had otherwise.
  void refocus(Input* in) {
    wlr_text_input_v3* ti = in->ti;
    bool ours = focus_ && wl_resource_get_client(ti->resource) == wl_resource_get_client(focus_->resource);
    if (ti->focused_surface && ti->focused_surface != (ours ? focus_ : nullptr)) {
      panel(in, false);
      wlr_text_input_v3_send_leave(ti);
    }
    if (ours && ti->focused_surface != focus_) wlr_text_input_v3_send_enter(ti, focus_);
  }

  // Only the text input with the keyboard focus asks for the panel.
  void panel(Input* in, bool show) {
    if (show && (!focus_ || in->ti->focused_surface != focus_)) return;
    if (in->shown == show) return;
    in->shown = show;
    if (onPanel_) onPanel_(show);
  }

  wlr_text_input_manager_v3* manager_ = nullptr;
  std::vector<Input*> inputs_;
  wlr_surface* focus_ = nullptr;
  std::function<void(bool)> onPanel_;
};
