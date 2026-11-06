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
#include <GLES2/gl2.h>
#endif
#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
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
#ifdef JSUI_WL_MIR_GL
    destroyGl();
#endif
    if (window_) mir_window_release_sync(window_);
    window_ = nullptr;
    stream_ = nullptr;
    return createSoftwareWindow(err);
  }

  bool gpu() const { return gpu_; }

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
  bool makeCurrent() { return gpu_ && eglMakeCurrent(dpy_, surf_, surf_, ctx_); }
  EGLDisplay eglDisplay() const { return dpy_; }

  // The texture JSUI draws the desktop into (w_ x h_, GL bottom-left origin).
  // *fresh: newly (re)allocated, so it holds no previous frame.
  unsigned desktopTexture(bool* fresh) {
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

  // Draws the desktop texture onto the panel (rotated) and shows it.
  void presentGpu() {
    if (!gpu_ || !tex_) return;
    const double blitStart = nowMs();
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
    int pw = w_, ph = h_;
    if (rot_ == 90 || rot_ == 270) std::swap(pw, ph);
    glViewport(0, 0, pw, ph);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(prog_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_);
    // Skia sets its own sampling state on the texture it renders into (mip
    // levels, filters); an incomplete texture samples as opaque black.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (es3_) {
      glTexParameteri(GL_TEXTURE_2D, 0x813C /* GL_TEXTURE_BASE_LEVEL */, 0);
      glTexParameteri(GL_TEXTURE_2D, 0x813D /* GL_TEXTURE_MAX_LEVEL */, 0);
    }
    glUniform1i(samplerLoc_, 0);
    glUniform1f(solidLoc_, debugSolid_ ? 1.0f : 0.0f);
    if (!vao_) {
      glBindBuffer(GL_ARRAY_BUFFER, vbo_);
      glEnableVertexAttribArray(0);
      glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 16, nullptr);
      if (vertexAttribDivisor_) vertexAttribDivisor_(0, 0);
    }
    if (debugRed_) {
      glClearColor(1, 0, 0, 1);
      glClear(GL_COLOR_BUFFER_BIT);
    } else {
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    if (!vao_) glDisableVertexAttribArray(0);
    if (bindVertexArray_) bindVertexArray_(0);
    unsigned char px[4] = {0, 0, 0, 0};
    if (debugFrames_ > 0) glReadPixels(pw / 2, ph / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    GLenum glErr = glGetError();
    const double t1 = nowMs();
    EGLBoolean ok = eglSwapBuffers(dpy_, surf_);
    lastSwapMs_ = nowMs() - t1;
    lastBlitMs_ = t1 - blitStart;
    if (debugFrames_ > 0) {
      --debugFrames_;
      std::fprintf(stderr, "[jsui-wl] gpu present: swap=%d egl=0x%x gl=0x%x centre=%u,%u,%u,%u panel %dx%d\n", int(ok),
                   eglGetError(), glErr, px[0], px[1], px[2], px[3], pw, ph);
    }
  }

  // The desktop as RGBA rows, top row first (screenshots).
  bool readDesktop(std::vector<uint8_t>* out) {
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
  static double nowMs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return double(ts.tv_sec) * 1000.0 + double(ts.tv_nsec) / 1e6;
  }

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
    if (!buildBlit(why)) {
      destroyGl();
      return false;
    }
    bindVertexArray_ = reinterpret_cast<void (*)(GLuint)>(eglGetProcAddress("glBindVertexArray"));
    if (!bindVertexArray_) bindVertexArray_ = reinterpret_cast<void (*)(GLuint)>(eglGetProcAddress("glBindVertexArrayOES"));
    gpu_ = true;
    texFresh_ = true;
    glError_.clear();
    return true;
  }

  static std::string hex(unsigned v) {
    char b[16];
    std::snprintf(b, sizeof b, "%x", v);
    return b;
  }

  // The rotated full-screen quad: panel corner -> desktop texture coordinate.
  bool buildBlit(std::string* why) {
    static const char* vs =
        "attribute vec4 a;\n"
        "varying vec2 t;\n"
        "void main() { t = a.zw; gl_Position = vec4(a.xy, 0.0, 1.0); }\n";
    static const char* fs =
        "precision mediump float;\n"
        "varying vec2 t;\n"
        "uniform sampler2D s;\n"
        "uniform float solid;\n"
        "void main() { gl_FragColor = mix(vec4(texture2D(s, t).rgb, 1.0), vec4(0.0, 1.0, 0.0, 1.0), solid); }\n";
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
    // Strip corners in NDC; (u, vt) is the panel position (top-left origin),
    // (X, Yt) the desktop position it shows (top-left origin) for the
    // clockwise rotation, and the texture is sampled bottom-left origin.
    float q[16];
    const float ndc[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    for (int i = 0; i < 4; ++i) {
      float u = (ndc[i][0] + 1) / 2, vt = 1 - (ndc[i][1] + 1) / 2, X, Yt;
      if (rot_ == 90) X = vt, Yt = 1 - u;
      else if (rot_ == 270) X = 1 - vt, Yt = u;
      else if (rot_ == 180) X = 1 - u, Yt = 1 - vt;
      else X = u, Yt = vt;
      q[i * 4 + 0] = ndc[i][0];
      q[i * 4 + 1] = ndc[i][1];
      q[i * 4 + 2] = X;
      q[i * 4 + 3] = 1 - Yt;
    }
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
      glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 16, nullptr);
      if (vertexAttribDivisor_) vertexAttribDivisor_(0, 0);
      bindVa(0);
    }
    return true;
  }

  void destroyGl() {
    if (dpy_ == EGL_NO_DISPLAY) return;
    if (ctx_ != EGL_NO_CONTEXT && surf_ != EGL_NO_SURFACE && eglMakeCurrent(dpy_, surf_, surf_, ctx_)) {
      if (tex_) glDeleteTextures(1, &tex_);
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
  GLint solidLoc_ = -1;
  void (*vertexAttribDivisor_)(GLuint, GLuint) = nullptr;
  bool debugSolid_ = false;  // JSUI_WL_GPU_DEBUG=2: solid green quad (geometry test)
  int debugFrames_ = 0;  // JSUI_WL_GPU_DEBUG: log the first presents
  double lastBlitMs_ = 0, lastSwapMs_ = 0;
  bool debugRed_ = false;  // JSUI_WL_GPU_RED: clear the panel red instead of drawing (swap path test)
#endif
  bool pointerDown_ = false;
  int w_ = 0, h_ = 0;  // desktop size
  int rot_ = 0;
  int fd_ = -1;
  std::mutex mu_;
  std::vector<MirHostEvent> queue_;
};
