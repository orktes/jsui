// JSUI in SDL3: a 2D arena drawn with SDL_Renderer and the shared HUD
// (integrations/shared/ui/app.jsx) on top.
//
//   jsui-sdl3 [--cpu] [--dev] [--renderer metal|vulkan|opengl|direct3d12|software|...]
//             [bundle.js | http://127.0.0.1:5173/bundle.js]
//
// Rendering. Skia draws the UI on the GPU, straight into the SDL_Texture that
// SDL_Renderer composites over the arena with premultiplied alpha (gpu.h):
// Graphite on SDL's Metal device on macOS, Graphite on SDL's Vulkan device
// elsewhere, or Ganesh in SDL's OpenGL context when the library has no
// Graphite backend. The example asks SDL for that render driver unless
// --renderer names one. On other renderers (software, Direct3D, SDL's GPU
// renderer), with --cpu, or if the GPU path fails, the UI is rasterised on
// the CPU into a buffer kept between frames (jsui_render_rgba_incremental)
// and only the damaged rectangle is uploaded to a streaming texture. Either
// way the UI is repainted only when something in it changed.
//
// Input pass-through. Events go to the UI first; jsui_send_* returns whether
// the UI consumed one. The rest drives the game: drag empty space to pan,
// wheel to zoom, Space spawns a unit, Esc / Start pauses, the right stick
// pans.
//
// Per-frame data. Every frame the game writes each unit's screen position
// into the `entities` frame buffer (x, y, visible, hue; logical px), which
// moves the nameplates without JS; fps, unit count and the pass-through
// counters go to the `game` shared state.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "example_host.h"
#include "gpu.h"
#include "jsui/jsui.h"

namespace {

using jsui_example::Options;

// ------------------------------------------------- SDL -> DOM key names

const char* domCode(SDL_Scancode sc) {
  static char buf[16];
  if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) {
    std::snprintf(buf, sizeof buf, "Key%c", 'A' + (sc - SDL_SCANCODE_A));
    return buf;
  }
  if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) {
    std::snprintf(buf, sizeof buf, "Digit%c", '1' + (sc - SDL_SCANCODE_1));
    return buf;
  }
  if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) {
    std::snprintf(buf, sizeof buf, "F%d", 1 + (sc - SDL_SCANCODE_F1));
    return buf;
  }
  if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) {
    std::snprintf(buf, sizeof buf, "Numpad%c", '1' + (sc - SDL_SCANCODE_KP_1));
    return buf;
  }
  switch (sc) {
    case SDL_SCANCODE_0: return "Digit0";
    case SDL_SCANCODE_RETURN: return "Enter";
    case SDL_SCANCODE_ESCAPE: return "Escape";
    case SDL_SCANCODE_BACKSPACE: return "Backspace";
    case SDL_SCANCODE_TAB: return "Tab";
    case SDL_SCANCODE_SPACE: return "Space";
    case SDL_SCANCODE_MINUS: return "Minus";
    case SDL_SCANCODE_EQUALS: return "Equal";
    case SDL_SCANCODE_LEFTBRACKET: return "BracketLeft";
    case SDL_SCANCODE_RIGHTBRACKET: return "BracketRight";
    case SDL_SCANCODE_BACKSLASH: return "Backslash";
    case SDL_SCANCODE_SEMICOLON: return "Semicolon";
    case SDL_SCANCODE_APOSTROPHE: return "Quote";
    case SDL_SCANCODE_GRAVE: return "Backquote";
    case SDL_SCANCODE_COMMA: return "Comma";
    case SDL_SCANCODE_PERIOD: return "Period";
    case SDL_SCANCODE_SLASH: return "Slash";
    case SDL_SCANCODE_CAPSLOCK: return "CapsLock";
    case SDL_SCANCODE_INSERT: return "Insert";
    case SDL_SCANCODE_HOME: return "Home";
    case SDL_SCANCODE_PAGEUP: return "PageUp";
    case SDL_SCANCODE_DELETE: return "Delete";
    case SDL_SCANCODE_END: return "End";
    case SDL_SCANCODE_PAGEDOWN: return "PageDown";
    case SDL_SCANCODE_RIGHT: return "ArrowRight";
    case SDL_SCANCODE_LEFT: return "ArrowLeft";
    case SDL_SCANCODE_DOWN: return "ArrowDown";
    case SDL_SCANCODE_UP: return "ArrowUp";
    case SDL_SCANCODE_KP_ENTER: return "NumpadEnter";
    case SDL_SCANCODE_KP_0: return "Numpad0";
    case SDL_SCANCODE_LCTRL: return "ControlLeft";
    case SDL_SCANCODE_LSHIFT: return "ShiftLeft";
    case SDL_SCANCODE_LALT: return "AltLeft";
    case SDL_SCANCODE_LGUI: return "MetaLeft";
    case SDL_SCANCODE_RCTRL: return "ControlRight";
    case SDL_SCANCODE_RSHIFT: return "ShiftRight";
    case SDL_SCANCODE_RALT: return "AltRight";
    case SDL_SCANCODE_RGUI: return "MetaRight";
    case SDL_SCANCODE_AC_BACK: return "BrowserBack";
    default: return nullptr;
  }
}

