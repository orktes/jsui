// jsui-wl on Ubuntu Touch: present the shell through Mir's client API.
//
// Ubuntu Touch's screen belongs to lomiri-system-compositor (a Mir 1.8 server
// on Android's hwcomposer). Lomiri itself is a nested Mir server: it connects
// to the system compositor as a mirclient session ($MIR_SERVER_HOST_SOCKET,
// named $MIR_SERVER_NAME so the system compositor knows which session to
// show) and puts one fullscreen window up. jsui-wl does the same in --mir
// mode: wlroots runs headless (it still serves Wayland to the apps), and each
// shell frame (which already holds every window, see main.cpp) is copied into
// that Mir window. Touch, pointer and key events of the window come back on
// Mir's thread and are queued for the Wayland event loop.
//
// With JSUI_WL_MIR_GL the window is a GPU (EGL/GLES) window by default: Mir 1's
// android2 client platform hands out a real ANativeWindow, which libhybris'
// "null" EGL platform passes straight to the Android EGL driver. JSUI then draws
// the desktop with Skia on the GPU into a texture, and one textured draw puts
// it on the panel, rotated. If any EGL step fails the window falls back to a
// software buffer that the CPU frame is copied into.
#pragma once

// The mirclient window API (buffer stream of a normal window, software
// buffers) is deprecated in Mir 1.x but is what Mir 1.8 on Ubuntu Touch has.
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

#include <mir_toolkit/mir_client_library.h>
#ifdef JSUI_WL_MIR_GL
#include <EGL/egl.h>
#ifdef JSUI_WL_ANDROID_WINDOW
#include <system/window.h>
#endif
#include <GLES2/gl2.h>
using GlSync = struct __GLsync*;  // GLES 3 sync objects (gl3.h's GLsync)
#endif
#include <sys/eventfd.h>

#include <memory>

#include "mir_vulkan.h"
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
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
#ifdef JSUI_WL_MIR_VK
    vk_.reset();
#endif
#ifdef JSUI_WL_MIR_GL
    destroyGl();
#endif
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
  int rotation() const { return rot_; }

  // Turns the desktop to `deg` while running (main thread; the render
  // context current with the GPU window). The desktop size swaps on a
  // quarter turn, the panel's stays. With the GPU window the last frame is
  // kept and turns from the old angle to the new one, fading into the new
  // frames (kRotateMs). False on the Vulkan window (not supported there).
  bool rotateTo(int deg) {
    deg = ((deg % 360) + 360) % 360;
    deg -= deg % 90;
#ifdef JSUI_WL_MIR_VK
    if (vk_) return false;
#endif
    if (deg == rot_) return true;
#ifdef JSUI_WL_MIR_GL
    if (gpu_ && tex_) {
      const double now = nowMs();
      float from = float(rot_.load());
      int fromW = w_, fromH = h_;
      if (oldTex_) {
        // Turned again mid-turn: start from where the picture is now, with
        // the latest frame.
        BlitPose p = poseAt(now);
        from = p.angle;
        fromW = int(p.w);
        fromH = int(p.h);
        doomedTex_.push_back(oldTex_);
      }
      oldTex_ = tex_;
      oldW_ = fromW;
      oldH_ = fromH;
      tex_ = 0;
      texFresh_ = true;
      animFrom_ = from;
      animStart_ = now;
      animDone_ = false;
    }
#endif
    if ((deg - rot_) % 180 != 0) {
      const int w = w_;
      w_ = h_.load();
      h_ = w;
    }
    rot_ = deg;
#ifdef JSUI_WL_MIR_GL
    texFresh_ = true;
#endif
    return true;
  }
  // A turn is on screen: present every frame until it is done.
  bool rotating() const {
#ifdef JSUI_WL_MIR_GL
    return oldTex_ && !animDone_;
#else
    return false;
#endif
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

  // wantGpu: try an EGL window first (see the top of this file).
  bool createWindow(int w, int h, uint32_t outputId, bool wantGpu, std::string* err) {
    w_ = w;
    h_ = h;
    outputId_ = outputId;
#ifdef JSUI_WL_MIR_VK
    // Opt-in (JSUI_WL_VULKAN=1): on the Tab A7's Adreno 610 Graphite records
    // a frame in half the CPU time of Ganesh but takes about twice the GPU
    // time, and apps' GPU buffers aren't imported into Vulkan yet.
    const char* vkEnv = getenv("JSUI_WL_VULKAN");
    if (wantGpu && vkEnv && vkEnv[0] == '1') {
      std::string why;
      if (createVkWindow(&why)) return true;
      vkError_ = why;
    }
#endif
#ifdef JSUI_WL_MIR_GL
    if (wantGpu) {
      std::string why;
      if (createGlWindow(&why)) return true;
      glError_ = why;
    }
#else
    (void)wantGpu;
#endif
    return createSoftwareWindow(err);
  }

  // Why the GPU window could not be used ("" when it is in use or not asked).
  const std::string& gpuError() const { return glError_; }

  // Gives up the GPU window (e.g. Skia could not use the GL context) for a
  // software one.
  bool fallBackToSoftware(std::string* err) {
#ifdef JSUI_WL_MIR_VK
    vk_.reset();
#endif
#ifdef JSUI_WL_MIR_GL
    destroyGl();
#endif
    if (window_) mir_window_release_sync(window_);
    window_ = nullptr;
    stream_ = nullptr;
    return createSoftwareWindow(err);
  }

  bool gpu() const { return gpu_; }
#ifdef JSUI_WL_MIR_VK
  // The Vulkan presenter when the window is a Vulkan one (else null), and
  // why it isn't ("" when it is or wasn't tried).
  MirVulkan* vulkan() const { return vk_.get(); }
  const std::string& vulkanError() const { return vkError_; }
#endif

  bool createSoftwareWindow(std::string* err) {
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
    if (!makeWindow(format_, mir_buffer_usage_software, err)) return false;
    mir_buffer_stream_set_swapinterval(stream_, 0);  // the Wayland loop paces frames
    gpu_ = false;
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
#ifdef JSUI_WL_MIR_GL
    texFresh_ = true;  // reallocated at the next desktopTexture()
#endif
  }

  // Software window: copies an RGBA (R first) desktop frame into the window's
  // next buffer, rotated onto the panel, and shows it.
  void present(const uint8_t* rgba, int width, int height, size_t stride) {
    if (!stream_ || gpu_) return;
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
      // Panel pixel (px, py) shows desktop pixel (lx, ly). Walked in 32x32
      // tiles so both sides stay in cache (a straight walk strides the source
      // a whole row per pixel when rotating by 90 / 270).
      constexpr int T = 32;
      const int pw = r.width, ph = r.height;
      for (int ty = 0; ty < ph; ty += T) {
        const int ey = std::min(ph, ty + T);
        for (int tx = 0; tx < pw; tx += T) {
          const int ex = std::min(pw, tx + T);
          for (int py = ty; py < ey; ++py) {
            auto* d = reinterpret_cast<uint32_t*>(dst + size_t(py) * size_t(r.stride));
            for (int px = tx; px < ex; ++px) {
              int lx, ly;
              if (rot_ == 90) lx = py, ly = height - 1 - px;
              else if (rot_ == 270) lx = width - 1 - py, ly = px;
              else lx = width - 1 - px, ly = height - 1 - py;
              if (lx < 0 || ly < 0 || lx >= width || ly >= height) continue;
              uint32_t v;
              std::memcpy(&v, rgba + size_t(ly) * stride + size_t(lx) * 4, 4);
              if (swapRB_) v = (v & 0xff00ff00u) | ((v & 0xffu) << 16) | ((v >> 16) & 0xffu) | 0xff000000u;
              d[px] = v;
            }
          }
        }
      }
    }
    mir_buffer_stream_swap_buffers_sync(stream_);
  }

