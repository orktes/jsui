// JSUI in sokol: a 3D scene drawn with sokol_gfx (through sokol_gl) on the
// GL backend, and the shared HUD (integrations/shared/ui/app.jsx) on top.
//
//   jsui-sokol [--dev] [bundle.js | http://127.0.0.1:5173/bundle.js]
//
// Rendering. After the scene's sokol pass, Skia (Ganesh) draws the UI over
// it straight into sokol_app's framebuffer (the swapchain's gl.framebuffer) with
// JSUI_GL_NO_CLEAR, on sokol's GL context; sg_reset_state_cache() then tells
// sokol_gfx that GL state changed behind its back. The runtime keeps its last
// frame in a texture of its own and repaints only what changed, so a frame
// where the UI is idle costs one textured draw. --cpu draws the UI from a CPU
// raster instead: jsui_render_rgba_incremental keeps a buffer up to date, it
// is written into a write-transient sg_image each frame (sokol_gfx's
// transient images do not keep their contents between frames) and
// composited with a premultiplied sokol_gl quad.
//
// Input pass-through. sokol_app events go to the UI first; jsui_send_*
// returns whether the UI consumed one. The rest drives the game: click a
// unit to select it (a ray from the camera against each spinning cube,
// jsui_example::rayBox) or empty ground to clear the selection, drag empty
// space to orbit, wheel to zoom, Space spawns a unit, Esc pauses.
// sokol_app has no gamepad API, so this host forwards none.
//
// Per-frame data. Each frame the game projects every unit to the screen and
// writes x, y, visible, hue into the `entities` frame buffer, which moves the
// nameplates without JS; fps, unit count, the selected unit and the
// pass-through counters go to the `game` shared state.
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#include "util/sokol_gl.h"

#if defined(__APPLE__)
#include <OpenGL/gl3.h>
#elif defined(_WIN32)
#include <windows.h>
#include <GL/gl.h>
#else
#define GL_GLEXT_PROTOTYPES  // glBindFramebuffer
#include <GL/gl.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "example_host.h"
#include "glfw_keys.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkStream.h"
#include "include/encode/SkPngEncoder.h"
#include "jsui/jsui.h"

namespace {

using jsui_example::Options;

// ---------------------------------------------------- a little matrix math
// Column-major 4x4, as sokol_gl's sgl_load_matrix takes them.
struct Mat4 {
  float m[16];
};
struct Vec3 {
  float x, y, z;
};
Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 normalize(Vec3 a) {
  float l = std::sqrt(dot(a, a));
  return {a.x / l, a.y / l, a.z / l};
}
Mat4 mul(const Mat4& a, const Mat4& b) {
  Mat4 r{};
  for (int c = 0; c < 4; c++)
    for (int row = 0; row < 4; row++)
      for (int k = 0; k < 4; k++) r.m[c * 4 + row] += a.m[k * 4 + row] * b.m[c * 4 + k];
  return r;
}
Mat4 perspective(float fovyRad, float aspect, float zn, float zf) {
  float f = 1.f / std::tan(fovyRad / 2);
  Mat4 r{};
  r.m[0] = f / aspect;
  r.m[5] = f;
  r.m[10] = (zf + zn) / (zn - zf);
  r.m[11] = -1;
  r.m[14] = 2 * zf * zn / (zn - zf);
  return r;
}
Mat4 lookAt(Vec3 eye, Vec3 center, Vec3 up) {
  Vec3 f = normalize(sub(center, eye)), s = normalize(cross(f, up)), u = cross(s, f);
  Mat4 r{};
  r.m[0] = s.x, r.m[4] = s.y, r.m[8] = s.z;
  r.m[1] = u.x, r.m[5] = u.y, r.m[9] = u.z;
  r.m[2] = -f.x, r.m[6] = -f.y, r.m[10] = -f.z;
  r.m[12] = -dot(s, eye), r.m[13] = -dot(u, eye), r.m[14] = dot(f, eye);
  r.m[15] = 1;
  return r;
}

struct Unit {
  float angle, radius, speed, height, size, spin;
  uint8_t r, g, b;
};

const uint8_t kHues[7][3] = {{242, 195, 91}, {79, 209, 255}, {178, 107, 255}, {74, 222, 128},
                             {255, 154, 60}, {79, 140, 255}, {255, 77, 106}};

class Game : public jsui_example::SelfTestHost {
 public:
  Options opt;

