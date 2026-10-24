// JSUI in raylib: a small 3D scene with the shared HUD
// (integrations/shared/ui/app.jsx) on top.
//
//   jsui-raylib [--cpu] [--dev] [bundle.js | http://127.0.0.1:5173/bundle.js]
//
// Rendering. By default Skia draws the UI with OpenGL (Ganesh) straight into
// the texture of a raylib RenderTexture2D, on raylib's own GL context, only
// when the UI changed; raylib then composites that texture over the scene
// with premultiplied alpha like any other texture. --cpu rasterises the UI on
// the CPU instead (jsui_render_rgba_incremental) and uploads only the damaged
// rectangle with UpdateTextureRec.
//
// Input pass-through. Every mouse, wheel, key and gamepad event goes to the
// UI first; jsui_send_* returns whether the UI consumed it. What it did not
// consume drives the game: drag empty space to orbit the camera, wheel to
// zoom, Space spawns a unit, Esc / Start pauses, the right stick orbits.
//
// Per-frame data. The game projects every unit to the screen each frame and
// writes x, y, visible, hue into the `entities` frame buffer
// (jsui_map_buffer), which moves the nameplates without running JS; fps,
// unit count and the pass-through counters go to the `game` shared state.
#include <raylib.h>
#include <raymath.h>
#include <rlgl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "example_host.h"
#include "glfw_keys.h"
#include "jsui/jsui.h"

namespace {

using jsui_example::Options;

// The GL calls the example makes itself, to put back the state raylib
// expects after Skia drew (see restoreRaylibState).
struct GL {
  void (*Enable)(unsigned) = nullptr;
  void (*Disable)(unsigned) = nullptr;
  void (*BlendFunc)(unsigned, unsigned) = nullptr;
  void (*BlendEquation)(unsigned) = nullptr;
  void (*CullFace)(unsigned) = nullptr;
  void (*FrontFace)(unsigned) = nullptr;
  void (*DepthFunc)(unsigned) = nullptr;
  void (*DepthMask)(unsigned char) = nullptr;
  void (*ColorMask)(unsigned char, unsigned char, unsigned char, unsigned char) = nullptr;
  void (*Viewport)(int, int, int, int) = nullptr;
  void (*BindFramebuffer)(unsigned, unsigned) = nullptr;
  void (*ActiveTexture)(unsigned) = nullptr;
  void (*PixelStorei)(unsigned, int) = nullptr;
  void (*BindVertexArray)(unsigned) = nullptr;
  void (*UseProgram)(unsigned) = nullptr;
  const unsigned char* (*GetString)(unsigned) = nullptr;
  bool load() {
    bool ok = true;
    auto sym = [&](auto& f, const char* n) {
      f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(rlGetProcAddress(n));
      ok &= f != nullptr;
    };
    sym(Enable, "glEnable");
    sym(Disable, "glDisable");
    sym(BlendFunc, "glBlendFunc");
    sym(BlendEquation, "glBlendEquation");
    sym(CullFace, "glCullFace");
    sym(FrontFace, "glFrontFace");
    sym(DepthFunc, "glDepthFunc");
    sym(DepthMask, "glDepthMask");
    sym(ColorMask, "glColorMask");
    sym(Viewport, "glViewport");
    sym(BindFramebuffer, "glBindFramebuffer");
    sym(ActiveTexture, "glActiveTexture");
    sym(PixelStorei, "glPixelStorei");
    sym(BindVertexArray, "glBindVertexArray");
    sym(UseProgram, "glUseProgram");
    sym(GetString, "glGetString");
    return ok;
  }
};

struct Unit {
  float angle, radius, speed, height, size, phase;
  Color color;
};

const Color kHues[] = {{242, 195, 91, 255}, {79, 209, 255, 255}, {178, 107, 255, 255}, {74, 222, 128, 255},
                       {255, 154, 60, 255}, {79, 140, 255, 255},  {255, 77, 106, 255}};

// raylib gamepad button -> W3C standard gamepad index.
int standardButton(int b) {
  switch (b) {
    case GAMEPAD_BUTTON_RIGHT_FACE_DOWN: return 0;   // A / Cross
    case GAMEPAD_BUTTON_RIGHT_FACE_RIGHT: return 1;  // B / Circle
    case GAMEPAD_BUTTON_RIGHT_FACE_LEFT: return 2;   // X / Square
    case GAMEPAD_BUTTON_RIGHT_FACE_UP: return 3;     // Y / Triangle
    case GAMEPAD_BUTTON_LEFT_TRIGGER_1: return 4;
    case GAMEPAD_BUTTON_RIGHT_TRIGGER_1: return 5;
    case GAMEPAD_BUTTON_LEFT_TRIGGER_2: return 6;
    case GAMEPAD_BUTTON_RIGHT_TRIGGER_2: return 7;
    case GAMEPAD_BUTTON_MIDDLE_LEFT: return 8;   // Back / Select
    case GAMEPAD_BUTTON_MIDDLE_RIGHT: return 9;  // Start
    case GAMEPAD_BUTTON_LEFT_THUMB: return 10;
    case GAMEPAD_BUTTON_RIGHT_THUMB: return 11;
    case GAMEPAD_BUTTON_LEFT_FACE_UP: return 12;
    case GAMEPAD_BUTTON_LEFT_FACE_DOWN: return 13;
    case GAMEPAD_BUTTON_LEFT_FACE_LEFT: return 14;
    case GAMEPAD_BUTTON_LEFT_FACE_RIGHT: return 15;
    case GAMEPAD_BUTTON_MIDDLE: return 16;
    default: return -1;
  }
}

class Game {
 public:
  explicit Game(const Options& o) : opt_(o) {}