// DOM KeyboardEvent.key (US layout for shifted symbols).
std::string domKey(SDL_Keycode key, SDL_Keymod mods) {
  bool shift = (mods & SDL_KMOD_SHIFT) != 0;
  bool caps = (mods & SDL_KMOD_CAPS) != 0;
  if (key >= 'a' && key <= 'z') return std::string(1, char((shift != caps) ? std::toupper(int(key)) : int(key)));
  if (key >= 32 && key < 127) {
    if (shift) {
      static const char* from = "1234567890-=[]\\;',./`";
      static const char* to = "!@#$%^&*()_+{}|:\"<>?~";
      if (const char* p = std::strchr(from, int(key))) return std::string(1, to[p - from]);
    }
    return std::string(1, char(key));
  }
  if (key >= SDLK_F1 && key <= SDLK_F12) return "F" + std::to_string(1 + int(key - SDLK_F1));
  switch (key) {
    case SDLK_RETURN: case SDLK_KP_ENTER: return "Enter";
    case SDLK_ESCAPE: return "Escape";
    case SDLK_BACKSPACE: return "Backspace";
    case SDLK_TAB: return "Tab";
    case SDLK_DELETE: return "Delete";
    case SDLK_INSERT: return "Insert";
    case SDLK_HOME: return "Home";
    case SDLK_END: return "End";
    case SDLK_PAGEUP: return "PageUp";
    case SDLK_PAGEDOWN: return "PageDown";
    case SDLK_LEFT: return "ArrowLeft";
    case SDLK_RIGHT: return "ArrowRight";
    case SDLK_UP: return "ArrowUp";
    case SDLK_DOWN: return "ArrowDown";
    case SDLK_LSHIFT: case SDLK_RSHIFT: return "Shift";
    case SDLK_LCTRL: case SDLK_RCTRL: return "Control";
    case SDLK_LALT: case SDLK_RALT: return "Alt";
    case SDLK_LGUI: case SDLK_RGUI: return "Meta";
    case SDLK_CAPSLOCK: return "CapsLock";
    case SDLK_AC_BACK: return "BrowserBack";
    default: return "Unidentified";
  }
}

int jsuiMods(SDL_Keymod m) {
  return ((m & SDL_KMOD_SHIFT) ? JSUI_MOD_SHIFT : 0) | ((m & SDL_KMOD_CTRL) ? JSUI_MOD_CTRL : 0) |
         ((m & SDL_KMOD_ALT) ? JSUI_MOD_ALT : 0) | ((m & SDL_KMOD_GUI) ? JSUI_MOD_META : 0);
}

