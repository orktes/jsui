// jsui-wl: a Wayland compositor (wlroots 0.17) whose whole desktop shell is
// the same JSUI React app as jsui-wm's (../ui/shell.jsx), made for 11"
// touchscreens.
//
//   jsui-wl [--scale 1.5] [--size 1920x1200] [--bundle shell.js | --dev URL]
//           [--anchors file.json] [--control socket] [--mouse] [--no-apps-scan]
//           [--autostart CMD]... [--app-env NAME=VALUE]... [--app-env-nogpu NAME=VALUE]...
//           [--verbose]
//
// How it fits together (compare ../main.cpp, the X11 host)
//
//  * Output. One output (wlroots picks the backend: headless in Docker with
//    WLR_BACKENDS=headless, DRM + libinput on a device). The scene graph holds
//    a single buffer: the shell's JSUI frame, rendered on the CPU with
//    jsui_render_rgba_incremental straight into that buffer.
//
//  * Clients. Client surfaces are not in the scene. On every commit of an
//    xdg toplevel the host composites its surface tree (subsurfaces
//    included, cropped to the xdg geometry) into RGBA and hands it to JSUI as
//    host texture "win:<id>"; xdg popups (menus) become "win:<id>" too and are
//    listed in wm.popups. The shell draws everything: frames, title bars,
//    animations, the switcher, exactly as on X11. Server-side decorations are
//    requested (xdg-decoration, KDE server-decoration for GTK 3).
//
//  * Input. The compositor routes every press itself: into the shell when
//    the shell's regions (wm:shellRegions) cover the point, the point is on a
//    window's title bar / border, or on the wallpaper; into the client
//    surface below otherwise (focusing and raising it first). Pointer input
//    reaches JSUI as touch (one finger) unless --mouse; touchscreen input as
//    multi-touch. The on-screen keyboard types into clients through a
//    virtual wlr_keyboard.
//
//  * Same protocol as the X11 host: shared state wm.screen / wm.windows /
//    wm.stack / wm.active / wm.popups / wm.apps / wm.system, messages
//    wm:focus, wm:move, wm:resize, ... (see ../README.md).
//
//  * --control <socket>: a line protocol for test drivers (docker/tour.py):
//    "down X Y", "move X Y", "up", "key NAME", "text UTF8", "shot FILE.ppm".
//    Screen px. wlr-screencopy, xdg-output and the virtual pointer / keyboard
//    protocols are there too, so grim and wayvnc work.
#include "../json.h"
#ifdef JSUI_WL_MIR
#include "mir_host.h"
#endif

#include <dirent.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <pixman.h>
#include <pwd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <xkbcommon/xkbcommon.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <set>
#include <string>
#include <vector>

// wlroots headers are C99 (`float color[static 4]`), which C++ rejects.
extern "C" {
#define static
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/multi.h>
#include <wlr/backend/wayland.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pixman.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_touch.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>
#undef static
}

#include "jsui/jsui.h"
#ifdef JSUI_WL_MIR
#include "wlegl.h"
#endif
#include "text_input_v2.h"

#ifndef JSUI_WM_BUNDLE
#define JSUI_WM_BUNDLE "integrations/x11-wm/ui/dist/shell.js"
#endif

namespace {

bool g_verbose = false;

// Under systemd (Ubuntu Touch runs the shell as lomiri-full-shell.service,
// Type=notify): tell it we are up, and point the user session's later
// launches (url-dispatcher, ...) at our Wayland socket.
void notifySystemd(const std::string& waylandSocket) {
  const char* path = getenv("NOTIFY_SOCKET");
  if (!path || !*path) return;
  if (fork() == 0) {
    std::string env = "WAYLAND_DISPLAY=" + waylandSocket;
    execlp("dbus-update-activation-environment", "dbus-update-activation-environment", "--systemd", env.c_str(),
           (char*)nullptr);
    _exit(127);
  }
  int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path, sizeof addr.sun_path - 1);
  if (path[0] == '@') addr.sun_path[0] = 0;  // abstract socket
  const char msg[] = "READY=1";
  sendto(fd, msg, sizeof msg - 1, 0, reinterpret_cast<sockaddr*>(&addr),
         socklen_t(offsetof(sockaddr_un, sun_path) + std::strlen(path)));
  close(fd);
}

void logf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::fprintf(stderr, "[jsui-wl] ");
  std::vfprintf(stderr, fmt, ap);
  std::fprintf(stderr, "\n");
  va_end(ap);
}
#define VLOG(...) \
  do {            \
    if (g_verbose) logf(__VA_ARGS__); \
  } while (0)

double nowMs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

