// jsui-viewer: run a JSUI bundle in a window.
//
//   jsui-viewer [--dev] [--width W] [--height H] <bundle path or URL>
//               [--frames N] [--screenshot out.png]
//
// --dev polls the dev server (`jsui dev`) and reloads on change; JS errors
// are shown in an overlay (Esc dismisses). F5 reloads. Mouse, wheel, touch,
// keyboard, text input and game controllers are forwarded to the runtime.
// --frames / --screenshot run a fixed number of frames and exit (CI, e.g.
// with SDL_VIDEODRIVER=dummy).
#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "jsui/jsui.h"
#include "keymap.h"

namespace {

struct Options {
  std::string bundle;
  bool dev = false;
  int width = 1280, height = 720;
  long frames = -1;
  std::string screenshot;
};

void usage() {
  std::printf(
      "usage: jsui-viewer [--dev] [--width W] [--height H] [--frames N] [--screenshot out.png] <bundle.js|url>\n"
      "  --dev        poll the dev server and reload on change, show errors on screen\n"
      "  F5 reloads the bundle, Esc dismisses the error overlay (dev mode)\n");
}

bool parseArgs(int argc, char** argv, Options* o) {
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    if (a == "--dev") o->dev = true;
    else if (a == "--width" || a == "--height" || a == "--frames" || a == "--screenshot") {
      const char* v = next();
      if (!v) {
        std::fprintf(stderr, "missing value for %s\n", a.c_str());
        return false;
      }
      if (a == "--width") o->width = std::atoi(v);
      else if (a == "--height") o->height = std::atoi(v);
      else if (a == "--frames") o->frames = std::atol(v);
      else o->screenshot = v;
    } else if (a == "-h" || a == "--help") {
      usage();
      std::exit(0);
    } else if (!a.empty() && a[0] == '-') {
      std::fprintf(stderr, "unknown option %s\n", a.c_str());
      return false;
    } else {
      o->bundle = a;
    }
  }
  if (o->bundle.empty()) {
    usage();
    return false;
  }
  if (o->width <= 0 || o->height <= 0) {
    std::fprintf(stderr, "bad window size\n");
    return false;
  }
  if (!o->screenshot.empty() && o->frames < 0) o->frames = 60;
  return true;
}

struct Pad {
  SDL_GameController* controller;
  int index;
  bool trigger[2] = {false, false};
};

class Viewer {
 public:
  explicit Viewer(const Options& o) : opt_(o) {}
  ~Viewer() {
    if (rt_) jsui_destroy(rt_);
    for (auto& [id, p] : pads_) SDL_GameControllerClose(p.controller);
    for (auto& [k, c] : cursors_) SDL_FreeCursor(c);
    if (texture_) SDL_DestroyTexture(texture_);
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    SDL_Quit();
  }