// SDL gamepad button -> W3C standard gamepad index.
int standardButton(int b) {
  switch (b) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return 0;
    case SDL_GAMEPAD_BUTTON_EAST: return 1;
    case SDL_GAMEPAD_BUTTON_WEST: return 2;
    case SDL_GAMEPAD_BUTTON_NORTH: return 3;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return 4;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return 5;
    case SDL_GAMEPAD_BUTTON_BACK: return 8;
    case SDL_GAMEPAD_BUTTON_START: return 9;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return 10;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return 11;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return 12;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return 13;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return 14;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return 15;
    case SDL_GAMEPAD_BUTTON_GUIDE: return 16;
    default: return -1;
  }
}

SDL_SystemCursor systemCursor(const std::string& c) {
  if (c == "pointer") return SDL_SYSTEM_CURSOR_POINTER;
  if (c == "text") return SDL_SYSTEM_CURSOR_TEXT;
  if (c == "crosshair") return SDL_SYSTEM_CURSOR_CROSSHAIR;
  if (c == "wait") return SDL_SYSTEM_CURSOR_WAIT;
  if (c == "move" || c == "grab" || c == "grabbing") return SDL_SYSTEM_CURSOR_MOVE;
  if (c == "not-allowed") return SDL_SYSTEM_CURSOR_NOT_ALLOWED;
  if (c == "ew-resize" || c == "col-resize") return SDL_SYSTEM_CURSOR_EW_RESIZE;
  if (c == "ns-resize" || c == "row-resize") return SDL_SYSTEM_CURSOR_NS_RESIZE;
  return SDL_SYSTEM_CURSOR_DEFAULT;
}

// ------------------------------------------------------------- the game

struct Unit {
  float cx, cy, rx, ry, speed, phase, size;
  SDL_FColor color;
};

const SDL_FColor kHues[] = {{0.95f, 0.76f, 0.36f, 1}, {0.31f, 0.82f, 1.f, 1},  {0.70f, 0.42f, 1.f, 1},
                            {0.29f, 0.87f, 0.50f, 1}, {1.f, 0.60f, 0.24f, 1},  {0.31f, 0.55f, 1.f, 1},
                            {1.f, 0.30f, 0.42f, 1}};

struct Pad {
  SDL_Gamepad* gamepad;
  int index;
  bool trigger[2] = {false, false};
  float axes[4] = {0, 0, 0, 0};
};

class Game {
 public:
  explicit Game(const Options& o, std::string renderer) : opt_(o), rendererName_(std::move(renderer)) {}

  ~Game() {
    if (rt_) jsui_destroy(rt_);
    for (auto& [id, p] : pads_) SDL_CloseGamepad(p.gamepad);
    for (auto& [k, c] : cursors_) SDL_DestroyCursor(c);
    gpu_.reset();
    if (ui_) SDL_DestroyTexture(ui_);
    if (renderer_) sdl3_gpu::destroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    SDL_Quit();
  }

  int run() {
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");  // touch is forwarded as touch
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) {
      std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
      return 1;
    }
    window_ = SDL_CreateWindow("JSUI + SDL3", opt_.width, opt_.height,
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window_) {
      std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
      return 1;
    }
    // The GPU path's render driver, unless --renderer (or --cpu) says otherwise.
    const char* preferred = sdl3_gpu::preferredDriver();
    if (rendererName_.empty() && !opt_.cpu && preferred) {
      renderer_ = sdl3_gpu::createRenderer(window_, preferred);
      if (!renderer_) std::fprintf(stderr, "warning: SDL's %s renderer failed (%s); using SDL's default\n", preferred, SDL_GetError());
    }
    if (!renderer_) renderer_ = sdl3_gpu::createRenderer(window_, rendererName_.empty() ? nullptr : rendererName_.c_str());
    if (!renderer_) {
      std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
      return 1;
    }
    bool deterministic = opt_.frames >= 0 || opt_.selftest;
    SDL_SetRenderVSync(renderer_, deterministic ? 0 : 1);
    const char* backend = SDL_GetRendererName(renderer_);
    std::printf("SDL renderer: %s\n", backend ? backend : "?");
    hud_.engine = "SDL " + std::to_string(SDL_MAJOR_VERSION) + "." + std::to_string(SDL_MINOR_VERSION) + "." +
                  std::to_string(SDL_MICRO_VERSION);
    backend_ = backend ? backend : "?";
    if (!opt_.cpu) {
      gpu_ = sdl3_gpu::createTarget(renderer_);
      if (gpu_) std::printf("GPU: %s (%s)\n", gpu_->describe().c_str(), gpu_->device().c_str());
      else std::fprintf(stderr, "note: no GPU path for SDL's %s renderer; the UI is rasterised on the CPU\n", backend_.c_str());
    }
    setRendererName();