  void init() {
    sg_desc gd{};
    gd.environment = sglue_environment();
    gd.logger.func = slog_func;
    sg_setup(&gd);
    sgl_desc_t sd{};
    sd.logger.func = slog_func;
    sgl_setup(&sd);
    sg_pipeline_desc pd{};
    pd.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
    pd.depth.write_enabled = true;
    pd.cull_mode = SG_CULLMODE_NONE;
    scenePip_ = sgl_make_pipeline(&pd);
    // Premultiplied alpha, for the --cpu UI quad.
    sg_pipeline_desc ud{};
    ud.colors[0].blend.enabled = true;
    ud.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_ONE;
    ud.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    ud.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
    ud.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    uiPip_ = sgl_make_pipeline(&ud);
    sg_sampler_desc smp{};
    smp.min_filter = SG_FILTER_LINEAR;
    smp.mag_filter = SG_FILTER_LINEAR;
    smp.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
    smp.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    sampler_ = sg_make_sampler(&smp);

    if (!opt.cpu) {
      gpu_ = jsui_gpu_create_gl(nullptr, nullptr);  // the platform's GL loader, sokol_app's context is current
      if (!gpu_) {
        std::fprintf(stderr, "warning: GPU renderer unavailable, falling back to the CPU renderer\n");
        opt.cpu = true;
      } else {
        const GLubyte* r = glGetString(GL_RENDERER);
        const GLubyte* v = glGetString(GL_VERSION);
        std::printf("GPU: %s / %s\n", r ? (const char*)r : "?", v ? (const char*)v : "?");
      }
    }
    hud_.engine = "sokol";
    hud_.renderer = opt.cpu ? "sokol_gfx GL · CPU raster into a dynamic sg_image"
                            : "sokol_gfx GL · Skia Ganesh into the swapchain framebuffer";

    JsuiConfig cfg;
    jsui_config_init(&cfg);
    cfg.width = int(sapp_widthf() / sapp_dpi_scale());
    cfg.height = int(sapp_heightf() / sapp_dpi_scale());
    cfg.pixel_ratio = sapp_dpi_scale();
    cfg.dev_mode = opt.dev;
    cfg.js_thread = opt.jsThread;
    cfg.message_user = this;
    cfg.on_message = [](void* u, const char* name, const char* json) { static_cast<Game*>(u)->onMessage(name, json); };
    rt_ = jsui_create(&cfg);
    std::printf("JS thread: %s\n", jsui_example::jsThreadName(jsui_js_thread(rt_)));
    jsui_set_clipboard_callbacks(
        rt_, [](void*) -> const char* { return sapp_get_clipboard_string(); },
        [](void*, const char* text) { sapp_set_clipboard_string(text); }, nullptr);
    for (int i = 0; i < 9; i++) spawn();
    resize();
    hud_.publish(rt_);
    if (jsui_load_bundle_url(rt_, opt.bundle.c_str()) != 0)
      std::fprintf(stderr, "failed to load %s: %s\n", opt.bundle.c_str(), jsui_last_error(rt_));
  }

  void frame() {
    double now = double(frameCount_) * 1000.0 / 60.0;
    bool deterministic = opt.frames >= 0 || opt.selftest;
    float dt = deterministic ? 1.f / 60.f : float(sapp_frame_duration());
    if (!deterministic) {
      clock_ += sapp_frame_duration() * 1000.0;
      now = clock_;
    }
    if (pixelW_ != sapp_width() || pixelH_ != sapp_height()) resize();
    update(dt);
    Mat4 viewProj = mul(proj_, view_);
    publishFrameData(viewProj);
    fps_.frame(now);
    hud_.fps = fps_.fps;
    hud_.entities = int(units_.size());
    hud_.publish(rt_);
    jsui_tick(rt_, now);
    if (opt.selftest) {
      int r = selftest_.step(rt_, *this, logicalW_, logicalH_, hud_);
      if (r != 0) {
        exitCode_ = r > 0 ? 0 : 1;
        quit_ = true;
      }
    }
    updateCursor();
    if (opt.cpu) paintUiCpu();

    // The scene, in sokol's swapchain pass.
    sg_pass pass{};
    pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
    pass.action.colors[0].clear_value = {9 / 255.f, 12 / 255.f, 22 / 255.f, 1};
    pass.swapchain = sglue_swapchain();  // acquires the swapchain: once per frame
    if (pass.swapchain.invalid) return;
    fbo_ = pass.swapchain.gl.framebuffer;
    sg_begin_pass(&pass);
    drawScene();
    if (opt.cpu) drawUiQuad();
    sgl_draw();
    sg_end_pass();

    // The UI, by Skia, over the scene in the same framebuffer.
    if (!opt.cpu) {
      JsuiGLTarget t{};
      t.fbo = fbo_;
      t.width = pixelW_;
      t.height = pixelH_;
      t.stencil_bits = 8;  // sokol_app's default framebuffer is DEPTH_STENCIL
      t.samples = sapp_sample_count();
      t.flags = JSUI_GL_NO_CLEAR;
      jsui_render_gl_target(rt_, gpu_, &t);
      sg_reset_state_cache();  // Skia changed GL state behind sokol_gfx's back
    }

    frameCount_++;
    bool last = quit_ || (opt.frames >= 0 && long(frameCount_) >= opt.frames);
    if (last && !opt.screenshot.empty()) screenshot(opt.screenshot);
    sg_commit();
    if (last) sapp_quit();
  }