#ifdef JSUI_WL_MIR_GL
  // GPU window. Makes the window's GL context current.
  // With the present thread (see presentGpu), the main thread renders in a
  // context of its own that shares the desktop texture with the window's.
  bool makeCurrent() {
    if (!gpu_) return false;
    if (threaded_) return eglMakeCurrent(dpy_, renderSurf_, renderSurf_, renderCtx_);
    return eglMakeCurrent(dpy_, surf_, surf_, ctx_);
  }
  EGLDisplay eglDisplay() const { return dpy_; }
  bool presentThread() const { return threaded_; }
  // Readable after each swap of the present thread: a vsync-aligned frame
  // clock for the main loop.
  int swapFd() const { return swapFd_; }
  // JSUI_WL_GPU_FINISH with the present thread: the last frame's GPU time
  // (what was left of it when the present thread took it) and the blit's.
  double lastRenderGpuMs() const { return threadRenderGpuMs_.load(); }
  double lastBlitGpuMs() const { return threadBlitGpuMs_.load(); }

  // A copy of the desktop texture as it is now, and a blurred 1/16-size one
  // (four 2x linear downscales of the copy), all on the GPU in the current
  // context: textures of their own, replaced by the next call. False
  // without GLES 3 (glBlitFramebuffer).
  struct Snapshot {
    unsigned full = 0, blurred = 0;
    int fullW = 0, fullH = 0, blurW = 0, blurH = 0;
  };
  bool snapshotDesktop(Snapshot* out) {
    if (!tex_ || !es3_) return false;
    if (!blitFramebuffer_)
      blitFramebuffer_ = reinterpret_cast<void (*)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)>(
          eglGetProcAddress("glBlitFramebuffer"));
    if (!blitFramebuffer_) return false;
    GLuint fb[2];
    glGenFramebuffers(2, fb);
    GLuint src = tex_;
    int sw = w_, sh = h_;
    for (int i = 0; i < kSnapLevels; ++i) {
      int dw = i == 0 ? sw : std::max(1, sw / 2), dh = i == 0 ? sh : std::max(1, sh / 2);
      if (!snapTex_[i] || snapW_[i] != dw || snapH_[i] != dh) {
        if (!snapTex_[i]) glGenTextures(1, &snapTex_[i]);
        glBindTexture(GL_TEXTURE_2D, snapTex_[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, 0x8058 /* GL_RGBA8 */, dw, dh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        snapW_[i] = dw;
        snapH_[i] = dh;
      }
      glBindFramebuffer(0x8CA8 /* GL_READ_FRAMEBUFFER */, fb[0]);
      glFramebufferTexture2D(0x8CA8, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, src, 0);
      glBindFramebuffer(0x8CA9 /* GL_DRAW_FRAMEBUFFER */, fb[1]);
      glFramebufferTexture2D(0x8CA9, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, snapTex_[i], 0);
      blitFramebuffer_(0, 0, sw, sh, 0, 0, dw, dh, GL_COLOR_BUFFER_BIT, i == 0 ? GL_NEAREST : GL_LINEAR);
      src = snapTex_[i];
      sw = dw;
      sh = dh;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(2, fb);
    out->full = snapTex_[0];
    out->fullW = snapW_[0];
    out->fullH = snapH_[0];
    out->blurred = src;
    out->blurW = sw;
    out->blurH = sh;
    return true;
  }

  // The texture JSUI draws the desktop into (w_ x h_, GL bottom-left origin).
  // *fresh: newly (re)allocated, so it holds no previous frame.
  unsigned desktopTexture(bool* fresh) {
    if (threaded_) {
      // The GPU must have read the last frame (the present thread's blit)
      // before this one is drawn into the texture: a GPU-side wait, the CPU
      // goes on.
      GlSync blit = nullptr;
      {
        std::unique_lock<std::mutex> lock(presentMu_);
        presentCv_.wait(lock, [&] { return blitSubmitted_; });
        std::swap(blit, blitFence_);
      }
      if (blit) {
        glWaitSync_(blit, 0, ~0ull /* GL_TIMEOUT_IGNORED */);
        glDeleteSync_(blit);
      }
    }
    // Textures of finished turns: the blits that drew them are submitted.
    if (oldTex_ && animDone_) {
      doomedTex_.push_back(oldTex_);
      oldTex_ = 0;
    }
    if (!doomedTex_.empty()) {
      glDeleteTextures(GLsizei(doomedTex_.size()), doomedTex_.data());
      doomedTex_.clear();
    }
    *fresh = texFresh_;
    if (texFresh_) {
      if (!tex_) glGenTextures(1, &tex_);
      glBindTexture(GL_TEXTURE_2D, tex_);
      glTexImage2D(GL_TEXTURE_2D, 0, es3_ ? 0x8058 /* GL_RGBA8 */ : GL_RGBA, w_, h_, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                   nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      texFresh_ = false;
    }
    return tex_;
  }
  unsigned textureFormat() const { return es3_ ? 0x8058u : 0u; }
  double lastBlitMs() const { return lastBlitMs_; }
  double lastSwapMs() const { return lastSwapMs_; }
  int desktopWidth() const { return w_; }
  int desktopHeight() const { return h_; }

  // Shows the desktop texture on the panel (rotated). The swap waits until
  // the GPU has run the frame (~24 ms of a full repaint on the Tab A7), so
  // by default a present thread does the blit and the swap: the main thread
  // paints the next frame meanwhile and only waits here when the previous
  // swap hasn't returned yet (JSUI_WL_GPU_THREAD=0: all on this thread).
  void presentGpu() {
    if (!gpu_ || !tex_) return;
    const Blit b = makeBlit();
    if (!threaded_) {
      const double t0 = nowMs();
      drawBlit(b);
      if (finish_) glFinish();
      const double t1 = nowMs();
      swap();
      lastSwapMs_ = nowMs() - t1;
      lastBlitMs_ = t1 - t0;
      return;
    }
    const double t0 = nowMs();
    GlSync rendered = glFenceSync_(0x9117 /* GL_SYNC_GPU_COMMANDS_COMPLETE */, 0);
    glFlush();
    {
      std::unique_lock<std::mutex> lock(presentMu_);
      presentCv_.wait(lock, [&] { return !presentBusy_; });
      if (renderedFence_) glDeleteSync_(renderedFence_);  // never picked up (not expected)
      renderedFence_ = rendered;
      blitNext_ = b;
      presentBusy_ = true;
      blitSubmitted_ = false;
    }
    presentCv_.notify_all();
    lastBlitMs_ = nowMs() - t0;  // the main thread's wait for the previous swap
    lastSwapMs_ = threadSwapMs_.load();
  }

 private:
  void swap() {
    EGLBoolean ok = eglSwapBuffers(dpy_, surf_);
    if (debugFrames_ > 0) {
      --debugFrames_;
      std::fprintf(stderr, "[jsui-wl] gpu present: swap=%d egl=0x%x gl=0x%x centre=%u,%u,%u,%u\n", int(ok),
                   eglGetError(), debugGlErr_, debugPx_[0], debugPx_[1], debugPx_[2], debugPx_[3]);
    }
  }

  void presentLoop() {
    eglMakeCurrent(dpy_, surf_, surf_, ctx_);
    for (;;) {
      GlSync rendered = nullptr;
      Blit b;
      {
        std::unique_lock<std::mutex> lock(presentMu_);
        presentCv_.wait(lock, [&] { return stopPresent_ || renderedFence_; });
        if (stopPresent_) break;
        std::swap(rendered, renderedFence_);
        b = blitNext_;
      }
      glWaitSync_(rendered, 0, ~0ull);
      glDeleteSync_(rendered);
      double tb = 0;
      if (finish_) {
        // JSUI_WL_GPU_FINISH: the frame's own GPU work, then the blit's.
        const double t0 = nowMs();
        glFinish();
        threadRenderGpuMs_ = nowMs() - t0;
        tb = nowMs();
      }
      drawBlit(b);
      if (finish_) {
        glFinish();
        threadBlitGpuMs_ = nowMs() - tb;
      }
      GlSync blit = glFenceSync_(0x9117, 0);
      glFlush();
      {
        std::lock_guard<std::mutex> lock(presentMu_);
        blitFence_ = blit;
        blitSubmitted_ = true;
      }
      presentCv_.notify_all();
      if (tickOnSubmit_) {
        // The main loop starts the next frame now, while this swap waits for
        // Mir's vsync, not once it returns: started after it, the frame's
        // CPU work (~6 ms), its GPU work (~8 ms) and this blit (~3 ms) came to
        // just over a vsync, and frames slipped to two (the launcher at 49
        // fps, now 54; the switcher 52, now 56). Its hand-over still waits
        // for this swap (presentGpu), so no frame queues behind Mir.
        // JSUI_WL_TICK_ON_SUBMIT=0: after the swap, as before.
        uint64_t one = 1;
        (void)!write(swapFd_, &one, sizeof one);
      }
      if (finish_) glFinish();
      const double t1 = nowMs();
      swap();
      threadSwapMs_ = nowMs() - t1;
      {
        std::lock_guard<std::mutex> lock(presentMu_);
        presentBusy_ = false;
      }
      presentCv_.notify_all();
      // The swap returned (Mir took the frame, at its vsync): the main loop
      // starts the next frame now (see swapFd()).
      uint64_t one = 1;
      (void)!write(swapFd_, &one, sizeof one);
    }
    eglMakeCurrent(dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  }

  void stopPresentThread() {
    if (!presentThread_.joinable()) return;
    {
      std::lock_guard<std::mutex> lock(presentMu_);
      stopPresent_ = true;
    }
    presentCv_.notify_all();
    presentThread_.join();
    stopPresent_ = false;
  }

  // Where the desktop picture is on the panel: turned `angle` degrees
  // clockwise about the panel's centre, w x h panel px before the turn.
  struct BlitPose {
    float angle = 0, w = 0, h = 0, oldAlpha = 0;
  };
  static constexpr double kRotateMs = 420;
  BlitPose poseAt(double now) const {
    BlitPose p{float(rot_.load()), float(w_.load()), float(h_.load()), 0};
    if (!oldTex_ || animDone_) return p;
    const float t = float(std::clamp((now - animStart_) / kRotateMs, 0.0, 1.0));
    const float e = t < 0.5f ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3.0f) / 2;  // ease in-out
    float d = p.angle - animFrom_;
    while (d > 180) d -= 360;
    while (d <= -180) d += 360;
    p.angle = animFrom_ + d * e;
    p.w = float(oldW_) + (p.w - float(oldW_)) * e;
    p.h = float(oldH_) + (p.h - float(oldH_)) * e;
    // The old frame holds while the new layout settles, then fades.
    p.oldAlpha = std::clamp(1 - (t - 0.2f) / 0.6f, 0.0f, 1.0f);
    return p;
  }
  // One blit's quads: the desktop texture and, during a turn, the frame
  // from before it on top.
  struct Blit {
    int pw = 0, ph = 0;
    int n = 0;
    bool smooth = false;  // turning: linear filtering, black around
    GLuint tex[2] = {};
    float m[2][4] = {};
    float alpha[2] = {1, 1};
  };
  Blit makeBlit() {
    Blit b;
    b.pw = w_;
    b.ph = h_;
    if (rot_ == 90 || rot_ == 270) std::swap(b.pw, b.ph);
    const double now = nowMs();
    BlitPose p = poseAt(now);
    float sc = 1;
    if (oldTex_ && !animDone_) {
      if (now - animStart_ >= kRotateMs) animDone_ = true;  // this blit is the last of the turn (see desktopTexture)
      b.smooth = !animDone_;
    }
    const float a = p.angle * float(M_PI) / 180, c = std::cos(a), sn = std::sin(a);
    if (b.smooth) {
      // Fit the turning picture inside the panel.
      const float bw = std::fabs(p.w * c) + std::fabs(p.h * sn), bh = std::fabs(p.w * sn) + std::fabs(p.h * c);
      sc = std::min(float(b.pw) / bw, float(b.ph) / bh);
    }
    auto quad = [&](GLuint tex, float alpha) {
      int i = b.n++;
      b.tex[i] = tex;
      b.alpha[i] = alpha;
      // Desktop-local corner (x, y) in -0.5..0.5 (y down) -> panel px from the centre (y down).
      b.m[i][0] = c * p.w * sc;
      b.m[i][1] = -sn * p.h * sc;
      b.m[i][2] = sn * p.w * sc;
      b.m[i][3] = c * p.h * sc;
    };
    quad(tex_, 1);
    if (b.smooth && p.oldAlpha > 0) quad(oldTex_, p.oldAlpha);
    return b;
  }

  // The desktop texture as a rotated full-panel quad into the window's
  // back buffer (the window's context current).
  void drawBlit(const Blit& b) {
    // Our own vertex array (ES3): Skia's leftover attribute state (enables,
    // pointers, instancing divisors) is per-VAO, so it can't leak into ours.
    if (bindVertexArray_) bindVertexArray_(vao_);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (es3_) {
      glDisable(0x8C89 /* GL_RASTERIZER_DISCARD */);
      glDisable(0x8D69 /* GL_PRIMITIVE_RESTART_FIXED_INDEX */);
    }
    glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    glDisable(GL_SAMPLE_COVERAGE);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDisable(GL_DITHER);
    const int pw = b.pw, ph = b.ph;
    glViewport(0, 0, pw, ph);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(prog_);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(samplerLoc_, 0);
    glUniform1f(solidLoc_, debugSolid_ ? 1.0f : 0.0f);
    glUniform2f(halfLoc_, float(pw) / 2, float(ph) / 2);
    if (!vao_) {
      glBindBuffer(GL_ARRAY_BUFFER, vbo_);
      glEnableVertexAttribArray(0);
      glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, nullptr);
      if (vertexAttribDivisor_) vertexAttribDivisor_(0, 0);
    }
    if (debugRed_ || b.smooth) {
      glClearColor(debugRed_ ? 1 : 0, 0, 0, 1);
      glClear(GL_COLOR_BUFFER_BIT);
    }
    for (int i = 0; i < b.n && !debugRed_; ++i) {
      glBindTexture(GL_TEXTURE_2D, b.tex[i]);
      // Skia sets its own sampling state on the texture it renders into (mip
      // levels, filters); an incomplete texture samples as opaque black.
      const GLint filter = b.smooth ? GL_LINEAR : GL_NEAREST;
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      if (es3_) {
        glTexParameteri(GL_TEXTURE_2D, 0x813C /* GL_TEXTURE_BASE_LEVEL */, 0);
        glTexParameteri(GL_TEXTURE_2D, 0x813D /* GL_TEXTURE_MAX_LEVEL */, 0);
      }
      glUniform4fv(mLoc_, 1, b.m[i]);
      glUniform1f(alphaLoc_, b.alpha[i]);
      if (b.alpha[i] < 1) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      }
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
      glDisable(GL_BLEND);
    }
    if (!vao_) glDisableVertexAttribArray(0);
    if (bindVertexArray_) bindVertexArray_(0);
    if (debugFrames_ > 0) glReadPixels(pw / 2, ph / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, debugPx_);
    debugGlErr_ = glGetError();
  }

 public:

  // The desktop as RGBA rows, top row first (screenshots).
  // The desktop as it is now, scaled down to dw x dh (GPU blit, then a
  // small read; for `record`, a few ms a frame), top row first.
  bool readDesktopScaled(std::vector<uint8_t>* out, int dw, int dh) {
    if (!gpu_ || !tex_ || !makeCurrent()) return false;
    using Blit = void (*)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
    static Blit blit = reinterpret_cast<Blit>(eglGetProcAddress("glBlitFramebuffer"));
    if (!blit) return false;
    if (!recFb_[0]) {
      glGenFramebuffers(2, recFb_);
      glGenRenderbuffers(1, &recRb_);
    }
    if (recW_ != dw || recH_ != dh) {
      glBindRenderbuffer(GL_RENDERBUFFER, recRb_);
      glRenderbufferStorage(GL_RENDERBUFFER, 0x8058 /* GL_RGBA8 */, dw, dh);
      recW_ = dw;
      recH_ = dh;
    }
    glBindFramebuffer(0x8CA8 /* GL_READ_FRAMEBUFFER */, recFb_[0]);
    glFramebufferTexture2D(0x8CA8, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_, 0);
    glBindFramebuffer(0x8CA9 /* GL_DRAW_FRAMEBUFFER */, recFb_[1]);
    glFramebufferRenderbuffer(0x8CA9, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, recRb_);
    blit(0, 0, w_, h_, 0, dh, dw, 0, GL_COLOR_BUFFER_BIT, GL_LINEAR);  // flipped: top row first
    glBindFramebuffer(0x8CA8, recFb_[1]);
    out->resize(size_t(dw) * size_t(dh) * 4);
    glReadPixels(0, 0, dw, dh, GL_RGBA, GL_UNSIGNED_BYTE, out->data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
  }

  bool readDesktop(std::vector<uint8_t>* out) {
#ifdef JSUI_WL_MIR_VK
    if (vk_) return vk_->readDesktop(out);
#endif
    if (!gpu_ || !tex_ || !makeCurrent()) return false;
    GLuint fb = 0;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_, 0);
    std::vector<uint8_t> up(size_t(w_) * size_t(h_) * 4);
    glReadPixels(0, 0, w_, h_, GL_RGBA, GL_UNSIGNED_BYTE, up.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fb);
    out->resize(up.size());
    const size_t row = size_t(w_) * 4;
    for (int y = 0; y < h_; ++y) std::memcpy(out->data() + size_t(y) * row, up.data() + size_t(h_ - 1 - y) * row, row);
    return true;
  }
#endif

 private:
  GLuint recFb_[2] = {0, 0}, recRb_ = 0;
  int recW_ = 0, recH_ = 0;
  static double nowMs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return double(ts.tv_sec) * 1000.0 + double(ts.tv_nsec) / 1e6;
  }