  int run() {
    unsigned flags = FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_HIGHDPI | FLAG_VSYNC_HINT | FLAG_MSAA_4X_HINT;
    // Fixed-frame runs (CI under Xvfb) go as fast as they can, on any visual.
    if (opt_.frames >= 0 || opt_.selftest) flags &= ~(FLAG_VSYNC_HINT | FLAG_MSAA_4X_HINT);
    SetConfigFlags(flags);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(opt_.width, opt_.height, "JSUI + raylib");
    if (!IsWindowReady()) return 1;
    SetExitKey(KEY_NULL);  // Esc belongs to the UI / the pause menu

    if (!opt_.cpu && !initGpu()) {
      std::fprintf(stderr, "warning: GPU renderer unavailable, falling back to the CPU renderer\n");
      opt_.cpu = true;
    }
    hud_.engine = "raylib " RAYLIB_VERSION;
    hud_.renderer = opt_.cpu ? "CPU raster · UpdateTextureRec" : "Skia Ganesh GL · into a RenderTexture2D";

    JsuiConfig cfg;
    jsui_config_init(&cfg);
    cfg.width = GetScreenWidth();
    cfg.height = GetScreenHeight();
    cfg.dev_mode = opt_.dev;
    cfg.message_user = this;
    cfg.on_message = [](void* u, const char* name, const char* json) { static_cast<Game*>(u)->onMessage(name, json); };
    rt_ = jsui_create(&cfg);
    jsui_set_clipboard_callbacks(
        rt_,
        [](void*) -> const char* { return GetClipboardText(); },
        [](void*, const char* text) { SetClipboardText(text); }, nullptr);
    for (int i = 0; i < 8; i++) spawn();
    resize();
    hud_.publish(rt_);  // before the bundle runs: its first render sees it
    if (jsui_load_bundle_url(rt_, opt_.bundle.c_str()) != 0)
      std::fprintf(stderr, "failed to load %s: %s\n", opt_.bundle.c_str(), jsui_last_error(rt_));

    int result = 0;
    long frame = 0;
    while (!quit_ && !WindowShouldClose()) {
      double now = GetTime() * 1000.0;
      if (IsWindowResized()) resize();
      if (!opt_.selftest) forwardInput();
      float dt = GetFrameTime();
      if (opt_.frames >= 0 || opt_.selftest) dt = 1.f / 60.f;  // deterministic runs
      update(dt);
      publishFrameData();
      fps_.frame(now);
      hud_.fps = fps_.fps;
      hud_.entities = int(units_.size());
      hud_.publish(rt_);

      jsui_tick(rt_, now);
      if (opt_.selftest) {
        int r = selftest_.step(rt_, float(GetScreenWidth()), float(GetScreenHeight()), int(units_.size()));
        if (r != 0) {
          result = r > 0 ? 0 : 1;
          quit_ = true;
        }
      }
      updateCursor();
      paintUi();

      BeginDrawing();
      ClearBackground({9, 12, 22, 255});
      drawScene();
      // The UI is premultiplied: ONE, ONE_MINUS_SRC_ALPHA.
      BeginBlendMode(BLEND_ALPHA_PREMULTIPLY);
      // GL textures are bottom-up: the GPU path draws its texture flipped,
      // the CPU path uploaded top-down rows.
      float h = float(ui_.texture.height);
      Rectangle src{0, 0, float(ui_.texture.width), opt_.cpu ? h : -h};
      DrawTexturePro(ui_.texture, src, {0, 0, float(GetScreenWidth()), float(GetScreenHeight())}, {0, 0}, 0, WHITE);
      EndBlendMode();
      frame++;
      bool last = quit_ || (opt_.frames >= 0 && frame >= opt_.frames);
      if (last && !opt_.screenshot.empty()) screenshot(opt_.screenshot);
      EndDrawing();
      if (opt_.frames >= 0 && frame >= opt_.frames) break;
    }

    const char* err = jsui_last_error(rt_);
    if (err) std::fprintf(stderr, "JS error: %s\n", err);
    jsui_destroy(rt_);
    if (gpu_) jsui_gpu_destroy(gpu_);  // raylib's GL context still current
    UnloadRenderTexture(ui_);
    CloseWindow();
    return result ? result : (err ? 1 : 0);
  }