  void cleanup() {
    const char* err = jsui_last_error(rt_);
    if (err) {
      std::fprintf(stderr, "JS error: %s\n", err);
      if (exitCode_ == 0) exitCode_ = 1;
    }
    jsui_destroy(rt_);
    if (gpu_) jsui_gpu_destroy(gpu_);  // GL context still current
    sgl_shutdown();
    sg_shutdown();
  }

  void event(const sapp_event* e) {
    if (opt.selftest || !rt_) return;
    float s = sapp_dpi_scale();
    float x = e->mouse_x / s, y = e->mouse_y / s;  // sokol reports framebuffer px
    int mods = ((e->modifiers & SAPP_MODIFIER_SHIFT) ? JSUI_MOD_SHIFT : 0) |
               ((e->modifiers & SAPP_MODIFIER_CTRL) ? JSUI_MOD_CTRL : 0) |
               ((e->modifiers & SAPP_MODIFIER_ALT) ? JSUI_MOD_ALT : 0) |
               ((e->modifiers & SAPP_MODIFIER_SUPER) ? JSUI_MOD_META : 0);
    switch (e->type) {
      case SAPP_EVENTTYPE_MOUSE_MOVE:
        mouseMove(x, y, e->mouse_dx / s, e->mouse_dy / s);
        break;
      case SAPP_EVENTTYPE_MOUSE_DOWN:
      case SAPP_EVENTTYPE_MOUSE_UP: {
        JsuiMouseButton b = e->mouse_button == SAPP_MOUSEBUTTON_RIGHT    ? JSUI_MOUSE_RIGHT
                            : e->mouse_button == SAPP_MOUSEBUTTON_MIDDLE ? JSUI_MOUSE_MIDDLE
                                                                         : JSUI_MOUSE_LEFT;
        mouseButton(x, y, b, e->type == SAPP_EVENTTYPE_MOUSE_DOWN, mods);
        break;
      }
      case SAPP_EVENTTYPE_MOUSE_SCROLL:
        // DOM: positive deltaY scrolls down, ~40 px per notch.
        if (!jsui_send_mouse_wheel(rt_, x, y, -e->scroll_x * 40.f, -e->scroll_y * 40.f)) {
          distance_ = std::clamp(distance_ * (e->scroll_y > 0 ? 0.9f : 1.1f), 6.f, 40.f);
          hud_.wheel++;
        }
        break;
      case SAPP_EVENTTYPE_MOUSE_LEAVE: jsui_send_mouse_leave(rt_); break;
      case SAPP_EVENTTYPE_KEY_DOWN:
      case SAPP_EVENTTYPE_KEY_UP: {
        bool down = e->type == SAPP_EVENTTYPE_KEY_DOWN;
        int k = int(e->key_code);  // GLFW values
        std::string key = jsui_example::glfwDomKey(k, mods & JSUI_MOD_SHIFT);
        int consumed = jsui_send_key(rt_, key.c_str(), jsui_example::glfwDomCode(k), down, e->key_repeat, mods);
        if (consumed || !down || e->key_repeat) break;
        // Not taken by the UI: the game's own bindings.
        if (e->key_code == SAPP_KEYCODE_SPACE) spawn();
        else if (e->key_code == SAPP_KEYCODE_ESCAPE) hud_.paused = !hud_.paused;
        else break;
        hud_.keys++;
        break;
      }
      case SAPP_EVENTTYPE_CHAR:
        if (e->char_code >= 32 && e->char_code != 127)
          jsui_send_text(rt_, jsui_example::utf8(e->char_code).c_str());
        break;
      case SAPP_EVENTTYPE_TOUCHES_BEGAN:
      case SAPP_EVENTTYPE_TOUCHES_MOVED:
      case SAPP_EVENTTYPE_TOUCHES_ENDED:
      case SAPP_EVENTTYPE_TOUCHES_CANCELLED: {
        int phase = e->type == SAPP_EVENTTYPE_TOUCHES_BEGAN ? 0 : e->type == SAPP_EVENTTYPE_TOUCHES_MOVED ? 1 : 2;
        for (int i = 0; i < e->num_touches; i++) {
          if (!e->touches[i].changed) continue;
          jsui_send_touch(rt_, int(e->touches[i].identifier & 0x7fffffff), phase, e->touches[i].pos_x / s,
                          e->touches[i].pos_y / s);
        }
        break;
      }
      default: break;
    }
  }