    JsuiConfig cfg;
    jsui_config_init(&cfg);
    cfg.width = opt_.width;
    cfg.height = opt_.height;
    cfg.dev_mode = opt_.dev;
    cfg.message_user = this;
    cfg.on_message = [](void* u, const char* name, const char* json) { static_cast<Game*>(u)->onMessage(name, json); };
    rt_ = jsui_create(&cfg);
    jsui_set_clipboard_callbacks(
        rt_,
        [](void* u) -> const char* {
          auto* self = static_cast<Game*>(u);
          char* t = SDL_GetClipboardText();
          self->clipboard_ = t ? t : "";
          SDL_free(t);
          return self->clipboard_.c_str();
        },
        [](void*, const char* text) { SDL_SetClipboardText(text); }, this);
    for (int i = 0; i < 10; i++) spawn();
    resize();
    hud_.publish(rt_);
    if (jsui_load_bundle_url(rt_, opt_.bundle.c_str()) != 0)
      std::fprintf(stderr, "failed to load %s: %s\n", opt_.bundle.c_str(), jsui_last_error(rt_));
    SDL_StartTextInput(window_);

    Uint64 start = SDL_GetTicksNS();
    long frame = 0;
    int result = 0;
    while (!quit_) {
      SDL_Event e;
      while (SDL_PollEvent(&e)) {
        if (!opt_.selftest || e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) handle(e);
      }
      double now = double(SDL_GetTicksNS() - start) / 1e6;
      float dt = deterministic ? 1.f / 60.f : std::min(0.1f, float(now - last_) / 1000.f);
      last_ = now;
      update(dt);
      publishFrameData();
      fps_.frame(now);
      hud_.fps = fps_.fps;
      hud_.entities = int(units_.size());
      hud_.publish(rt_);

      jsui_tick(rt_, now);
      if (opt_.selftest) {
        int r = selftest_.step(rt_, float(logicalW_), float(logicalH_), int(units_.size()));
        if (r != 0) {
          result = r > 0 ? 0 : 1;
          quit_ = true;
        }
      }
      updateCursor();
      paintUi();

      SDL_SetRenderScale(renderer_, ratio_, ratio_);  // draw in logical px
      drawScene();
      SDL_FRect dst{0, 0, float(logicalW_), float(logicalH_)};
      SDL_RenderTexture(renderer_, gpu_ ? gpu_->texture() : ui_, nullptr, &dst);
      if (gpu_) gpu_->beforePresent();
      frame++;
      bool lastFrame = quit_ || (opt_.frames >= 0 && frame >= opt_.frames);
      if (lastFrame && !opt_.screenshot.empty()) screenshot(opt_.screenshot);
      SDL_RenderPresent(renderer_);
      if (opt_.frames >= 0 && frame >= opt_.frames) break;
    }
    const char* err = jsui_last_error(rt_);
    if (err) std::fprintf(stderr, "JS error: %s\n", err);
    return result ? result : (err ? 1 : 0);
  }

 private:
  // Window size -> runtime viewport and a UI texture of the output's size.
  void resize() {
    int lw = 0, lh = 0, pw = 0, ph = 0;
    SDL_GetWindowSize(window_, &lw, &lh);
    SDL_GetRenderOutputSize(renderer_, &pw, &ph);
    if (lw <= 0 || lh <= 0 || pw <= 0 || ph <= 0) return;
    logicalW_ = lw;
    logicalH_ = lh;
    ratio_ = float(pw) / float(lw);
    jsui_set_viewport(rt_, lw, lh, ratio_);
    if ((ui_ || gpu_) && pw == pixelW_ && ph == pixelH_) return;
    pixelW_ = pw;
    pixelH_ = ph;
    createUiTexture();
  }

  void createUiTexture() {
    uiFresh_ = true;
    if (gpu_) {
      if (gpu_->resize(pixelW_, pixelH_)) return;
      fallBackToCpu("could not create the GPU path's texture");
      return;
    }
    pixels_.assign(size_t(pixelW_) * pixelH_ * 4, 0);
    if (ui_) SDL_DestroyTexture(ui_);
    // RGBA bytes in memory, premultiplied, as the runtime paints them.
    ui_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, pixelW_, pixelH_);
    SDL_SetTextureBlendMode(ui_, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
    SDL_SetTextureScaleMode(ui_, SDL_SCALEMODE_LINEAR);
  }

  void fallBackToCpu(const char* why) {
    std::fprintf(stderr, "warning: %s; falling back to the CPU renderer\n", why);
    gpu_.reset();
    setRendererName();
    createUiTexture();
  }

  void setRendererName() {
    hud_.renderer = "SDL_Renderer (" + backend_ + ") · " +
                    (gpu_ ? gpu_->describe() : std::string("CPU raster · dirty-rect upload"));
  }

  void paintUi() {
    if (gpu_) {
      if (gpu_->paint(rt_, uiFresh_)) uiFresh_ = false;
      else fallBackToCpu("the GPU render failed");
      if (gpu_) return;
    }
    if (!ui_ || (!jsui_needs_redraw(rt_) && !uiFresh_)) return;
    size_t row = size_t(pixelW_) * 4;
    JsuiRect dirty;
    if (uiFresh_) {
      jsui_render_rgba(rt_, pixels_.data(), pixelW_, pixelH_, row);
      dirty = {0, 0, pixelW_, pixelH_};
      uiFresh_ = false;
    } else if (!jsui_render_rgba_incremental(rt_, pixels_.data(), pixelW_, pixelH_, row, &dirty)) {
      return;
    }
    if (dirty.width <= 0 || dirty.height <= 0) return;
    SDL_Rect r{dirty.x, dirty.y, dirty.width, dirty.height};
    SDL_UpdateTexture(ui_, &r, pixels_.data() + size_t(dirty.y) * row + size_t(dirty.x) * 4, int(row));
  }

  void screenshot(const std::string& path) {
    SDL_Surface* s = SDL_RenderReadPixels(renderer_, nullptr);
    if (!s || !SDL_SavePNG(s, path.c_str())) std::fprintf(stderr, "screenshot failed: %s\n", SDL_GetError());
    if (s) SDL_DestroySurface(s);
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

  float rnd(float a, float b) { return a + (b - a) * SDL_randf(); }

  void spawn() {
    if (units_.size() >= 48) units_.erase(units_.begin());
    Unit u;
    u.cx = rnd(-120, 120);
    u.cy = rnd(-80, 80);
    u.rx = rnd(120, 420);
    u.ry = rnd(80, 260);
    u.speed = rnd(0.25f, 0.8f) * (SDL_rand(2) ? 1.f : -1.f);
    u.phase = rnd(0, 6.283f);
    u.size = rnd(12, 20);
    u.color = kHues[spawned_++ % 7];
    units_.push_back(u);
  }

  // World -> logical screen px.
  SDL_FPoint toScreen(float x, float y) const {
    return {(x - camX_) * zoom_ + logicalW_ * 0.5f, (y - camY_) * zoom_ + logicalH_ * 0.5f};
  }
  SDL_FPoint unitPos(const Unit& u) const {
    float a = u.phase + time_ * u.speed;
    return {u.cx + std::cos(a) * u.rx, u.cy + std::sin(a * 1.3f) * u.ry};
  }

  void update(float dt) {
    if (!hud_.paused) time_ += dt;
    camX_ += panVelocity_[0] * dt * 600.f / zoom_;
    camY_ += panVelocity_[1] * dt * 600.f / zoom_;
  }

  void drawScene() {
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, 9, 12, 22, 255);
    SDL_RenderClear(renderer_);
    // Grid, 80 world units apart.
    SDL_SetRenderDrawColor(renderer_, 148, 170, 255, 26);
    float step = 80.f;
    float x0 = camX_ - logicalW_ * 0.5f / zoom_, x1 = camX_ + logicalW_ * 0.5f / zoom_;
    float y0 = camY_ - logicalH_ * 0.5f / zoom_, y1 = camY_ + logicalH_ * 0.5f / zoom_;
    for (float x = std::floor(x0 / step) * step; x <= x1; x += step) {
      SDL_FPoint a = toScreen(x, y0), b = toScreen(x, y1);
      SDL_RenderLine(renderer_, a.x, a.y, b.x, b.y);
    }
    for (float y = std::floor(y0 / step) * step; y <= y1; y += step) {
      SDL_FPoint a = toScreen(x0, y), b = toScreen(x1, y);
      SDL_RenderLine(renderer_, a.x, a.y, b.x, b.y);
    }
    // The core in the middle, then the units: glowing diamonds.
    drawDiamond(toScreen(0, 0), 46 * zoom_, time_ * 0.3f, {0.1f, 0.14f, 0.25f, 1}, {0.31f, 0.82f, 1.f, 0.35f});
    for (auto& u : units_) {
      SDL_FPoint p = toScreen(unitPos(u).x, unitPos(u).y);
      SDL_FColor glow = u.color;
      glow.a = 0.22f;
      drawDiamond(p, u.size * zoom_, time_ * u.speed * 3.f, u.color, glow);
    }
  }

  void drawDiamond(SDL_FPoint c, float r, float angle, SDL_FColor fill, SDL_FColor glow) {
    auto quad = [&](float rr, SDL_FColor inner, SDL_FColor outer) {
      SDL_Vertex v[5];
      v[0] = {c, inner, {0, 0}};
      for (int i = 0; i < 4; i++) {
        float a = angle + i * 1.5707964f;
        v[1 + i] = {{c.x + std::cos(a) * rr, c.y + std::sin(a) * rr}, outer, {0, 0}};
      }
      int idx[] = {0, 1, 2, 0, 2, 3, 0, 3, 4, 0, 4, 1};
      SDL_RenderGeometry(renderer_, nullptr, v, 5, idx, 12);
    };
    SDL_FColor clear = glow;
    clear.a = 0;
    quad(r * 2.2f, glow, clear);
    SDL_FColor edge = fill;
    edge.r *= 0.6f;
    edge.g *= 0.6f;
    edge.b *= 0.6f;
    quad(r, fill, edge);
  }

  // Unit screen positions for the nameplates: x, y, visible, hue (logical px).
  void publishFrameData() {
    float* b = static_cast<float*>(jsui_map_buffer(rt_, "entities", JSUI_BUFFER_F32, units_.size() * 4));
    if (!b) return;
    for (size_t i = 0; i < units_.size(); i++) {
      SDL_FPoint w = unitPos(units_[i]);
      SDL_FPoint s = toScreen(w.x, w.y - units_[i].size * 0.4f);
      bool visible = s.x > -60 && s.y > -60 && s.x < logicalW_ + 60 && s.y < logicalH_ + 60;
      b[i * 4 + 0] = std::round(s.x);
      b[i * 4 + 1] = std::round(s.y - units_[i].size * zoom_ * 0.6f);
      b[i * 4 + 2] = visible ? 1.f : 0.f;
      b[i * 4 + 3] = float(i % 7);
    }
    jsui_commit_buffer(rt_, "entities");
  }

  int padIndexFor(SDL_JoystickID id) {
    auto it = pads_.find(id);
    return it == pads_.end() ? -1 : it->second.index;
  }

  void handle(const SDL_Event& e) {
    switch (e.type) {
      case SDL_EVENT_QUIT: quit_ = true; break;
      case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
      case SDL_EVENT_WINDOW_RESIZED: resize(); break;
      case SDL_EVENT_WINDOW_MOUSE_LEAVE: jsui_send_mouse_leave(rt_); break;
      case SDL_EVENT_MOUSE_MOTION:
        if (e.motion.which == SDL_TOUCH_MOUSEID) break;
        jsui_send_mouse_move(rt_, e.motion.x, e.motion.y);
        if (dragging_) {
          camX_ -= e.motion.xrel / zoom_;
          camY_ -= e.motion.yrel / zoom_;
        }
        break;
      case SDL_EVENT_MOUSE_BUTTON_DOWN:
      case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (e.button.which == SDL_TOUCH_MOUSEID || e.button.button > SDL_BUTTON_RIGHT) break;
        JsuiMouseButton b = e.button.button == SDL_BUTTON_RIGHT    ? JSUI_MOUSE_RIGHT
                            : e.button.button == SDL_BUTTON_MIDDLE ? JSUI_MOUSE_MIDDLE
                                                                   : JSUI_MOUSE_LEFT;
        bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        int consumed = jsui_send_mouse_button(rt_, e.button.x, e.button.y, b, down, jsuiMods(SDL_GetModState()));
        if (b == JSUI_MOUSE_LEFT) {
          if (down && !consumed) {
            dragging_ = true;  // the press is the game's: pan while held
            hud_.clicks++;
          } else if (!down) {
            dragging_ = false;
          }
        }
        break;
      }
      case SDL_EVENT_MOUSE_WHEEL: {
        float dx = e.wheel.x, dy = e.wheel.y;
        if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
          dx = -dx;
          dy = -dy;
        }
        // DOM: positive deltaY scrolls down, ~40 px per notch.
        if (!jsui_send_mouse_wheel(rt_, e.wheel.mouse_x, e.wheel.mouse_y, dx * 40.f, -dy * 40.f)) {
          zoom_ = std::clamp(zoom_ * (dy > 0 ? 1.1f : 0.9f), 0.35f, 3.f);
          hud_.wheel++;
        }
        break;
      }
      case SDL_EVENT_FINGER_DOWN:
      case SDL_EVENT_FINGER_MOTION:
      case SDL_EVENT_FINGER_UP: {
        int phase = e.type == SDL_EVENT_FINGER_DOWN ? 0 : e.type == SDL_EVENT_FINGER_MOTION ? 1 : 2;
        jsui_send_touch(rt_, int(e.tfinger.fingerID & 0x7fffffff), phase, e.tfinger.x * logicalW_,
                        e.tfinger.y * logicalH_);
        break;
      }
      case SDL_EVENT_KEY_DOWN:
      case SDL_EVENT_KEY_UP: {
        bool down = e.type == SDL_EVENT_KEY_DOWN;
        std::string key = domKey(e.key.key, e.key.mod);
        int consumed = jsui_send_key(rt_, key.c_str(), domCode(e.key.scancode), down, e.key.repeat, jsuiMods(e.key.mod));
        if (consumed || !down || e.key.repeat) break;
        // Not taken by the UI: the game's own bindings.
        if (e.key.key == SDLK_SPACE) spawn();
        else if (e.key.key == SDLK_ESCAPE) hud_.paused = !hud_.paused;
        else break;
        hud_.keys++;
        break;
      }
      case SDL_EVENT_TEXT_INPUT: jsui_send_text(rt_, e.text.text); break;
      case SDL_EVENT_GAMEPAD_ADDED: {
        if (pads_.count(e.gdevice.which)) break;
        SDL_Gamepad* gp = SDL_OpenGamepad(e.gdevice.which);
        if (!gp) break;
        int index = 0;
        for (bool used = true; used;) {
          used = false;
          for (auto& [k, p] : pads_)
            if (p.index == index) used = true;
          if (used) index++;
        }
        pads_.emplace(e.gdevice.which, Pad{gp, index});
        const char* name = SDL_GetGamepadName(gp);
        jsui_set_gamepad_connected(rt_, index, 1, name ? name : "SDL gamepad");
        break;
      }
      case SDL_EVENT_GAMEPAD_REMOVED: {
        auto it = pads_.find(e.gdevice.which);
        if (it == pads_.end()) break;
        jsui_set_gamepad_connected(rt_, it->second.index, 0, nullptr);
        SDL_CloseGamepad(it->second.gamepad);
        pads_.erase(it);
        break;
      }
      case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
      case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        int pad = padIndexFor(e.gbutton.which);
        int b = standardButton(e.gbutton.button);
        bool down = e.gbutton.down;
        if (pad < 0 || b < 0) break;
        int consumed = jsui_send_gamepad_button(rt_, pad, b, down, down ? 1.f : 0.f);
        if (down && !consumed && b == 9) {  // Start, when the UI did not take it
          hud_.paused = !hud_.paused;
          hud_.pad++;
        }
        break;
      }
      case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        auto it = pads_.find(e.gaxis.which);
        if (it == pads_.end()) break;
        Pad& p = it->second;
        float v = std::max(-1.f, float(e.gaxis.value) / 32767.f);
        if (e.gaxis.axis <= SDL_GAMEPAD_AXIS_RIGHTY) {
          if (std::fabs(v) < 0.12f) v = 0;  // dead zone
          int a = e.gaxis.axis;             // LEFTX, LEFTY, RIGHTX, RIGHTY = standard axes 0-3
          if (v == p.axes[a]) break;
          p.axes[a] = v;
          jsui_send_gamepad_axis(rt_, p.index, a, v);
          // The right stick is not used by the UI's focus navigation: pan.
          if (a >= 2) {
            bool was = panVelocity_[0] != 0 || panVelocity_[1] != 0;
            panVelocity_[a - 2] = v;
            if (!was && v != 0) hud_.pad++;
          }
          break;
        }
        // Triggers are analog buttons 6 / 7 of the standard mapping.
        int t = e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? 0 : 1;
        bool now = p.trigger[t] ? v > 0.3f : v > 0.5f;
        if (now != p.trigger[t]) {
          p.trigger[t] = now;
          jsui_send_gamepad_button(rt_, p.index, 6 + t, now, std::max(0.f, v));
        }
        break;
      }
      default: break;
    }
  }

  void updateCursor() {
    std::string c = jsui_get_cursor(rt_);
    if (c == cursor_) return;
    cursor_ = c;
    if (c == "none") {
      SDL_HideCursor();
      return;
    }
    SDL_ShowCursor();
    SDL_SystemCursor sc = systemCursor(c);
    auto it = cursors_.find(int(sc));
    if (it == cursors_.end()) it = cursors_.emplace(int(sc), SDL_CreateSystemCursor(sc)).first;
    if (it->second) SDL_SetCursor(it->second);
  }

  Options opt_;
  std::string rendererName_;
  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  SDL_Texture* ui_ = nullptr;  // the CPU path's texture
  std::unique_ptr<sdl3_gpu::UiTarget> gpu_;
  std::string backend_;
  JsuiRuntime* rt_ = nullptr;
  std::vector<uint8_t> pixels_;
  bool uiFresh_ = true;
  int pixelW_ = 0, pixelH_ = 0, logicalW_ = 0, logicalH_ = 0;
  float ratio_ = 1;
  std::string clipboard_, cursor_;
  jsui_example::HudState hud_;
  jsui_example::FpsCounter fps_;
  jsui_example::SelfTest selftest_;
  std::vector<Unit> units_;
  size_t spawned_ = 0;
  float time_ = 0;
  double last_ = 0;
  float camX_ = 0, camY_ = 0, zoom_ = 1;
  float panVelocity_[2] = {0, 0};
  bool dragging_ = false, quit_ = false;
  std::map<SDL_JoystickID, Pad> pads_;
  std::map<int, SDL_Cursor*> cursors_;
};

}  // namespace

int main(int argc, char** argv) {
  // --renderer is this example's own option; the rest are shared.
  std::string renderer;
  std::vector<char*> args;
  for (int i = 0; i < argc; i++) {
    if (std::strcmp(argv[i], "--renderer") == 0 && i + 1 < argc) renderer = argv[++i];
    else args.push_back(argv[i]);
  }
  Options opt;
  if (!jsui_example::parseOptions(int(args.size()), args.data(), &opt)) return 2;
  Game game(opt, renderer);
  return game.run();
}