#ifdef JSUI_WL_MIR_VK
  bool createVkWindow(std::string* why) {
    std::string err;
    if (!makeWindow(mir_pixel_format_abgr_8888, mir_buffer_usage_hardware, &err)) {
      *why = "Mir hardware window: " + err;
      return false;
    }
    int pw = w_, ph = h_;
    if (rot_ == 90 || rot_ == 270) std::swap(pw, ph);
    vk_ = std::make_unique<MirVulkan>();
    if (vk_->create(reinterpret_cast<void*>(mir_buffer_stream_get_egl_native_window(stream_)), pw, ph, w_, h_, rot_,
                    why))
      return true;
    vk_.reset();
    mir_window_release_sync(window_);
    window_ = nullptr;
    stream_ = nullptr;
    return false;
  }
#endif

  bool makeWindow(MirPixelFormat format, MirBufferUsage usage, std::string* err) {
    int pw = w_, ph = h_;
    if (rot_ == 90 || rot_ == 270) std::swap(pw, ph);
    MirWindowSpec* spec = mir_create_normal_window_spec(conn_, pw, ph);
    mir_window_spec_set_pixel_format(spec, format);
    mir_window_spec_set_buffer_usage(spec, usage);
    mir_window_spec_set_name(spec, "jsui-wl");
    mir_window_spec_set_fullscreen_on_output(spec, outputId_);
    mir_window_spec_set_state(spec, mir_window_state_fullscreen);
    mir_window_spec_set_event_handler(spec, &MirHost::onEvent, this);
    window_ = mir_create_window_sync(spec);
    mir_window_spec_release(spec);
    if (!mir_window_is_valid(window_)) {
      *err = mir_window_get_error_message(window_);
      mir_window_release_sync(window_);
      window_ = nullptr;
      return false;
    }
    stream_ = mir_window_get_buffer_stream(window_);
    return true;
  }