  int exitCode() const { return exitCode_; }

 private:
  // Mouse input, from sokol_app events or the self-test.
  void mouseMove(float x, float y, float dx, float dy) {
    jsui_send_mouse_move(rt_, x, y);
    if (dragging_) {
      yaw_ += dx * 0.006f;
      pitch_ += dy * 0.006f;
      click_.move(x, y);
    }
    mouse_[0] = x;
    mouse_[1] = y;
  }

  void mouseButton(float x, float y, JsuiMouseButton b, bool down, int mods) {
    int consumed = jsui_send_mouse_button(rt_, x, y, b, down, mods);
    if (b != JSUI_MOUSE_LEFT) return;
    if (down && !consumed) {
      dragging_ = true;  // the press is the game's: orbit while held, select if it was a click
      click_.press(x, y);
      hud_.clicks++;
    } else if (!down && dragging_) {
      dragging_ = false;
      if (click_.release(x, y)) hud_.selected = pick(x, y);
    }
  }

  // SelfTestHost.
  void click(float x, float y) override {
    mouseMove(x, y, x - mouse_[0], y - mouse_[1]);
    mouseButton(x, y, JSUI_MOUSE_LEFT, true, 0);
    mouseButton(x, y, JSUI_MOUSE_LEFT, false, 0);
  }

  // The unit under (x, y), nearest first: the camera ray through that point
  // against each unit's cube (turned by its spin).
  int pick(float x, float y) override {
    float tanHalf = std::tan(kFovy * 0.5f), aspect = logicalW_ / std::max(1.f, logicalH_);
    float nx = (x / logicalW_ * 2 - 1) * tanHalf * aspect, ny = (1 - y / logicalH_ * 2) * tanHalf;
    Vec3 d = normalize({forward_.x + side_.x * nx + up_.x * ny, forward_.y + side_.y * nx + up_.y * ny,
                        forward_.z + side_.z * nx + up_.z * ny});
    const float o[3] = {eye_.x, eye_.y, eye_.z}, dir[3] = {d.x, d.y, d.z};
    int best = -1;
    float bestT = 0;
    for (size_t i = 0; i < units_.size(); i++) {
      Vec3 p = unitPos(units_[i]);
      float sz = units_[i].size;
      const float c[3] = {p.x, p.y, p.z}, h[3] = {sz, sz, sz};
      float t = jsui_example::rayBox(o, dir, c, h, time_ * units_[i].spin);
      if (t >= 0 && (best < 0 || t < bestT)) {
        best = int(i);
        bestT = t;
      }
    }
    return best;
  }

  bool unitOnScreen(int i, float* x, float* y) override {
    if (i < 0 || i >= int(units_.size())) return false;
    return project(unitPos(units_[i]), x, y) && *x >= 0 && *y >= 0 && *x < logicalW_ && *y < logicalH_;
  }

  // World -> logical px; false behind the camera.
  bool project(Vec3 p, float* x, float* y) const {
    Mat4 vp = mul(proj_, view_);
    float cx = vp.m[0] * p.x + vp.m[4] * p.y + vp.m[8] * p.z + vp.m[12];
    float cy = vp.m[1] * p.x + vp.m[5] * p.y + vp.m[9] * p.z + vp.m[13];
    float cw = vp.m[3] * p.x + vp.m[7] * p.y + vp.m[11] * p.z + vp.m[15];
    *x = (cx / cw * 0.5f + 0.5f) * logicalW_;
    *y = (1 - (cy / cw * 0.5f + 0.5f)) * logicalH_;
    return cw > 0.1f;
  }