  int run() {
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");  // touch is forwarded as touch
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
      std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
      return 1;
    }
    std::string title = "JSUI - " + opt_.bundle;
    window_ = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, opt_.width, opt_.height,
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window_) {
      std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
      return 1;
    }
    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer_) renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer_) {
      std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
      return 1;
    }
    // The runtime produces premultiplied RGBA.
    premul_ = SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD,
                                         SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD);

    JsuiConfig cfg;
    jsui_config_init(&cfg);
    cfg.width = opt_.width;
    cfg.height = opt_.height;
    cfg.dev_mode = opt_.dev ? 1 : 0;
    cfg.on_message = [](void*, const char* name, const char* json) {
      std::printf("[message] %s %s\n", name, json);
    };
    rt_ = jsui_create(&cfg);
    updateViewport();
    jsui_load_bundle_url(rt_, opt_.bundle.c_str());
    SDL_StartTextInput();

    start_ = SDL_GetPerformanceCounter();
    long frame = 0;
    while (running_) {
      SDL_Event e;
      while (SDL_PollEvent(&e)) handle(e);
      double now = double(SDL_GetPerformanceCounter() - start_) * 1000.0 / double(SDL_GetPerformanceFrequency());
      jsui_tick(rt_, now);
      updateCursor();
      bool drew = false;
      if (jsui_needs_redraw(rt_) || texture_ == nullptr) {
        present();
        drew = true;
      }
      if (opt_.frames >= 0 && ++frame >= opt_.frames) break;
      // Without vsync (software / dummy driver) or when idle, don't spin.
      if (!drew) SDL_WaitEventTimeout(nullptr, 4);
    }
    if (!opt_.screenshot.empty()) {
      void* png = nullptr;
      size_t size = 0;
      if (jsui_render_png(rt_, &png, &size) == 0) {
        if (FILE* f = std::fopen(opt_.screenshot.c_str(), "wb")) {
          std::fwrite(png, 1, size, f);
          std::fclose(f);
        }
        jsui_free(png);
      }
    }
    const char* err = jsui_last_error(rt_);
    return err ? 1 : 0;
  }

 private:
  void updateViewport() {
    int lw = 0, lh = 0, pw = 0, ph = 0;
    SDL_GetWindowSize(window_, &lw, &lh);
    SDL_GetRendererOutputSize(renderer_, &pw, &ph);
    if (lw <= 0 || lh <= 0) return;
    ratio_ = pw > 0 ? float(pw) / float(lw) : 1.f;
    logicalW_ = lw;
    logicalH_ = lh;
    jsui_set_viewport(rt_, lw, lh, ratio_);
    int dw = int(std::lround(lw * ratio_)), dh = int(std::lround(lh * ratio_));
    if (dw != pixelW_ || dh != pixelH_) {
      pixelW_ = dw;
      pixelH_ = dh;
      pixels_.assign(size_t(dw) * dh * 4, 0);
      if (texture_) SDL_DestroyTexture(texture_);
      texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, dw, dh);
      if (texture_) SDL_SetTextureBlendMode(texture_, premul_);
    }
  }

  void present() {
    if (!texture_ || pixels_.empty()) return;
    jsui_render_rgba(rt_, pixels_.data(), pixelW_, pixelH_, size_t(pixelW_) * 4);
    SDL_UpdateTexture(texture_, nullptr, pixels_.data(), pixelW_ * 4);
    SDL_SetRenderDrawColor(renderer_, 24, 24, 28, 255);
    SDL_RenderClear(renderer_);
    SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
    SDL_RenderPresent(renderer_);
  }

  void updateCursor() {
    std::string c = jsui_get_cursor(rt_);
    if (c == cursorName_) return;
    cursorName_ = c;
    SDL_SystemCursor sc;
    if (!viewer::systemCursor(c, &sc)) {
      SDL_ShowCursor(SDL_DISABLE);
      return;
    }
    SDL_ShowCursor(SDL_ENABLE);
    auto it = cursors_.find(int(sc));
    if (it == cursors_.end()) it = cursors_.emplace(int(sc), SDL_CreateSystemCursor(sc)).first;
    if (it->second) SDL_SetCursor(it->second);
  }

  int padIndexFor(SDL_JoystickID id) {
    auto it = pads_.find(id);
    return it == pads_.end() ? -1 : it->second.index;
  }

  void handle(const SDL_Event& e) {
    switch (e.type) {
      case SDL_QUIT: running_ = false; break;
      case SDL_WINDOWEVENT:
        if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED)
          updateViewport();
        else if (e.window.event == SDL_WINDOWEVENT_LEAVE)
          jsui_send_mouse_leave(rt_);
        else if (e.window.event == SDL_WINDOWEVENT_EXPOSED)
          present();
        break;
      case SDL_MOUSEMOTION:
        if (e.motion.which != SDL_TOUCH_MOUSEID) jsui_send_mouse_move(rt_, float(e.motion.x), float(e.motion.y));
        break;
      case SDL_MOUSEBUTTONDOWN:
      case SDL_MOUSEBUTTONUP: {
        if (e.button.which == SDL_TOUCH_MOUSEID) break;
        JsuiMouseButton b = e.button.button == SDL_BUTTON_RIGHT    ? JSUI_MOUSE_RIGHT
                            : e.button.button == SDL_BUTTON_MIDDLE ? JSUI_MOUSE_MIDDLE
                                                                   : JSUI_MOUSE_LEFT;
        if (e.button.button > SDL_BUTTON_RIGHT) break;  // X1/X2: not mapped
        jsui_send_mouse_button(rt_, float(e.button.x), float(e.button.y), b, e.type == SDL_MOUSEBUTTONDOWN,
                               viewer::jsuiMods(SDL_GetModState()));
        break;
      }
      case SDL_MOUSEWHEEL: {
        int mx = 0, my = 0;
        SDL_GetMouseState(&mx, &my);
        float dx = e.wheel.preciseX, dy = e.wheel.preciseY;
        if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
          dx = -dx;
          dy = -dy;
        }
        // DOM: positive deltaY scrolls down; ~40 px per notch like browsers.
        jsui_send_mouse_wheel(rt_, float(mx), float(my), dx * 40.f, -dy * 40.f);
        break;
      }
      case SDL_FINGERDOWN:
      case SDL_FINGERMOTION:
      case SDL_FINGERUP: {
        int phase = e.type == SDL_FINGERDOWN ? 0 : e.type == SDL_FINGERMOTION ? 1 : 2;
        int id = int(e.tfinger.fingerId & 0x7fffffff);
        jsui_send_touch(rt_, id, phase, e.tfinger.x * float(logicalW_), e.tfinger.y * float(logicalH_));
        break;
      }
      case SDL_KEYDOWN:
      case SDL_KEYUP: {
        bool down = e.type == SDL_KEYDOWN;
        if (e.key.keysym.sym == SDLK_F5 && !(e.key.keysym.mod & KMOD_CTRL)) {
          if (down && !e.key.repeat) jsui_reload(rt_);
          break;
        }
        std::string key = viewer::domKey(e.key.keysym.sym, e.key.keysym.mod);
        jsui_send_key(rt_, key.c_str(), viewer::domCode(e.key.keysym.scancode), down, e.key.repeat != 0,
                      viewer::jsuiMods(e.key.keysym.mod));
        break;
      }
      case SDL_TEXTINPUT: jsui_send_text(rt_, e.text.text); break;
      case SDL_CONTROLLERDEVICEADDED: {
        SDL_GameController* gc = SDL_GameControllerOpen(e.cdevice.which);
        if (!gc) break;
        SDL_JoystickID id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc));
        if (pads_.count(id)) {
          SDL_GameControllerClose(gc);
          break;
        }
        int index = 0;
        for (bool used = true; used; index++) {
          used = false;
          for (auto& [k, p] : pads_)
            if (p.index == index) used = true;
          if (!used) break;
        }
        pads_.emplace(id, Pad{gc, index});
        const char* name = SDL_GameControllerName(gc);
        jsui_set_gamepad_connected(rt_, index, 1, name ? name : "SDL GameController");
        break;
      }
      case SDL_CONTROLLERDEVICEREMOVED: {
        auto it = pads_.find(e.cdevice.which);
        if (it == pads_.end()) break;
        jsui_set_gamepad_connected(rt_, it->second.index, 0, nullptr);
        SDL_GameControllerClose(it->second.controller);
        pads_.erase(it);
        break;
      }
      case SDL_CONTROLLERBUTTONDOWN:
      case SDL_CONTROLLERBUTTONUP: {
        int pad = padIndexFor(e.cbutton.which);
        int b = viewer::gamepadButton(SDL_GameControllerButton(e.cbutton.button));
        bool down = e.type == SDL_CONTROLLERBUTTONDOWN;
        if (pad >= 0 && b >= 0) jsui_send_gamepad_button(rt_, pad, b, down, down ? 1.f : 0.f);
        break;
      }
      case SDL_CONTROLLERAXISMOTION: {
        auto it = pads_.find(e.caxis.which);
        if (it == pads_.end()) break;
        float v = std::max(-1.f, float(e.caxis.value) / 32767.f);
        switch (e.caxis.axis) {
          case SDL_CONTROLLER_AXIS_LEFTX: jsui_send_gamepad_axis(rt_, it->second.index, 0, v); break;
          case SDL_CONTROLLER_AXIS_LEFTY: jsui_send_gamepad_axis(rt_, it->second.index, 1, v); break;
          case SDL_CONTROLLER_AXIS_RIGHTX: jsui_send_gamepad_axis(rt_, it->second.index, 2, v); break;
          case SDL_CONTROLLER_AXIS_RIGHTY: jsui_send_gamepad_axis(rt_, it->second.index, 3, v); break;
          case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
          case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: {
            // Standard mapping: triggers are analog buttons 6 / 7 (pressed with hysteresis).
            int t = e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? 0 : 1;
            bool& pressed = it->second.trigger[t];
            bool now = pressed ? v > 0.3f : v > 0.5f;
            if (now != pressed) {
              pressed = now;
              jsui_send_gamepad_button(rt_, it->second.index, 6 + t, now, std::max(0.f, v));
            }
            break;
          }
          default: break;
        }
        break;
      }
      default: break;
    }
  }

  Options opt_;
  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  SDL_Texture* texture_ = nullptr;
  SDL_BlendMode premul_ = SDL_BLENDMODE_BLEND;
  JsuiRuntime* rt_ = nullptr;
  std::vector<uint8_t> pixels_;
  int pixelW_ = 0, pixelH_ = 0, logicalW_ = 0, logicalH_ = 0;
  float ratio_ = 1;
  Uint64 start_ = 0;
  bool running_ = true;
  std::map<SDL_JoystickID, Pad> pads_;
  std::map<int, SDL_Cursor*> cursors_;
  std::string cursorName_;
};

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parseArgs(argc, argv, &opt)) return 2;
  Viewer v(opt);
  return v.run();
}