#ifdef JSUI_WL_MIR_GL
  bool createGlWindow(std::string* why) {
    // libhybris: pass Mir's ANativeWindow through untouched.
    setenv("EGL_PLATFORM", "null", 0);
    dpy_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy_ == EGL_NO_DISPLAY || !eglInitialize(dpy_, nullptr, nullptr)) {
      *why = "eglInitialize failed";
      dpy_ = EGL_NO_DISPLAY;
      return false;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                            EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(dpy_, attrs, &cfg, 1, &n) || n < 1) {
      *why = "no RGBA8888 EGL config";
      destroyGl();
      return false;
    }
    MirPixelFormat fmt = mir_connection_get_egl_pixel_format(conn_, dpy_, cfg);
    std::string err;
    if (fmt == mir_pixel_format_invalid || !makeWindow(fmt, mir_buffer_usage_hardware, &err)) {
      *why = "Mir hardware window: " + (err.empty() ? std::string("no pixel format for the EGL config") : err);
      destroyGl();
      return false;
    }
    auto native = reinterpret_cast<EGLNativeWindowType>(mir_buffer_stream_get_egl_native_window(stream_));
#ifdef JSUI_WL_ANDROID_WINDOW
    // JSUI_WL_MIR_BUFFERS=N: ask Mir's ANativeWindow for N buffers (#254:
    // with two, a swap Mir holds for a second vsync stalls the next frame).
    auto* anw = reinterpret_cast<ANativeWindow*>(native);
    const char* nb = getenv("JSUI_WL_MIR_BUFFERS");
    if (anw && nb) {
      int minUndequeued = -1;
      anw->query(anw, NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS, &minUndequeued);
      const int r = native_window_set_buffer_count(anw, std::atoi(nb));
      std::fprintf(stderr, "[jsui-wl] Mir window: %s buffers asked, perform() = %d (min undequeued %d)\n", nb, r,
                   minUndequeued);
    }