  void resize() {
    pixelW_ = sapp_width();
    pixelH_ = sapp_height();
    float s = sapp_dpi_scale();
    logicalW_ = float(pixelW_) / s;
    logicalH_ = float(pixelH_) / s;
    jsui_set_viewport(rt_, int(std::lround(logicalW_)), int(std::lround(logicalH_)), s);
    if (!opt.cpu) return;
    if (uiImage_.id) {
      sg_destroy_view(uiView_);
      sg_destroy_image(uiImage_);
    }
    sg_image_desc id{};
    id.width = pixelW_;
    id.height = pixelH_;
    id.pixel_format = SG_PIXELFORMAT_RGBA8;
    id.usage.write_transient = true;
    uiImage_ = sg_make_image(&id);
    sg_view_desc vd{};
    vd.texture.image = uiImage_;
    uiView_ = sg_make_view(&vd);
    pixels_.assign(size_t(pixelW_) * pixelH_ * 4, 0);
    uiFresh_ = true;
  }

  // --cpu: repaint the damaged part of the kept buffer, then hand the whole
  // buffer to this frame's transient image.
  void paintUiCpu() {
    size_t row = size_t(pixelW_) * 4;
    JsuiRect dirty;
    if (uiFresh_) jsui_render_rgba(rt_, pixels_.data(), pixelW_, pixelH_, row);
    else if (jsui_needs_redraw(rt_)) jsui_render_rgba_incremental(rt_, pixels_.data(), pixelW_, pixelH_, row, &dirty);
    uiFresh_ = false;
    sg_write_image_desc w{};
    w.src.data = {pixels_.data(), pixels_.size()};
    w.dst.image = uiImage_;
    w.size = {pixelW_, pixelH_, 1};
    sg_write_image_transient(&w);
  }

  void drawUiQuad() {
    sgl_defaults();
    sgl_load_pipeline(uiPip_);
    sgl_enable_texture();
    sgl_texture(uiView_, sampler_);
    sgl_matrix_mode_projection();
    sgl_ortho(0, 1, 1, 0, -1, 1);  // y down, row 0 of the image at the top
    sgl_begin_quads();
    sgl_v2f_t2f_c4b(0, 0, 0, 0, 255, 255, 255, 255);
    sgl_v2f_t2f_c4b(1, 0, 1, 0, 255, 255, 255, 255);
    sgl_v2f_t2f_c4b(1, 1, 1, 1, 255, 255, 255, 255);
    sgl_v2f_t2f_c4b(0, 1, 0, 1, 255, 255, 255, 255);
    sgl_end();
  }

  void screenshot(const std::string& path) {
    std::vector<uint8_t> px(size_t(pixelW_) * pixelH_ * 4);
    glFinish();
#if defined(_WIN32)
    // Windows' gl.h is GL 1.1: glBindFramebuffer comes from the driver.
    using BindFramebufferFn = void(APIENTRY*)(GLenum, GLuint);
    auto glBindFramebuffer = reinterpret_cast<BindFramebufferFn>(wglGetProcAddress("glBindFramebuffer"));
    if (glBindFramebuffer)
#endif
    glBindFramebuffer(0x8D40 /* GL_FRAMEBUFFER */, fbo_);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, pixelW_, pixelH_, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    sg_reset_state_cache();
    size_t row = size_t(pixelW_) * 4;
    std::vector<uint8_t> tmp(row);
    for (int y = 0; y < pixelH_ / 2; y++) {  // GL rows are bottom-up
      uint8_t* a = px.data() + size_t(y) * row;
      uint8_t* b = px.data() + size_t(pixelH_ - 1 - y) * row;
      std::memcpy(tmp.data(), a, row);
      std::memcpy(a, b, row);
      std::memcpy(b, tmp.data(), row);
    }
    SkPixmap pm(SkImageInfo::Make(pixelW_, pixelH_, kRGBA_8888_SkColorType, kOpaque_SkAlphaType), px.data(), row);
    SkFILEWStream f(path.c_str());
    if (!f.isValid() || !SkPngEncoder::Encode(&f, pm, {})) std::fprintf(stderr, "screenshot failed: %s\n", path.c_str());
  }

