// jsui-wl on Ubuntu Touch: present the shell through Mir's client API.
//
// Ubuntu Touch's screen belongs to lomiri-system-compositor (a Mir 1.8 server
// on Android's hwcomposer). Lomiri itself is a nested Mir server: it connects
// to the system compositor as a mirclient session ($MIR_SERVER_HOST_SOCKET,
// named $MIR_SERVER_NAME so the system compositor knows which session to
// show) and puts one fullscreen window up. jsui-wl does the same in --mir
// mode: wlroots runs headless (it still serves Wayland to the apps), and each
// shell frame (which already holds every window, see main.cpp) is copied into
// a software buffer of that Mir window. Touch, pointer and key events of the
// window come back on Mir's thread and are queued for the Wayland event loop.
#pragma once

// The mirclient window API (buffer stream of a normal window, software
// buffers) is deprecated in Mir 1.x but is what Mir 1.8 on Ubuntu Touch has.
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

#include <mir_toolkit/mir_client_library.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

struct MirHostEvent {
  enum Kind { Touch, Pointer, Key, Resize, Close } kind;
  int id = 0;     // touch id; key: evdev scan code
  int phase = 0;  // touch / pointer: 0 down, 1 move, 2 up; key: 1 down, 0 up
  float x = 0, y = 0;
  int w = 0, h = 0;
};

class MirHost {
 public:
  ~MirHost() {
    if (window_) mir_window_release_sync(window_);
    if (conn_) mir_connection_release(conn_);
    if (fd_ >= 0) close(fd_);
  }

  // Connects to the host Mir server. Returns false (with a message) if there
  // is none.
  bool connect(const char* socket, const char* name, std::string* err) {
    conn_ = mir_connect_sync(socket, name);
    if (!mir_connection_is_valid(conn_)) {
      *err = mir_connection_get_error_message(conn_);
      mir_connection_release(conn_);
      conn_ = nullptr;
      return false;
    }
    fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    return true;
  }

  // Clockwise rotation (0, 90, 180, 270) of the desktop on the panel: the
  // Tab A7's panel is natively 1200x2000 portrait, so a tablet held in
  // landscape needs 90 or 270. Sizes and events in this class's API are in
  // desktop (logical) coordinates; the Mir window is in panel coordinates.
  void setRotation(int deg) {
    deg = ((deg % 360) + 360) % 360;
    rot_ = deg - deg % 90;
  }

  // The size of the first connected output's current mode, rotated.
  bool outputSize(int* w, int* h, uint32_t* outputId) {
    MirDisplayConfig* cfg = mir_connection_create_display_configuration(conn_);
    if (!cfg) return false;
    bool found = false;
    for (int i = 0; i < mir_display_config_get_num_outputs(cfg) && !found; ++i) {
      const MirOutput* o = mir_display_config_get_output(cfg, size_t(i));
      if (mir_output_get_connection_state(o) != mir_output_connection_state_connected || !mir_output_is_enabled(o)) continue;
      const MirOutputMode* m = mir_output_get_current_mode(o);
      if (!m) continue;
      *w = mir_output_mode_get_width(m);
      *h = mir_output_mode_get_height(m);
      *outputId = uint32_t(mir_output_get_id(o));
      // A portrait panel mounted sideways (the Tab A7's is landscape already).
      MirOrientation rot = mir_output_get_orientation(o);
      if (rot == mir_orientation_left || rot == mir_orientation_right) std::swap(*w, *h);
      if (rot_ == 90 || rot_ == 270) std::swap(*w, *h);
      found = true;
    }
    mir_display_config_release(cfg);
    return found;
  }

