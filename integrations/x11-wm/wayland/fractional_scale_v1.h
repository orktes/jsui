// wp_fractional_scale_v1 (staging): tells the apps that ask (Chromium /
// Electron, GTK 4, Qt 6) the scale to render at, the shell's own (1.75 on the
// Tab A7). Such an app treats the sizes it is configured with as logical
// and draws its buffer that many times larger, setting the surface's size
// back with wp_viewporter. jsui-wl keeps windows in screen px: for a surface
// that asked here (has()), it divides what it configures by the scale and
// multiplies what the app reports (Client::cs in main.cpp).
// wlroots has wlr_fractional_scale_v1 too, but cannot say which surfaces
// asked, so this is the protocol itself (a global and one object per surface).
// Only some programs get the shell's scale (`allow`, by process name:
// Chromium / Electron); the rest hear 1 and draw as before. GTK 3 binds the
// protocol too, and on the Tab A7 its GPU path (libhybris EGL) never
// attached a buffer at the scaled size.
#pragma once

#include <wayland-server-core.h>

#include <cmath>
#include <functional>
#include <memory>
#include <unordered_map>

extern "C" {
#define static
#include <wlr/types/wlr_compositor.h>
#undef static
}

#include "fractional-scale-v1-protocol.h"

class FractionalScale {
 public:
  bool create(wl_display* display, double scale, std::function<bool(wl_client*)> allow) {
    scale_ = scale;
    allow_ = std::move(allow);
    global_ = wl_global_create(display, &wp_fractional_scale_manager_v1_interface, 1, this, &FractionalScale::bindManager);
    return global_ != nullptr;
  }

  // The surface asked for its scale and was given the shell's.
  bool has(wlr_surface* s) const {
    auto it = s ? objects_.find(s) : objects_.end();
    return it != objects_.end() && it->second->scaled;
  }

  // The shell's scale changed: every surface that asked hears the new one.
  void setScale(double scale) {
    scale_ = scale;
    for (auto& [s, o] : objects_)
      if (o->scaled) send(o->resource, scale_);
  }

 private:
  struct Object {
    wl_resource* resource;
    bool scaled;
    wl_listener surfaceDestroy;
    FractionalScale* self;
    wlr_surface* surface;
  };

  static void send(wl_resource* r, double scale) {
    wp_fractional_scale_v1_send_preferred_scale(r, uint32_t(std::lround(scale * 120)));
  }

  static void bindManager(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* r = wl_resource_create(client, &wp_fractional_scale_manager_v1_interface, int(version), id);
    if (!r) return wl_client_post_no_memory(client);
    static const struct wp_fractional_scale_manager_v1_interface impl = {
        [](wl_client*, wl_resource* r) { wl_resource_destroy(r); },
        &FractionalScale::getFractionalScale,
    };
    wl_resource_set_implementation(r, &impl, data, nullptr);
  }

  static void getFractionalScale(wl_client* client, wl_resource* manager, uint32_t id, wl_resource* surfaceRes) {
    auto* self = static_cast<FractionalScale*>(wl_resource_get_user_data(manager));
    wlr_surface* surface = wlr_surface_from_resource(surfaceRes);
    if (self->objects_.count(surface)) {
      wl_resource_post_error(manager, WP_FRACTIONAL_SCALE_MANAGER_V1_ERROR_FRACTIONAL_SCALE_EXISTS,
                             "the surface has a fractional scale object already");
      return;
    }
    wl_resource* r = wl_resource_create(client, &wp_fractional_scale_v1_interface, wl_resource_get_version(manager), id);
    if (!r) return wl_client_post_no_memory(client);
    static const struct wp_fractional_scale_v1_interface impl = {
        [](wl_client*, wl_resource* r) { wl_resource_destroy(r); },
    };
    // Held by pointer: the listener's links must not move when the map grows.
    auto& slot = self->objects_[surface];
    slot = std::make_unique<Object>();
    Object& o = *slot;
    o.resource = r;
    o.scaled = !self->allow_ || self->allow_(client);
    o.self = self;
    o.surface = surface;
    o.surfaceDestroy.notify = [](wl_listener* l, void*) {
      Object* ob = wl_container_of(l, ob, surfaceDestroy);
      wl_resource_set_user_data(ob->resource, nullptr);
      ob->self->erase(ob->surface);
    };
    wl_signal_add(&surface->events.destroy, &o.surfaceDestroy);
    wl_resource_set_implementation(r, &impl, &o, [](wl_resource* r) {
      if (auto* ob = static_cast<Object*>(wl_resource_get_user_data(r))) ob->self->erase(ob->surface);
    });
    send(r, o.scaled ? self->scale_ : 1.0);
  }

  void erase(wlr_surface* s) {
    auto it = objects_.find(s);
    if (it == objects_.end()) return;
    wl_list_remove(&it->second->surfaceDestroy.link);
    wl_resource_set_user_data(it->second->resource, nullptr);
    objects_.erase(it);
  }

  wl_global* global_ = nullptr;
  double scale_ = 1;
  std::function<bool(wl_client*)> allow_;
  std::unordered_map<wlr_surface*, std::unique_ptr<Object>> objects_;
};