  void onMessage(const char* name, const char* json) {
    selftest_.onMessage(name, json);
    if (!std::strcmp(name, "game:spawn")) {
      int n = std::clamp(int(jsui_example::jsonNumber(json, "count", 1)), 1, 50);
      for (int i = 0; i < n; i++) spawn();
    } else if (!std::strcmp(name, "game:clear")) {
      units_.clear();
      hud_.selected = -1;
    } else if (!std::strcmp(name, "game:pause")) {
      hud_.paused = jsui_example::jsonBool(json, "paused");
    } else if (!std::strcmp(name, "game:quit")) {
      quit_ = true;
    }
  }

  float rnd(float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng_); }

  void spawn() {
    if (units_.size() >= 48) {
      units_.erase(units_.begin());
      if (hud_.selected >= 0) hud_.selected--;  // -1 if it was the one removed
    }
    Unit u;
    u.angle = rnd(0, 6.283f);
    u.radius = rnd(3.f, 9.f);
    u.speed = rnd(0.2f, 0.7f) * (rnd(0, 1) < 0.5f ? 1.f : -1.f);
    u.height = rnd(0.6f, 2.4f);
    u.size = rnd(0.35f, 0.65f);
    u.spin = rnd(0.5f, 2.f);
    const uint8_t* c = kHues[spawned_++ % 7];
    u.r = c[0], u.g = c[1], u.b = c[2];
    units_.push_back(u);
  }

  Vec3 unitPos(const Unit& u) const {
    return {std::cos(u.angle) * u.radius, u.height + 0.3f * std::sin(time_ * 1.7f + u.angle * 3.f),
            std::sin(u.angle) * u.radius};
  }

  void update(float dt) {
    if (!hud_.paused) {
      time_ += dt;
      for (auto& u : units_) u.angle += u.speed * dt;
    }
    pitch_ = std::clamp(pitch_, 0.15f, 1.35f);
    eye_ = {std::cos(yaw_) * std::cos(pitch_) * distance_, std::sin(pitch_) * distance_,
            std::sin(yaw_) * std::cos(pitch_) * distance_};
    view_ = lookAt(eye_, {0, 1, 0}, {0, 1, 0});
    proj_ = perspective(kFovy, float(pixelW_) / float(std::max(1, pixelH_)), 0.1f, 200.f);
    // The camera's basis, for picking rays (as lookAt builds it).
    forward_ = normalize(sub({0, 1, 0}, eye_));
    side_ = normalize(cross(forward_, {0, 1, 0}));
    up_ = cross(side_, forward_);
  }