  bool createWindow(int w, int h, uint32_t outputId, std::string* err) {
    // RGBA in memory (R first) is Mir's ABGR_8888 on little-endian.
    MirPixelFormat formats[16];
    unsigned n = 0;
    mir_connection_get_available_surface_formats(conn_, formats, 16, &n);
    format_ = mir_pixel_format_invalid;
    for (unsigned i = 0; i < n; ++i) {
      if (formats[i] == mir_pixel_format_abgr_8888 || formats[i] == mir_pixel_format_xbgr_8888) {
        format_ = formats[i];
        break;
      }
    }
    if (format_ == mir_pixel_format_invalid) {
      for (unsigned i = 0; i < n; ++i)
        if (formats[i] == mir_pixel_format_argb_8888 || formats[i] == mir_pixel_format_xrgb_8888) {
          format_ = formats[i];
          swapRB_ = true;
          break;
        }
    }
    if (format_ == mir_pixel_format_invalid && n == 0) format_ = mir_pixel_format_abgr_8888;  // none listed: try
    if (format_ == mir_pixel_format_invalid) {
      *err = "no 32-bit RGB window format (" + std::to_string(n) + " offered)";
      return false;
    }
    w_ = w;
    h_ = h;
    int pw = w, ph = h;
    if (rot_ == 90 || rot_ == 270) std::swap(pw, ph);
    MirWindowSpec* spec = mir_create_normal_window_spec(conn_, pw, ph);
    mir_window_spec_set_pixel_format(spec, format_);
    mir_window_spec_set_buffer_usage(spec, mir_buffer_usage_software);
    mir_window_spec_set_name(spec, "jsui-wl");
    mir_window_spec_set_fullscreen_on_output(spec, outputId);
    mir_window_spec_set_state(spec, mir_window_state_fullscreen);
    mir_window_spec_set_event_handler(spec, &MirHost::onEvent, this);
    window_ = mir_create_window_sync(spec);
    mir_window_spec_release(spec);
    if (!mir_window_is_valid(window_)) {
      *err = mir_window_get_error_message(window_);
      return false;
    }
    stream_ = mir_window_get_buffer_stream(window_);
    mir_buffer_stream_set_swapinterval(stream_, 0);  // the Wayland loop paces frames
    return true;
  }

  // Readable when events are queued (add it to the Wayland event loop).
  int fd() const { return fd_; }

  std::vector<MirHostEvent> takeEvents() {
    uint64_t v;
    while (read(fd_, &v, sizeof v) > 0) {
    }
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<MirHostEvent> out;
    out.swap(queue_);
    return out;
  }

  // w, h: desktop size.
  void resize(int w, int h) {
    if (!stream_ || (w == w_ && h == h_)) return;
    w_ = w;
    h_ = h;
    if (rot_ == 90 || rot_ == 270) std::swap(w, h);
    mir_buffer_stream_set_size(stream_, w, h);
  }

  // Copies an RGBA (R first) desktop frame into the window's next buffer,
  // rotated onto the panel, and shows it.
  void present(const uint8_t* rgba, int width, int height, size_t stride) {
    if (!stream_) return;
    MirGraphicsRegion r;
    if (!mir_buffer_stream_get_graphics_region(stream_, &r)) return;
    auto* dst = reinterpret_cast<uint8_t*>(r.vaddr);
    if (rot_ == 0) {
      int w = std::min(width, r.width), h = std::min(height, r.height);
      for (int y = 0; y < h; ++y) {
        const uint8_t* s = rgba + size_t(y) * stride;
        uint8_t* d = dst + size_t(y) * size_t(r.stride);
        if (!swapRB_) std::memcpy(d, s, size_t(w) * 4);
        else
          for (int x = 0; x < w; ++x) put(d + x * 4, s + x * 4);
      }
    } else {
      // Panel pixel (px, py) shows desktop pixel (lx, ly).
      for (int py = 0; py < r.height; ++py) {
        uint8_t* d = dst + size_t(py) * size_t(r.stride);
        for (int px = 0; px < r.width; ++px) {
          int lx, ly;
          if (rot_ == 90) lx = py, ly = height - 1 - px;
          else if (rot_ == 270) lx = width - 1 - py, ly = px;
          else lx = width - 1 - px, ly = height - 1 - py;
          if (lx < 0 || ly < 0 || lx >= width || ly >= height) continue;
          put(d + px * 4, rgba + size_t(ly) * stride + size_t(lx) * 4);
        }
      }
    }
    mir_buffer_stream_swap_buffers_sync(stream_);
  }

 private:
  void put(uint8_t* d, const uint8_t* s) const {
    if (swapRB_) {
      d[0] = s[2], d[1] = s[1], d[2] = s[0], d[3] = 255;
    } else {
      std::memcpy(d, s, 4);
    }
  }