#endif
    surf_ = eglCreateWindowSurface(dpy_, cfg, native, nullptr);
    if (surf_ == EGL_NO_SURFACE) {
      *why = "eglCreateWindowSurface failed (0x" + hex(eglGetError()) + ")";
      destroyGl();
      return false;
    }
    for (int version : {3, 2}) {
      const EGLint cattrs[] = {EGL_CONTEXT_CLIENT_VERSION, version, EGL_NONE};
      ctx_ = eglCreateContext(dpy_, cfg, EGL_NO_CONTEXT, cattrs);
      if (ctx_ != EGL_NO_CONTEXT) {
        es3_ = version == 3;
        break;
      }
    }
    if (ctx_ == EGL_NO_CONTEXT || !eglMakeCurrent(dpy_, surf_, surf_, ctx_)) {
      *why = "no GLES context (0x" + hex(eglGetError()) + ")";
      destroyGl();
      return false;
    }
    const char* si = getenv("JSUI_WL_GPU_SWAPINTERVAL");
    eglSwapInterval(dpy_, si ? std::atoi(si) : 0);  // 0: the Wayland loop paces frames
    if (const char* d = getenv("JSUI_WL_GPU_DEBUG")) {
      debugFrames_ = std::atoi(d) > 0 ? 8 : 0;
      debugSolid_ = std::atoi(d) == 2;
    }
    if (const char* r = getenv("JSUI_WL_GPU_RED")) debugRed_ = r[0] == '1';
    if (const char* f = getenv("JSUI_WL_GPU_FINISH")) finish_ = f[0] == '1';
    if (const char* t = getenv("JSUI_WL_TICK_ON_SUBMIT")) tickOnSubmit_ = t[0] != '0';
    if (!buildBlit(why)) {
      destroyGl();
      return false;
    }
    bindVertexArray_ = reinterpret_cast<void (*)(GLuint)>(eglGetProcAddress("glBindVertexArray"));
    if (!bindVertexArray_) bindVertexArray_ = reinterpret_cast<void (*)(GLuint)>(eglGetProcAddress("glBindVertexArrayOES"));
    gpu_ = true;
    texFresh_ = true;
    glError_.clear();
    const char* th = getenv("JSUI_WL_GPU_THREAD");
    if (!(th && th[0] == '0')) startPresentThread(cfg);
    return true;
  }

  // A second context in the window's share group for the main thread
  // (surfaceless, or on a 1x1 pbuffer) and the present thread on the
  // window's. GLES 3 fences order the two on the GPU. Without any of it,
  // presenting stays on the main thread.
  void startPresentThread(EGLConfig cfg) {
    if (!es3_) return;
    glFenceSync_ = reinterpret_cast<GlSync (*)(GLenum, GLbitfield)>(eglGetProcAddress("glFenceSync"));
    glWaitSync_ = reinterpret_cast<void (*)(GlSync, GLbitfield, uint64_t)>(eglGetProcAddress("glWaitSync"));
    glDeleteSync_ = reinterpret_cast<void (*)(GlSync)>(eglGetProcAddress("glDeleteSync"));
    if (!glFenceSync_ || !glWaitSync_ || !glDeleteSync_) return;
    const EGLint cattrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    renderCtx_ = eglCreateContext(dpy_, cfg, ctx_, cattrs);
    if (renderCtx_ == EGL_NO_CONTEXT) return;
    renderSurf_ = EGL_NO_SURFACE;
    if (!eglMakeCurrent(dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, renderCtx_)) {
      const EGLint pattrs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
      renderSurf_ = eglCreatePbufferSurface(dpy_, cfg, pattrs);
      if (renderSurf_ == EGL_NO_SURFACE || !eglMakeCurrent(dpy_, renderSurf_, renderSurf_, renderCtx_)) {
        if (renderSurf_ != EGL_NO_SURFACE) eglDestroySurface(dpy_, renderSurf_);
        renderSurf_ = EGL_NO_SURFACE;
        eglMakeCurrent(dpy_, surf_, surf_, ctx_);
        eglDestroyContext(dpy_, renderCtx_);
        renderCtx_ = EGL_NO_CONTEXT;
        return;
      }
    }
    // The window's context is the present thread's from now on.
    threaded_ = true;
    if (swapFd_ < 0) swapFd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    blitSubmitted_ = true;
    presentBusy_ = false;
    presentThread_ = std::thread([this] { presentLoop(); });
  }

  static std::string hex(unsigned v) {
    char b[16];
    std::snprintf(b, sizeof b, "%x", v);
    return b;
  }

  // The rotated full-screen quad: panel corner -> desktop texture coordinate.
  bool buildBlit(std::string* why) {
    // a: a corner of the desktop, -0.5..0.5 (y down); m: its turn and size
    // in panel px (Blit::m); halfSize: half the panel.
    static const char* vs =
        "attribute vec2 a;\n"
        "uniform vec4 m;\n"
        "uniform vec2 halfSize;\n"
        "varying vec2 t;\n"
        "void main() {\n"
        "  vec2 p = vec2(m.x * a.x + m.y * a.y, m.z * a.x + m.w * a.y);\n"
        "  t = vec2(a.x + 0.5, 0.5 - a.y);\n"
        "  gl_Position = vec4(p.x / halfSize.x, -p.y / halfSize.y, 0.0, 1.0);\n"
        "}\n";
    static const char* fs =
        "precision mediump float;\n"
        "varying vec2 t;\n"
        "uniform sampler2D s;\n"
        "uniform float solid;\n"
        "uniform float alpha;\n"
        "void main() { gl_FragColor = vec4(mix(texture2D(s, t).rgb, vec3(0.0, 1.0, 0.0), solid), alpha); }\n";
    auto compile = [](GLenum type, const char* src) {
      GLuint sh = glCreateShader(type);
      glShaderSource(sh, 1, &src, nullptr);
      glCompileShader(sh);
      GLint ok = 0;
      glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
      return ok ? sh : 0u;
    };
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) {
      *why = "blit shader did not compile";
      return false;
    }
    prog_ = glCreateProgram();
    glAttachShader(prog_, v);
    glAttachShader(prog_, f);
    glBindAttribLocation(prog_, 0, "a");
    glLinkProgram(prog_);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(prog_, GL_LINK_STATUS, &ok);
    if (!ok) {
      *why = "blit program did not link";
      return false;
    }
    samplerLoc_ = glGetUniformLocation(prog_, "s");
    solidLoc_ = glGetUniformLocation(prog_, "solid");
    mLoc_ = glGetUniformLocation(prog_, "m");
    halfLoc_ = glGetUniformLocation(prog_, "halfSize");
    alphaLoc_ = glGetUniformLocation(prog_, "alpha");
    // Strip corners (bottom-left, bottom-right, top-left, top-right), the
    // desktop's own coordinates; the turn is the vertex shader's (Blit).
    const float q[8] = {-0.5f, 0.5f, 0.5f, 0.5f, -0.5f, -0.5f, 0.5f, -0.5f};
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STATIC_DRAW);
    vertexAttribDivisor_ = reinterpret_cast<void (*)(GLuint, GLuint)>(eglGetProcAddress("glVertexAttribDivisor"));
    auto genVertexArrays = reinterpret_cast<void (*)(GLsizei, GLuint*)>(eglGetProcAddress("glGenVertexArrays"));
    auto bindVa = reinterpret_cast<void (*)(GLuint)>(eglGetProcAddress("glBindVertexArray"));
    if (es3_ && genVertexArrays && bindVa) {
      genVertexArrays(1, &vao_);
      bindVa(vao_);
      glBindBuffer(GL_ARRAY_BUFFER, vbo_);
      glEnableVertexAttribArray(0);
      glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, nullptr);
      if (vertexAttribDivisor_) vertexAttribDivisor_(0, 0);
      bindVa(0);
    }
    return true;
  }

  void destroyGl() {
    if (dpy_ == EGL_NO_DISPLAY) return;
    stopPresentThread();
    if (threaded_) {
      if (renderedFence_) glDeleteSync_(renderedFence_);
      if (blitFence_) glDeleteSync_(blitFence_);
      renderedFence_ = blitFence_ = nullptr;
      eglMakeCurrent(dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      eglDestroyContext(dpy_, renderCtx_);
      if (renderSurf_ != EGL_NO_SURFACE) eglDestroySurface(dpy_, renderSurf_);
      renderCtx_ = EGL_NO_CONTEXT;
      renderSurf_ = EGL_NO_SURFACE;
      threaded_ = false;
    }
    if (ctx_ != EGL_NO_CONTEXT && surf_ != EGL_NO_SURFACE && eglMakeCurrent(dpy_, surf_, surf_, ctx_)) {
      if (tex_) glDeleteTextures(1, &tex_);
      for (GLuint& st : snapTex_)
        if (st) glDeleteTextures(1, &st), st = 0;
      if (vbo_) glDeleteBuffers(1, &vbo_);
      if (prog_) glDeleteProgram(prog_);
    }
    tex_ = vbo_ = prog_ = 0;
    eglMakeCurrent(dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (ctx_ != EGL_NO_CONTEXT) eglDestroyContext(dpy_, ctx_);
    if (surf_ != EGL_NO_SURFACE) eglDestroySurface(dpy_, surf_);
    eglTerminate(dpy_);
    ctx_ = EGL_NO_CONTEXT;
    surf_ = EGL_NO_SURFACE;
    dpy_ = EGL_NO_DISPLAY;
    gpu_ = false;
  }
#endif

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
  bool gpu_ = false;
#ifdef JSUI_WL_MIR_VK
  std::unique_ptr<MirVulkan> vk_;
  std::string vkError_;
#endif
  std::string glError_;
  uint32_t outputId_ = 0;
#ifdef JSUI_WL_MIR_GL
  EGLDisplay dpy_ = EGL_NO_DISPLAY;
  EGLSurface surf_ = EGL_NO_SURFACE;
  EGLContext ctx_ = EGL_NO_CONTEXT;
  bool es3_ = false;
  bool texFresh_ = true;
  GLuint tex_ = 0, vbo_ = 0, prog_ = 0;
  GLint samplerLoc_ = -1;
  void (*bindVertexArray_)(GLuint) = nullptr;
  GLuint vao_ = 0;
  GLint solidLoc_ = -1, mLoc_ = -1, halfLoc_ = -1, alphaLoc_ = -1;
  // A turn (rotateTo): the frame from before it, its size, where it started
  // and when; textures to free once their last blit is submitted.
  GLuint oldTex_ = 0;
  int oldW_ = 0, oldH_ = 0;
  float animFrom_ = 0;
  double animStart_ = 0;
  bool animDone_ = true;
  std::vector<GLuint> doomedTex_;
  Blit blitNext_;  // the present thread's next blit (presentMu_)
  void (*vertexAttribDivisor_)(GLuint, GLuint) = nullptr;
  bool debugSolid_ = false;  // JSUI_WL_GPU_DEBUG=2: solid green quad (geometry test)
  int debugFrames_ = 0;  // JSUI_WL_GPU_DEBUG: log the first presents
  double lastBlitMs_ = 0, lastSwapMs_ = 0;
  // JSUI_WL_GPU_FINISH: glFinish before the swap, so the blit time holds the
  // frame's GPU work and the swap time only the wait for a free buffer.
  bool finish_ = false;
  static constexpr int kSnapLevels = 5;  // snapshotDesktop: the copy, then 1/2 .. 1/16
  GLuint snapTex_[kSnapLevels] = {};
  int snapW_[kSnapLevels] = {}, snapH_[kSnapLevels] = {};
  void (*blitFramebuffer_)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum) = nullptr;
  // The present thread (presentGpu).
  bool threaded_ = false;
  int swapFd_ = -1;  // present thread -> main loop: a swap returned
  EGLContext renderCtx_ = EGL_NO_CONTEXT;  // the main thread's, sharing ctx_'s objects
  EGLSurface renderSurf_ = EGL_NO_SURFACE;  // its pbuffer when surfaceless isn't supported
  std::thread presentThread_;
  std::mutex presentMu_;
  std::condition_variable presentCv_;
  GlSync renderedFence_ = nullptr;  // a frame for the present thread
  GlSync blitFence_ = nullptr;      // its blit, for the next frame's render to wait on
  bool blitSubmitted_ = true, presentBusy_ = false, stopPresent_ = false;
  bool tickOnSubmit_ = true;  // JSUI_WL_TICK_ON_SUBMIT (presentLoop)
  std::atomic<double> threadSwapMs_{0};
  std::atomic<double> threadRenderGpuMs_{0}, threadBlitGpuMs_{0};  // JSUI_WL_GPU_FINISH
  GlSync (*glFenceSync_)(GLenum, GLbitfield) = nullptr;
  void (*glWaitSync_)(GlSync, GLbitfield, uint64_t) = nullptr;
  void (*glDeleteSync_)(GlSync) = nullptr;
  unsigned char debugPx_[4] = {0, 0, 0, 0};
  GLenum debugGlErr_ = 0;
  bool debugRed_ = false;  // JSUI_WL_GPU_RED: clear the panel red instead of drawing (swap path test)
#endif
  bool pointerDown_ = false;
  // Desktop size and rotation; atomic for Mir's event thread (toDesktop).
  std::atomic<int> w_{0}, h_{0};
  std::atomic<int> rot_{0};
  int fd_ = -1;
  std::mutex mu_;
  std::vector<MirHostEvent> queue_;
};