  void cube(Vec3 c, float h, float rot, uint8_t r, uint8_t g, uint8_t b) {
    float cs = std::cos(rot), sn = std::sin(rot);
    auto p = [&](float x, float y, float z) {
      return Vec3{c.x + (x * cs - z * sn) * h, c.y + y * h, c.z + (x * sn + z * cs) * h};
    };
    // Six faces, shaded by direction.
    const float faces[6][4][3] = {
        {{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}},     {{1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}, {1, 1, -1}},
        {{-1, 1, 1}, {1, 1, 1}, {1, 1, -1}, {-1, 1, -1}},     {{-1, -1, -1}, {1, -1, -1}, {1, -1, 1}, {-1, -1, 1}},
        {{1, -1, 1}, {1, -1, -1}, {1, 1, -1}, {1, 1, 1}},     {{-1, -1, -1}, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1}}};
    const float shade[6] = {0.85f, 0.6f, 1.f, 0.4f, 0.75f, 0.7f};
    for (int f = 0; f < 6; f++) {
      uint8_t fr = uint8_t(r * shade[f]), fg = uint8_t(g * shade[f]), fb = uint8_t(b * shade[f]);
      for (int v = 0; v < 4; v++) {
        Vec3 q = p(faces[f][v][0], faces[f][v][1], faces[f][v][2]);
        sgl_v3f_c4b(q.x, q.y, q.z, fr, fg, fb, 255);
      }
    }
  }

  void drawScene() {
    sgl_defaults();
    sgl_load_pipeline(scenePip_);
    sgl_matrix_mode_projection();
    sgl_load_matrix(proj_.m);
    sgl_matrix_mode_modelview();
    sgl_load_matrix(view_.m);
    // Floor and rings.
    sgl_begin_quads();
    sgl_v3f_c4b(-30, 0, -30, 14, 19, 34, 255);
    sgl_v3f_c4b(30, 0, -30, 14, 19, 34, 255);
    sgl_v3f_c4b(30, 0, 30, 14, 19, 34, 255);
    sgl_v3f_c4b(-30, 0, 30, 14, 19, 34, 255);
    sgl_end();
    sgl_begin_lines();
    for (int ring = 1; ring <= 5; ring++) {
      float r = ring * 2.f;
      for (int i = 0; i < 96; i++) {
        float a0 = i * 6.2831853f / 96, a1 = (i + 1) * 6.2831853f / 96;
        sgl_v3f_c4b(std::cos(a0) * r, 0.01f, std::sin(a0) * r, 79, 209, 255, 70);
        sgl_v3f_c4b(std::cos(a1) * r, 0.01f, std::sin(a1) * r, 79, 209, 255, 70);
      }
    }
    for (int i = 0; i < 12; i++) {
      float a = i * 6.2831853f / 12;
      sgl_v3f_c4b(std::cos(a) * 2, 0.01f, std::sin(a) * 2, 148, 170, 255, 50);
      sgl_v3f_c4b(std::cos(a) * 10, 0.01f, std::sin(a) * 10, 148, 170, 255, 50);
    }
    sgl_end();
    sgl_begin_quads();
    cube({0, 1.1f, 0}, 1.1f, time_ * 0.3f, 40, 56, 100);
    for (size_t i = 0; i < units_.size(); i++) {
      const Unit& u = units_[i];
      if (int(i) == hud_.selected) {
        // Selected: lit towards white, pulsing.
        float k = 0.35f + 0.15f * std::sin(time_ * 6.f);
        auto lit = [&](uint8_t c) { return uint8_t(c + (255 - c) * k); };
        cube(unitPos(u), u.size, time_ * u.spin, lit(u.r), lit(u.g), lit(u.b));
      } else {
        cube(unitPos(u), u.size, time_ * u.spin, u.r, u.g, u.b);
      }
    }
    sgl_end();
    if (hud_.selected >= 0 && hud_.selected < int(units_.size())) drawSelection(units_[hud_.selected]);
  }

  // A white outline box around the selected unit and rings on the ground
  // under it.
  void drawSelection(const Unit& u) {
    Vec3 c = unitPos(u);
    float h = u.size * 1.2f, rot = time_ * u.spin, cs = std::cos(rot), sn = std::sin(rot);
    auto p = [&](float x, float y, float z) {
      return Vec3{c.x + (x * cs - z * sn) * h, c.y + y * h, c.z + (x * sn + z * cs) * h};
    };
    sgl_begin_lines();
    const float e[12][2][3] = {{{-1, -1, -1}, {1, -1, -1}}, {{1, -1, -1}, {1, -1, 1}},   {{1, -1, 1}, {-1, -1, 1}},
                               {{-1, -1, 1}, {-1, -1, -1}}, {{-1, 1, -1}, {1, 1, -1}},   {{1, 1, -1}, {1, 1, 1}},
                               {{1, 1, 1}, {-1, 1, 1}},     {{-1, 1, 1}, {-1, 1, -1}},   {{-1, -1, -1}, {-1, 1, -1}},
                               {{1, -1, -1}, {1, 1, -1}},   {{1, -1, 1}, {1, 1, 1}},     {{-1, -1, 1}, {-1, 1, 1}}};
    for (auto& edge : e)
      for (auto& v : edge) {
        Vec3 q = p(v[0], v[1], v[2]);
        sgl_v3f_c4b(q.x, q.y, q.z, 255, 255, 255, 255);
      }
    float r0 = u.size * (1.9f + 0.15f * std::sin(time_ * 4.f));
    for (int ring = 0; ring < 3; ring++) {
      float r = r0 + ring * 0.06f;
      for (int i = 0; i < 64; i++) {
        float a0 = i * 6.2831853f / 64, a1 = (i + 1) * 6.2831853f / 64;
        sgl_v3f_c4b(c.x + std::cos(a0) * r, 0.02f, c.z + std::sin(a0) * r, u.r, u.g, u.b, 255);
        sgl_v3f_c4b(c.x + std::cos(a1) * r, 0.02f, c.z + std::sin(a1) * r, u.r, u.g, u.b, 255);
      }
    }
    sgl_end();
  }

  // Unit screen positions for the nameplates: x, y, visible, hue (logical px).
  void publishFrameData(const Mat4& vp) {
    float* b = static_cast<float*>(jsui_map_buffer(rt_, "entities", JSUI_BUFFER_F32, units_.size() * 4));
    if (!b) return;
    for (size_t i = 0; i < units_.size(); i++) {
      Vec3 p = unitPos(units_[i]);
      p.y += units_[i].size + 0.35f;
      float cx = vp.m[0] * p.x + vp.m[4] * p.y + vp.m[8] * p.z + vp.m[12];
      float cy = vp.m[1] * p.x + vp.m[5] * p.y + vp.m[9] * p.z + vp.m[13];
      float cw = vp.m[3] * p.x + vp.m[7] * p.y + vp.m[11] * p.z + vp.m[15];
      float sx = (cx / cw * 0.5f + 0.5f) * logicalW_;
      float sy = (1 - (cy / cw * 0.5f + 0.5f)) * logicalH_;
      bool visible = cw > 0.1f && sx > -60 && sy > -60 && sx < logicalW_ + 60 && sy < logicalH_ + 60;
      b[i * 4 + 0] = std::round(sx);
      b[i * 4 + 1] = std::round(sy);
      b[i * 4 + 2] = visible ? 1.f : 0.f;
      b[i * 4 + 3] = float(i % 7);
    }
    jsui_commit_buffer(rt_, "entities");
  }

  void updateCursor() {
    std::string c = jsui_get_cursor(rt_);
    if (c == cursor_) return;
    cursor_ = c;
    sapp_show_mouse(c != "none");
    sapp_mouse_cursor mc = SAPP_MOUSECURSOR_DEFAULT;
    if (c == "pointer") mc = SAPP_MOUSECURSOR_POINTING_HAND;
    else if (c == "text") mc = SAPP_MOUSECURSOR_IBEAM;
    else if (c == "crosshair") mc = SAPP_MOUSECURSOR_CROSSHAIR;
    else if (c == "not-allowed") mc = SAPP_MOUSECURSOR_NOT_ALLOWED;
    else if (c == "ew-resize" || c == "col-resize") mc = SAPP_MOUSECURSOR_RESIZE_EW;
    else if (c == "ns-resize" || c == "row-resize") mc = SAPP_MOUSECURSOR_RESIZE_NS;
    else if (c == "move" || c == "grab" || c == "grabbing") mc = SAPP_MOUSECURSOR_RESIZE_ALL;
    sapp_set_mouse_cursor(mc);
  }

  JsuiRuntime* rt_ = nullptr;
  JsuiGpuContext* gpu_ = nullptr;
  sgl_pipeline scenePip_{}, uiPip_{};
  sg_image uiImage_{};
  sg_view uiView_{};
  sg_sampler sampler_{};
  std::vector<uint8_t> pixels_;
  bool uiFresh_ = true;
  int pixelW_ = 0, pixelH_ = 0;
  uint32_t fbo_ = 0;  // sokol_app's GL framebuffer for this frame
  float logicalW_ = 0, logicalH_ = 0;
  static constexpr float kFovy = 50.f * 3.14159265f / 180.f;
  Mat4 view_{}, proj_{};
  Vec3 eye_{}, forward_{}, side_{}, up_{};  // the camera, for picking
  jsui_example::HudState hud_;
  jsui_example::FpsCounter fps_;
  jsui_example::SelfTest selftest_;
  jsui_example::ClickTracker click_;
  float mouse_[2] = {-1, -1};
  std::vector<Unit> units_;
  size_t spawned_ = 0;
  std::mt19937 rng_{7};
  float time_ = 0;
  float yaw_ = 0.6f, pitch_ = 0.5f, distance_ = 17.f;
  bool dragging_ = false, quit_ = false;
  uint64_t frameCount_ = 0;
  double clock_ = 0;
  int exitCode_ = 0;
  std::string cursor_;
};

Game* game = nullptr;

}  // namespace

sapp_desc sokol_main(int argc, char* argv[]) {
  static Game g;
  game = &g;
  if (!jsui_example::parseOptions(argc, argv, &g.opt)) std::exit(2);
  sapp_desc d{};
  d.init_cb = [] { game->init(); };
  d.frame_cb = [] { game->frame(); };
  d.cleanup_cb = [] {
    game->cleanup();
    // sokol_app returns from its run loop without an exit code: hand on the
    // self-test's verdict.
    std::fflush(stdout);
    std::_Exit(game->exitCode());
  };
  d.event_cb = [](const sapp_event* e) { game->event(e); };
  d.width = g.opt.width;
  d.height = g.opt.height;
  d.high_dpi = true;
  d.window_title = "JSUI + sokol";
  d.enable_clipboard = true;
  d.clipboard_size = 64 * 1024;
  d.swap_interval = (g.opt.frames >= 0 || g.opt.selftest) ? 0 : 1;
  d.logger.func = slog_func;
  return d;
}