std::string lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool fileExists(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// A wl_listener that runs a lambda, for signals that live as long as the
// compositor.
template <typename F>
void listen(wl_signal* sig, wl_listener* l, F&& f) {
  using Fn = std::decay_t<F>;
  struct Box {
    wl_listener l;
    Fn fn;
  };
  (void)l;
  auto* box = new Box{{}, std::forward<F>(f)};
  box->l.notify = [](wl_listener* ll, void* data) {
    Box* b = reinterpret_cast<Box*>(ll);
    b->fn(data);
  };
  wl_signal_add(sig, &box->l);
}

// Listeners we need to remove again (per-client ones) are kept in a list.
struct Hook {
  wl_listener l{};
  std::function<void(void*)> fn;
};

Hook* hook(wl_signal* sig, std::function<void(void*)> fn) {
  auto* h = new Hook;
  h->fn = std::move(fn);
  h->l.notify = [](wl_listener* ll, void* data) {
    Hook* hh = reinterpret_cast<Hook*>(ll);
    hh->fn(data);
  };
  wl_signal_add(sig, &h->l);
  return h;
}

void unhook(std::vector<Hook*>& hooks) {
  for (Hook* h : hooks) {
    wl_list_remove(&h->l.link);
    delete h;
  }
  hooks.clear();
}

// ------------------------------------------------------------------ Options

struct Options {
  std::string bundle = JSUI_WM_BUNDLE;
  std::string devUrl, anchors, control;
  float scale = 0;
  int width = 1920, height = 1200;
  bool mouse = false;
  bool scanApps = true;
  bool mir = false;       // --mir: show the desktop through a host Mir server (Ubuntu Touch)
  std::string mirSocket;  // default $MIR_SERVER_HOST_SOCKET
  int rotate = 0;         // --rotate DEG: clockwise rotation of the desktop on a --mir panel
  std::vector<std::string> autostart;
  std::vector<std::string> appEnv;  // --app-env NAME=VALUE: set (empty VALUE: unset) for launched apps only
  std::vector<std::string> appEnvNoGpu;  // --app-env-nogpu NAME=VALUE: same, when apps can't hand over GPU buffers
};

void usage() {
  std::fprintf(stderr,
               "usage: jsui-wl [--scale 1.5] [--size 1920x1200] [--bundle shell.js | --dev URL]\n"
               "               [--anchors file.json] [--control socket] [--mouse] [--no-apps-scan]\n"
               "               [--autostart CMD]... [--app-env NAME=VALUE]... [--app-env-nogpu NAME=VALUE]...\n"
               "               [--mir [--mir-socket PATH] [--rotate 0|90|180|270]]\n"
               "               [--verbose]\n");
}

bool parseArgs(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](std::string& out) {
      if (i + 1 >= argc) return false;
      out = argv[++i];
      return true;
    };
    std::string v;
    if (a == "--scale" && next(v)) o.scale = std::strtof(v.c_str(), nullptr);
    else if (a == "--size" && next(v)) std::sscanf(v.c_str(), "%dx%d", &o.width, &o.height);
    else if (a == "--bundle" && next(v)) o.bundle = v;
    else if (a == "--dev" && next(v)) o.devUrl = v;
    else if (a == "--anchors" && next(v)) o.anchors = v;
    else if (a == "--control" && next(v)) o.control = v;
    else if (a == "--autostart" && next(v)) o.autostart.push_back(v);
    else if (a == "--app-env" && next(v) && v.find('=') != std::string::npos) o.appEnv.push_back(v);
    else if (a == "--app-env-nogpu" && next(v) && v.find('=') != std::string::npos) o.appEnvNoGpu.push_back(v);
    else if (a == "--mouse") o.mouse = true;
    else if (a == "--no-apps-scan") o.scanApps = false;
    else if (a == "--mir") o.mir = true;
    else if (a == "--mir-socket" && next(v)) o.mirSocket = v, o.mir = true;
    else if (a == "--rotate" && next(v)) o.rotate = std::atoi(v.c_str());
    else if (a == "--verbose" || a == "-v") g_verbose = true;
    else if (a == "--help" || a == "-h") {
      usage();
      std::exit(0);
    } else {
      usage();
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------- Apps

struct App {
  std::string id, name, comment, exec, icon, wmClass, categories;
  bool terminal = false;
  bool squareIcon = false;  // a full-bleed square (Ubuntu Touch's Suru icons), shown clipped to the tile shape
};

// Desktop files' own icons (absolute paths: Ubuntu Touch's full-bleed Suru
// squares, SVG or PNG) as PNGs with the tile shape cut out: jsui-icons, next
// to jsui-wl, makes them into ~/.cache/jsui-wm/icons (JSUI's Image decodes
// PNG, and drawing them under a rounded clip each was slow); until it has,
// the app shows its glyph. g_svgIcons: source -> PNG pairs still to make.
std::vector<std::pair<std::string, std::string>> g_svgIcons;
std::set<std::string> g_svgTried;

std::string svgIconPng(const std::string& svg) {
  const char* home = getenv("HOME");
  char name[32];
  std::snprintf(name, sizeof name, "%016zx.png", std::hash<std::string>{}(svg + "#tile"));
  return std::string(home ? home : "/tmp") + "/.cache/jsui-wm/icons/" + name;
}

std::string findIcon(const std::string& icon) {
  if (icon.empty()) return "";
  auto ends = [&](const char* ext) { return icon.size() > 4 && icon.compare(icon.size() - 4, 4, ext) == 0; };
  if (icon[0] == '/' && (ends(".svg") || ends(".png")) && fileExists(icon)) {
    std::string png = svgIconPng(icon);
    struct stat a, b;
    if (stat(png.c_str(), &a) == 0 && stat(icon.c_str(), &b) == 0 && a.st_mtime >= b.st_mtime) return png;
    if (g_svgTried.insert(icon).second) g_svgIcons.emplace_back(icon, png);
    return "";
  }
  if (icon[0] == '/') return "";
  static const char* themes[] = {"hicolor", "Adwaita", "gnome", "Papirus", "breeze"};
  static const char* sizes[] = {"128x128", "96x96", "64x64", "256x256", "72x72", "48x48", "512x512", "32x32"};
  for (const char* theme : themes)
    for (const char* size : sizes) {
      std::string p = std::string("/usr/share/icons/") + theme + "/" + size + "/apps/" + icon + ".png";
      if (fileExists(p)) return p;
    }
  std::string p = "/usr/share/pixmaps/" + icon + ".png";
  return fileExists(p) ? p : "";
}

std::string cleanExec(const std::string& exec) {
  std::string out;
  for (size_t i = 0; i < exec.size(); ++i) {
    if (exec[i] == '%' && i + 1 < exec.size()) {
      if (exec[i + 1] == '%') out += '%';
      ++i;
      continue;
    }
    out += exec[i];
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

std::string shellQuote(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') out += "'\\''";
    else out += c;
  }
  return out + "'";
}

std::vector<App> scanApps(const std::string& terminal) {
  std::vector<App> apps;
  const bool unconfined = fileExists("/usr/bin/aa-exec") && access("/sys/kernel/security/apparmor", F_OK) == 0;
  std::set<std::string> seen;
  std::vector<std::string> dirs = {"/usr/share/applications", "/usr/local/share/applications"};
  if (const char* home = getenv("HOME")) dirs.push_back(std::string(home) + "/.local/share/applications");
  for (const auto& dir : dirs) {
    DIR* d = opendir(dir.c_str());
    if (!d) continue;
    while (dirent* e = readdir(d)) {
      std::string file = e->d_name;
      if (file.size() < 9 || file.substr(file.size() - 8) != ".desktop") continue;
      std::ifstream in(dir + "/" + file);
      App app;
      app.id = file.substr(0, file.size() - 8);
      bool inEntry = false, hidden = false, isApp = false;
      std::string path, appId;
      std::string line;
      while (std::getline(in, line)) {
        if (!line.empty() && line[0] == '[') {
          inEntry = line == "[Desktop Entry]";
          continue;
        }
        if (!inEntry) continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "Name") app.name = v;
        else if (k == "Comment") app.comment = v;
        else if (k == "Exec") app.exec = cleanExec(v);
        else if (k == "Icon") app.icon = v;
        else if (k == "StartupWMClass") app.wmClass = v;
        else if (k == "Categories") app.categories = v;
        else if (k == "Terminal") app.terminal = v == "true";
        else if (k == "Type") isApp = v == "Application";
        else if (k == "Path") path = v;
        else if (k == "X-Lomiri-Application-ID") appId = v;
        else if ((k == "NoDisplay" || k == "Hidden") && v == "true") hidden = true;
        else if (k == "OnlyShowIn") hidden = true;
      }
      if (!isApp || hidden || app.name.empty() || app.exec.empty() || seen.count(app.id)) continue;
      if (app.terminal) app.exec = terminal + " " + app.exec;
      // Ubuntu Touch click apps: ubuntu-app-launch runs them confined
      // (aa-exec-click), from their package directory, with APP_ID set. There
      // is no app confinement here yet: run them directly, from Path=.
      const std::string aa = "aa-exec-click ";
      if (app.exec.compare(0, aa.size(), aa) == 0) {
        size_t dash = app.exec.find(" -- ");
        if (dash != std::string::npos) app.exec = app.exec.substr(dash + 4);
      }
      // Some system apps (messaging, the browser) have AppArmor profiles that
      // attach by path and keep them from reading the Qt Wayland plugin this
      // package brings: run every app unconfined, as the click apps above.
      if (unconfined) app.exec = "aa-exec -p unconfined -- " + app.exec;
      // Click packages carry their own binaries, libraries and QML modules,
      // which ubuntu-app-launch puts on the paths.
      if (!appId.empty() && !path.empty()) {
        std::string p = shellQuote(path);
        app.exec = "APP_DIR=" + p + " PATH=" + p + ":" + p + "/lib/aarch64-linux-gnu/bin:\"$PATH\" LD_LIBRARY_PATH=" + p +
                   "/lib/aarch64-linux-gnu:" + p + "/lib QML2_IMPORT_PATH=" + p + "/lib/aarch64-linux-gnu " + app.exec;
      }
      if (!appId.empty()) app.exec = "APP_ID=" + shellQuote(appId) + " " + app.exec;
      if (!path.empty()) app.exec = "cd " + shellQuote(path) + " && " + app.exec;
      app.squareIcon = !app.icon.empty() && app.icon[0] == '/';
      app.icon = findIcon(app.icon);
      seen.insert(app.id);
      apps.push_back(app);
    }
    closedir(d);
  }
  std::sort(apps.begin(), apps.end(), [](const App& a, const App& b) { return lower(a.name) < lower(b.name); });
  return apps;
}

// Environment for launched apps that the compositor itself must not have
// (on Ubuntu Touch: apps on Mesa's EGL while jsui-wl is on libhybris').
std::vector<std::string> g_appEnv;

pid_t launch(const std::string& cmd) {
  if (cmd.empty()) return -1;
  logf("launch: %s", cmd.c_str());
  pid_t pid = fork();
  if (pid == 0) {
    setsid();
    signal(SIGCHLD, SIG_DFL);
    for (const auto& kv : g_appEnv) {
      size_t eq = kv.find('=');
      std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
      if (v.empty()) unsetenv(k.c_str());
      else setenv(k.c_str(), v.c_str(), 1);
    }
    execl("/bin/sh", "sh", "-c", cmd.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  return pid;
}

// The parent of `pid` (0 when it is gone).
pid_t parentPid(pid_t pid) {
  std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
  std::string line;
  if (!std::getline(in, line)) return 0;
  size_t close = line.rfind(')');  // the command may hold spaces and parentheses
  if (close == std::string::npos) return 0;
  int ppid = 0;
  return std::sscanf(line.c_str() + close + 1, " %*c %d", &ppid) == 1 ? pid_t(ppid) : 0;
}

// DOM key names for keysyms (xkbcommon keysyms are the X11 ones).
struct DomKey {
  const char* key;
  const char* code;
};

DomKey domKey(xkb_keysym_t sym, const char* text) {
  switch (sym) {
    case XKB_KEY_Return: case XKB_KEY_KP_Enter: return {"Enter", "Enter"};
    case XKB_KEY_BackSpace: return {"Backspace", "Backspace"};
    case XKB_KEY_Escape: return {"Escape", "Escape"};
    case XKB_KEY_Tab: case XKB_KEY_ISO_Left_Tab: return {"Tab", "Tab"};
    case XKB_KEY_Left: return {"ArrowLeft", "ArrowLeft"};
    case XKB_KEY_Right: return {"ArrowRight", "ArrowRight"};
    case XKB_KEY_Up: return {"ArrowUp", "ArrowUp"};
    case XKB_KEY_Down: return {"ArrowDown", "ArrowDown"};
    case XKB_KEY_Delete: return {"Delete", "Delete"};
    case XKB_KEY_Home: return {"Home", "Home"};
    case XKB_KEY_End: return {"End", "End"};
    case XKB_KEY_space: return {" ", "Space"};
    case XKB_KEY_Shift_L: return {"Shift", "ShiftLeft"};
    case XKB_KEY_Shift_R: return {"Shift", "ShiftRight"};
    case XKB_KEY_Control_L: return {"Control", "ControlLeft"};
    case XKB_KEY_Control_R: return {"Control", "ControlRight"};
    case XKB_KEY_Alt_L: return {"Alt", "AltLeft"};
    case XKB_KEY_Super_L: return {"Meta", "MetaLeft"};
    default: break;
  }
  return {text && *text ? text : nullptr, nullptr};
}

// The keysym the shell's on-screen keyboard means by a DOM key name.
xkb_keysym_t keysymFor(const std::string& key) {
  static const std::map<std::string, xkb_keysym_t> named = {
      {"Enter", XKB_KEY_Return},   {"Backspace", XKB_KEY_BackSpace}, {"Tab", XKB_KEY_Tab},
      {"Escape", XKB_KEY_Escape},  {"ArrowLeft", XKB_KEY_Left},      {"ArrowRight", XKB_KEY_Right},
      {"ArrowUp", XKB_KEY_Up},     {"ArrowDown", XKB_KEY_Down},      {" ", XKB_KEY_space},
      {"Delete", XKB_KEY_Delete},  {"Home", XKB_KEY_Home},           {"End", XKB_KEY_End},
  };
  auto it = named.find(key);
  if (it != named.end()) return it->second;
  // One UTF-8 character.
  const unsigned char* p = reinterpret_cast<const unsigned char*>(key.c_str());
  uint32_t cp = p[0];
  if (cp >= 0xF0) cp = ((p[0] & 7u) << 18) | ((p[1] & 63u) << 12) | ((p[2] & 63u) << 6) | (p[3] & 63u);
  else if (cp >= 0xE0) cp = ((p[0] & 15u) << 12) | ((p[1] & 63u) << 6) | (p[2] & 63u);
  else if (cp >= 0xC0) cp = ((p[0] & 31u) << 6) | (p[1] & 63u);
  return xkb_utf32_to_keysym(cp);
}

// ------------------------------------------------------------ shell buffer

// The buffer the shell renders into and the scene shows: CPU memory that the
// pixman renderer reads in place.
struct FrameBuffer {
  wlr_buffer base;
  std::vector<uint8_t> data;
  int stride = 0;
};

const wlr_buffer_impl kFrameBufferImpl = {
    /*destroy*/ [](wlr_buffer* b) { delete reinterpret_cast<FrameBuffer*>(b); },
    /*get_dmabuf*/ nullptr,
    /*get_shm*/ nullptr,
    /*begin_data_ptr_access*/
    [](wlr_buffer* b, uint32_t, void** data, uint32_t* format, size_t* stride) -> bool {
      auto* fb = reinterpret_cast<FrameBuffer*>(b);
      *data = fb->data.data();
      *format = DRM_FORMAT_XBGR8888;  // bytes R, G, B, x: JSUI's RGBA
      *stride = size_t(fb->stride);
      return true;
    },
    /*end_data_ptr_access*/ [](wlr_buffer*) {},
};

// ------------------------------------------------------------------ WM

struct Rect {
  int x = 0, y = 0, w = 0, h = 0;
  bool contains(double px, double py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

enum class Target { None, Shell, Surface };

struct Client {
  uint32_t id = 0;
  bool popup = false;
  wlr_xdg_surface* xdg = nullptr;
  wlr_xdg_toplevel* tl = nullptr;   // toplevels
  wlr_xdg_popup* xpopup = nullptr;  // popups
  uint32_t parentId = 0;            // popups: the client they belong to
  std::vector<Hook*> hooks;
  bool mapped = false, placed = false;

  // content (the xdg geometry) in screen px
  int x = 0, y = 0, w = 1, h = 1;
  int geoX = 0, geoY = 0;  // xdg geometry offset inside the surface
  // Shown at this scale (< 1: a maximized client that keeps a size bigger
  // than the work area, letterboxed; see fitMaximized). x / y are where it
  // is shown, w / h its own size; input is mapped back through it.
  double fit = 1;
  std::vector<uint8_t> rgba;
  int texW = 0, texH = 0;
  bool hasTexture = false;

  std::string title, appIdRaw, appId;
  uint32_t transientFor = 0;
  bool decorated = true;
  bool minimized = false, maximized = false, fullscreen = false;
  int snap = 0;  // 1 left, 2 right
  Rect restore;
  bool hasRestore = false;
  int minW = 1, minH = 1, maxW = 0, maxH = 0;
  int pid = 0;
  double mappedAt = 0;
  double reassertedAt = -1e9;  // last time a maximized size was sent again (onCommit)
  bool dialog = false;
};

class WM {
 public:
  explicit WM(const Options& o) : opt_(o) {}
  int run();

 private:
  // wlroots setup
  bool setup();
  void newOutput(wlr_output* output);
  void allocFrame();
  void resizeScreen(int w, int h);
  void newInput(wlr_input_device* dev);
  void newKeyboard(wlr_keyboard* kb);
  void newToplevel(wlr_xdg_surface* xdg);
  void newPopup(wlr_xdg_popup* popup);
  void setupVirtualKeyboard();
  void setupControl();

  // JSUI
  bool createRuntime();
  void publishScreen();
  void publishApps();
  // Runs jsui-icons for g_svgIcons, then scans and publishes the apps again.
  void rasterizeIcons();
  wl_event_source* iconSrc_ = nullptr;
  void publishSystem();
  void onMessage(const char* name, const char* json);
  void onStateChange(const char* key, const char* json);
  void writeAnchors(const wmjson::Value& v);

  // clients
  Client* find(uint32_t id);
  Client* findSurface(wlr_surface* s);
  void onMap(Client* c);
  void onUnmap(Client* c);
  void onDestroy(Client* c);
  void onCommit(Client* c);
  void capture(Client* c);
  void place(Client* c);
  void configure(Client* c, int x, int y, int w, int h);
  void refitWorkArea();
  // Text fields in apps ask for the on-screen keyboard (text_input_v2.h);
  // the shell tells how much of the screen it takes (wm:osk).
  TextInputV2 textInput_;
  int oskH_ = 0;
  void fitMaximized(Client* c);
  void toSurface(const Client* c, double* x, double* y) const;
  void raise(Client* c);
  void focus(Client* c);
  void focusTop();
  void close(Client* c);
  void minimize(Client* c);
  void unminimize(Client* c);
  void setMaximized(Client* c, bool on);
  void setSnap(Client* c, int side);
  void setFullscreen(Client* c, bool on);
  Rect workArea() const;
  void matchApp(Client* c);
  Rect popupRect(Client* p) const;
  double popupFit(Client* p);

  // state publishing
  std::string windowJson(const Client* c) const;
  void publishWindow(const Client* c);
  void publishStack();
  void publishPopups();
  void publishActive();

  // input
  Target route(double x, double y, wlr_surface** surface, double* sx, double* sy);
  void pointerButton(bool down, uint32_t button, uint32_t timeMs);
  void pointerMotion(uint32_t timeMs);
  void touchEvent(int id, int phase, double x, double y, uint32_t timeMs);
  void keyboardKey(wlr_keyboard* kb, uint32_t keycode, bool down, uint32_t timeMs);
  void typeKey(const std::string& key, int mods);
  void sendShellPointer(int phase, double x, double y);
  void controlLine(const std::string& line);
  void writeScreenshot(const std::string& path);

  // frames
  void scheduleTick(double delayMs);
  void tick();
  void scheduleFrame();
  void onFrame();

  int P(double logical) const { return int(std::lround(logical * scale_)); }
  double L(double px) const { return px / double(scale_); }

  Options opt_;
  wl_display* display_ = nullptr;
  wl_event_loop* loop_ = nullptr;
  wlr_backend* backend_ = nullptr;
  wlr_renderer* renderer_ = nullptr;
  wlr_allocator* allocator_ = nullptr;
  wlr_scene* scene_ = nullptr;
  wlr_scene_output_layout* sceneLayout_ = nullptr;
  wlr_output_layout* layout_ = nullptr;
  wlr_output* output_ = nullptr;
  wlr_scene_output* sceneOutput_ = nullptr;
  wlr_scene_buffer* shellNode_ = nullptr;
  FrameBuffer* frame_ = nullptr;
  wlr_xdg_shell* xdgShell_ = nullptr;
  wlr_seat* seat_ = nullptr;
  wlr_cursor* cursor_ = nullptr;
  wlr_keyboard vkbd_{};  // the on-screen keyboard's device
  xkb_keymap* keymap_ = nullptr;
  wl_event_source* tickTimer_ = nullptr;
  wl_event_source* systemTimer_ = nullptr;  // publishSystem again (battery, Wi-Fi)
  std::string lastSystem_;
  int frameLog_ = 0;  // JSUI_WL_PERF: frames still to log in full (after an unmap)
  bool tickPending_ = false;
  bool frameScheduled_ = false;
  bool shellDirty_ = false;
  pixman_region32_t shellDamage_;
  int sw_ = 0, sh_ = 0;
  float scale_ = 1.5f;
  std::string socketName_;

  double topBar_ = 40, dock_ = 84, homeBar_ = 24, titleBar_ = 46, border_ = 12;

  JsuiRuntime* rt_ = nullptr;
  std::map<uint32_t, Client*> clients_;
  std::vector<uint32_t> stack_;   // toplevels, bottom to top
  std::vector<uint32_t> popups_;  // bottom to top
  uint32_t nextId_ = 1;
  uint32_t active_ = 0;
  std::vector<App> apps_;
  std::map<uint32_t, double> releaseAt_;
  bool tabletMode_ = true;
  double splitRatio_ = 0.5;
  bool keyboardGrabbed_ = false;
  int cascade_ = 0;
  std::vector<Rect> shellRegions_;

  // pointer: where the current press goes (implicit grab)
  Target pointerTarget_ = Target::None;
  bool pointerDown_ = false;
  // touch points: id -> target
  std::map<int, Target> touches_;
  struct Pos {
    double x = 0, y = 0;
  };
  std::map<int, Pos> touchPos_;  // each touch's last position (up events may carry none)
  // host-side interactive move / resize (xdg request_move / request_resize)
  uint32_t mrId_ = 0;
  uint32_t mrEdges_ = 0;
  double mrX_ = 0, mrY_ = 0;
  Rect mrStart_;

#ifdef JSUI_WL_MIR
  MirHost* mir_ = nullptr;
  uint32_t mirOutput_ = 0;
  // Turning the desktop on the panel while running (Quick Settings'
  // auto-rotate, or "rotate DEG" on the control socket), and the
  // jsui-orientation helper that reads the accelerometer for it.
  void rotateTo(int deg);
  void publishRotation();
  void autoRotate(bool on);
  // jsui-notifyd (next to jsui-wl): the session's notification server. Its
  // lines (JSON) go to the shell as "wm:notification".
  void startNotifyd();
  wl_event_source* notifySrc_ = nullptr;
  std::string notifyBuf_;
  int notifyIn_ = -1;  // jsui-notifyd's stdin: "action ID" for a tapped notification
  void syncAppTheme(bool dark);
  bool dark_ = true;  // the shell's darkMode (DEFAULT_SETTINGS in ui/wm.js)
  void orientationLine(const std::string& line);
  bool autoRotate_ = false;
  pid_t orientPid_ = -1;
  wl_event_source* orientSrc_ = nullptr;
  std::string orientBuf_;
  // --mir on the GPU: Skia draws the desktop into the Mir window's GL context.
  JsuiGpuContext* gpu_ = nullptr;
  bool vulkan_ = false;  // gpu_ is Graphite on MirVulkan's device (else Ganesh GL)
  // Apps' GPU buffers (android_wlegl, wlegl.h): the one each window shows,
  // locked until the next one replaces it or the window's texture goes.
  bool gpuApps_ = false;
  std::map<uint32_t, wlr_buffer*> gpuBuffers_;
  bool captureGpu(Client* c);
  // Full-screen panels get a blurred snapshot of the screen under them
  // ("wm:backdrop", opaque), so JSUI culls what is below
  // (JSUI_WL_SNAPSHOT_BG=0: they draw over the live desktop).
  bool snapshotBg_ = false;
  void snapshotBackdrop(bool on);
  void haptic(int ms);
  double lastHaptic_ = 0;
  // Screen brightness and output volume (the shell's Quick Settings): the
  // backlight through repowerd (Ubuntu Touch: setUserBrightness takes the
  // backlight's own 0..max_brightness), the volume through PulseAudio
  // (pactl). A slider drag sends many values: the latest is applied at most
  // every 80 ms. Their levels at start go out once in wm.system
  // ("backlight", "volume"), so the sliders start where the device is.
  struct Levels {
    double brightness = -1, volume = -1;
    int muted = -1;
  };
  Levels wantLevels_, appliedLevels_;
  wl_event_source* levelsTimer_ = nullptr;
  std::string startBacklight_, startVolume_;  // the wm.system fields, read once
  bool backlightRead_ = false, volumeRead_ = false;
  void setLevels(const wmjson::Value& v, bool brightness);
  // A volume button: one step (1/16) up or down, applied now and told to the
  // shell (wm:volumeKey) for its indicator and slider.
  void volumeStep(int dir);
  double volumeNow_ = -1;  // the output volume as last read or set (0..1)
  void applyLevels();
  void readStartLevels();
  // GPU boost (Adreno: kgsl devfreq). Its governor ramps up too slowly for
  // a 300 ms animation (the Tab A7 stays at 320-600 of its 950 MHz) and
  // nothing here sends Android's touch boost. From a touch, and while the
  // frames it set off are drawn, min_freq is raised to JSUI_WL_GPU_BOOST
  // MHz (default 745, 0: off) and set back to the lowest frequency 500 ms
  // after the last of either (see gpuBoost). Needs min_freq writable by the session (install.sh --system:
  // jsui-wm-gpu-boost.service); without that, or without the node, nothing
  // happens.
  std::string boostPath_;
  long boostHz_ = 0, boostRestHz_ = 0;
  bool boosted_ = false;
  double boostUntil_ = 0, boostTouch_ = 0;
  wl_event_source* boostTimer_ = nullptr;
  void setupGpuBoost();
  void gpuBoost(bool touch);
  void gpuBoostWrite(long hz);
  void removeWindowTexture(uint32_t id);
  bool renderGpu();
  double lastTickMs_ = 0, lastRenderMs_ = 0;  // the last tick's / paint's time (slow-frame log)
  bool vsyncTicks_ = false;  // ticks follow the present thread's swaps (see tick)
  // Mir's swap interval is 0, so a swap of a cheap frame returns at once and
  // ticks that follow swaps ran at ~180 fps (a finger dragging the dock):
  // frames no one sees, taking the GPU from the ones they do. Presents start
  // at most every minFrameMs_ (JSUI_WL_MAX_FPS, default 60, with 8% slack
  // for jitter).
  double minFrameMs_ = 0, lastPresentAt_ = -1e9;
  void presentMir();
  void sendFrameDone();
  // Frame cost on the Mir path (JSUI_WL_PERF=1): paint + present, logged every 60 frames.
  // JSUI_WL_PERF=2 also logs every frame as a slow one would be, with the
  // times of touches, app commits (GPU apps) and returned swaps, from which
  // touch-to-swap latency can be read.
  bool perf_ = false, perfEvery_ = false;
  double cpuPaintMs_ = 0;  // CPU raster since the last presented frame
  double gpuRenderMs_ = 0, gpuBlitMs_ = 0, gpuSwapMs_ = 0;  // GPU frame parts (perf window sums)
  double gpuRenderGpuMs_ = 0, gpuBlitGpuMs_ = 0;  // JSUI_WL_GPU_FINISH: GPU time of the paint / the blit
  double perfPaint_ = 0, perfPresent_ = 0, perfMax_ = 0;
  double perfStart_ = 0, perfLast_ = 0, perfGapMax_ = 0;  // frame pacing over the perf window
  double perfTick_ = 0, perfTickMax_ = 0;
  // Paint stats summed over the perf window (jsui_get_paint_stats).
  double perfMpx_ = 0;  // megapixels repainted
  long long perfLayers_ = 0, perfLayerDraws_ = 0, perfLayerRenders_ = 0, perfDraws_ = 0, perfShadowRenders_ = 0;
  void perfPaintStats() {
    JsuiPaintStats ps;
    jsui_get_paint_stats(rt_, &ps);
    perfMpx_ += double(ps.repainted_pixels) / 1e6;
    perfLayers_ += ps.save_layers;
    perfLayerDraws_ += ps.layer_draws;
    perfLayerRenders_ += ps.layer_renders;
    perfDraws_ += ps.draws;
    perfShadowRenders_ += ps.shadow_renders;
  }  // jsui_tick (JS, layout) since the last perf line
  int perfTicks_ = 0;
  int perfFrames_ = 0;
  void perfFrame(double paintMs, double presentMs);
  bool setupMir();
  bool showMir();
  void mirEvents();
#endif

  int controlFd_ = -1;
  int jsReadyFd_ = -1;  // JS thread -> Wayland loop wake-up (on_js_ready)
  std::map<std::string, pid_t> launched_;  // wm:launch command -> the shell it ran in
  // Launches still waiting for their first window: an app that hangs before
  // it shows one (Ubuntu Touch's Camera) is ended after kLaunchTimeoutMs and
  // the shell told (wm:launchFailed), instead of an icon that does nothing.
  struct PendingLaunch {
    pid_t pid;
    std::string name;
    double at;
    std::string exec;
  };
  std::vector<PendingLaunch> pendingLaunches_;
  wl_event_source* launchTimer_ = nullptr;
  void checkLaunches();
  bool descends(pid_t pid, pid_t ancestor) const;
  std::map<int, std::string> controlBuf_;
  std::map<int, wl_event_source*> controlSources_;
};

// ------------------------------------------------------------- setup

bool WM::setup() {
  // Apps' GPU buffers (wlegl.h) never become pixman textures: wlroots says so
  // twice on every commit.
  wlr_log_init(g_verbose ? WLR_DEBUG : WLR_ERROR, [](wlr_log_importance, const char* fmt, va_list args) {
    if (std::strstr(fmt, "Failed to upload buffer") || std::strstr(fmt, "Failed to create texture")) return;
    std::fprintf(stderr, "[wlr] ");
    std::vfprintf(stderr, fmt, args);
    std::fprintf(stderr, "\n");
  });
  display_ = wl_display_create();
  loop_ = wl_display_get_event_loop(display_);
  backend_ = wlr_backend_autocreate(display_, nullptr);
  if (!backend_) {
    logf("cannot create a wlroots backend (WLR_BACKENDS=headless for no screen)");
    return false;
  }
  renderer_ = wlr_renderer_autocreate(backend_);
  if (!renderer_) {
    logf("cannot create a renderer (WLR_RENDERER=pixman needs no GPU)");
    return false;
  }
  wlr_renderer_init_wl_display(renderer_, display_);
  allocator_ = wlr_allocator_autocreate(backend_, renderer_);
  wlr_compositor_create(display_, 5, renderer_);
  wlr_subcompositor_create(display_);
  wlr_data_device_manager_create(display_);
  layout_ = wlr_output_layout_create();
  wlr_xdg_output_manager_v1_create(display_, layout_);
  wlr_screencopy_manager_v1_create(display_);
  scene_ = wlr_scene_create();
  sceneLayout_ = wlr_scene_attach_output_layout(scene_, layout_);
  pixman_region32_init(&shellDamage_);

  listen(&backend_->events.new_output, nullptr, [this](void* d) { newOutput(static_cast<wlr_output*>(d)); });
  listen(&backend_->events.new_input, nullptr, [this](void* d) { newInput(static_cast<wlr_input_device*>(d)); });

  xdgShell_ = wlr_xdg_shell_create(display_, 3);
  listen(&xdgShell_->events.new_surface, nullptr, [this](void* d) {
    auto* xdg = static_cast<wlr_xdg_surface*>(d);
    if (xdg->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) newToplevel(xdg);
    else if (xdg->role == WLR_XDG_SURFACE_ROLE_POPUP) newPopup(xdg->popup);
  });

  // Ask for server-side decorations: the shell draws every title bar.
  auto* kde = wlr_server_decoration_manager_create(display_);
  wlr_server_decoration_manager_set_default_mode(kde, WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
  auto* xdgDeco = wlr_xdg_decoration_manager_v1_create(display_);
  listen(&xdgDeco->events.new_toplevel_decoration, nullptr, [](void* d) {
    auto* deco = static_cast<wlr_xdg_toplevel_decoration_v1*>(d);
    auto* h = new Hook;
    h->fn = [deco](void*) {
      if (deco->toplevel->base->initialized)
        wlr_xdg_toplevel_decoration_v1_set_mode(deco, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    };
    h->l.notify = [](wl_listener* l, void* data) { reinterpret_cast<Hook*>(l)->fn(data); };
    wl_signal_add(&deco->events.request_mode, &h->l);
    // A decoration made before the first commit: set the mode on the
    // surface's first configure (wlroots merges it in).
    wlr_xdg_toplevel_decoration_v1_set_mode(deco, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    listen(&deco->events.destroy, nullptr, [h](void*) {
      wl_list_remove(&h->l.link);
      delete h;
    });
  });

  cursor_ = wlr_cursor_create();
  wlr_cursor_attach_output_layout(cursor_, layout_);
  listen(&cursor_->events.motion, nullptr, [this](void* d) {
    auto* e = static_cast<wlr_pointer_motion_event*>(d);
    wlr_cursor_move(cursor_, &e->pointer->base, e->delta_x, e->delta_y);
    pointerMotion(e->time_msec);
  });
  listen(&cursor_->events.motion_absolute, nullptr, [this](void* d) {
    auto* e = static_cast<wlr_pointer_motion_absolute_event*>(d);
    wlr_cursor_warp_absolute(cursor_, &e->pointer->base, e->x, e->y);
    pointerMotion(e->time_msec);
  });
  listen(&cursor_->events.button, nullptr, [this](void* d) {
    auto* e = static_cast<wlr_pointer_button_event*>(d);
    pointerButton(e->state == WLR_BUTTON_PRESSED, e->button, e->time_msec);
  });
  listen(&cursor_->events.axis, nullptr, [this](void* d) {
    auto* e = static_cast<wlr_pointer_axis_event*>(d);
    wlr_surface* s = nullptr;
    double sx = 0, sy = 0;
    if (route(cursor_->x, cursor_->y, &s, &sx, &sy) == Target::Shell) {
      float dx = e->orientation == WLR_AXIS_ORIENTATION_HORIZONTAL ? float(e->delta * 4) : 0.f;
      float dy = e->orientation == WLR_AXIS_ORIENTATION_VERTICAL ? float(e->delta * 4) : 0.f;
      jsui_set_event_time(rt_, nowMs());
      jsui_send_mouse_wheel_ex(rt_, float(L(cursor_->x)), float(L(cursor_->y)), dx, dy, JSUI_WHEEL_STEPS);
      scheduleTick(0);
    } else {
      wlr_seat_pointer_notify_axis(seat_, e->time_msec, e->orientation, e->delta, e->delta_discrete, e->source);
    }
  });
  listen(&cursor_->events.frame, nullptr, [this](void*) { wlr_seat_pointer_notify_frame(seat_); });
  listen(&cursor_->events.touch_down, nullptr, [this](void* d) {
    auto* e = static_cast<wlr_touch_down_event*>(d);
    double x = 0, y = 0;
    wlr_cursor_absolute_to_layout_coords(cursor_, &e->touch->base, e->x, e->y, &x, &y);
    touchEvent(e->touch_id, 0, x, y, e->time_msec);
  });
  listen(&cursor_->events.touch_motion, nullptr, [this](void* d) {
    auto* e = static_cast<wlr_touch_motion_event*>(d);
    double x = 0, y = 0;
    wlr_cursor_absolute_to_layout_coords(cursor_, &e->touch->base, e->x, e->y, &x, &y);
    touchEvent(e->touch_id, 1, x, y, e->time_msec);
  });
  listen(&cursor_->events.touch_up, nullptr, [this](void* d) {
    auto* e = static_cast<wlr_touch_up_event*>(d);
    touchEvent(e->touch_id, 2, 0, 0, e->time_msec);
  });
  listen(&cursor_->events.touch_cancel, nullptr, [this](void* d) {
    auto* e = static_cast<wlr_touch_cancel_event*>(d);
    touchEvent(e->touch_id, 3, 0, 0, e->time_msec);
  });
  listen(&cursor_->events.touch_frame, nullptr, [this](void*) { wlr_seat_touch_notify_frame(seat_); });

  seat_ = wlr_seat_create(display_, "seat0");
  wlr_seat_set_capabilities(seat_, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD | WL_SEAT_CAPABILITY_TOUCH);
  textInput_.create(display_, [this](bool show) {
    jsui_post_message(rt_, "wm:textInput", show ? "{\"show\":true}" : "{\"show\":false}");
    scheduleTick(0);
  });
  listen(&seat_->events.request_set_selection, nullptr, [this](void* d) {
    auto* e = static_cast<wlr_seat_request_set_selection_event*>(d);
    wlr_seat_set_selection(seat_, e->source, e->serial);
  });

  auto* vptr = wlr_virtual_pointer_manager_v1_create(display_);
  listen(&vptr->events.new_virtual_pointer, nullptr, [this](void* d) {
    auto* e = static_cast<wlr_virtual_pointer_v1_new_pointer_event*>(d);
    newInput(&e->new_pointer->pointer.base);
  });
  auto* vkbd = wlr_virtual_keyboard_manager_v1_create(display_);
  listen(&vkbd->events.new_virtual_keyboard, nullptr, [this](void* d) {
    auto* k = static_cast<wlr_virtual_keyboard_v1*>(d);
    newInput(&k->keyboard.base);
  });
  setupVirtualKeyboard();

  const char* socket = wl_display_add_socket_auto(display_);
  if (!socket) {
    logf("cannot open a Wayland socket");
    return false;
  }
  socketName_ = socket;
  if (!wlr_backend_start(backend_)) {
    logf("cannot start the backend");
    return false;
  }
  // Headless (Docker, CI): no screen until we add one.
  if (!output_) {
    struct Ctx {
      int w, h;
    } ctx{opt_.width, opt_.height};
    auto add = [](wlr_backend* b, void* u) {
      auto* k = static_cast<Ctx*>(u);
      if (wlr_backend_is_headless(b)) wlr_headless_add_output(b, unsigned(k->w), unsigned(k->h));
      // Nested in another compositor (Ubuntu Touch's lomiri-system-compositor,
      // or any desktop): one toplevel of the host is our screen.
      if (wlr_backend_is_wl(b)) {
        if (wlr_output* o = wlr_wl_output_create(b)) wlr_wl_output_set_title(o, "jsui-wl");
      }
    };
    if (wlr_backend_is_multi(backend_)) wlr_multi_for_each_backend(backend_, add, &ctx);
    else add(backend_, &ctx);
  }
  if (!output_) {
    logf("no output");
    return false;
  }
  setenv("WAYLAND_DISPLAY", socket, 1);
  setenv("XDG_SESSION_TYPE", "wayland", 1);
  setenv("GDK_BACKEND", "wayland", 0);
  setenv("QT_QPA_PLATFORM", "wayland", 0);
  setenv("QT_WAYLAND_DISABLE_WINDOWDECORATION", "1", 0);  // the shell draws the frames
  unsetenv("DISPLAY");
  logf("listening on WAYLAND_DISPLAY=%s", socket);
  tickTimer_ = wl_event_loop_add_timer(loop_, [](void* u) -> int {
    static_cast<WM*>(u)->tick();
    return 0;
  }, this);
  setupGpuBoost();
  systemTimer_ = wl_event_loop_add_timer(loop_, [](void* u) -> int {
    auto* w = static_cast<WM*>(u);
    if (w->rt_) w->publishSystem();
    wl_event_source_timer_update(w->systemTimer_, 30000);
    return 0;
  }, this);
  wl_event_source_timer_update(systemTimer_, 30000);
  setupControl();
  return output_ != nullptr;
}

void WM::newOutput(wlr_output* output) {
  if (output_) return;  // one screen
  wlr_output_init_render(output, allocator_, renderer_);
  wlr_output_state st;
  wlr_output_state_init(&st);
  wlr_output_state_set_enabled(&st, true);
  if (wlr_output_mode* mode = wlr_output_preferred_mode(output)) wlr_output_state_set_mode(&st, mode);
  // A nested (wayland backend) output takes no refresh rate.
  else wlr_output_state_set_custom_mode(&st, opt_.width, opt_.height, wlr_output_is_wl(output) ? 0 : 60000);
  wlr_output_commit_state(output, &st);
  wlr_output_state_finish(&st);
  output_ = output;
  wlr_output_layout_add_auto(layout_, output);
  sceneOutput_ = wlr_scene_output_create(scene_, output);
  wlr_scene_output_layout_add_output(sceneLayout_, wlr_output_layout_get(layout_, output), sceneOutput_);
  sw_ = output->width;
  sh_ = output->height;
  scale_ = opt_.scale > 0 ? opt_.scale : sw_ >= 1800 ? 1.5f : sw_ >= 1500 ? 1.25f : 1.0f;
  logf("output %s %dx%d px, scale %.2f (%dx%d logical)", output->name, sw_, sh_, scale_, int(L(sw_)), int(L(sh_)));
  // Apps render at scale 1 (no fractional-scale protocol here); GTK scales its
  // text to the shell's scale instead.
  setenv("GDK_DPI_SCALE", std::to_string(scale_).c_str(), 0);

  allocFrame();
  shellNode_ = wlr_scene_buffer_create(&scene_->tree, &frame_->base);

  listen(&output->events.frame, nullptr, [this](void*) { onFrame(); });
  // The host compositor (wayland backend) sizes our toplevel: follow it.
  listen(&output->events.request_state, nullptr, [this, output](void* d) {
    auto* e = static_cast<wlr_output_event_request_state*>(d);
    wlr_output_commit_state(output, e->state);
    if (output->width != sw_ || output->height != sh_) resizeScreen(output->width, output->height);
  });
}

void WM::allocFrame() {
  if (frame_) wlr_buffer_drop(&frame_->base);
  frame_ = new FrameBuffer;
  frame_->stride = sw_ * 4;
  frame_->data.assign(size_t(frame_->stride) * size_t(sh_), 0);
  wlr_buffer_init(&frame_->base, &kFrameBufferImpl, sw_, sh_);
}

void WM::resizeScreen(int w, int h) {
  if (w <= 0 || h <= 0) return;
  logf("screen resized to %dx%d px", w, h);
  sw_ = w;
  sh_ = h;
  if (opt_.scale <= 0) scale_ = sw_ >= 1800 ? 1.5f : sw_ >= 1500 ? 1.25f : 1.0f;
  allocFrame();
  wlr_scene_buffer_set_buffer(shellNode_, &frame_->base);
  if (!rt_) return;
  jsui_set_viewport(rt_, int(std::lround(sw_ / scale_)), int(std::lround(sh_ / scale_)), scale_);
  publishScreen();
  Rect wa = workArea();
  for (auto& [id, c] : clients_) {
    if (c->popup || !c->mapped) continue;
    if (c->fullscreen) configure(c, 0, 0, sw_, sh_);
    else if (c->maximized) configure(c, wa.x, wa.y, wa.w, wa.h);
    else if (c->snap) setSnap(c, c->snap);
  }
  scheduleFrame();
}

void WM::newInput(wlr_input_device* dev) {
  switch (dev->type) {
    case WLR_INPUT_DEVICE_KEYBOARD: newKeyboard(wlr_keyboard_from_input_device(dev)); break;
    case WLR_INPUT_DEVICE_POINTER:
    case WLR_INPUT_DEVICE_TOUCH: wlr_cursor_attach_input_device(cursor_, dev); break;
    default: break;
  }
}

void WM::newKeyboard(wlr_keyboard* kb) {
  if (!kb->keymap) {
    xkb_context* ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    xkb_keymap* km = xkb_keymap_new_from_names(ctx, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
    wlr_keyboard_set_keymap(kb, km);
    xkb_keymap_unref(km);
    xkb_context_unref(ctx);
  }
  wlr_keyboard_set_repeat_info(kb, 30, 400);
  listen(&kb->events.key, nullptr, [this, kb](void* d) {
    auto* e = static_cast<wlr_keyboard_key_event*>(d);
    keyboardKey(kb, e->keycode, e->state == WL_KEYBOARD_KEY_STATE_PRESSED, e->time_msec);
  });
  listen(&kb->events.modifiers, nullptr, [this, kb](void*) {
    wlr_seat_set_keyboard(seat_, kb);
    wlr_seat_keyboard_notify_modifiers(seat_, &kb->modifiers);
  });
  wlr_seat_set_keyboard(seat_, kb);
}

// The on-screen keyboard types through this device: a plain US keymap.
void WM::setupVirtualKeyboard() {
  static const wlr_keyboard_impl impl = {"jsui-osk", nullptr};
  wlr_keyboard_init(&vkbd_, &impl, "jsui-osk");
  xkb_context* ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  keymap_ = xkb_keymap_new_from_names(ctx, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
  wlr_keyboard_set_keymap(&vkbd_, keymap_);
  xkb_context_unref(ctx);
  wlr_seat_set_keyboard(seat_, &vkbd_);
}

// ------------------------------------------------------------ clients

Client* WM::find(uint32_t id) {
  auto it = clients_.find(id);
  return it == clients_.end() ? nullptr : it->second;
}

Client* WM::findSurface(wlr_surface* s) {
  if (!s) return nullptr;
  wlr_surface* root = wlr_surface_get_root_surface(s);
  for (auto& [id, c] : clients_)
    if (c->xdg && c->xdg->surface == root) return c;
  return nullptr;
}

void WM::newToplevel(wlr_xdg_surface* xdg) {
  auto* c = new Client;
  c->id = nextId_++;
  c->xdg = xdg;
  c->tl = xdg->toplevel;
  clients_[c->id] = c;
  wl_client_get_credentials(wl_resource_get_client(xdg->resource), &c->pid, nullptr, nullptr);
  c->hooks.push_back(hook(&xdg->surface->events.map, [this, c](void*) { onMap(c); }));
  c->hooks.push_back(hook(&xdg->surface->events.unmap, [this, c](void*) { onUnmap(c); }));
  c->hooks.push_back(hook(&xdg->surface->events.commit, [this, c](void*) { onCommit(c); }));
  c->hooks.push_back(hook(&xdg->events.destroy, [this, c](void*) { onDestroy(c); }));
  c->hooks.push_back(hook(&c->tl->events.set_title, [this, c](void*) {
    c->title = c->tl->title ? c->tl->title : "";
    publishWindow(c);
  }));
  c->hooks.push_back(hook(&c->tl->events.set_app_id, [this, c](void*) {
    c->appIdRaw = c->tl->app_id ? c->tl->app_id : "";
    matchApp(c);
    publishWindow(c);
  }));
  c->hooks.push_back(hook(&c->tl->events.set_parent, [this, c](void*) {
    Client* p = c->tl->parent ? findSurface(c->tl->parent->base->surface) : nullptr;
    c->transientFor = p ? p->id : 0;
    publishWindow(c);
  }));
  c->hooks.push_back(hook(&c->tl->events.request_maximize, [this, c](void*) {
    if (c->xdg->initialized) setMaximized(c, c->tl->requested.maximized);
  }));
  c->hooks.push_back(hook(&c->tl->events.request_fullscreen, [this, c](void*) {
    if (c->xdg->initialized) setFullscreen(c, c->tl->requested.fullscreen);
  }));
  c->hooks.push_back(hook(&c->tl->events.request_minimize, [this, c](void*) {
    if (c->mapped) minimize(c);
  }));
  c->hooks.push_back(hook(&c->tl->events.request_move, [this, c](void*) {
    if (!pointerDown_ || c->maximized || c->fullscreen) return;
    mrId_ = c->id;
    mrEdges_ = 0;
    mrX_ = cursor_->x;
    mrY_ = cursor_->y;
    mrStart_ = {c->x, c->y, c->w, c->h};
  }));
  c->hooks.push_back(hook(&c->tl->events.request_resize, [this, c](void* d) {
    auto* e = static_cast<wlr_xdg_toplevel_resize_event*>(d);
    if (!pointerDown_ || c->maximized || c->fullscreen) return;
    mrId_ = c->id;
    mrEdges_ = e->edges;
    mrX_ = cursor_->x;
    mrY_ = cursor_->y;
    mrStart_ = {c->x, c->y, c->w, c->h};
  }));
}

void WM::newPopup(wlr_xdg_popup* popup) {
  auto* c = new Client;
  c->id = nextId_++;
  c->popup = true;
  c->xdg = popup->base;
  c->xpopup = popup;
  Client* parent = findSurface(popup->parent);
  c->parentId = parent ? parent->id : 0;
  clients_[c->id] = c;
  // Keep menus on screen: the box is relative to the toplevel's geometry.
  if (parent) {
    Client* top = parent;
    while (top && top->popup) top = find(top->parentId);
    if (top) {
      // The screen in the toplevel's surface coordinates (through its fit).
      const double f = top->fit > 0 ? top->fit : 1;
      wlr_box box{int(std::lround(-top->x / f + top->geoX)), int(std::lround(-top->y / f + top->geoY)),
                  int(std::lround(sw_ / f)), int(std::lround(sh_ / f))};
      wlr_xdg_popup_unconstrain_from_box(popup, &box);
    }
  }
  c->hooks.push_back(hook(&c->xdg->surface->events.map, [this, c](void*) { onMap(c); }));
  c->hooks.push_back(hook(&c->xdg->surface->events.unmap, [this, c](void*) { onUnmap(c); }));
  c->hooks.push_back(hook(&c->xdg->surface->events.commit, [this, c](void*) { onCommit(c); }));
  c->hooks.push_back(hook(&c->xdg->events.destroy, [this, c](void*) { onDestroy(c); }));
}

void WM::matchApp(Client* c) {
  c->appId.clear();
  std::string want = lower(c->appIdRaw);
  if (want.empty()) return;
  for (const App& a : apps_) {
    std::string id = lower(a.id);
    if (id == want || lower(a.wmClass) == want || id == want + ".desktop" ||
        (id.size() > want.size() && id.compare(id.size() - want.size(), want.size(), want) == 0 &&
         id[id.size() - want.size() - 1] == '.') ||
        lower(a.name) == want) {
      c->appId = a.id;
      return;
    }
  }
  // "foot" -> "org.codeberg.dnkl.foot" style ids, or a program the exec runs:
  // Ubuntu Touch apps' ids end in their binary's name
  // ("ubports.terminal.lomiri-terminal-app" for a click app whose exec ends
  // in "lomiri-terminal-app"; scanApps prefixes env, cd and aa-exec).
  // Several desktop files can run one binary (Camera and Barcode Reader):
  // the one the window was launched from wins.
  std::string last = want.substr(want.rfind('.') == std::string::npos ? 0 : want.rfind('.') + 1);
  const App* found = nullptr;
  for (const App& a : apps_) {
    std::istringstream words(a.exec);
    std::string w;
    bool runs = false;
    while (!runs && words >> w) {
      std::string exe = lower(w.substr(w.rfind('/') == std::string::npos ? 0 : w.rfind('/') + 1));
      runs = exe == want || exe == last;
    }
    if (!runs) continue;
    auto l = launched_.find(a.exec);
    if (l != launched_.end() && c->pid > 0 && descends(c->pid, l->second)) {
      found = &a;
      break;
    }
    if (!found) found = &a;
  }
  if (found) c->appId = found->id;
}

Rect WM::workArea() const {
  Rect r;
  r.x = 0;
  r.y = P(topBar_ + titleBar_);
  r.w = sw_;
  r.h = sh_ - P(tabletMode_ ? homeBar_ : dock_) - r.y;
  // Apps make room for the on-screen keyboard (wm:osk).
  if (oskH_ > 0) r.h = std::min(r.h, sh_ - oskH_ - r.y);
  return r;
}

void WM::place(Client* c) {
  c->placed = true;
  c->minW = std::max(1, c->tl->current.min_width);
  c->minH = std::max(1, c->tl->current.min_height);
  c->maxW = c->tl->current.max_width;
  c->maxH = c->tl->current.max_height;
  c->dialog = c->tl->parent != nullptr || (c->maxW > 0 && c->maxW == c->minW && c->maxH == c->minH);
  wlr_box geo;
  wlr_xdg_surface_get_geometry(c->xdg, &geo);
  int w = std::max(geo.width, P(280)), h = std::max(geo.height, P(160));
  Rect wa = workArea();
  if (c->tl->parent) {
    if (Client* p = findSurface(c->tl->parent->base->surface)) {
      c->x = p->x + (p->w - w) / 2;
      c->y = p->y + (p->h - h) / 2;
    }
  } else {
    c->x = wa.x + P(60) + P(36) * (cascade_ % 6);
    c->y = wa.y + P(24) + P(36) * (cascade_ % 6);
    ++cascade_;
  }
  c->x = std::clamp(c->x, 0, std::max(0, sw_ - w));
  c->y = std::clamp(c->y, wa.y, std::max(wa.y, wa.y + wa.h - h));
  if (tabletMode_ && !c->dialog) {
    c->restore = {c->x, c->y, std::min(w, wa.w), std::min(h, wa.h)};
    c->hasRestore = true;
    c->maximized = true;
    c->x = wa.x;
    c->y = wa.y;
    w = wa.w;
    h = wa.h;
    wlr_xdg_toplevel_set_maximized(c->tl, true);
  }
  c->w = w;
  c->h = h;
  wlr_xdg_toplevel_set_size(c->tl, w, h);
}

void WM::configure(Client* c, int x, int y, int w, int h) {
  if (c->maxW > 0) w = std::min(w, c->maxW);
  if (c->maxH > 0) h = std::min(h, c->maxH);
  w = std::max({1, w, c->minW});
  h = std::max({1, h, c->minH});
  c->x = x;
  c->y = y;
  if (w != c->w || h != c->h) {
    VLOG("configure %u %dx%d", c->id, w, h);
    wlr_xdg_toplevel_set_size(c->tl, w, h);
  }
  c->w = w;
  c->h = h;
  if (c->maximized && c->mapped && !c->popup) {
    // Until it commits the new size it shows the one it has, fitted to the
    // work area (a client that keeps its own size may never commit one:
    // after a turn of the screen it would hang off the edge).
    wlr_box geo;
    wlr_xdg_surface_get_geometry(c->xdg, &geo);
    if (geo.width > 0 && geo.height > 0) {
      c->w = geo.width;
      c->h = geo.height;
    }
    fitMaximized(c);
  }
  publishWindow(c);
}

// A maximized window whose client keeps a size of its own (Ubuntu Touch's
// Clock draws 1210x1540 whatever it is told): narrower than the work area,
// centred in it; bigger, scaled down to fit (letterboxed), centred, with
// input mapped back (see fit). Anything else is shown 1:1.
// Screen px -> the client's surface coordinates (its fit, see Client::fit).
void WM::toSurface(const Client* c, double* x, double* y) const {
  if (c->popup) {
    *x = (*x - c->x) / c->fit;
    *y = (*y - c->y) / c->fit;
  } else {
    *x = (*x - c->x) / c->fit + c->geoX;
    *y = (*y - c->y) / c->fit + c->geoY;
  }
}

void WM::fitMaximized(Client* c) {
  c->fit = 1;
  if (c->popup || !c->maximized || c->snap || c->w <= 0 || c->h <= 0) return;
  Rect wa = workArea();
  c->fit = std::min({1.0, double(wa.w) / c->w, double(wa.h) / c->h});
  c->x = wa.x + std::max(0, int(std::lround((wa.w - c->w * c->fit) / 2)));
  c->y = wa.y + std::max(0, int(std::lround((wa.h - c->h * c->fit) / 2)));
}

void WM::onCommit(Client* c) {
  if (!c->xdg->initialized) return;
  if (!c->popup && !c->placed && c->xdg->initial_commit) place(c);
  if (!c->popup && !c->placed) {
    place(c);
  }
  if (!c->mapped) return;
  wlr_box geo;
  wlr_xdg_surface_get_geometry(c->xdg, &geo);
  c->geoX = geo.x;
  c->geoY = geo.y;
  if (!c->popup && (geo.width != c->w || geo.height != c->h) && geo.width > 0 && geo.height > 0) {
    // The client picked its own size (or follows our configure).
    VLOG("commit %u %dx%d (was %dx%d)", c->id, geo.width, geo.height, c->w, c->h);
    c->w = geo.width;
    c->h = geo.height;
    fitMaximized(c);
    publishWindow(c);
    // A maximized client that resized itself (Camera's QML binds its width
    // to the viewfinder's aspect) is told the work area again, at most once
    // a second; one that keeps its own size anyway stays fitted (above).
    Rect wa = workArea();
    if (c->maximized && !c->snap && !c->fullscreen && (geo.width != wa.w || geo.height != wa.h) &&
        (c->maxW <= 0 || c->maxW >= wa.w) && (c->maxH <= 0 || c->maxH >= wa.h) && nowMs() - c->reassertedAt > 1000) {
      c->reassertedAt = nowMs();
      wlr_xdg_toplevel_set_size(c->tl, wa.w, wa.h);
    }
  }
  if (c->popup) {
    Rect r = popupRect(c);
    double f = popupFit(c);
    if (r.x != c->x || r.y != c->y || r.w != c->w || r.h != c->h || f != c->fit) {
      c->x = r.x;
      c->y = r.y;
      c->w = r.w;
      c->h = r.h;
      c->fit = f;
      publishPopups();
    }
  }
  capture(c);
  scheduleFrame();
}

// A popup's place on screen (surface rect, not cropped to its geometry).
Rect WM::popupRect(Client* p) const {
  int x = 0, y = 0;
  wlr_xdg_popup* pop = p->xpopup;
  Rect r;
  // Walk up to the toplevel, adding each popup's position.
  int px = 0, py = 0;
  wlr_xdg_popup_get_toplevel_coords(pop, pop->current.geometry.x - pop->base->current.geometry.x,
                                    pop->current.geometry.y - pop->base->current.geometry.y, &px, &py);
  Client* top = nullptr;
  for (Client* c = const_cast<WM*>(this)->find(p->parentId); c; c = const_cast<WM*>(this)->find(c->parentId)) {
    if (!c->popup) {
      top = c;
      break;
    }
  }
  // A letterboxed toplevel (fit < 1) scales its popups with it.
  if (top) {
    x = top->x + int(std::lround((px - top->geoX) * top->fit));
    y = top->y + int(std::lround((py - top->geoY) * top->fit));
  }
  r.x = x;
  r.y = y;
  r.w = p->xdg->surface->current.width;
  r.h = p->xdg->surface->current.height;
  return r;
}

// A popup's scale: its toplevel's fit.
double WM::popupFit(Client* p) {
  for (Client* c = find(p->parentId); c; c = find(c->parentId))
    if (!c->popup) return c->fit;
  return 1;
}

void WM::capture(Client* c) {
  // No buffer (a closing app's last commit): keep the last frame, which the
  // shell animates out. Capturing would swap it for an empty texture, and
  // the window blinked out for a few frames before its closing copy showed.
  if (!c->xdg->surface->current.buffer) {
    if (perf_) logf("commit %u without a buffer at %.0f", c->id, nowMs());
    return;
  }
  if (gpuApps_ && captureGpu(c)) return;
  if (gpuBuffers_.count(c->id)) removeWindowTexture(c->id);  // back on shm
  wlr_surface* root = c->xdg->surface;
  int ox = c->popup ? 0 : -c->geoX, oy = c->popup ? 0 : -c->geoY;
  int w = c->popup ? root->current.width : c->w, h = c->popup ? root->current.height : c->h;
  if (w <= 0 || h <= 0) return;
  if (c->texW != w || c->texH != h) {
    c->rgba.assign(size_t(w) * size_t(h) * 4, 0);
    c->texW = w;
    c->texH = h;
  } else {
    std::fill(c->rgba.begin(), c->rgba.end(), 0);
  }
  pixman_image_t* dst = pixman_image_create_bits_no_clear(PIXMAN_a8b8g8r8, w, h,
                                                         reinterpret_cast<uint32_t*>(c->rgba.data()), w * 4);
  struct Ctx {
    pixman_image_t* dst;
    int ox, oy;
    bool any;
  } ctx{dst, ox, oy, false};
  wlr_surface_for_each_surface(
      root,
      [](wlr_surface* s, int sx, int sy, void* u) {
        auto* k = static_cast<Ctx*>(u);
        wlr_texture* tex = wlr_surface_get_texture(s);
        if (!tex || !wlr_texture_is_pixman(tex)) return;
        pixman_image_t* src = wlr_pixman_texture_get_image(tex);
        if (!src) return;
        int bw = pixman_image_get_width(src), bh = pixman_image_get_height(src);
        int dw = s->current.width, dh = s->current.height;
        // HiDPI buffers (buffer_scale > 1): scale down to surface size.
        pixman_transform_t t;
        pixman_transform_init_scale(&t, pixman_double_to_fixed(double(bw) / std::max(1, dw)),
                                    pixman_double_to_fixed(double(bh) / std::max(1, dh)));
        bool scaled = bw != dw || bh != dh;
        if (scaled) {
          pixman_image_set_transform(src, &t);
          pixman_image_set_filter(src, PIXMAN_FILTER_BILINEAR, nullptr, 0);
        }
        pixman_image_composite32(k->any ? PIXMAN_OP_OVER : PIXMAN_OP_SRC, src, nullptr, k->dst, 0, 0, 0, 0,
                                 sx + k->ox, sy + k->oy, dw, dh);
        if (scaled) pixman_image_set_transform(src, nullptr);
        k->any = true;
      },
      &ctx);
  pixman_image_unref(dst);
  if (!ctx.any) return;
  // Opaque windows: no alpha holes from x8 buffers (pixman fills x8 as 0xff on SRC).
  jsui_set_texture_pixels(rt_, ("win:" + std::to_string(c->id)).c_str(), c->rgba.data(), w, h, size_t(w) * 4, 0);
  if (!c->hasTexture) {
    c->hasTexture = true;
    if (c->popup) publishPopups();
    else publishWindow(c);
  }
  scheduleTick(0);
}

// A window drawn on the GPU (android_wlegl): its buffer's texture becomes the
// window's host texture as is. Subsurfaces aren't composited on this path
// (Qt's GL windows have none).
bool WM::captureGpu(Client* c) {
#ifdef JSUI_WL_MIR_GL
  wlr_buffer* buf = c->xdg->surface->current.buffer;
  wlegl::Buffer* b = wlegl::from(buf);
  if (!b || !mir_->makeCurrent()) return false;
  GLuint tex = wlegl::texture(b);
  if (!tex) return false;
  JsuiTexture t{};
  t.kind = JSUI_TEXTURE_GL;
  t.handle = reinterpret_cast<void*>(uintptr_t(tex));
  t.width = buf->width;
  t.height = buf->height;
  // Toplevels of GPU apps are opaque (Qt's window background; no compositor
  // translucency on Ubuntu Touch): JSUI then culls what they cover.
  t.flags = b->opaque || !c->popup ? JSUI_TEXTURE_OPAQUE : 0;
  std::string key = "win:" + std::to_string(c->id);
  if (perfEvery_) logf("app commit %u at %.0f", c->id, nowMs());
  if (jsui_set_texture(rt_, key.c_str(), &t) == 0) jsui_texture_updated(rt_, key.c_str());
  wlr_buffer* held = gpuBuffers_[c->id];
  if (held != buf) {
    gpuBuffers_[c->id] = wlr_buffer_lock(buf);
    if (held) wlr_buffer_unlock(held);
  }
  c->texW = buf->width;
  c->texH = buf->height;
  c->rgba.clear();
  if (!c->hasTexture) {
    c->hasTexture = true;
    if (c->popup) publishPopups();
    else publishWindow(c);
  }
  scheduleTick(0);
  return true;
#else
  (void)c;
  return false;
#endif
}

// A short buzz through Ubuntu Touch's haptics daemon (hfd), fire and
// forget; nothing where there is none (busctl fails quietly).
void WM::haptic(int ms) {
#ifdef JSUI_WL_MIR
  if (!mir_) return;
  const double now = nowMs();
  if (now - lastHaptic_ < 60) return;
  lastHaptic_ = now;
  ms = std::clamp(ms, 5, 200);
  pid_t pid = fork();
  if (pid == 0) {
    signal(SIGCHLD, SIG_DFL);
    int nul = open("/dev/null", O_RDWR);
    if (nul >= 0) dup2(nul, 1), dup2(nul, 2);
    std::string d = std::to_string(ms);
    execlp("busctl", "busctl", "--system", "call", "com.lomiri.hfd", "/com/lomiri/hfd", "com.lomiri.hfd.Vibrator",
           "vibrate", "i", d.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
#else
  (void)ms;
#endif
}

namespace {
// Runs argv detached, output discarded (fire and forget).
void spawnQuiet(const std::vector<std::string>& argv) {
  pid_t pid = fork();
  if (pid != 0) return;
  signal(SIGCHLD, SIG_DFL);
  int nul = open("/dev/null", O_RDWR);
  if (nul >= 0) dup2(nul, 1), dup2(nul, 2);
  std::vector<char*> a;
  for (const auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
  a.push_back(nullptr);
  execvp(a[0], a.data());
  _exit(127);
}

// The first backlight's level (0..1), or -1.
double backlightLevel(int* max = nullptr) {
  DIR* d = opendir("/sys/class/backlight");
  if (!d) return -1;
  double level = -1;
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    std::string base = std::string("/sys/class/backlight/") + e->d_name;
    std::ifstream bf(base + "/brightness"), mf(base + "/max_brightness");
    int b = -1, m = 0;
    if (bf >> b && mf >> m && m > 0) {
      level = std::clamp(double(b) / m, 0.0, 1.0);
      if (max) *max = m;
      break;
    }
  }
  closedir(d);
  return level;
}

// A command's first line of output ("" when it fails).
std::string firstLine(const char* cmd) {
  std::string out;
  if (FILE* f = popen(cmd, "r")) {
    char buf[512];
    if (fgets(buf, sizeof buf, f)) out = buf;
    pclose(f);
  }
  return out;
}
}  // namespace

void WM::readStartLevels() {
#ifdef JSUI_WL_MIR
  if (!mir_) {
    backlightRead_ = volumeRead_ = true;
    return;
  }
  // Only a lit screen's level is the user's (off reads 0, and repowerd dims
  // it before it goes off): with the screen off at start, "backlight" is -1
  // (there is one, level unknown) and the next touch or refresh
  // (systemTimer_, 30 s) tries again.
  if (!backlightRead_) {
    const double b = backlightLevel();
    if (b < 0) {
      backlightRead_ = true;  // no backlight
    } else if (b > 0) {
      backlightRead_ = true;
      startBacklight_ = ",\"backlight\":" + std::to_string(b);
    } else {
      startBacklight_ = ",\"backlight\":-1";  // there is one; its level comes later
    }
  }
  if (volumeRead_) return;
  volumeRead_ = true;
  // "Volume: front-left: 39321 /  60% / -13.31 dB, ..." and "Mute: no".
  std::string vol = firstLine("pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null");
  std::string mute = firstLine("pactl get-sink-mute @DEFAULT_SINK@ 2>/dev/null");
  size_t pct = vol.find('%');
  if (pct != std::string::npos) {
    size_t start = vol.find_last_not_of("0123456789", pct - 1);
    int v = atoi(vol.c_str() + (start == std::string::npos ? 0 : start + 1));
    volumeNow_ = std::clamp(v, 0, 100) / 100.0;
    startVolume_ = ",\"volume\":{\"level\":" + std::to_string(std::clamp(v, 0, 100) / 100.0) +
                    ",\"muted\":" + (mute.find("yes") != std::string::npos ? "true" : "false") + "}";
  }
#endif
}

void WM::setLevels(const wmjson::Value& v, bool brightness) {
  if (brightness) {
    wantLevels_.brightness = std::clamp(v["value"].num(1), 0.0, 1.0);
  } else {
    wantLevels_.volume = std::clamp(v["value"].num(0.6), 0.0, 1.0);
    wantLevels_.muted = v["muted"].truthy() ? 1 : 0;
    volumeNow_ = wantLevels_.volume;
  }
  if (!levelsTimer_)
    levelsTimer_ = wl_event_loop_add_timer(loop_, [](void* u) -> int {
      static_cast<WM*>(u)->applyLevels();
      return 0;
    }, this);
  // Pending already: the timer applies the latest values.
  if (wantLevels_.brightness != appliedLevels_.brightness || wantLevels_.volume != appliedLevels_.volume ||
      wantLevels_.muted != appliedLevels_.muted)
    wl_event_source_timer_update(levelsTimer_, 80);
}

void WM::volumeStep(int dir) {
  if (!volumeRead_) readStartLevels();
  double cur = volumeNow_ >= 0 ? volumeNow_ : 0.5;
  // To the next step of 1/16, so a level from the slider lands on the grid.
  double next = std::clamp((std::round(cur * 16) + dir) / 16.0, 0.0, 1.0);
  wmjson::Value v;
  v.type = wmjson::Value::Object;
  wmjson::Value num;
  num.type = wmjson::Value::Number;
  num.n = next;
  v.o["value"] = num;
  setLevels(v, false);
  applyLevels();  // a button press: now, not after the drag coalescing
  if (rt_) jsui_post_message(rt_, "wm:volumeKey", ("{\"level\":" + std::to_string(next) + "}").c_str());
}

void WM::applyLevels() {
#ifdef JSUI_WL_MIR
  if (!mir_) return;
  Levels w = wantLevels_, &a = appliedLevels_;
  if (w.brightness >= 0 && w.brightness != a.brightness) {
    int max = 0;
    if (backlightLevel(&max) >= 0 && max > 0) {
      // Never off: the slider's bottom is the dimmest lit level.
      const int level = std::max(int(std::lround(w.brightness * max)), std::max(1, max / 25));
      spawnQuiet({"busctl", "--system", "call", "com.canonical.Unity.Screen", "/com/canonical/Unity/Screen",
                  "com.canonical.Unity.Screen", "setUserBrightness", "i", std::to_string(level)});
    }
    a.brightness = w.brightness;
  }
  if (w.volume >= 0 && w.volume != a.volume) {
    spawnQuiet({"pactl", "set-sink-volume", "@DEFAULT_SINK@", std::to_string(int(std::lround(w.volume * 100))) + "%"});
    a.volume = w.volume;
  }
  if (w.muted >= 0 && w.muted != a.muted) {
    spawnQuiet({"pactl", "set-sink-mute", "@DEFAULT_SINK@", w.muted ? "1" : "0"});
    a.muted = w.muted;
  }
#endif
}

void WM::snapshotBackdrop(bool on) {
  if (!on) {
    jsui_remove_texture(rt_, "wm:backdrop");
    jsui_remove_texture(rt_, "wm:under");
    return;
  }
#ifdef JSUI_WL_MIR_GL
  if (!snapshotBg_ || !gpu_ || vulkan_ || !mir_->makeCurrent()) return;
  // The desktop texture still holds the last frame: what the panel opens over.
  MirHost::Snapshot snap;
  if (!mir_->snapshotDesktop(&snap)) return;
  auto set = [&](const char* key, unsigned tex, int w, int h) {
    JsuiTexture t{};
    t.kind = JSUI_TEXTURE_GL;
    t.handle = reinterpret_cast<void*>(uintptr_t(tex));
    t.width = w;
    t.height = h;
    t.flags = JSUI_TEXTURE_OPAQUE | JSUI_TEXTURE_BOTTOM_LEFT_ORIGIN;
    if (jsui_set_texture(rt_, key, &t) == 0) jsui_texture_updated(rt_, key);
  };
  set("wm:under", snap.full, snap.fullW, snap.fullH);
  set("wm:backdrop", snap.blurred, snap.blurW, snap.blurH);
#endif
}

bool WM::descends(pid_t pid, pid_t ancestor) const {
  for (int depth = 0; pid > 1 && depth < 8; ++depth) {
    if (pid == ancestor) return true;
    pid = parentPid(pid);
  }
  return false;
}

void WM::checkLaunches() {
  constexpr double kLaunchTimeoutMs = 25000;
  const double now = nowMs();
  for (auto it = pendingLaunches_.begin(); it != pendingLaunches_.end();) {
    bool shown = false;
    for (auto& [id, c] : clients_)
      if (!c->popup && c->mapped && c->pid > 0 && descends(c->pid, it->pid)) shown = true;
    // Gone without a window (handed over to a running instance, or failed
    // on its own and said so on stderr): nothing to do.
    if (shown || kill(it->pid, 0) != 0) {
      // The shell's loading window goes (wm:launchEnded) also when the app
      // handed over to a running instance and showed no window of its own.
      std::string j = "{\"exec\":" + wmjson::quoted(it->exec) + ",\"shown\":" + (shown ? "true" : "false") + "}";
      jsui_post_message(rt_, "wm:launchEnded", j.c_str());
      it = pendingLaunches_.erase(it);
      continue;
    }
    if (now - it->at < kLaunchTimeoutMs) {
      ++it;
      continue;
    }
    logf("launch: %s showed no window in %.0f s, ending it", it->name.c_str(), kLaunchTimeoutMs / 1000);
    kill(-it->pid, SIGKILL);  // its session (launch() setsid()s)
    std::string j = "{\"name\":" + wmjson::quoted(it->name) + ",\"exec\":" + wmjson::quoted(it->exec) + "}";
    jsui_post_message(rt_, "wm:launchFailed", j.c_str());
    scheduleTick(0);
    it = pendingLaunches_.erase(it);
  }
  if (!pendingLaunches_.empty()) wl_event_source_timer_update(launchTimer_, 5000);
}

void WM::removeWindowTexture(uint32_t id) {
  jsui_remove_texture(rt_, ("win:" + std::to_string(id)).c_str());
  auto it = gpuBuffers_.find(id);
  if (it == gpuBuffers_.end()) return;
  if (it->second) wlr_buffer_unlock(it->second);
  gpuBuffers_.erase(it);
}

void WM::onMap(Client* c) {
  c->mapped = true;
  c->mappedAt = nowMs();
  if (c->popup) {
    Rect r = popupRect(c);
    c->x = r.x;
    c->y = r.y;
    c->w = r.w;
    c->h = r.h;
    c->fit = popupFit(c);
    popups_.push_back(c->id);
    capture(c);
    publishPopups();
    return;
  }
  c->title = c->tl->title ? c->tl->title : "";
  c->appIdRaw = c->tl->app_id ? c->tl->app_id : "";
  matchApp(c);
  wlr_box geo;
  wlr_xdg_surface_get_geometry(c->xdg, &geo);
  c->geoX = geo.x;
  c->geoY = geo.y;
  if (geo.width > 0) c->w = geo.width;
  if (geo.height > 0) c->h = geo.height;
  fitMaximized(c);
  stack_.push_back(c->id);
  logf("map %u \"%s\" (%s) %dx%d", c->id, c->title.c_str(), c->appIdRaw.c_str(), c->w, c->h);
  capture(c);
  publishWindow(c);
  publishStack();
  focus(c);
}

void WM::onUnmap(Client* c) {
  c->mapped = false;
  if (c->popup) {
    popups_.erase(std::remove(popups_.begin(), popups_.end(), c->id), popups_.end());
    publishPopups();
    removeWindowTexture(c->id);
    return;
  }
  stack_.erase(std::remove(stack_.begin(), stack_.end(), c->id), stack_.end());
  if (mrId_ == c->id) mrId_ = 0;
  // The shell animates the last frame out, then releases it (wm:release).
  releaseAt_[c->id] = nowMs() + 1500;
  if (perf_) frameLog_ = 24;  // JSUI_WL_PERF: every frame of the close animation
  if (perf_) logf("unmap %u at %.0f", c->id, nowMs());
  std::string j = "{\"" + std::to_string(c->id) + "\":null}";
  jsui_patch_state(rt_, "wm.windows", j.c_str());
  publishStack();
  if (active_ == c->id) {
    active_ = 0;
    focusTop();
  }
  c->hasTexture = false;
  c->placed = false;
}

void WM::onDestroy(Client* c) {
  if (c->mapped) onUnmap(c);
  unhook(c->hooks);
  clients_.erase(c->id);
  delete c;
}

void WM::raise(Client* c) {
  auto it = std::find(stack_.begin(), stack_.end(), c->id);
  if (it == stack_.end()) return;
  stack_.erase(it);
  stack_.push_back(c->id);
  // Dialogs stay above their parent.
  for (auto& [id, d] : clients_)
    if (d->mapped && !d->popup && d->transientFor == c->id) {
      stack_.erase(std::remove(stack_.begin(), stack_.end(), id), stack_.end());
      stack_.push_back(id);
    }
  publishStack();
}

void WM::focus(Client* c) {
  Client* prev = find(active_);
  if (prev && prev != c && prev->tl) wlr_xdg_toplevel_set_activated(prev->tl, false);
  active_ = c ? c->id : 0;
  textInput_.focus(c && c->tl ? c->xdg->surface->resource : nullptr);
  if (c && c->tl) {
    wlr_xdg_toplevel_set_activated(c->tl, true);
    wlr_keyboard* kb = wlr_seat_get_keyboard(seat_);
    if (kb) {
      wlr_seat_keyboard_notify_enter(seat_, c->xdg->surface, kb->keycodes, kb->num_keycodes, &kb->modifiers);
    }
  } else {
    wlr_seat_keyboard_notify_clear_focus(seat_);
  }
  publishActive();
}

void WM::focusTop() {
  for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
    Client* c = find(*it);
    if (c && c->mapped && !c->minimized) {
      focus(c);
      return;
    }
  }
  focus(nullptr);
}

void WM::close(Client* c) {
  if (c->tl) wlr_xdg_toplevel_send_close(c->tl);
}

void WM::minimize(Client* c) {
  if (c->minimized) return;
  c->minimized = true;
  publishWindow(c);
  if (active_ == c->id) focusTop();
}

void WM::unminimize(Client* c) {
  if (!c->minimized) return;
  c->minimized = false;
  publishWindow(c);
}

void WM::setMaximized(Client* c, bool on) {
  if (on) {
    if (!c->maximized && !c->snap) {
      c->restore = {c->x, c->y, c->w, c->h};
      c->hasRestore = true;
    }
    c->snap = 0;
    c->maximized = true;
    wlr_xdg_toplevel_set_tiled(c->tl, 0);
    wlr_xdg_toplevel_set_maximized(c->tl, true);
    Rect wa = workArea();
    configure(c, wa.x, wa.y, wa.w, wa.h);
  } else {
    c->maximized = false;
    wlr_xdg_toplevel_set_maximized(c->tl, false);
    if (c->hasRestore) configure(c, c->restore.x, c->restore.y, c->restore.w, c->restore.h);
  }
  fitMaximized(c);
  publishWindow(c);
}

void WM::setSnap(Client* c, int side) {
  if (side == 0) {
    c->snap = 0;
    wlr_xdg_toplevel_set_tiled(c->tl, 0);
    if (c->hasRestore) configure(c, c->restore.x, c->restore.y, c->restore.w, c->restore.h);
    publishWindow(c);
    return;
  }
  if (!c->maximized && !c->snap) {
    c->restore = {c->x, c->y, c->w, c->h};
    c->hasRestore = true;
  }
  c->maximized = false;
  c->snap = side;
  wlr_xdg_toplevel_set_maximized(c->tl, false);
  wlr_xdg_toplevel_set_tiled(c->tl, WLR_EDGE_TOP | WLR_EDGE_BOTTOM | (side == 1 ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT));
  Rect wa = workArea();
  int left = int(std::lround(wa.w * splitRatio_)) - P(5);
  int right0 = int(std::lround(wa.w * splitRatio_)) + P(5);
  configure(c, side == 1 ? wa.x : wa.x + right0, wa.y, side == 1 ? left : wa.w - right0, wa.h);
}

void WM::setFullscreen(Client* c, bool on) {
  if (on == c->fullscreen) return;
  if (on) {
    if (!c->maximized && !c->snap) {
      c->restore = {c->x, c->y, c->w, c->h};
      c->hasRestore = true;
    }
    c->fullscreen = true;
    wlr_xdg_toplevel_set_fullscreen(c->tl, true);
    configure(c, 0, 0, sw_, sh_);
    raise(c);
  } else {
    c->fullscreen = false;
    wlr_xdg_toplevel_set_fullscreen(c->tl, false);
    if (c->maximized) {
      Rect wa = workArea();
      configure(c, wa.x, wa.y, wa.w, wa.h);
    } else if (c->hasRestore) {
      configure(c, c->restore.x, c->restore.y, c->restore.w, c->restore.h);
    }
  }
  publishWindow(c);
}

// ------------------------------------------------------------- JSUI

bool WM::createRuntime() {
  JsuiConfig cfg;
  jsui_config_init(&cfg);
  cfg.width = int(std::lround(sw_ / scale_));
  cfg.height = int(std::lround(sh_ / scale_));
  cfg.pixel_ratio = scale_;
  cfg.dev_mode = opt_.devUrl.empty() ? 0 : 1;
  cfg.on_message = [](void* u, const char* name, const char* json) { static_cast<WM*>(u)->onMessage(name, json); };
  cfg.message_user = this;
  cfg.on_state = [](void* u, const char* key, const char* json) { static_cast<WM*>(u)->onStateChange(key, json); };
  cfg.state_user = this;
  cfg.audio_disabled = 1;
  // React on a thread of its own: input, animations, layout and paint don't
  // wait for JS. It wakes the Wayland loop when it has work for a tick
  // (JSUI_WL_JS_THREAD=0: JS inside the tick, as before).
  const char* jsThread = getenv("JSUI_WL_JS_THREAD");
  if (!(jsThread && jsThread[0] == '0')) {
    jsReadyFd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (jsReadyFd_ >= 0) {
      cfg.js_thread = JSUI_JS_THREAD_ON;
      cfg.on_js_ready = [](void* u) {
        uint64_t one = 1;
        (void)!write(static_cast<WM*>(u)->jsReadyFd_, &one, sizeof one);
      };
      cfg.js_ready_user = this;
      wl_event_loop_add_fd(loop_, jsReadyFd_, WL_EVENT_READABLE, [](int fd, uint32_t, void* u) -> int {
        uint64_t n;
        (void)!read(fd, &n, sizeof n);
        static_cast<WM*>(u)->scheduleTick(0);
        return 0;
      }, this);
    }
  }
  rt_ = jsui_create(&cfg);
  if (!rt_) return false;
#ifdef JSUI_WL_MIR
  if (perf_) {
    jsui_set_profiling(rt_, 1);
    jsui_set_paint_counting(rt_, 1);
  }
#endif
  publishScreen();
  publishSystem();
  if (opt_.scanApps) {
    apps_ = scanApps("foot");
    rasterizeIcons();
    publishApps();
  }
  jsui_set_state(rt_, "wm.windows", "{}");
  jsui_set_state(rt_, "wm.stack", "[]");
  jsui_set_state(rt_, "wm.popups", "{}");
  jsui_set_state(rt_, "wm.active", "0");
  const std::string& src = opt_.devUrl.empty() ? opt_.bundle : opt_.devUrl;
  if (jsui_load_bundle_url(rt_, src.c_str()) != 0) {
    logf("cannot load the shell bundle %s", src.c_str());
    return false;
  }
  return true;
}

void WM::publishScreen() {
  std::string j = "{\"width\":" + wmjson::num(L(sw_)) + ",\"height\":" + wmjson::num(L(sh_)) +
                  ",\"pixelRatio\":" + wmjson::num(scale_) + ",\"topBar\":" + wmjson::num(topBar_) +
                  ",\"dock\":" + wmjson::num(dock_) + ",\"homeBar\":" + wmjson::num(homeBar_) +
                  ",\"titleBar\":" + wmjson::num(titleBar_) + ",\"border\":" + wmjson::num(border_) +
                  ",\"touch\":" + (opt_.mouse ? "false" : "true") + ",\"multiTouch\":true,\"wayland\":true}";
  jsui_set_state(rt_, "wm.screen", j.c_str());
}

void WM::rasterizeIcons() {
  if (g_svgIcons.empty() || iconSrc_) return;
  char exe[4096];
  ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
  if (len <= 0) return;
  exe[len] = 0;
  std::string helper(exe);
  helper = helper.substr(0, helper.rfind('/') + 1) + "jsui-icons";
  if (access(helper.c_str(), X_OK) != 0) {
    g_svgIcons.clear();
    return;
  }
  std::vector<std::string> args = {helper, "192"};
  for (const auto& [svg, png] : g_svgIcons) {
    args.push_back(svg);
    args.push_back(png);
  }
  g_svgIcons.clear();
  int fds[2];
  if (pipe2(fds, O_CLOEXEC) != 0) return;
  pid_t pid = fork();
  if (pid == 0) {
    signal(SIGCHLD, SIG_DFL);
    dup2(fds[1], 1);
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    execv(helper.c_str(), argv.data());
    _exit(127);
  }
  ::close(fds[1]);
  if (pid < 0) {
    ::close(fds[0]);
    return;
  }
  logf("icons: rasterizing %zu SVG icons", (args.size() - 2) / 2);
  iconSrc_ = wl_event_loop_add_fd(loop_, fds[0], WL_EVENT_READABLE, [](int fd, uint32_t, void* u) -> int {
    auto* w = static_cast<WM*>(u);
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof buf);
    if (n > 0) return 0;  // one line per PNG; the rescan at the end picks them all up
    wl_event_source_remove(w->iconSrc_);
    w->iconSrc_ = nullptr;
    ::close(fd);
    w->apps_ = scanApps("foot");
    w->publishApps();
    return 0;
  }, this);
}

void WM::publishApps() {
  std::string j = "[";
  for (size_t i = 0; i < apps_.size(); ++i) {
    const App& a = apps_[i];
    if (i) j += ',';
    j += "{\"id\":" + wmjson::quoted(a.id) + ",\"name\":" + wmjson::quoted(a.name) +
         ",\"comment\":" + wmjson::quoted(a.comment) + ",\"exec\":" + wmjson::quoted(a.exec) +
         ",\"icon\":" + wmjson::quoted(a.icon.empty() ? "" : "file://" + a.icon) +
         ",\"iconShape\":" + (a.squareIcon ? "\"square\"" : "\"free\"") +
         ",\"categories\":" + wmjson::quoted(a.categories) + "}";
  }
  j += "]";
  jsui_set_state(rt_, "wm.apps", j.c_str());
}

void WM::publishSystem() {
  char host[256] = "linux";
  gethostname(host, sizeof host);
  std::string user = "user";
  if (passwd* pw = getpwuid(getuid())) user = pw->pw_name;
  // The battery: a supply of type Battery with a capacity (the Tab A7 also
  // lists its fuel gauges, type BMS, with one), else any with a capacity;
  // plugged: a mains / USB supply online.
  std::string battery = "null";
  if (DIR* d = opendir("/sys/class/power_supply")) {
    int pct = -1, anyPct = -1;
    bool charging = false, anyCharging = false, plugged = false;
    while (dirent* e = readdir(d)) {
      if (e->d_name[0] == '.') continue;
      std::string base = std::string("/sys/class/power_supply/") + e->d_name;
      std::ifstream typeF(base + "/type"), capF(base + "/capacity"), statusF(base + "/status"), onlineF(base + "/online");
      std::string type, st;
      int cap = -1, online = 0;
      typeF >> type;
      statusF >> st;
      if (type == "Mains" || type.compare(0, 3, "USB") == 0) {
        if (onlineF >> online && online > 0) plugged = true;
        continue;
      }
      if (!(capF >> cap) || cap < 0) continue;
      if (type == "Battery" && pct < 0) {
        pct = cap;
        charging = st == "Charging";
      } else if (anyPct < 0) {
        anyPct = cap;
        anyCharging = st == "Charging";
      }
    }
    closedir(d);
    if (pct < 0) pct = anyPct, charging = anyCharging;
    if (pct >= 0)
      battery = "{\"percent\":" + std::to_string(pct) + ",\"charging\":" + (charging ? "true" : "false") +
                ",\"plugged\":" + (plugged ? "true" : "false") + "}";
  }
  // A wireless interface, if the device has a working one (the Tab A7's
  // port has none: no Wi-Fi driver).
  std::string wifi = "null";
  if (DIR* d = opendir("/sys/class/net")) {
    while (dirent* e = readdir(d)) {
      std::string name = e->d_name;
      if (name[0] != '.' && fileExists("/sys/class/net/" + name + "/operstate") &&
          access(("/sys/class/net/" + name + "/wireless").c_str(), F_OK) == 0) {
        std::ifstream st("/sys/class/net/" + name + "/operstate");
        std::string state;
        st >> state;
        wifi = "{\"iface\":" + wmjson::quoted(name) + ",\"up\":" + (state == "up" ? "true" : "false") + "}";
        break;
      }
    }
    closedir(d);
  }
  std::string j = "{\"user\":" + wmjson::quoted(user) + ",\"hostname\":" + wmjson::quoted(host) +
                  ",\"battery\":" + battery + ",\"wifi\":" + wifi + ",\"xtest\":true";
#ifdef JSUI_WL_MIR
  // The desktop can turn (rotateTo) and an accelerometer may say which way.
  if (mir_ && !vulkan_ && access("/run/sensord.sock", F_OK) == 0) j += ",\"rotate\":true";
#endif
  if (!backlightRead_ || !volumeRead_) readStartLevels();
  j += startBacklight_ + startVolume_;
  j += "}";
  if (j == lastSystem_) return;  // refreshed every 30 s (systemTimer_): publish changes only
  lastSystem_ = j;
  jsui_set_state(rt_, "wm.system", j.c_str());
}

// The shell's dark style, flipped, also flips Lomiri apps' theme (their
// toolkit reads ~/.config/lomiri-ui-toolkit/theme.ini). Only on a change
// from the shell's default (dark; wm.settings starts from it every
// session): a session start leaves the file alone, so System Settings' own
// "dark mode for apps" switch keeps working.
void WM::syncAppTheme(bool dark) {
  if (dark == dark_) return;
  dark_ = dark;
#ifdef JSUI_WL_MIR
  if (!mir_) return;
  const char* home = getenv("HOME");
  if (!home) return;
  std::string dir = std::string(home) + "/.config/lomiri-ui-toolkit";
  std::string path = dir + "/theme.ini", tmp = path + ".tmp";
  mkdir(dir.c_str(), 0755);
  if (FILE* f = fopen(tmp.c_str(), "w")) {
    fprintf(f, "[General]\ntheme=Lomiri.Components.Themes.%s\n", dark ? "SuruDark" : "Ambiance");
    fclose(f);
    if (rename(tmp.c_str(), path.c_str()) == 0) logf("apps' theme: %s", dark ? "dark" : "light");
  }
#endif
}

void WM::onStateChange(const char* key, const char* json) {
  if (std::strcmp(key, "wm.settings") != 0) return;
  wmjson::Value v = wmjson::parse(json);
  if (v["darkMode"].type != wmjson::Value::Null) syncAppTheme(v["darkMode"].truthy());
#ifdef JSUI_WL_MIR
  if (mir_ && v["autoRotate"].truthy() != autoRotate_) autoRotate(!autoRotate_);
#endif
  bool tablet = v["tabletMode"].truthy();
  if (tablet == tabletMode_) return;
  tabletMode_ = tablet;
  refitWorkArea();
}

// Maximized and split windows follow a change of the work area.
void WM::refitWorkArea() {
  Rect wa = workArea();
  for (auto& [id, c] : clients_) {
    if (c->popup || !c->mapped || c->fullscreen) continue;
    if (c->maximized) configure(c, wa.x, wa.y, wa.w, wa.h);
    else if (c->snap) setSnap(c, c->snap);
  }
}

void WM::writeAnchors(const wmjson::Value& v) {
  if (opt_.anchors.empty() || v.type != wmjson::Value::Object) return;
  std::string out = "{";
  bool first = true;
  for (const auto& [name, r] : v.o) {
    if (!first) out += ',';
    first = false;
    out += wmjson::quoted(name) + ":[" + std::to_string(P(r[size_t(0)].num())) + "," +
           std::to_string(P(r[size_t(1)].num())) + "," + std::to_string(P(r[size_t(2)].num())) + "," +
           std::to_string(P(r[size_t(3)].num())) + "]";
  }
  out += "}\n";
  std::string tmp = opt_.anchors + ".tmp";
  if (FILE* f = std::fopen(tmp.c_str(), "w")) {
    std::fputs(out.c_str(), f);
    std::fclose(f);
    std::rename(tmp.c_str(), opt_.anchors.c_str());
  }
}

void WM::onMessage(const char* name, const char* json) {
  wmjson::Value v = wmjson::parse(json);
  std::string n = name;
  VLOG("message %s %s", name, json ? json : "");
  Client* c = nullptr;
  if (v["id"].type == wmjson::Value::Number) {
    c = find(uint32_t(v["id"].num()));
    if (c && (c->popup || !c->mapped)) c = nullptr;
  }
  if (n == "wm:shellRegions") {
    shellRegions_.clear();
    const wmjson::Value& rects = v["rects"];
    for (size_t i = 0; i < rects.size(); ++i) {
      const wmjson::Value& r = rects[i];
      int x = P(r[size_t(0)].num()), y = P(r[size_t(1)].num());
      int w = P(r[size_t(0)].num() + r[size_t(2)].num()) - x, h = P(r[size_t(1)].num() + r[size_t(3)].num()) - y;
      if (w > 0 && h > 0) shellRegions_.push_back({x, y, w, h});
    }
  } else if (n == "wm:anchors") {
    writeAnchors(v);
  } else if (n == "wm:launch") {
    // An app already running from the same launch (Ubuntu Touch apps are
    // single instance: a second one just exits) is brought to the front.
    const std::string exec = v["exec"].str();
    auto it = launched_.find(exec);
    if (it != launched_.end()) {
      for (auto& [id, cl] : clients_) {
        if (cl->popup || !cl->mapped || cl->pid <= 0) continue;
        if (!descends(cl->pid, it->second)) continue;
        if (cl->minimized) unminimize(cl);
        raise(cl);
        focus(cl);
        return;
      }
    }
    pid_t pid = launch(exec);
    if (pid > 0) {
      launched_[exec] = pid;
      std::string name = v["name"].str();
      for (const App& a : apps_)
        if (name.empty() && a.exec == exec) name = a.name;
      pendingLaunches_.push_back({pid, name.empty() ? exec : name, nowMs(), exec});
      if (!launchTimer_)
        launchTimer_ = wl_event_loop_add_timer(loop_, [](void* u) -> int {
          static_cast<WM*>(u)->checkLaunches();
          return 0;
        }, this);
      wl_event_source_timer_update(launchTimer_, 5000);
    }
  } else if (n == "wm:focus" && c) {
    if (c->minimized) unminimize(c);
    raise(c);
    focus(c);
  } else if (n == "wm:move" && c) {
    if (c->maximized || c->snap) {
      c->maximized = false;
      c->snap = 0;
      wlr_xdg_toplevel_set_maximized(c->tl, false);
      wlr_xdg_toplevel_set_tiled(c->tl, 0);
      int w = c->hasRestore ? c->restore.w : c->w, h = c->hasRestore ? c->restore.h : c->h;
      configure(c, P(v["x"].num()), P(v["y"].num()), w, h);
    } else {
      configure(c, P(v["x"].num()), P(v["y"].num()), c->w, c->h);
    }
  } else if (n == "wm:resize" && c) {
    c->maximized = false;
    c->snap = 0;
    configure(c, P(v["x"].num()), P(v["y"].num()), P(v["w"].num()), P(v["h"].num()));
  } else if (n == "wm:close" && c) {
    close(c);
  } else if (n == "wm:minimize" && c) {
    minimize(c);
  } else if (n == "wm:restore" && c) {
    if (c->minimized) unminimize(c);
    else if (c->maximized) setMaximized(c, false);
    else if (c->snap) setSnap(c, 0);
    raise(c);
    focus(c);
  } else if (n == "wm:maximize" && c) {
    setMaximized(c, v["on"].type == wmjson::Value::Null ? !c->maximized : v["on"].truthy());
  } else if (n == "wm:snap" && c) {
    const std::string& side = v["side"].str();
    setSnap(c, side == "left" ? 1 : side == "right" ? 2 : 0);
  } else if (n == "wm:split") {
    Client* l = find(uint32_t(v["left"].num()));
    Client* r = find(uint32_t(v["right"].num()));
    if (v["ratio"].type == wmjson::Value::Number) splitRatio_ = std::clamp(v["ratio"].num(), 0.25, 0.75);
    if (l && l->minimized) unminimize(l);
    if (r && r->minimized) unminimize(r);
    if (l) setSnap(l, 1);
    if (r) setSnap(r, 2);
    if (l) raise(l);
    if (r) {
      raise(r);
      focus(r);
    }
  } else if (n == "wm:splitRatio") {
    splitRatio_ = std::clamp(v["ratio"].num(0.5), 0.25, 0.75);
    for (auto& [id, cl] : clients_)
      if (!cl->popup && cl->mapped && cl->snap && !cl->minimized) setSnap(cl, cl->snap);
  } else if (n == "wm:fullscreen" && c) {
    setFullscreen(c, v["on"].type == wmjson::Value::Null ? !c->fullscreen : v["on"].truthy());
  } else if (n == "wm:notificationAction") {
    if (notifyIn_ >= 0) {
      std::string l = "action " + std::to_string(int64_t(v["id"].num(0))) + "\n";
      ssize_t w = write(notifyIn_, l.data(), l.size());
      (void)w;
    }
  } else if (n == "wm:haptic") {
    haptic(int(v["ms"].num(12)));
  } else if (n == "wm:brightness") {
    setLevels(v, true);
  } else if (n == "wm:volume") {
    setLevels(v, false);
  } else if (n == "wm:snapshot") {
    snapshotBackdrop(v["on"].truthy());
  } else if (n == "wm:osk") {
    int h = P(v["height"].num());
    if (h != oskH_) {
      oskH_ = h;
      refitWorkArea();
    }
  } else if (n == "wm:textInputClosed") {
    textInput_.panelClosed();
  } else if (n == "wm:release") {
    uint32_t id = uint32_t(v["id"].num());
    Client* cl = find(id);
    if (!cl || !cl->mapped) {
      releaseAt_.erase(id);
      removeWindowTexture(id);
    }
  } else if (n == "wm:keyboard") {
    keyboardGrabbed_ = v["grab"].truthy();
  } else if (n == "wm:typeKey") {
    typeKey(v["key"].str(), int(v["mods"].num()));
  } else if (n == "wm:closeAll") {
    std::vector<uint32_t> ids = stack_;
    for (uint32_t id : ids)
      if (Client* cc = find(id)) close(cc);
  } else if (n == "wm:quit") {
    wl_display_terminate(display_);
  } else if (n == "wm:reloadApps") {
    apps_ = scanApps("foot");
    rasterizeIcons();
    publishApps();
  }
}

// ------------------------------------------------------------ state out

std::string WM::windowJson(const Client* c) const {
  bool resizable = c->maxW == 0 || c->maxW != c->minW || c->maxH != c->minH;
  const char* type = c->dialog ? "dialog" : "normal";
  std::string j = "{\"id\":" + std::to_string(c->id) + ",\"title\":" + wmjson::quoted(c->title) +
                  ",\"wmClass\":" + wmjson::quoted(c->appIdRaw) + ",\"instance\":" + wmjson::quoted(c->appIdRaw) +
                  ",\"appId\":" + wmjson::quoted(c->appId) + ",\"x\":" + wmjson::num(L(c->x)) +
                  ",\"y\":" + wmjson::num(L(c->y)) + ",\"w\":" + wmjson::num(L(c->w * c->fit)) +
                  ",\"h\":" + wmjson::num(L(c->h * c->fit)) + ",\"minW\":" + wmjson::num(L(c->minW)) + ",\"minH\":" + wmjson::num(L(c->minH)) +
                  ",\"restoreW\":" + wmjson::num(L(c->hasRestore ? c->restore.w : c->w)) +
                  ",\"restoreH\":" + wmjson::num(L(c->hasRestore ? c->restore.h : c->h)) + ",\"type\":\"" + type +
                  "\",\"decorated\":" + (c->decorated ? "true" : "false") +
                  ",\"resizable\":" + (resizable ? "true" : "false") +
                  ",\"transientFor\":" + std::to_string(c->transientFor) +
                  ",\"minimized\":" + (c->minimized ? "true" : "false") +
                  ",\"maximized\":" + (c->maximized ? "true" : "false") +
                  ",\"fullscreen\":" + (c->fullscreen ? "true" : "false") + ",\"snap\":\"" +
                  (c->snap == 1 ? "left" : c->snap == 2 ? "right" : "") +
                  "\",\"icon\":false,\"ready\":" + (c->hasTexture ? "true" : "false") +
                  ",\"pid\":" + std::to_string(c->pid) + ",\"mappedAt\":" + wmjson::num(c->mappedAt) + "}";
  return j;
}

void WM::publishWindow(const Client* c) {
  if (c->popup || !c->mapped) return;
  std::string j = "{\"" + std::to_string(c->id) + "\":" + windowJson(c) + "}";
  jsui_patch_state(rt_, "wm.windows", j.c_str());
  scheduleTick(0);
}

void WM::publishStack() {
  std::string j = "[";
  for (size_t i = 0; i < stack_.size(); ++i) {
    if (i) j += ',';
    j += std::to_string(stack_[i]);
  }
  j += "]";
  jsui_set_state(rt_, "wm.stack", j.c_str());
  scheduleTick(0);
}

void WM::publishPopups() {
  std::string j = "{";
  bool first = true;
  for (uint32_t id : popups_) {
    Client* c = find(id);
    if (!c || !c->mapped) continue;
    if (!first) j += ',';
    first = false;
    j += "\"" + std::to_string(id) + "\":{\"id\":" + std::to_string(id) + ",\"x\":" + wmjson::num(L(c->x)) +
         ",\"y\":" + wmjson::num(L(c->y)) + ",\"w\":" + wmjson::num(L(c->w * c->fit)) +
         ",\"h\":" + wmjson::num(L(c->h * c->fit)) + ",\"ready\":" + (c->hasTexture ? "true" : "false") + "}";
  }
  j += "}";
  jsui_set_state(rt_, "wm.popups", j.c_str());
  scheduleTick(0);
}

void WM::publishActive() {
  jsui_set_state(rt_, "wm.active", std::to_string(active_).c_str());
  scheduleTick(0);
}

// ------------------------------------------------------------------ input

// Who gets a press at (x, y) screen px: the shell (its regions, title bars,
// borders, the wallpaper) or a client surface (with surface-local coords).
Target WM::route(double x, double y, wlr_surface** surface, double* sx, double* sy) {
  *surface = nullptr;
  for (const Rect& r : shellRegions_)
    if (r.contains(x, y)) return Target::Shell;
  for (auto it = popups_.rbegin(); it != popups_.rend(); ++it) {
    Client* p = find(*it);
    if (!p || !p->mapped) continue;
    Rect r{p->x, p->y, int(p->w * p->fit), int(p->h * p->fit)};
    if (!r.contains(x, y)) continue;
    double lx = 0, ly = 0;
    wlr_surface* s = wlr_surface_surface_at(p->xdg->surface, (x - p->x) / p->fit, (y - p->y) / p->fit, &lx, &ly);
    if (s) {
      *surface = s;
      *sx = lx;
      *sy = ly;
      return Target::Surface;
    }
  }
  int tb = P(titleBar_), b = P(border_);
  for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
    Client* c = find(*it);
    if (!c || !c->mapped || c->minimized || !c->hasTexture) continue;
    Rect content{c->x, c->y, int(c->w * c->fit), int(c->h * c->fit)};
    if (content.contains(x, y)) {
      double lx = 0, ly = 0;
      wlr_surface* s =
          wlr_xdg_surface_surface_at(c->xdg, (x - c->x) / c->fit + c->geoX, (y - c->y) / c->fit + c->geoY, &lx, &ly);
      if (s) {
        *surface = s;
        *sx = lx;
        *sy = ly;
        return Target::Surface;
      }
      return Target::Shell;
    }
    if (c->decorated && !c->fullscreen) {
      int m = c->maximized ? 0 : b;
      Rect frame{c->x - m, c->y - tb, c->w + 2 * m, c->h + tb + m};
      if (frame.contains(x, y)) return Target::Shell;
    }
  }
  return Target::Shell;
}

void WM::sendShellPointer(int phase, double x, double y) {
  float lx = float(L(x)), ly = float(L(y));
  jsui_set_event_time(rt_, nowMs());
  if (opt_.mouse) {
    if (phase == 1) jsui_send_mouse_move(rt_, lx, ly);
    else jsui_send_mouse_button(rt_, lx, ly, JSUI_MOUSE_LEFT, phase == 0 ? 1 : 0, 0);
  } else {
    jsui_send_touch(rt_, 0, phase, lx, ly);
  }
  scheduleTick(0);
}

void WM::pointerMotion(uint32_t timeMs) {
  double x = cursor_->x, y = cursor_->y;
  if (mrId_) {
    Client* c = find(mrId_);
    if (!c) {
      mrId_ = 0;
      return;
    }
    int dx = int(x - mrX_), dy = int(y - mrY_);
    Rect r = mrStart_;
    if (mrEdges_ == 0) {
      r.x += dx;
      r.y = std::max(P(topBar_ + titleBar_), r.y + dy);
    } else {
      if (mrEdges_ & WLR_EDGE_RIGHT) r.w = std::max(c->minW, mrStart_.w + dx);
      if (mrEdges_ & WLR_EDGE_LEFT) {
        r.w = std::max(c->minW, mrStart_.w - dx);
        r.x = mrStart_.x + mrStart_.w - r.w;
      }
      if (mrEdges_ & WLR_EDGE_BOTTOM) r.h = std::max(c->minH, mrStart_.h + dy);
      if (mrEdges_ & WLR_EDGE_TOP) {
        r.h = std::max(c->minH, mrStart_.h - dy);
        r.y = mrStart_.y + mrStart_.h - r.h;
      }
    }
    configure(c, r.x, r.y, r.w, r.h);
    return;
  }
  if (pointerDown_ && pointerTarget_ == Target::Shell) {
    sendShellPointer(1, x, y);
    return;
  }
  if (pointerDown_ && pointerTarget_ == Target::Surface) {
    // Implicit grab: the seat keeps sending to the pressed surface.
    wlr_surface* focused = seat_->pointer_state.focused_surface;
    if (focused) {
      Client* c = findSurface(focused);
      // Subsurface offsets: relative to the root surface (good enough for
      // drags inside one surface).
      double lx = x, ly = y;
      if (c) toSurface(c, &lx, &ly);
      wlr_seat_pointer_notify_motion(seat_, timeMs, lx, ly);
    }
    return;
  }
  wlr_surface* s = nullptr;
  double sx = 0, sy = 0;
  Target t = route(x, y, &s, &sx, &sy);
  if (t == Target::Surface) {
    wlr_seat_pointer_notify_enter(seat_, s, sx, sy);
    wlr_seat_pointer_notify_motion(seat_, timeMs, sx, sy);
  } else {
    wlr_seat_pointer_notify_clear_focus(seat_);
    if (opt_.mouse) sendShellPointer(1, x, y);
  }
}

void WM::pointerButton(bool down, uint32_t button, uint32_t timeMs) {
  double x = cursor_->x, y = cursor_->y;
  if (!down && mrId_) {
    mrId_ = 0;
    pointerDown_ = false;
    pointerTarget_ = Target::None;
    wlr_seat_pointer_notify_button(seat_, timeMs, button, WLR_BUTTON_RELEASED);
    return;
  }
  // An xdg popup grab (open menu) owns the pointer: clicks outside dismiss it.
  bool popupGrab = !popups_.empty() && seat_->pointer_state.grab != seat_->pointer_state.default_grab;
  if (down) {
    wlr_surface* s = nullptr;
    double sx = 0, sy = 0;
    Target t = route(x, y, &s, &sx, &sy);
    if (popupGrab && t != Target::Surface) {
      wlr_seat_pointer_notify_button(seat_, timeMs, button, WLR_BUTTON_PRESSED);
      pointerTarget_ = Target::Surface;
      pointerDown_ = true;
      return;
    }
    if (pointerDown_ && pointerTarget_ == Target::Shell) sendShellPointer(3, x, y);
    pointerDown_ = true;
    pointerTarget_ = t;
    if (t == Target::Shell) {
      wlr_seat_pointer_notify_clear_focus(seat_);
      sendShellPointer(0, x, y);
      return;
    }
    Client* c = findSurface(s);
    if (c && !c->popup && c->id != active_) {
      raise(c);
      focus(c);
    }
    wlr_seat_pointer_notify_enter(seat_, s, sx, sy);
    wlr_seat_pointer_notify_motion(seat_, timeMs, sx, sy);
    wlr_seat_pointer_notify_button(seat_, timeMs, button, WLR_BUTTON_PRESSED);
  } else {
    Target t = pointerTarget_;
    pointerDown_ = false;
    pointerTarget_ = Target::None;
    if (t == Target::Shell) sendShellPointer(2, x, y);
    else wlr_seat_pointer_notify_button(seat_, timeMs, button, WLR_BUTTON_RELEASED);
    pointerMotion(timeMs);  // re-enter whatever is under the pointer now
  }
}

// Set back to the rest frequency when killed (jsui-session's restart sends
// SIGTERM): only async-signal-safe calls, on what setupGpuBoost prepared.
char g_boostPath[128];
char g_boostRest[24];
void gpuBoostSignal(int sig) {
  int fd = open(g_boostPath, O_WRONLY);
  if (fd >= 0) {
    ssize_t n = write(fd, g_boostRest, strlen(g_boostRest));
    (void)n;
    close(fd);
  }
  signal(sig, SIG_DFL);
  raise(sig);
}

void WM::setupGpuBoost() {
  const char* env = getenv("JSUI_WL_GPU_BOOST");
  const long mhz = env ? atol(env) : 745;
  const char* dirEnv = getenv("JSUI_WL_GPU_BOOST_DEVFREQ");  // another devfreq directory (tests)
  const char* dir = dirEnv && *dirEnv ? dirEnv : "/sys/class/kgsl/kgsl-3d0/devfreq";
  std::string path = std::string(dir) + "/min_freq";
  if (mhz <= 0 || access(path.c_str(), W_OK) != 0) return;
  // The frequencies the governor knows: the boost is the lowest at or above
  // the one asked for (else the highest), the rest the lowest of all (not
  // min_freq as found: a jsui-wl that died boosted left it raised).
  std::ifstream in(std::string(dir) + "/available_frequencies");
  long hz, lo = 0, hi = 0, pick = 0;
  while (in >> hz) {
    if (hz <= 0) continue;
    if (!lo || hz < lo) lo = hz;
    if (hz > hi) hi = hz;
    if (hz >= mhz * 1000000 && (!pick || hz < pick)) pick = hz;
  }
  if (!lo) return;
  boostPath_ = path;
  boostHz_ = pick ? pick : hi;
  boostRestHz_ = lo;
  gpuBoostWrite(boostRestHz_);
  if (path.size() < sizeof(g_boostPath)) {
    snprintf(g_boostPath, sizeof(g_boostPath), "%s", path.c_str());
    snprintf(g_boostRest, sizeof(g_boostRest), "%ld\n", boostRestHz_);
    for (int sig : {SIGTERM, SIGINT, SIGHUP})
      if (signal(sig, gpuBoostSignal) == SIG_IGN) signal(sig, SIG_IGN);  // ignored stays ignored
  }
  boostTimer_ = wl_event_loop_add_timer(loop_, [](void* u) -> int {
    auto* w = static_cast<WM*>(u);
    const double left = w->boostUntil_ - nowMs();
    if (left > 1) {
      wl_event_source_timer_update(w->boostTimer_, int(left) + 1);
    } else if (w->boosted_) {
      w->boosted_ = false;
      w->gpuBoostWrite(w->boostRestHz_);
    }
    return 0;
  }, this);
  logf("GPU boost: min_freq %ld MHz from a touch while its frames are drawn, else %ld MHz", boostHz_ / 1000000,
       boostRestHz_ / 1000000);
}

void WM::gpuBoostWrite(long hz) {
  // Writes fail when min_freq would exceed max_freq (a thermal cap): then
  // the governor's own choice stands.
  FILE* f = fopen(boostPath_.c_str(), "w");
  if (!f) return;
  fprintf(f, "%ld\n", hz);
  fclose(f);
}

void WM::gpuBoost(bool touch) {
  if (!boostTimer_) return;
  // A touch starts it; frames keep it while they come, up to 2 s after the
  // last touch (an app redrawing on its own, a clock's second hand or a
  // video, does not hold the GPU up).
  constexpr double kHoldMs = 500, kAfterTouchMs = 2000;
  const double now = nowMs();
  if (touch) boostTouch_ = now;
  else if (!boosted_ || now - boostTouch_ > kAfterTouchMs) return;
  boostUntil_ = now + kHoldMs;
  if (!boosted_) {
    boosted_ = true;
    gpuBoostWrite(boostHz_);
    wl_event_source_timer_update(boostTimer_, int(kHoldMs));
  }
}

void WM::touchEvent(int id, int phase, double x, double y, uint32_t timeMs) {
  if (phase == 0 && !backlightRead_) publishSystem();  // the screen is lit now: its level (readStartLevels)
  gpuBoost(true);
  if (perfEvery_) logf("touch %d at %.0f", phase, nowMs());
  int tid = 1 + (id % 1000);
  if (phase == 0) {
    wlr_surface* s = nullptr;
    double sx = 0, sy = 0;
    Target t = route(x, y, &s, &sx, &sy);
    touches_[id] = t;
    touchPos_[id] = {x, y};
    if (t == Target::Shell) {
      jsui_set_event_time(rt_, nowMs());
      jsui_send_touch(rt_, tid, 0, float(L(x)), float(L(y)));
      scheduleTick(0);
    } else {
      Client* c = findSurface(s);
      if (c && !c->popup && c->id != active_) {
        raise(c);
        focus(c);
      }
      wlr_seat_touch_notify_down(seat_, s, timeMs, id, sx, sy);
    }
    return;
  }
  auto it = touches_.find(id);
  if (it == touches_.end()) return;
  if (it->second == Target::Shell) {
    jsui_set_event_time(rt_, nowMs());
    // Where the finger was last, on release too: JSUI fires the click (and
    // its pageX / pageY) there. (Not the cursor's position: on a touch-only
    // panel it never moves; libinput's up events carry none.)
    Pos& at = touchPos_[id];
    if (phase == 1) at = {x, y};
    jsui_send_touch(rt_, tid, phase, float(L(at.x)), float(L(at.y)));
    scheduleTick(0);
  } else {
    wlr_touch_point* pt = wlr_seat_touch_get_point(seat_, id);
    if (phase == 1 && pt) {
      Client* c = findSurface(pt->surface);
      double lx = x, ly = y;
      if (c) toSurface(c, &lx, &ly);
      wlr_seat_touch_notify_motion(seat_, timeMs, id, lx, ly);
    } else if (phase == 2) {
      wlr_seat_touch_notify_up(seat_, timeMs, id);
    } else if (phase == 3 && pt) {
      wlr_seat_touch_notify_cancel(seat_, pt->surface);
    }
  }
  if (phase >= 2) {
    touches_.erase(it);
    touchPos_.erase(id);
  }
}

void WM::keyboardKey(wlr_keyboard* kb, uint32_t keycode, bool down, uint32_t timeMs) {
  uint32_t xkbcode = keycode + 8;
  const xkb_keysym_t* syms = nullptr;
  int n = xkb_state_key_get_syms(kb->xkb_state, xkbcode, &syms);
  xkb_keysym_t sym = n > 0 ? syms[0] : XKB_KEY_NoSymbol;
  uint32_t mods = wlr_keyboard_get_modifiers(kb);
  bool alt = mods & WLR_MODIFIER_ALT, logo = mods & WLR_MODIFIER_LOGO;
  // Global shortcuts.
  if (sym == XKB_KEY_Super_L || sym == XKB_KEY_Super_R) {
    if (!down) jsui_post_message(rt_, "wm:hotkey", "{\"name\":\"super\"}");
    scheduleTick(0);
    return;
  }
  Client* c = find(active_);
  if (down && ((alt && sym == XKB_KEY_Tab) || (logo && (sym == XKB_KEY_Up || sym == XKB_KEY_Down || sym == XKB_KEY_Left ||
                                                        sym == XKB_KEY_Right)) || (alt && sym == XKB_KEY_F4))) {
    if (sym == XKB_KEY_Tab) jsui_post_message(rt_, "wm:hotkey", "{\"name\":\"switch\"}");
    else if (sym == XKB_KEY_F4 && c) close(c);
    else if (sym == XKB_KEY_Up && c) setMaximized(c, true);
    else if (sym == XKB_KEY_Down && c) c->maximized || c->snap ? setMaximized(c, false) : minimize(c);
    else if (sym == XKB_KEY_Left && c) setSnap(c, 1);
    else if (sym == XKB_KEY_Right && c) setSnap(c, 2);
    scheduleTick(0);
    return;
  }
  if (keyboardGrabbed_) {
    char text[16] = {0};
    xkb_state_key_get_utf8(kb->xkb_state, xkbcode, text, sizeof text);
    DomKey dk = domKey(sym, text);
    int jm = ((mods & WLR_MODIFIER_SHIFT) ? JSUI_MOD_SHIFT : 0) | ((mods & WLR_MODIFIER_CTRL) ? JSUI_MOD_CTRL : 0) |
             (alt ? JSUI_MOD_ALT : 0);
    jsui_send_key(rt_, dk.key, dk.code, down ? 1 : 0, 0, jm);
    if (down && static_cast<unsigned char>(text[0]) >= 0x20 && text[0] != 0x7f) jsui_send_text(rt_, text);
    scheduleTick(0);
    return;
  }
  wlr_seat_set_keyboard(seat_, kb);
  wlr_seat_keyboard_notify_key(seat_, timeMs, keycode, down ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
}

// The on-screen keyboard: into the shell's own text field while it has the
// keyboard, otherwise into the focused client through the virtual keyboard.
void WM::typeKey(const std::string& key, int mods) {
  if (key.empty()) return;
  if (keyboardGrabbed_) {
    jsui_send_key(rt_, key.c_str(), nullptr, 1, 0, mods);
    if (key.size() <= 4 && static_cast<unsigned char>(key[0]) >= 0x20 && key != "Enter") jsui_send_text(rt_, key.c_str());
    jsui_send_key(rt_, key.c_str(), nullptr, 0, 0, mods);
    scheduleTick(0);
    return;
  }
  xkb_keysym_t sym = keysymFor(key);
  if (sym == XKB_KEY_NoSymbol) return;
  // Find a keycode (and whether it needs Shift) for the keysym.
  struct Find {
    xkb_keymap* km;
    xkb_keysym_t sym;
    xkb_keycode_t code = 0;
    bool shift = false;
  } f{keymap_, sym};
  xkb_keymap_key_for_each(
      keymap_,
      [](xkb_keymap* km, xkb_keycode_t code, void* u) {
        auto* k = static_cast<Find*>(u);
        if (k->code) return;
        for (xkb_level_index_t lvl = 0; lvl < 2; ++lvl) {
          const xkb_keysym_t* syms = nullptr;
          int n = xkb_keymap_key_get_syms_by_level(km, code, 0, lvl, &syms);
          for (int i = 0; i < n; ++i)
            if (syms[i] == k->sym) {
              k->code = code;
              k->shift = lvl == 1;
              return;
            }
        }
      },
      &f);
  if (!f.code) return;
  Client* c = find(active_);
  if (!c) return;
  uint32_t timeMs = uint32_t(nowMs());
  wlr_seat_set_keyboard(seat_, &vkbd_);
  wlr_seat_keyboard_notify_enter(seat_, c->xdg->surface, nullptr, 0, &vkbd_.modifiers);
  wlr_keyboard_modifiers m{};
  xkb_mod_index_t shiftIdx = xkb_keymap_mod_get_index(keymap_, XKB_MOD_NAME_SHIFT);
  xkb_mod_index_t ctrlIdx = xkb_keymap_mod_get_index(keymap_, XKB_MOD_NAME_CTRL);
  if (f.shift || (mods & JSUI_MOD_SHIFT)) m.depressed |= 1u << shiftIdx;
  if (mods & JSUI_MOD_CTRL) m.depressed |= 1u << ctrlIdx;
  wlr_seat_keyboard_notify_modifiers(seat_, &m);
  wlr_seat_keyboard_notify_key(seat_, timeMs, f.code - 8, WL_KEYBOARD_KEY_STATE_PRESSED);
  wlr_seat_keyboard_notify_key(seat_, timeMs + 1, f.code - 8, WL_KEYBOARD_KEY_STATE_RELEASED);
  wlr_keyboard_modifiers none{};
  wlr_seat_keyboard_notify_modifiers(seat_, &none);
}

// ------------------------------------------------------------- control

void WM::setupControl() {
  if (opt_.control.empty()) return;
  unlink(opt_.control.c_str());
  controlFd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", opt_.control.c_str());
  if (bind(controlFd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(controlFd_, 4) != 0) {
    logf("cannot listen on %s", opt_.control.c_str());
    ::close(controlFd_);
    controlFd_ = -1;
    return;
  }
  wl_event_loop_add_fd(loop_, controlFd_, WL_EVENT_READABLE, [](int fd, uint32_t, void* u) -> int {
    WM* wm = static_cast<WM*>(u);
    int cfd = accept4(fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (cfd < 0) return 0;
    wm->controlSources_[cfd] = wl_event_loop_add_fd(wm->loop_, cfd, WL_EVENT_READABLE, [](int c, uint32_t mask, void* uu) -> int {
      WM* w = static_cast<WM*>(uu);
      char buf[4096];
      ssize_t n = read(c, buf, sizeof buf);
      if (n <= 0 || (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR))) {
        // Remove the source before closing: the fd number gets reused (by
        // the next Wayland client, say).
        wl_event_source_remove(w->controlSources_[c]);
        w->controlSources_.erase(c);
        w->controlBuf_.erase(c);
        ::close(c);
        return 0;
      }
      std::string& b = w->controlBuf_[c];
      b.append(buf, size_t(n));
      size_t nl;
      while ((nl = b.find('\n')) != std::string::npos) {
        std::string line = b.substr(0, nl);
        b.erase(0, nl + 1);
        w->controlLine(line);
        std::string ok = "ok\n";
        (void)!write(c, ok.data(), ok.size());
      }
      return 0;
    }, wm);
    return 0;
  }, this);
  logf("control socket %s", opt_.control.c_str());
}

void WM::controlLine(const std::string& line) {
  char cmd[32] = {0};
  double x = 0, y = 0;
  int n = std::sscanf(line.c_str(), "%31s %lf %lf", cmd, &x, &y);
  if (n < 1) return;
  std::string c = cmd;
  uint32_t t = uint32_t(nowMs());
  if ((c == "down" || c == "move") && n == 3) {
    wlr_cursor_warp_closest(cursor_, nullptr, x, y);
    if (c == "down") pointerButton(true, 0x110 /* BTN_LEFT */, t);
    else pointerMotion(t);
    wlr_seat_pointer_notify_frame(seat_);
  } else if (c == "up") {
    pointerButton(false, 0x110, t);
    wlr_seat_pointer_notify_frame(seat_);
  } else if (c == "key") {
    std::string k = line.substr(4);
    if (keyboardGrabbed_) {
      jsui_send_key(rt_, k.c_str(), nullptr, 1, 0, 0);
      jsui_send_key(rt_, k.c_str(), nullptr, 0, 0, 0);
      scheduleTick(0);
    } else {
      typeKey(k, 0);
    }
  } else if (c == "text") {
    std::string s = line.size() > 5 ? line.substr(5) : "";
    for (size_t i = 0; i < s.size();) {
      size_t len = 1;
      unsigned char ch = static_cast<unsigned char>(s[i]);
      if (ch >= 0xF0) len = 4;
      else if (ch >= 0xE0) len = 3;
      else if (ch >= 0xC0) len = 2;
      typeKey(s.substr(i, len), 0);
      i += len;
    }
  } else if (c == "shot") {
    writeScreenshot(line.substr(5));
#ifdef JSUI_WL_MIR
  } else if (c == "rotate" && n >= 2) {
    rotateTo(int(x));
  } else if (c == "orientation" && line.size() > 12) {
    orientationLine(line.substr(12));  // as if from jsui-orientation (tests)
#endif
  } else if (c == "touch") {
    // touch ID PHASE X Y (phase 0 down, 1 move, 2 up; desktop px): a finger,
    // for scripted runs on touch-only devices.
    int id = 0, phase = 0;
    if (std::sscanf(line.c_str(), "touch %d %d %lf %lf", &id, &phase, &x, &y) == 4 && phase >= 0 && phase <= 2) {
      touchEvent(id, phase, x, y, t);
      wlr_seat_touch_notify_frame(seat_);
    }
  }
}

// The screen as PPM: the shell's frame (everything on screen is in it).
void WM::writeScreenshot(const std::string& path) {
#ifdef JSUI_WL_MIR_GL
  if (gpu_) {
    std::vector<uint8_t> px;
    if (!mir_->readDesktop(&px)) return;
    FILE* g = std::fopen(path.c_str(), "wb");
    if (!g) return;
    int w = mir_->desktopWidth(), h = mir_->desktopHeight();
    std::fprintf(g, "P6\n%d %d\n255\n", w, h);
    std::vector<uint8_t> row(size_t(w) * 3);
    for (int y = 0; y < h; ++y) {
      const uint8_t* s = px.data() + size_t(y) * size_t(w) * 4;
      for (int x = 0; x < w; ++x) {
        row[size_t(x) * 3] = s[x * 4];
        row[size_t(x) * 3 + 1] = s[x * 4 + 1];
        row[size_t(x) * 3 + 2] = s[x * 4 + 2];
      }
      std::fwrite(row.data(), 1, row.size(), g);
    }
    std::fclose(g);
    return;
  }
#endif
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f || !frame_) return;
  std::fprintf(f, "P6\n%d %d\n255\n", sw_, sh_);
  std::vector<uint8_t> row(size_t(sw_) * 3);
  for (int y = 0; y < sh_; ++y) {
    const uint8_t* s = frame_->data.data() + size_t(y) * size_t(frame_->stride);
    for (int x = 0; x < sw_; ++x) {
      row[size_t(x) * 3] = s[x * 4];
      row[size_t(x) * 3 + 1] = s[x * 4 + 1];
      row[size_t(x) * 3 + 2] = s[x * 4 + 2];
    }
    std::fwrite(row.data(), 1, row.size(), f);
  }
  std::fclose(f);
}

// ------------------------------------------------------------- frames

void WM::scheduleTick(double delayMs) {
  if (!tickTimer_) return;
  int ms = std::max(1, int(std::ceil(delayMs)));
  if (tickPending_ && delayMs > 0) return;
  tickPending_ = true;
  wl_event_source_timer_update(tickTimer_, ms);
}

void WM::tick() {
  tickPending_ = false;
  double now = nowMs();
  for (auto it = releaseAt_.begin(); it != releaseAt_.end();) {
    if (now >= it->second) {
      removeWindowTexture(it->first);
      it = releaseAt_.erase(it);
    } else {
      ++it;
    }
  }
  jsui_tick(rt_, now);
#ifdef JSUI_WL_MIR
  lastTickMs_ = nowMs() - now;
  if (perf_) {
    perfTick_ += nowMs() - now;
    perfTickMax_ = std::max(perfTickMax_, nowMs() - now);
    ++perfTicks_;
  }
#endif
  JsuiRect dirty{};
  bool presented = false;
  double capWait = 0;  // a frame is due but too soon after the last (minFrameMs_)
  double paintStart = nowMs();
#ifdef JSUI_WL_MIR
  if (gpu_) {
    // The Mir window is the screen: paint and present right after the tick
    // that changed something (waiting for the headless output's frame
    // clock as well put a second 16.7 ms clock in series: ~30 fps). The
    // present thread's swap paces it, and minFrameMs_ when swaps return
    // early.
    if (jsui_needs_redraw(rt_) || shellDirty_ || mir_->rotating()) {
      const double since = now - lastPresentAt_;
      if (since >= minFrameMs_) {
        lastPresentAt_ = now;
        presentMir();
        presented = true;
      } else {
        capWait = minFrameMs_ - since;
      }
    }
  } else
#endif
  if (frame_ && jsui_render_rgba_incremental(rt_, frame_->data.data(), sw_, sh_, size_t(frame_->stride), &dirty) &&
      dirty.width > 0 && dirty.height > 0) {
    pixman_region32_union_rect(&shellDamage_, &shellDamage_, dirty.x, dirty.y, unsigned(dirty.width),
                               unsigned(dirty.height));
    cpuPaintMs_ += nowMs() - paintStart;
    shellDirty_ = true;
    scheduleFrame();
  }
  double wait;
  bool turning = false;
#ifdef JSUI_WL_MIR
  turning = mir_ && mir_->rotating();
#endif
  if (capWait > 0) {
    wait = capWait;
  } else if (turning || jsui_wants_frame(rt_) || jsui_needs_redraw(rt_) || jsui_pending_loads(rt_) > 0) {
    // 60 Hz from this tick's start: the paint above is part of the period.
    // After a frame went to the present thread the next tick comes when
    // its swap returns instead (swapFd: Mir's vsync); a free-running timer
    // beat against that and dropped frames. The timer is then a fallback.
    wait = presented && vsyncTicks_ ? 100.0 : std::max(1.0, 1000.0 / 60.0 - (nowMs() - now));
  } else {
    double t = jsui_next_timer(rt_);
    wait = std::isfinite(t) ? std::clamp(t - nowMs(), 1.0, 1000.0) : 1000.0;
  }
  scheduleTick(wait);
}

void WM::scheduleFrame() {
  if (frameScheduled_ || !output_) return;
  frameScheduled_ = true;
  wlr_output_schedule_frame(output_);
}

#ifdef JSUI_WL_MIR
// The GPU path's frame (see tick): paint, hand to the present thread, and
// let the apps draw their next frames.
void WM::presentMir() {
  gpuBoost(false);
  double t0 = nowMs();
  renderGpu();
  shellDirty_ = false;  // nothing for the headless scene: the Mir window is the screen
  if (perf_) {
    const double ms = nowMs() - t0;
    perfFrame(0, ms);
    // A frame that will be seen as a hitch: what it was made of.
    if (ms + lastTickMs_ > 33 || frameLog_ > 0 || perfEvery_) {
      if (frameLog_ > 0 || perfEvery_) {
        frameLog_ = std::max(0, frameLog_ - 1);
        logf("frame at %.0f", nowMs());
      }
      JsuiPaintStats ps;
      jsui_get_paint_stats(rt_, &ps);
      logf("slow frame: tick %.1f ms, paint %.1f ms, wait for the present thread %.1f ms; %.2f Mpx, %d draws,"
           " %d offscreen layers, %d cached layers (%d rendered), %d shadow blurs, %d nodes",
           lastTickMs_, lastRenderMs_, ms - lastRenderMs_, double(ps.repainted_pixels) / 1e6, ps.draws, ps.save_layers,
           ps.layer_draws, ps.layer_renders, ps.shadow_renders, ps.nodes_painted);
#ifdef JSUI_WL_MIR_GL
      if (mir_->lastRenderGpuMs() > 0)  // JSUI_WL_GPU_FINISH: the previous frame's GPU time
      {
        // With the clock (Adreno: kgsl devfreq), the cost in cycles compares
        // across the governor's frequency changes.
        int mhz = 0;
        if (FILE* f = fopen("/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq", "r")) {
          long hz = 0;
          if (fscanf(f, "%ld", &hz) == 1) mhz = int(hz / 1000000);
          fclose(f);
        }
        logf("gpu time: paint %.1f ms, blit %.1f ms at %d MHz (%.1f Mcycles)", mir_->lastRenderGpuMs(),
             mir_->lastBlitGpuMs(), mhz, (mir_->lastRenderGpuMs() + mir_->lastBlitGpuMs()) * mhz / 1000);
      }
#endif
    }
  }
  sendFrameDone();
}
#endif

void WM::sendFrameDone() {
  timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  for (auto& [id, c] : clients_)
    if (c->mapped && c->xdg)
      wlr_surface_for_each_surface(
          c->xdg->surface,
          [](wlr_surface* s, int, int, void* u) { wlr_surface_send_frame_done(s, static_cast<timespec*>(u)); },
          &now);
}

void WM::onFrame() {
  frameScheduled_ = false;
#ifdef JSUI_WL_MIR
  if (mir_ && shellDirty_) {
    double t0 = nowMs();
    if (gpu_) {
      presentMir();
      return;
    } else {
      mir_->present(frame_->data.data(), sw_, sh_, size_t(frame_->stride));
      if (perf_) perfFrame(cpuPaintMs_, nowMs() - t0);
    }
    cpuPaintMs_ = 0;
  }
#endif
  if (shellDirty_) {
    shellDirty_ = false;
    wlr_scene_buffer_set_buffer_with_damage(shellNode_, &frame_->base, &shellDamage_);
    pixman_region32_clear(&shellDamage_);
  }
  wlr_scene_output_commit(sceneOutput_, nullptr);
  sendFrameDone();  // the toplevels and their subsurfaces
}

#ifdef JSUI_WL_MIR
// --mir: connect first, so wlroots' headless output takes the panel's size.
bool WM::setupMir() {
  std::string sock = opt_.mirSocket;
  if (sock.empty() && getenv("MIR_SERVER_HOST_SOCKET")) sock = getenv("MIR_SERVER_HOST_SOCKET");
  // The system compositor shows the session whose name lightdm made active.
  const char* name = getenv("MIR_SERVER_NAME") ? getenv("MIR_SERVER_NAME") : "jsui-wl";
  mir_ = new MirHost;
  mir_->setRotation(opt_.rotate);
  std::string err;
  if (!mir_->connect(sock.empty() ? nullptr : sock.c_str(), name, &err)) {
    logf("cannot connect to Mir (%s): %s", sock.empty() ? "default socket" : sock.c_str(), err.c_str());
    return false;
  }
#ifdef JSUI_WL_MIR_VK
  // libhybris' Vulkan switches (jsui-session) are jsui-wl's; apps keep their
  // defaults (--app-env given later still wins).
  g_appEnv.insert(g_appEnv.begin(), {"HYBRIS_VULKANPLATFORM=", "HYBRIS_LD_LIBRARY_PATH="});
#endif
  int w = 0, h = 0;
  uint32_t out = 0;
  if (mir_->outputSize(&w, &h, &out)) {
    opt_.width = w;
    opt_.height = h;
  }
  logf("Mir session \"%s\", output %dx%d (rotated %d)", name, opt_.width, opt_.height, opt_.rotate);
  publishRotation();
  // What jsui-wl sets for itself here and for its Mir window (EGL_PLATFORM
  // "null": libhybris passes Mir's ANativeWindow through) must not reach the
  // apps: with it their libhybris EGL skips its Wayland platform, Qt gets no
  // GL context and Lomiri's toolkit crashes.
  for (const char* k : {"WLR_BACKENDS", "WLR_RENDERER", "WLR_LIBINPUT_NO_DEVICES", "EGL_PLATFORM"})
    if (!getenv(k)) g_appEnv.insert(g_appEnv.begin(), std::string(k) + "=");
  setenv("WLR_BACKENDS", "headless", 1);
  setenv("WLR_RENDERER", "pixman", 1);
  setenv("WLR_LIBINPUT_NO_DEVICES", "1", 1);
  setenv("QT_QPA_PLATFORM", "wayland", 0);
  mirOutput_ = out;
  return true;
}

void WM::perfFrame(double paintMs, double presentMs) {
  if (perfFrames_ == 0) perfStart_ = nowMs();
  const double now = nowMs();
  // Frames during animations come back to back; a gap over 100 ms is idle
  // time between gestures, not a slow frame.
  if (perfFrames_ > 0 && now - perfLast_ < 100) perfGapMax_ = std::max(perfGapMax_, now - perfLast_);
  if (perfFrames_ > 0 && now - perfLast_ >= 100) perfStart_ += now - perfLast_;  // idle: not counted
  perfLast_ = now;
  perfPaint_ += paintMs;
  perfPresent_ += presentMs;
  perfMax_ = std::max(perfMax_, paintMs + presentMs);
  if (++perfFrames_ < 60) return;
  const double span = nowMs() - perfStart_;
  logf("perf (%s): %d frames in %.0f ms (%.1f fps while drawing; the longest gap %.1f ms), paint %.2f ms + present"
       " %.2f ms avg, worst %.2f ms",
       gpu_ ? "gpu" : "cpu", perfFrames_, span, span > 0 ? (perfFrames_ - 1) * 1000.0 / span : 0.0, perfGapMax_,
       perfPaint_ / perfFrames_, perfPresent_ / perfFrames_, perfMax_);
  perfGapMax_ = 0;
  if (gpu_)
    logf(vulkan_ ? "perf (vulkan): jsui render %.2f ms, present pass %.2f ms, acquire + present %.2f ms avg"
                 : mir_->presentThread() ? "perf (gpu): jsui render %.2f ms, wait for the present thread %.2f ms, its swap %.2f ms avg"
                                         : "perf (gpu): jsui render %.2f ms, blit %.2f ms, swap %.2f ms avg",
         gpuRenderMs_ / perfFrames_, gpuBlitMs_ / perfFrames_, gpuSwapMs_ / perfFrames_);
  if (gpuRenderGpuMs_ > 0)
    logf("perf (gpu): GPU time per frame: paint %.2f ms, blit %.2f ms", gpuRenderGpuMs_ / perfFrames_,
         gpuBlitGpuMs_ / perfFrames_);
  gpuRenderGpuMs_ = gpuBlitGpuMs_ = 0;
  logf("perf: %d ticks, jsui_tick %.2f ms avg, worst %.2f ms", perfTicks_, perfTicks_ ? perfTick_ / perfTicks_ : 0.0,
       perfTickMax_);
  logf("perf: per frame %.2f Mpx repainted, %.1f draws, %.2f offscreen layers, %.2f cached layer draws"
       " (%.2f rendered), %.2f shadow blurs",
       perfMpx_ / perfFrames_, double(perfDraws_) / perfFrames_, double(perfLayers_) / perfFrames_,
       double(perfLayerDraws_) / perfFrames_, double(perfLayerRenders_) / perfFrames_,
       double(perfShadowRenders_) / perfFrames_);
  perfMpx_ = 0;
  perfLayers_ = perfLayerDraws_ = perfLayerRenders_ = perfDraws_ = perfShadowRenders_ = 0;
  JsuiPerf p;
  jsui_get_perf(rt_, &p, 1);
  logf("perf: per frame paint %.2f, layout %.2f, style %.2f, commit %.2f, animated %.2f, input %.2f, js thread %.2f ms;"
       " %llu paragraphs, %llu GCs",
       p.paint_us / 1000 / perfFrames_, p.layout_us / 1000 / perfFrames_, p.style_us / 1000 / perfFrames_,
       p.commit_us / 1000 / perfFrames_, p.animated_us / 1000 / perfFrames_, p.input_us / 1000 / perfFrames_,
       p.js_busy_ms / perfFrames_, p.paragraph_builds, p.gc_collections);
  perfPaint_ = perfPresent_ = perfMax_ = 0;
  gpuRenderMs_ = gpuBlitMs_ = gpuSwapMs_ = 0;
  perfFrames_ = 0;
  perfTick_ = perfTickMax_ = 0;
  perfTicks_ = 0;
}

bool WM::renderGpu() {
#ifdef JSUI_WL_MIR_VK
  if (vulkan_) {
    if (!mir_->vulkan()->frame(rt_)) return false;
    if (perf_) {
      perfPaintStats();
      gpuRenderMs_ += mir_->vulkan()->lastRenderMs();
      gpuBlitMs_ += mir_->vulkan()->lastPassMs();
      gpuSwapMs_ += mir_->vulkan()->lastWaitMs();
    }
    return true;
  }
#endif
#ifdef JSUI_WL_MIR_GL
  if (!gpu_ || !mir_->makeCurrent()) return false;
  bool fresh = false;
  JsuiGLTarget t{};
  t.texture = mir_->desktopTexture(&fresh);
  t.format = mir_->textureFormat();
  t.width = mir_->desktopWidth();
  t.height = mir_->desktopHeight();
  // The texture keeps the last frame: only the damage is repainted into it.
  t.flags = fresh ? 0 : JSUI_GL_INCREMENTAL;
  double t0 = nowMs();
  if (jsui_render_gl_target(rt_, gpu_, &t) != 0) return false;
  double t1 = nowMs();
  lastRenderMs_ = t1 - t0;
  if (perf_) perfPaintStats();
  mir_->presentGpu();
  if (perf_) {
    gpuRenderMs_ += t1 - t0;
    gpuRenderGpuMs_ += mir_->lastRenderGpuMs();
    gpuBlitGpuMs_ += mir_->lastBlitGpuMs();
    gpuBlitMs_ += mir_->lastBlitMs();
    gpuSwapMs_ += mir_->lastSwapMs();
  }
  return true;
#else
  return false;
#endif
}

bool WM::showMir() {
  std::string err;
  const char* snapEnv = getenv("JSUI_WL_SNAPSHOT_BG");
  snapshotBg_ = !(snapEnv && snapEnv[0] == '0');
  const char* perfEnv = getenv("JSUI_WL_PERF");
  perf_ = perfEnv && (perfEnv[0] == '1' || perfEnv[0] == '2');
  perfEvery_ = perf_ && perfEnv[0] == '2';
  const char* gpuEnv = getenv("JSUI_WL_GPU");
  bool wantGpu = !(gpuEnv && gpuEnv[0] == '0');
  if (!mir_->createWindow(sw_, sh_, mirOutput_, wantGpu, &err)) {
    logf("cannot create the Mir window: %s", err.c_str());
    return false;
  }
#ifdef JSUI_WL_MIR_VK
  if (MirVulkan* vk = mir_->vulkan()) {
    gpu_ = vk->gpu();  // MirVulkan's
    vulkan_ = true;
    logf("rendering on the GPU (Skia Graphite on Vulkan, %s)", vk->deviceName().c_str());
    logf("apps can't hand over GPU buffers to the Vulkan renderer");
    shellDirty_ = true;
    scheduleFrame();
  } else if (wantGpu && !mir_->vulkanError().empty()) {
    logf("Vulkan unavailable (%s)", mir_->vulkanError().c_str());
  }
#endif
#ifdef JSUI_WL_MIR_GL
  if (vulkan_) {
  } else if (mir_->gpu()) {
    gpu_ = jsui_gpu_create_gl(nullptr, nullptr);
    if (gpu_) {
      logf("rendering on the GPU (GLES via EGL on the Mir window%s)",
           mir_->presentThread() ? ", presented from a thread of its own" : "");
      const char* fps = getenv("JSUI_WL_MAX_FPS");
      const double maxFps = fps && atof(fps) > 0 ? atof(fps) : 60;
      minFrameMs_ = 1000.0 / maxFps * 0.92;
      const char* vt = getenv("JSUI_WL_VSYNC_TICKS");
      if (mir_->presentThread() && mir_->swapFd() >= 0 && !(vt && vt[0] == '0')) {
        vsyncTicks_ = true;
        wl_event_loop_add_fd(loop_, mir_->swapFd(), WL_EVENT_READABLE, [](int fd, uint32_t, void* u) -> int {
          uint64_t n;
          (void)!read(fd, &n, sizeof n);
          auto* wm = static_cast<WM*>(u);
          if (wm->perfEvery_) logf("swap at %.0f", nowMs());
          if (wm->tickPending_) wm->scheduleTick(0);  // a frame is wanted: now
          return 0;
        }, this);
      }
      gpuApps_ = wlegl::bind(display_, mir_->eglDisplay());
      logf(gpuApps_ ? "apps draw on the GPU too (android_wlegl)" : "apps can't hand over GPU buffers");
      shellDirty_ = true;
      scheduleFrame();
    } else {
      logf("Skia could not use the GL context; rendering on the CPU");
      if (!mir_->fallBackToSoftware(&err)) {
        logf("cannot create the Mir window: %s", err.c_str());
        return false;
      }
    }
  } else if (wantGpu) {
    logf("GPU window unavailable (%s); rendering on the CPU", mir_->gpuError().c_str());
  }
#endif

  wl_event_loop_add_fd(loop_, mir_->fd(), WL_EVENT_READABLE, [](int, uint32_t, void* u) -> int {
    static_cast<WM*>(u)->mirEvents();
    return 0;
  }, this);
  return true;
}

void WM::mirEvents() {
  uint32_t t = uint32_t(nowMs());
  bool touched = false;
  for (const MirHostEvent& e : mir_->takeEvents()) {
    switch (e.kind) {
      case MirHostEvent::Touch:
        touchEvent(e.id, e.phase, e.x, e.y, t);
        touched = true;
        break;
      case MirHostEvent::Pointer:
        wlr_cursor_warp_closest(cursor_, nullptr, e.x, e.y);
        if (e.phase == 0) pointerButton(true, 0x110 /* BTN_LEFT */, t);
        else if (e.phase == 2) pointerButton(false, 0x110, t);
        else pointerMotion(t);
        wlr_seat_pointer_notify_frame(seat_);
        break;
      case MirHostEvent::Key: {
        // The volume buttons (evdev KEY_VOLUMEDOWN / KEY_VOLUMEUP) are the
        // shell's, as on Lomiri: a step of the output volume, not keys for
        // the focused app.
        if (e.id == 114 || e.id == 115) {
          if (e.phase) volumeStep(e.id == 115 ? 1 : -1);
          break;
        }
        wlr_keyboard_key_event ev{};
        ev.time_msec = t;
        ev.keycode = uint32_t(e.id);
        ev.update_state = true;
        ev.state = e.phase ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED;
        wlr_keyboard_notify_key(&vkbd_, &ev);
        keyboardKey(&vkbd_, uint32_t(e.id), e.phase != 0, t);
        break;
      }
      case MirHostEvent::Resize: {
        wlr_output_state st;
        wlr_output_state_init(&st);
        wlr_output_state_set_custom_mode(&st, e.w, e.h, 60000);
        wlr_output_commit_state(output_, &st);
        wlr_output_state_finish(&st);
        mir_->resize(e.w, e.h);
        resizeScreen(e.w, e.h);
        break;
      }
      case MirHostEvent::Close:
        wl_display_terminate(display_);
        break;
    }
  }
  if (touched) wlr_seat_touch_notify_frame(seat_);
}

void WM::rotateTo(int deg) {
  deg = ((deg % 360) + 360) % 360;
  deg -= deg % 90;
  if (!mir_ || deg == mir_->rotation()) return;
  if (gpu_) mir_->makeCurrent();
  if (!mir_->rotateTo(deg)) {
    logf("rotate: not supported on this window");
    return;
  }
  opt_.rotate = deg;
  publishRotation();
  const int w = mir_->desktopWidth(), h = mir_->desktopHeight();
  logf("rotated to %d (desktop %dx%d)", deg, w, h);
  if (w != sw_ || h != sh_) {
    wlr_output_state st;
    wlr_output_state_init(&st);
    wlr_output_state_set_custom_mode(&st, w, h, 60000);
    wlr_output_commit_state(output_, &st);
    wlr_output_state_finish(&st);
    resizeScreen(w, h);
  }
  shellDirty_ = true;
  scheduleTick(0);
}

// The desktop's turn on the panel, for apps: $XDG_RUNTIME_DIR/jsui-wl.rotation
// (degrees clockwise). jsui-camera-rotation.so turns the camera preview by it.
void WM::publishRotation() {
  const char* rt = getenv("XDG_RUNTIME_DIR");
  if (!rt) return;
  std::string path = std::string(rt) + "/jsui-wl.rotation";
  if (FILE* f = std::fopen((path + ".new").c_str(), "w")) {
    std::fprintf(f, "%d\n", opt_.rotate);
    std::fclose(f);
    std::rename((path + ".new").c_str(), path.c_str());
  }
}

// jsui-orientation's lines: the edge of the panel that is up. The panel's
// own top up is rotation 0 (portrait on the Tab A7); "flat" keeps the
// current one. JSUI_WL_ORIENTATION_OFFSET (degrees) is added for a sensor
// mounted turned against the panel.
void WM::orientationLine(const std::string& line) {
  int deg = line == "top" ? 0 : line == "right" ? 90 : line == "bottom" ? 180 : line == "left" ? 270 : -1;
  if (deg < 0) return;
  if (const char* off = getenv("JSUI_WL_ORIENTATION_OFFSET")) deg += std::atoi(off);
  rotateTo(deg);
}

void WM::autoRotate(bool on) {
  autoRotate_ = on;
  if (!on) {
    if (orientSrc_) wl_event_source_remove(orientSrc_);
    orientSrc_ = nullptr;
    if (orientPid_ > 0) kill(orientPid_, SIGTERM);
    orientPid_ = -1;
    orientBuf_.clear();
    logf("auto-rotate off");
    return;
  }
  // jsui-orientation sits next to jsui-wl.
  char exe[4096];
  ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
  if (len <= 0) return;
  exe[len] = 0;
  std::string helper = std::string(exe);
  helper = helper.substr(0, helper.rfind('/') + 1) + "jsui-orientation";
  if (access(helper.c_str(), X_OK) != 0) {
    logf("auto-rotate: no %s", helper.c_str());
    return;
  }
  int fds[2];
  if (pipe2(fds, O_CLOEXEC) != 0) return;
  pid_t pid = fork();
  if (pid == 0) {
    signal(SIGCHLD, SIG_DFL);
    prctl(PR_SET_PDEATHSIG, SIGTERM);  // goes with jsui-wl
    dup2(fds[1], 1);
    execl(helper.c_str(), helper.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  ::close(fds[1]);
  if (pid < 0) {
    ::close(fds[0]);
    return;
  }
  orientPid_ = pid;
  fcntl(fds[0], F_SETFL, O_NONBLOCK);
  orientSrc_ = wl_event_loop_add_fd(loop_, fds[0], WL_EVENT_READABLE, [](int fd, uint32_t mask, void* u) -> int {
    auto* w = static_cast<WM*>(u);
    char buf[256];
    ssize_t n = read(fd, buf, sizeof buf);
    if (n <= 0 && (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR) || n == 0)) {
      wl_event_source_remove(w->orientSrc_);
      w->orientSrc_ = nullptr;
      ::close(fd);
      w->orientPid_ = -1;
      logf("auto-rotate: jsui-orientation ended");
      return 0;
    }
    if (n < 0) return 0;
    w->orientBuf_.append(buf, size_t(n));
    size_t nl;
    while ((nl = w->orientBuf_.find('\n')) != std::string::npos) {
      std::string l = w->orientBuf_.substr(0, nl);
      w->orientBuf_.erase(0, nl + 1);
      w->orientationLine(l);
    }
    return 0;
  }, this);
  logf("auto-rotate on (%s, pid %d)", helper.c_str(), int(pid));
}
#endif

void WM::startNotifyd() {
  // Only with a session bus to serve on.
  const char* rt = getenv("XDG_RUNTIME_DIR");
  if (!getenv("DBUS_SESSION_BUS_ADDRESS") && !(rt && fileExists(std::string(rt) + "/bus"))) return;
  char exe[4096];
  ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
  if (len <= 0) return;
  exe[len] = 0;
  std::string helper = std::string(exe);
  helper = helper.substr(0, helper.rfind('/') + 1) + "jsui-notifyd";
  if (access(helper.c_str(), X_OK) != 0) return;
  // Its stdout to us (notifications), its stdin from us (taps, see
  // notificationAction).
  int fds[2], in[2];
  if (pipe2(fds, O_CLOEXEC) != 0) return;
  if (pipe2(in, O_CLOEXEC) != 0) {
    ::close(fds[0]);
    ::close(fds[1]);
    return;
  }
  pid_t pid = fork();
  if (pid == 0) {
    signal(SIGCHLD, SIG_DFL);
    prctl(PR_SET_PDEATHSIG, SIGTERM);  // goes with jsui-wl
    dup2(fds[1], 1);
    dup2(in[0], 0);
    execl(helper.c_str(), helper.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  ::close(fds[1]);
  ::close(in[0]);
  if (pid < 0) {
    ::close(fds[0]);
    ::close(in[1]);
    return;
  }
  notifyIn_ = in[1];
  fcntl(notifyIn_, F_SETFL, O_NONBLOCK);
  fcntl(fds[0], F_SETFL, O_NONBLOCK);
  notifySrc_ = wl_event_loop_add_fd(loop_, fds[0], WL_EVENT_READABLE, [](int fd, uint32_t mask, void* u) -> int {
    auto* w = static_cast<WM*>(u);
    char buf[1024];
    ssize_t n = read(fd, buf, sizeof buf);
    if (n == 0 || (n < 0 && (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)))) {
      // Ended: another server owns the name, or no bus after all.
      wl_event_source_remove(w->notifySrc_);
      w->notifySrc_ = nullptr;
      ::close(fd);
      if (w->notifyIn_ >= 0) ::close(w->notifyIn_);
      w->notifyIn_ = -1;
      return 0;
    }
    if (n < 0) return 0;
    w->notifyBuf_.append(buf, size_t(n));
    size_t nl;
    while ((nl = w->notifyBuf_.find('\n')) != std::string::npos) {
      std::string l = w->notifyBuf_.substr(0, nl);
      w->notifyBuf_.erase(0, nl + 1);
      if (!l.empty() && l[0] == '{' && w->rt_) jsui_post_message(w->rt_, "wm:notification", l.c_str());
    }
    return 0;
  }, this);
  logf("notifications: %s (pid %d)", helper.c_str(), int(pid));
}

int WM::run() {
#ifdef JSUI_WL_MIR
  if (opt_.mir && !setupMir()) return 1;
#else
  if (opt_.mir) {
    logf("built without Mir support (mirclient)");
    return 1;
  }
#endif
  if (!setup()) return 1;
#ifdef JSUI_WL_MIR
  if (mir_ && !showMir()) return 1;
#endif
  if (!gpuApps_) g_appEnv.insert(g_appEnv.end(), opt_.appEnvNoGpu.begin(), opt_.appEnvNoGpu.end());
  if (!createRuntime()) return 1;
  startNotifyd();
  for (const auto& cmd : opt_.autostart) launch(cmd);
  notifySystemd(socketName_);
  scheduleTick(0);
  wl_display_run(display_);
  logf("quit");
  if (boosted_) gpuBoostWrite(boostRestHz_);
  wl_display_destroy_clients(display_);
  if (rt_) jsui_destroy(rt_);
  wl_display_destroy(display_);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parseArgs(argc, argv, opt)) return 2;
  g_appEnv = opt.appEnv;
  signal(SIGCHLD, SIG_IGN);
  signal(SIGPIPE, SIG_IGN);
  WM wm(opt);
  return wm.run();
}