 private:
  bool initGpu() {
    if (!gl_.load()) return false;
    // raylib loaded GL through GLFW; Skia resolves it the same way.
    gpu_ = jsui_gpu_create_gl([](void*, const char* name) { return rlGetProcAddress(name); }, nullptr);
    if (!gpu_) return false;
    const unsigned char* r = gl_.GetString(0x1F01);  // GL_RENDERER
    const unsigned char* v = gl_.GetString(0x1F02);  // GL_VERSION
    std::printf("GPU: %s / %s\n", r ? (const char*)r : "?", v ? (const char*)v : "?");
    return true;
  }

  // The composed frame (scene + UI), read back before the swap.
  void screenshot(const std::string& path) {
    rlDrawRenderBatchActive();
    int w = GetRenderWidth(), h = GetRenderHeight();
    Image img{rlReadScreenPixels(w, h), w, h, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    if (!ExportImage(img, path.c_str())) std::fprintf(stderr, "screenshot failed: %s\n", path.c_str());
    UnloadImage(img);
  }

  // Window size -> runtime viewport (logical px + pixel ratio) and a UI
  // texture of the framebuffer's size.
  void resize() {
    int lw = GetScreenWidth(), lh = GetScreenHeight();
    int pw = GetRenderWidth(), ph = GetRenderHeight();
    if (lw <= 0 || lh <= 0 || pw <= 0 || ph <= 0) return;
    jsui_set_viewport(rt_, lw, lh, float(pw) / float(lw));
    if (ui_.id && ui_.texture.width == pw && ui_.texture.height == ph) return;
    if (ui_.id) UnloadRenderTexture(ui_);
    ui_ = LoadRenderTexture(pw, ph);
    SetTextureFilter(ui_.texture, TEXTURE_FILTER_BILINEAR);
    pixels_.assign(size_t(pw) * ph * 4, 0);
    uiFresh_ = true;  // repaint into the new texture
  }

  void paintUi() {
    if (!jsui_needs_redraw(rt_) && !uiFresh_) return;
    int w = ui_.texture.width, h = ui_.texture.height;
    if (!opt_.cpu) {
      rlDrawRenderBatchActive();  // flush raylib's batch before touching GL
      JsuiGLTarget t{};
      t.texture = ui_.texture.id;  // Skia renders into it (an FBO of its own)
      t.width = w;
      t.height = h;
      if (jsui_render_gl_target(rt_, gpu_, &t) == 0) uiFresh_ = false;
      restoreRaylibState();
      return;
    }
    JsuiRect dirty;
    if (uiFresh_) {
      jsui_render_rgba(rt_, pixels_.data(), w, h, size_t(w) * 4);
      dirty = {0, 0, w, h};
      uiFresh_ = false;
    } else if (!jsui_render_rgba_incremental(rt_, pixels_.data(), w, h, size_t(w) * 4, &dirty)) {
      return;
    }
    if (dirty.width <= 0 || dirty.height <= 0) return;
    // UpdateTextureRec wants the rectangle's rows packed.
    rect_.resize(size_t(dirty.width) * dirty.height * 4);
    for (int y = 0; y < dirty.height; y++)
      std::memcpy(rect_.data() + size_t(y) * dirty.width * 4,
                  pixels_.data() + (size_t(dirty.y + y) * w + dirty.x) * 4, size_t(dirty.width) * 4);
    UpdateTextureRec(ui_.texture, {float(dirty.x), float(dirty.y), float(dirty.width), float(dirty.height)},
                     rect_.data());
  }

  // Skia leaves its own GL state behind (blend, scissor, cull, bound
  // framebuffer / program / vertex array, pixel store). raylib caches some
  // of it and assumes rlglInit's defaults for the rest: put those back.
  void restoreRaylibState() {
    gl_.BindFramebuffer(0x8D40, 0);  // GL_FRAMEBUFFER
    gl_.Viewport(0, 0, GetRenderWidth(), GetRenderHeight());
    gl_.Disable(0x0C11);  // GL_SCISSOR_TEST
    gl_.Disable(0x0B90);  // GL_STENCIL_TEST
    gl_.Enable(0x0BE2);   // GL_BLEND
    gl_.BlendEquation(0x8006);       // GL_FUNC_ADD
    gl_.BlendFunc(0x0302, 0x0303);   // GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA (raylib's BLEND_ALPHA)
    gl_.Enable(0x0B44);              // GL_CULL_FACE
    gl_.CullFace(0x0405);            // GL_BACK
    gl_.FrontFace(0x0901);           // GL_CCW
    gl_.DepthFunc(0x0203);           // GL_LEQUAL
    gl_.DepthMask(1);
    gl_.ColorMask(1, 1, 1, 1);
    gl_.ActiveTexture(0x84C0);       // GL_TEXTURE0
    gl_.PixelStorei(0x0CF2, 0);      // GL_UNPACK_ROW_LENGTH
    gl_.PixelStorei(0x0CF5, 4);      // GL_UNPACK_ALIGNMENT
    gl_.BindVertexArray(0);
    gl_.UseProgram(0);
  }

  void onMessage(const char* name, const char* json) {
    selftest_.onMessage(name, json);
    if (!std::strcmp(name, "game:spawn")) {
      int n = std::clamp(int(jsui_example::jsonNumber(json, "count", 1)), 1, 50);
      for (int i = 0; i < n; i++) spawn();
    } else if (!std::strcmp(name, "game:clear")) {
      units_.clear();
    } else if (!std::strcmp(name, "game:pause")) {
      hud_.paused = jsui_example::jsonBool(json, "paused");
    } else if (!std::strcmp(name, "game:quit")) {
      quit_ = true;
    }
  }

  void spawn() {
    if (units_.size() >= 48) units_.erase(units_.begin());
    int i = int(spawned_++);
    Unit u;
    u.angle = float(GetRandomValue(0, 628)) / 100.f;
    u.radius = 2.5f + float(GetRandomValue(0, 70)) / 10.f;
    u.speed = (GetRandomValue(0, 1) ? 1.f : -1.f) * (0.2f + float(GetRandomValue(0, 60)) / 100.f);
    u.height = 0.5f + float(GetRandomValue(0, 15)) / 10.f;
    u.size = 0.6f + float(GetRandomValue(0, 6)) / 10.f;
    u.phase = float(GetRandomValue(0, 628)) / 100.f;
    u.color = kHues[i % 7];
    units_.push_back(u);
  }

  Vector3 unitPos(const Unit& u) const {
    return {std::cos(u.angle) * u.radius, u.height + 0.25f * std::sin(time_ * 2.f + u.phase), std::sin(u.angle) * u.radius};
  }

  void update(float dt) {
    if (!hud_.paused) {
      time_ += dt;
      for (auto& u : units_) u.angle += u.speed * dt;
    }
    yaw_ += orbitVelocity_ * dt;
    pitch_ = std::clamp(pitch_, 0.15f, 1.35f);
    camera_.position = {std::cos(yaw_) * std::cos(pitch_) * distance_, std::sin(pitch_) * distance_,
                        std::sin(yaw_) * std::cos(pitch_) * distance_};
    camera_.target = {0, 0.8f, 0};
    camera_.up = {0, 1, 0};
    camera_.fovy = 50;
    camera_.projection = CAMERA_PERSPECTIVE;
  }

  void drawScene() {
    BeginMode3D(camera_);
    DrawPlane({0, 0, 0}, {40, 40}, {14, 19, 34, 255});
    DrawGrid(40, 1.0f);
    DrawCylinder({0, 0, 0}, 0.9f, 1.2f, 2.2f, 24, {26, 36, 64, 255});
    DrawCylinderWires({0, 0, 0}, 0.9f, 1.2f, 2.2f, 24, {79, 209, 255, 120});
    for (auto& u : units_) {
      Vector3 p = unitPos(u);
      DrawCube(p, u.size, u.size, u.size, u.color);
      DrawCubeWires(p, u.size, u.size, u.size, {255, 255, 255, 90});
      DrawCircle3D({p.x, 0.01f, p.z}, u.size * 0.6f, {1, 0, 0}, 90, {0, 0, 0, 90});
    }
    EndMode3D();
  }

  // Unit screen positions for the nameplates: x, y, visible, hue (logical px).
  void publishFrameData() {
    float* b = static_cast<float*>(jsui_map_buffer(rt_, "entities", JSUI_BUFFER_F32, units_.size() * 4));
    if (!b) return;
    Matrix view = GetCameraMatrix(camera_);
    float w = float(GetScreenWidth()), h = float(GetScreenHeight());
    for (size_t i = 0; i < units_.size(); i++) {
      Vector3 p = unitPos(units_[i]);
      p.y += units_[i].size * 0.5f + 0.35f;
      Vector3 inView = Vector3Transform(p, view);  // camera looks down -Z
      Vector2 s = GetWorldToScreen(p, camera_);
      bool visible = inView.z < -0.1f && s.x > -60 && s.y > -60 && s.x < w + 60 && s.y < h + 60;
      b[i * 4 + 0] = std::round(s.x);
      b[i * 4 + 1] = std::round(s.y);
      b[i * 4 + 2] = visible ? 1.f : 0.f;
      b[i * 4 + 3] = float(i % 7);
    }
    jsui_commit_buffer(rt_, "entities");
  }

  int mods() const {
    int m = 0;
    if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) m |= JSUI_MOD_SHIFT;
    if (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) m |= JSUI_MOD_CTRL;
    if (IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT)) m |= JSUI_MOD_ALT;
    if (IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER)) m |= JSUI_MOD_META;
    return m;
  }

  void sendKey(int key, bool down, bool repeat) {
    std::string name = jsui_example::glfwDomKey(key, mods() & JSUI_MOD_SHIFT);
    int consumed = jsui_send_key(rt_, name.c_str(), jsui_example::glfwDomCode(key), down, repeat, mods());
    if (consumed || !down || repeat) return;
    // Not taken by the UI: the game's own bindings.
    if (key == KEY_SPACE) spawn();
    else if (key == KEY_ESCAPE) hud_.paused = !hud_.paused;
    else return;
    hud_.keys++;
  }

  // raylib is polled, not event driven: turn this frame's state into the
  // runtime's events.
  void forwardInput() {
    Vector2 m = GetMousePosition();
    if (m.x != mouse_.x || m.y != mouse_.y) {
      jsui_send_mouse_move(rt_, m.x, m.y);
      if (dragging_) {
        yaw_ += (m.x - mouse_.x) * 0.006f;
        pitch_ += (m.y - mouse_.y) * 0.006f;
      }
      mouse_ = m;
    }
    const int buttons[3][2] = {{MOUSE_BUTTON_LEFT, JSUI_MOUSE_LEFT},
                               {MOUSE_BUTTON_MIDDLE, JSUI_MOUSE_MIDDLE},
                               {MOUSE_BUTTON_RIGHT, JSUI_MOUSE_RIGHT}};
    for (auto& b : buttons) {
      if (IsMouseButtonPressed(b[0])) {
        int consumed = jsui_send_mouse_button(rt_, m.x, m.y, JsuiMouseButton(b[1]), 1, mods());
        if (!consumed && b[0] == MOUSE_BUTTON_LEFT) {
          dragging_ = true;  // the press is the game's: orbit while held
          hud_.clicks++;
        }
      }
      if (IsMouseButtonReleased(b[0])) {
        jsui_send_mouse_button(rt_, m.x, m.y, JsuiMouseButton(b[1]), 0, mods());
        if (b[0] == MOUSE_BUTTON_LEFT) dragging_ = false;
      }
    }
    Vector2 wheel = GetMouseWheelMoveV();
    if (wheel.x != 0 || wheel.y != 0) {
      // DOM: positive deltaY scrolls down, ~40 px per notch.
      if (!jsui_send_mouse_wheel(rt_, m.x, m.y, -wheel.x * 40.f, -wheel.y * 40.f)) {
        distance_ = std::clamp(distance_ * (wheel.y > 0 ? 0.9f : 1.1f), 6.f, 40.f);
        hud_.wheel++;
      }
    }
    if (m.x < 0 || m.y < 0 || m.x >= GetScreenWidth() || m.y >= GetScreenHeight()) {
      if (!left_) jsui_send_mouse_leave(rt_);
      left_ = true;
    } else {
      left_ = false;
    }

    // Keys: presses come from raylib's queue, repeats and releases from the
    // keys held down.
    for (int k = GetKeyPressed(); k != 0; k = GetKeyPressed()) {
      if (keysDown_.insert(k).second) sendKey(k, true, false);
    }
    for (auto it = keysDown_.begin(); it != keysDown_.end();) {
      int k = *it;
      if (IsKeyReleased(k) || !IsKeyDown(k)) {
        it = keysDown_.erase(it);
        sendKey(k, false, false);
      } else {
        if (IsKeyPressedRepeat(k)) sendKey(k, true, true);
        ++it;
      }
    }
    for (int c = GetCharPressed(); c != 0; c = GetCharPressed()) jsui_send_text(rt_, jsui_example::utf8(c).c_str());

    // Gamepads 0-3, in the W3C standard layout.
    orbitVelocity_ = 0;
    for (int pad = 0; pad < 4; pad++) {
      bool available = IsGamepadAvailable(pad);
      if (available != padConnected_[pad]) {
        padConnected_[pad] = available;
        jsui_set_gamepad_connected(rt_, pad, available, available ? GetGamepadName(pad) : nullptr);
      }
      if (!available) continue;
      for (int b = GAMEPAD_BUTTON_LEFT_FACE_UP; b <= GAMEPAD_BUTTON_RIGHT_THUMB; b++) {
        int sb = standardButton(b);
        if (sb < 0) continue;
        if (IsGamepadButtonPressed(pad, b)) {
          if (!jsui_send_gamepad_button(rt_, pad, sb, 1, 1.f) && sb == 9) {
            hud_.paused = !hud_.paused;  // Start, when the UI did not take it
            hud_.pad++;
          }
        }
        if (IsGamepadButtonReleased(pad, b)) jsui_send_gamepad_button(rt_, pad, sb, 0, 0.f);
      }
      for (int a = 0; a < 4; a++) {
        float v = GetGamepadAxisMovement(pad, a);
        if (std::fabs(v) < 0.12f) v = 0;  // dead zone
        if (v != axes_[pad][a]) {
          axes_[pad][a] = v;
          jsui_send_gamepad_axis(rt_, pad, a, v);
        }
      }
      // The right stick is not used by the UI's focus navigation: orbit.
      if (axes_[pad][2] != 0) {
        orbitVelocity_ += axes_[pad][2] * 1.8f;
        if (!padOrbiting_[pad]) hud_.pad++;
        padOrbiting_[pad] = true;
      } else {
        padOrbiting_[pad] = false;
      }
    }
  }

  void updateCursor() {
    std::string c = jsui_get_cursor(rt_);
    if (c == cursor_) return;
    cursor_ = c;
    if (c == "none") {
      HideCursor();
      return;
    }
    ShowCursor();
    int rc = MOUSE_CURSOR_DEFAULT;
    if (c == "pointer") rc = MOUSE_CURSOR_POINTING_HAND;
    else if (c == "text") rc = MOUSE_CURSOR_IBEAM;
    else if (c == "crosshair") rc = MOUSE_CURSOR_CROSSHAIR;
    else if (c == "not-allowed") rc = MOUSE_CURSOR_NOT_ALLOWED;
    else if (c == "ew-resize" || c == "col-resize") rc = MOUSE_CURSOR_RESIZE_EW;
    else if (c == "ns-resize" || c == "row-resize") rc = MOUSE_CURSOR_RESIZE_NS;
    else if (c == "move" || c == "grab" || c == "grabbing") rc = MOUSE_CURSOR_RESIZE_ALL;
    SetMouseCursor(rc);
  }

  Options opt_;
  JsuiRuntime* rt_ = nullptr;
  JsuiGpuContext* gpu_ = nullptr;
  GL gl_;
  RenderTexture2D ui_{};
  bool uiFresh_ = true;
  std::vector<uint8_t> pixels_, rect_;
  jsui_example::HudState hud_;
  jsui_example::FpsCounter fps_;
  jsui_example::SelfTest selftest_;
  std::vector<Unit> units_;
  size_t spawned_ = 0;
  float time_ = 0;
  Camera3D camera_{};
  float yaw_ = 0.8f, pitch_ = 0.55f, distance_ = 16.f, orbitVelocity_ = 0;
  Vector2 mouse_{-1, -1};
  bool dragging_ = false, left_ = false, quit_ = false;
  std::set<int> keysDown_;
  bool padConnected_[4] = {};
  bool padOrbiting_[4] = {};
  float axes_[4][4] = {};
  std::string cursor_;
};

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!jsui_example::parseOptions(argc, argv, &opt)) return 2;
  Game game(opt);
  return game.run();
}