  // Panel coordinates -> desktop coordinates.
  void toDesktop(float* x, float* y) const {
    float px = *x, py = *y;
    if (rot_ == 90) *x = py, *y = float(h_) - px;
    else if (rot_ == 270) *x = float(w_) - py, *y = px;
    else if (rot_ == 180) *x = float(w_) - px, *y = float(h_) - py;
  }

  void push(MirHostEvent e) {
    if (e.kind == MirHostEvent::Touch || e.kind == MirHostEvent::Pointer) toDesktop(&e.x, &e.y);
    if (e.kind == MirHostEvent::Resize && (rot_ == 90 || rot_ == 270)) std::swap(e.w, e.h);
    {
      std::lock_guard<std::mutex> lock(mu_);
      queue_.push_back(e);
    }
    uint64_t one = 1;
    ssize_t n = write(fd_, &one, sizeof one);
    (void)n;
  }

  // Mir's thread.
  static void onEvent(MirWindow*, const MirEvent* ev, void* u) {
    auto* self = static_cast<MirHost*>(u);
    switch (mir_event_get_type(ev)) {
      case mir_event_type_input: {
        const MirInputEvent* ie = mir_event_get_input_event(ev);
        switch (mir_input_event_get_type(ie)) {
          case mir_input_event_type_touch: {
            const MirTouchEvent* te = mir_input_event_get_touch_event(ie);
            for (unsigned i = 0; i < mir_touch_event_point_count(te); ++i) {
              MirHostEvent e{MirHostEvent::Touch};
              e.id = int(mir_touch_event_id(te, i));
              MirTouchAction a = mir_touch_event_action(te, i);
              e.phase = a == mir_touch_action_down ? 0 : a == mir_touch_action_up ? 2 : 1;
              e.x = mir_touch_event_axis_value(te, i, mir_touch_axis_x);
              e.y = mir_touch_event_axis_value(te, i, mir_touch_axis_y);
              self->push(e);
            }
            break;
          }
          case mir_input_event_type_pointer: {
            const MirPointerEvent* pe = mir_input_event_get_pointer_event(ie);
            MirPointerAction a = mir_pointer_event_action(pe);
            MirHostEvent e{MirHostEvent::Pointer};
            e.x = mir_pointer_event_axis_value(pe, mir_pointer_axis_x);
            e.y = mir_pointer_event_axis_value(pe, mir_pointer_axis_y);
            bool down = mir_pointer_event_button_state(pe, mir_pointer_button_primary);
            if (a == mir_pointer_action_button_down && down && !self->pointerDown_) e.phase = 0;
            else if (a == mir_pointer_action_button_up && !down && self->pointerDown_) e.phase = 2;
            else if (a == mir_pointer_action_motion) e.phase = 1;
            else break;
            self->pointerDown_ = down;
            self->push(e);
            break;
          }
          case mir_input_event_type_key: {
            const MirKeyboardEvent* ke = mir_input_event_get_keyboard_event(ie);
            MirKeyboardAction a = mir_keyboard_event_action(ke);
            if (a == mir_keyboard_action_repeat) break;
            MirHostEvent e{MirHostEvent::Key};
            e.id = mir_keyboard_event_scan_code(ke);
            e.phase = a == mir_keyboard_action_down ? 1 : 0;
            self->push(e);
            break;
          }
          default:
            break;
        }
        break;
      }
      case mir_event_type_resize: {
        const MirResizeEvent* re = mir_event_get_resize_event(ev);
        MirHostEvent e{MirHostEvent::Resize};
        e.w = mir_resize_event_get_width(re);
        e.h = mir_resize_event_get_height(re);
        self->push(e);
        break;
      }
      case mir_event_type_close_window:
        self->push(MirHostEvent{MirHostEvent::Close});
        break;
      default:
        break;
    }
  }

  MirConnection* conn_ = nullptr;
  MirWindow* window_ = nullptr;
  MirBufferStream* stream_ = nullptr;
  MirPixelFormat format_ = mir_pixel_format_invalid;
  bool swapRB_ = false;
  bool pointerDown_ = false;
  int w_ = 0, h_ = 0;  // desktop size
  int rot_ = 0;
  int fd_ = -1;
  std::mutex mu_;
  std::vector<MirHostEvent> queue_;
};
