// jsui-wm: an X11 window manager and compositor whose whole desktop shell is a
// JSUI React app (ui/shell.jsx), made for 11" touchscreens.
//
//   jsui-wm [--display :1] [--scale 1.5] [--bundle shell.js | --dev URL]
//           [--anchors file.json] [--mouse] [--no-apps-scan] [--verbose]
//
// How it fits together
//
//  * Compositing. Every top-level window is redirected off screen
//    (XCompositeRedirectSubwindows, manual). Each frame the host copies the
//    damaged part of each window's pixmap (XDamage + MIT-SHM) into a JSUI host
//    texture "win:<id>"; the shell draws the windows itself with
//    <HostTexture>, with its own title bars, shadows and animations, inside a
//    full-screen JSUI frame. That frame is rasterised on the CPU (Xvfb has no
//    GPU) with jsui_render_rgba_incremental and only the dirty rectangle is
//    put on the Composite overlay window (XShmPutImage).
//
//  * Input. The overlay takes no input (empty input shape), so the X server
//    still routes pointer and keyboard input by the real window geometry. The
//    host keeps that geometry in step with what the shell draws:
//      - client windows sit where the shell draws their content, so presses on
//        a window's content go straight to the client (a passive grab on
//        unfocused windows focuses and raises them first, then replays);
//      - an InputOnly "frame" window just below each client covers its title
//        bar and resize borders, so presses there go to the shell even when
//        another window is below;
//      - one InputOnly "shell" window above every client covers whatever the
//        shell reports is on top (top bar, dock, open panels, the overview) via
//        wm:shellRegions; its input shape follows those rectangles;
//      - presses on the bare root window (the wallpaper) go to the shell too.
//    Pointer input reaches JSUI as touch by default (one finger: drag to
//    scroll, no hover), real XInput 2.2 touch events on the frame and shell
//    windows as multi-touch; --mouse sends mouse events instead.
//
//  * State. The host publishes the windows and everything else the shell shows
//    as JSUI shared state (wm.screen, wm.windows, wm.stack, wm.active,
//    wm.popups, wm.apps, wm.system) and the shell asks for changes with host
//    messages (wm:move, wm:resize, wm:focus, wm:close, wm:minimize,
//    wm:maximize, wm:snap, wm:launch, wm:typeKey, ...). See README.md.
// json.h first: Xlib #defines Bool and None.
#include "json.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>
#include <dirent.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "jsui/jsui.h"

#ifndef JSUI_WM_BUNDLE
#define JSUI_WM_BUNDLE "integrations/x11-wm/ui/dist/shell.js"
#endif

namespace {

bool g_verbose = false;
volatile sig_atomic_t g_quit = 0;

void logf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::fprintf(stderr, "[jsui-wm] ");
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

// X errors for windows that went away between our requests are normal for a
// window manager; log them in verbose mode only. BadAccess while selecting
// SubstructureRedirect on the root means another WM runs.
bool g_otherWm = false;
int onXError(Display* dpy, XErrorEvent* e) {
  if (e->error_code == BadAccess && e->request_code == 2 /* X_ChangeWindowAttributes */) {
    g_otherWm = true;
    return 0;
  }
  if (g_verbose) {
    char text[256];
    XGetErrorText(dpy, e->error_code, text, sizeof text);
    logf("X error: %s (request %d.%d, resource 0x%lx)", text, e->request_code, e->minor_code, e->resourceid);
  }
  return 0;
}

std::string lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string baseName(const std::string& path) {
  size_t slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool fileExists(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// ------------------------------------------------------------------ Options

struct Options {
  std::string display;
  std::string bundle = JSUI_WM_BUNDLE;
  std::string devUrl;
  std::string anchors;
  float scale = 0;  // 0: pick from the screen size
  bool mouse = false;
  bool scanApps = true;
  std::vector<std::string> autostart;
};

void usage() {
  std::fprintf(stderr,
               "usage: jsui-wm [--display :1] [--scale 1.5] [--bundle shell.js | --dev URL]\n"
               "               [--anchors file.json] [--mouse] [--no-apps-scan] [--verbose]\n"
               "               [--autostart CMD]...\n");
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
    if (a == "--display" && next(v)) o.display = v;
    else if (a == "--scale" && next(v)) o.scale = std::strtof(v.c_str(), nullptr);
    else if (a == "--bundle" && next(v)) o.bundle = v;
    else if (a == "--dev" && next(v)) o.devUrl = v;
    else if (a == "--anchors" && next(v)) o.anchors = v;
    else if (a == "--autostart" && next(v)) o.autostart.push_back(v);
    else if (a == "--mouse") o.mouse = true;
    else if (a == "--no-apps-scan") o.scanApps = false;
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
  std::string id;  // desktop file name without .desktop
  std::string name, comment, exec, icon, wmClass, categories;
  bool terminal = false;
};

std::string findIcon(const std::string& icon) {
  if (icon.empty()) return "";
  if (icon[0] == '/') return fileExists(icon) && icon.size() > 4 && icon.substr(icon.size() - 4) == ".png" ? icon : "";
  static const char* themes[] = {"hicolor", "Adwaita", "gnome", "Papirus", "breeze"};
  static const char* sizes[] = {"128x128", "96x96", "64x64", "256x256", "72x72", "48x48", "512x512", "32x32"};
  for (const char* theme : themes)
    for (const char* size : sizes) {
      std::string p = std::string("/usr/share/icons/") + theme + "/" + size + "/apps/" + icon + ".png";
      if (fileExists(p)) return p;
    }
  std::string p = "/usr/share/pixmaps/" + icon + ".png";
  if (fileExists(p)) return p;
  return "";
}

// Strips the %f/%U/... field codes from a desktop Exec line.
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

std::vector<App> scanApps() {
  std::vector<App> apps;
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
        else if ((k == "NoDisplay" || k == "Hidden") && v == "true") hidden = true;
        else if (k == "OnlyShowIn") hidden = true;  // another desktop's own tool
      }
      if (!isApp || hidden || app.name.empty() || app.exec.empty() || seen.count(app.id)) continue;
      if (app.terminal) app.exec = "xterm -e " + app.exec;
      app.icon = findIcon(app.icon);
      seen.insert(app.id);
      apps.push_back(app);
    }
    closedir(d);
  }
  std::sort(apps.begin(), apps.end(), [](const App& a, const App& b) { return lower(a.name) < lower(b.name); });
  return apps;
}

void launch(const std::string& cmd) {
  if (cmd.empty()) return;
  logf("launch: %s", cmd.c_str());
  pid_t pid = fork();
  if (pid == 0) {
    setsid();
    signal(SIGCHLD, SIG_DFL);
    execl("/bin/sh", "sh", "-c", cmd.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
}

// ------------------------------------------------------------- DOM keys

struct DomKey {
  const char* key;
  const char* code;
};

DomKey domKey(KeySym sym, const char* text) {
  switch (sym) {
    case XK_Return: case XK_KP_Enter: return {"Enter", "Enter"};
    case XK_BackSpace: return {"Backspace", "Backspace"};
    case XK_Escape: return {"Escape", "Escape"};
    case XK_Tab: case XK_ISO_Left_Tab: return {"Tab", "Tab"};
    case XK_Left: return {"ArrowLeft", "ArrowLeft"};
    case XK_Right: return {"ArrowRight", "ArrowRight"};
    case XK_Up: return {"ArrowUp", "ArrowUp"};
    case XK_Down: return {"ArrowDown", "ArrowDown"};
    case XK_Delete: return {"Delete", "Delete"};
    case XK_Home: return {"Home", "Home"};
    case XK_End: return {"End", "End"};
    case XK_space: return {" ", "Space"};
    case XK_Shift_L: return {"Shift", "ShiftLeft"};
    case XK_Shift_R: return {"Shift", "ShiftRight"};
    case XK_Control_L: return {"Control", "ControlLeft"};
    case XK_Control_R: return {"Control", "ControlRight"};
    case XK_Alt_L: return {"Alt", "AltLeft"};
    case XK_Super_L: return {"Meta", "MetaLeft"};
    default: break;
  }
  return {text && *text ? text : nullptr, nullptr};
}

// ------------------------------------------------------------------- WM

struct Rect {
  int x = 0, y = 0, w = 0, h = 0;
};

enum class WinType { Normal, Dialog, Utility, Splash, Dock, Desktop, Notification, Other };

struct Client {
  Window win = 0;
  Window frame = 0;  // InputOnly window under the title bar and borders
  bool managed = true;  // false: an override-redirect popup (menu, tooltip)
  bool mapped = false;
  bool viewable = false;  // shown on screen right now (not minimized)
  Damage damage = 0;
  Pixmap pixmap = 0;
  bool damaged = false;
  Rect dmg;  // damaged area (pixmap px) since the last capture
  int x = 0, y = 0, w = 1, h = 1, bw = 0;
  int depth = 24;
  Visual* visual = nullptr;
  std::vector<uint8_t> rgba;
  int texW = 0, texH = 0;
  bool hasTexture = false;

  std::string title, wmClass, wmInstance, appId;
  WinType type = WinType::Normal;
  Window transientFor = 0;
  bool decorated = true;
  bool deleteWindow = false, takeFocus = false, acceptsFocus = true;
  bool minimized = false, maximized = false, fullscreen = false;
  int snap = 0;  // 1 left, 2 right
  Rect restore;  // client geometry before maximize / snap / fullscreen
  bool hasRestore = false;
  int minW = 1, minH = 1, maxW = 0, maxH = 0;
  int ignoreUnmaps = 0;
  bool focusGrab = false;  // passive click-to-focus grab installed
  bool hasIcon = false;
  long pid = 0;
  double mappedAt = 0;
};

class WM {
 public:
  explicit WM(const Options& o) : opt_(o) {}
  int run();

 private:
  // X setup
  bool openDisplay();
  bool becomeWm();
  void initAtoms();
  void setupEwmh();
  bool setupCompositing();
  bool setupFramebuffer();
  void setupShellWindow();
  void setupInput();
  void grabKeys();

  // JSUI
  bool createRuntime();
  void publishScreen();
  void publishApps();
  void publishSystem();
  void onMessage(const char* name, const char* json);
  void onStateChange(const char* key, const char* json);

  // clients
  Client* find(Window w);
  Client* findByFrame(Window w);
  void scanExisting();
  void manage(Window w, bool existing);
  void addPopup(Window w);
  void unmanage(Client* c, bool destroyed);
  void readProps(Client* c);
  void readTitle(Client* c);
  void readNormalHints(Client* c);
  void readIcon(Client* c);
  void matchApp(Client* c);
  void place(Client* c);
  void configure(Client* c, int x, int y, int w, int h);
  void sendConfigureNotify(Client* c);
  void updateFrame(Client* c);
  void restack();
  void raise(Client* c);
  void focus(Client* c);
  void focusTop();
  void close(Client* c);
  void minimize(Client* c);
  void unminimize(Client* c);
  void setMaximized(Client* c, bool on);
  void setSnap(Client* c, int side);
  void setFullscreen(Client* c, bool on);
  void setWmState(Client* c, long state);
  void setNetWmState(Client* c);
  void updateClientList();
  void startCompositing(Client* c);
  void stopCompositing(Client* c);
  void capture(Client* c);
  void installFocusGrab(Client* c, bool on);
  Rect workArea() const;  // px

  // state publishing
  std::string windowJson(const Client* c) const;
  void publishWindow(const Client* c);
  void publishStack();
  void publishPopups();
  void publishActive();

  // input
  void pointerEvent(int type, float x, float y, int button, unsigned state, Time time);
  void handleXi(XGenericEventCookie* cookie);
  void keyEvent(XKeyEvent& e);
  void typeKey(const std::string& key, int mods);
  void setShellRegions(const wmjson::Value& rects);
  void startMoveResize(Client* c, int direction, int rootX, int rootY);
  void moveResizeMotion(int rootX, int rootY);
  void endMoveResize();

  // event loop
  void handleEvent(XEvent& e);
  void processDamage();
  void present();
  void writeAnchors(const wmjson::Value& v);

  int P(double logical) const { return int(std::lround(logical * scale_)); }
  double L(int px) const { return px / double(scale_); }

  Options opt_;
  Display* dpy_ = nullptr;
  int screen_ = 0;
  Window root_ = 0;
  int sw_ = 0, sh_ = 0;  // screen px
  float scale_ = 1.5f;
  int damageEvent_ = 0, damageError_ = 0, shapeEvent_ = 0, xiOpcode_ = -1;
  bool hasXTest_ = false;
  Window overlay_ = 0, shell_ = 0, check_ = 0;
  GC gc_ = 0;
  XShmSegmentInfo fbShm_{};
  XImage* fbImage_ = nullptr;
  std::vector<uint8_t> frame_;  // JSUI RGBA frame, kept between renders
  XShmSegmentInfo capShm_{};
  size_t capShmSize_ = 0;

  // logical metrics (the shell reads them from wm.screen)
  double topBar_ = 40, dock_ = 84, titleBar_ = 46, border_ = 12;

  JsuiRuntime* rt_ = nullptr;
  std::map<Window, Client*> clients_;
  std::vector<Window> stack_;   // managed clients, bottom to top
  std::vector<Window> popups_;  // override-redirect, bottom to top
  Window active_ = 0;
  std::vector<App> apps_;
  std::map<Window, double> releaseAt_;  // textures kept for close animations
  bool tabletMode_ = true;  // the shell's default (wm.settings.tabletMode)
  double splitRatio_ = 0.5;  // left share of the work area in split view
  bool keyboardGrabbed_ = false;
  int cascade_ = 0;
  bool pointerDown_ = false;
  bool stackDirty_ = false;

  // host-side move / resize (_NET_WM_MOVERESIZE from client-decorated windows)
  Window mrWin_ = 0;
  int mrDir_ = -1, mrStartX_ = 0, mrStartY_ = 0;
  Rect mrStart_;

  struct {
    Atom WM_PROTOCOLS, WM_DELETE_WINDOW, WM_TAKE_FOCUS, WM_STATE, WM_CHANGE_STATE, UTF8_STRING;
    Atom _NET_SUPPORTED, _NET_SUPPORTING_WM_CHECK, _NET_WM_NAME, _NET_CLIENT_LIST, _NET_CLIENT_LIST_STACKING;
    Atom _NET_ACTIVE_WINDOW, _NET_CLOSE_WINDOW, _NET_WM_STATE, _NET_WM_STATE_FULLSCREEN;
    Atom _NET_WM_STATE_MAXIMIZED_VERT, _NET_WM_STATE_MAXIMIZED_HORZ, _NET_WM_STATE_HIDDEN, _NET_WM_STATE_FOCUSED;
    Atom _NET_WM_WINDOW_TYPE, _NET_WM_WINDOW_TYPE_NORMAL, _NET_WM_WINDOW_TYPE_DIALOG, _NET_WM_WINDOW_TYPE_UTILITY;
    Atom _NET_WM_WINDOW_TYPE_SPLASH, _NET_WM_WINDOW_TYPE_DOCK, _NET_WM_WINDOW_TYPE_DESKTOP;
    Atom _NET_WM_WINDOW_TYPE_NOTIFICATION, _NET_WM_WINDOW_TYPE_TOOLBAR, _NET_WM_WINDOW_TYPE_MENU;
    Atom _NET_WM_ICON, _NET_WM_PID, _NET_WM_MOVERESIZE, _NET_FRAME_EXTENTS, _NET_WORKAREA;
    Atom _NET_NUMBER_OF_DESKTOPS, _NET_CURRENT_DESKTOP, _NET_DESKTOP_GEOMETRY, _NET_WM_ALLOWED_ACTIONS;
    Atom _NET_REQUEST_FRAME_EXTENTS, _MOTIF_WM_HINTS, _NET_WM_CM_S0;
  } a_{};
};

// ------------------------------------------------------------- X setup

bool WM::openDisplay() {
  dpy_ = XOpenDisplay(opt_.display.empty() ? nullptr : opt_.display.c_str());
  if (!dpy_) {
    logf("cannot open display %s", opt_.display.empty() ? getenv("DISPLAY") : opt_.display.c_str());
    return false;
  }
  if (!opt_.display.empty()) setenv("DISPLAY", opt_.display.c_str(), 1);
  XSetErrorHandler(onXError);
  screen_ = DefaultScreen(dpy_);
  root_ = RootWindow(dpy_, screen_);
  sw_ = DisplayWidth(dpy_, screen_);
  sh_ = DisplayHeight(dpy_, screen_);
  if (opt_.scale > 0) scale_ = opt_.scale;
  else scale_ = sw_ >= 2400 ? 2.0f : sw_ >= 1700 ? 1.5f : sw_ >= 1500 ? 1.25f : 1.0f;
  logf("screen %dx%d px, scale %.2f (%.0fx%.0f logical)", sw_, sh_, scale_, sw_ / scale_, sh_ / scale_);
  initAtoms();
  return true;
}

void WM::initAtoms() {
  struct {
    Atom* atom;
    const char* name;
  } list[] = {
#define A(n) {&a_.n, #n}
      A(WM_PROTOCOLS), A(WM_DELETE_WINDOW), A(WM_TAKE_FOCUS), A(WM_STATE), A(WM_CHANGE_STATE), A(UTF8_STRING),
      A(_NET_SUPPORTED), A(_NET_SUPPORTING_WM_CHECK), A(_NET_WM_NAME), A(_NET_CLIENT_LIST),
      A(_NET_CLIENT_LIST_STACKING), A(_NET_ACTIVE_WINDOW), A(_NET_CLOSE_WINDOW), A(_NET_WM_STATE),
      A(_NET_WM_STATE_FULLSCREEN), A(_NET_WM_STATE_MAXIMIZED_VERT), A(_NET_WM_STATE_MAXIMIZED_HORZ),
      A(_NET_WM_STATE_HIDDEN), A(_NET_WM_STATE_FOCUSED), A(_NET_WM_WINDOW_TYPE), A(_NET_WM_WINDOW_TYPE_NORMAL),
      A(_NET_WM_WINDOW_TYPE_DIALOG), A(_NET_WM_WINDOW_TYPE_UTILITY), A(_NET_WM_WINDOW_TYPE_SPLASH),
      A(_NET_WM_WINDOW_TYPE_DOCK), A(_NET_WM_WINDOW_TYPE_DESKTOP), A(_NET_WM_WINDOW_TYPE_NOTIFICATION),
      A(_NET_WM_WINDOW_TYPE_TOOLBAR), A(_NET_WM_WINDOW_TYPE_MENU), A(_NET_WM_ICON), A(_NET_WM_PID),
      A(_NET_WM_MOVERESIZE), A(_NET_FRAME_EXTENTS), A(_NET_WORKAREA), A(_NET_NUMBER_OF_DESKTOPS),
      A(_NET_CURRENT_DESKTOP), A(_NET_DESKTOP_GEOMETRY), A(_NET_WM_ALLOWED_ACTIONS), A(_NET_REQUEST_FRAME_EXTENTS),
      A(_MOTIF_WM_HINTS), A(_NET_WM_CM_S0),
#undef A
  };
  for (auto& e : list) *e.atom = XInternAtom(dpy_, e.name, False);
  char cm[32];
  std::snprintf(cm, sizeof cm, "_NET_WM_CM_S%d", screen_);
  a_._NET_WM_CM_S0 = XInternAtom(dpy_, cm, False);
}

bool WM::becomeWm() {
  g_otherWm = false;
  XSelectInput(dpy_, root_,
               SubstructureRedirectMask | SubstructureNotifyMask | ButtonPressMask | ButtonReleaseMask |
                   PointerMotionMask | PropertyChangeMask | StructureNotifyMask);
  XSync(dpy_, False);
  if (g_otherWm) {
    logf("another window manager is running on this display");
    return false;
  }
  return true;
}

void WM::setupEwmh() {
  check_ = XCreateSimpleWindow(dpy_, root_, -10, -10, 1, 1, 0, 0, 0);
  XChangeProperty(dpy_, check_, a_._NET_SUPPORTING_WM_CHECK, XA_WINDOW, 32, PropModeReplace,
                  reinterpret_cast<unsigned char*>(&check_), 1);
  const char* name = "JSUI WM";
  XChangeProperty(dpy_, check_, a_._NET_WM_NAME, a_.UTF8_STRING, 8, PropModeReplace,
                  reinterpret_cast<const unsigned char*>(name), int(std::strlen(name)));
  XChangeProperty(dpy_, root_, a_._NET_SUPPORTING_WM_CHECK, XA_WINDOW, 32, PropModeReplace,
                  reinterpret_cast<unsigned char*>(&check_), 1);
  Atom supported[] = {a_._NET_SUPPORTED, a_._NET_SUPPORTING_WM_CHECK, a_._NET_WM_NAME, a_._NET_CLIENT_LIST,
                      a_._NET_CLIENT_LIST_STACKING, a_._NET_ACTIVE_WINDOW, a_._NET_CLOSE_WINDOW, a_._NET_WM_STATE,
                      a_._NET_WM_STATE_FULLSCREEN, a_._NET_WM_STATE_MAXIMIZED_VERT, a_._NET_WM_STATE_MAXIMIZED_HORZ,
                      a_._NET_WM_STATE_HIDDEN, a_._NET_WM_STATE_FOCUSED, a_._NET_WM_WINDOW_TYPE, a_._NET_WM_ICON,
                      a_._NET_WM_PID, a_._NET_WM_MOVERESIZE, a_._NET_FRAME_EXTENTS, a_._NET_WORKAREA,
                      a_._NET_NUMBER_OF_DESKTOPS, a_._NET_CURRENT_DESKTOP, a_._NET_DESKTOP_GEOMETRY,
                      a_._NET_REQUEST_FRAME_EXTENTS};
  XChangeProperty(dpy_, root_, a_._NET_SUPPORTED, XA_ATOM, 32, PropModeReplace,
                  reinterpret_cast<unsigned char*>(supported), int(sizeof supported / sizeof supported[0]));
  long one = 1, zero = 0;
  XChangeProperty(dpy_, root_, a_._NET_NUMBER_OF_DESKTOPS, XA_CARDINAL, 32, PropModeReplace,
                  reinterpret_cast<unsigned char*>(&one), 1);
  XChangeProperty(dpy_, root_, a_._NET_CURRENT_DESKTOP, XA_CARDINAL, 32, PropModeReplace,
                  reinterpret_cast<unsigned char*>(&zero), 1);
  long geom[2] = {sw_, sh_};
  XChangeProperty(dpy_, root_, a_._NET_DESKTOP_GEOMETRY, XA_CARDINAL, 32, PropModeReplace,
                  reinterpret_cast<unsigned char*>(geom), 2);
  Rect wa = workArea();
  long area[4] = {wa.x, wa.y - P(titleBar_), wa.w, wa.h + P(titleBar_)};
  XChangeProperty(dpy_, root_, a_._NET_WORKAREA, XA_CARDINAL, 32, PropModeReplace,
                  reinterpret_cast<unsigned char*>(area), 4);
  XDefineCursor(dpy_, root_, XCreateFontCursor(dpy_, XC_left_ptr));
  // The compositing manager selection, so clients know translucency works.
  XSetSelectionOwner(dpy_, a_._NET_WM_CM_S0, check_, CurrentTime);
}

bool WM::setupCompositing() {
  int ev, err, major = 0, minor = 2;
  if (!XCompositeQueryExtension(dpy_, &ev, &err) || !XCompositeQueryVersion(dpy_, &major, &minor) ||
      (major == 0 && minor < 2)) {
    logf("the X server has no Composite 0.2+");
    return false;
  }
  if (!XDamageQueryExtension(dpy_, &damageEvent_, &damageError_)) {
    logf("the X server has no DAMAGE");
    return false;
  }
  int fixMajor = 2, fixMinor = 0;
  if (!XFixesQueryExtension(dpy_, &ev, &err) || !XFixesQueryVersion(dpy_, &fixMajor, &fixMinor) || fixMajor < 2) {
    logf("the X server has no XFixes 2+");
    return false;
  }
  if (!XShapeQueryExtension(dpy_, &shapeEvent_, &err)) {
    logf("the X server has no SHAPE");
    return false;
  }
  if (!XShmQueryExtension(dpy_)) {
    logf("the X server has no MIT-SHM (a remote display?)");
    return false;
  }
  int tev, terr, tmaj, tmin;
  hasXTest_ = XTestQueryExtension(dpy_, &tev, &terr, &tmaj, &tmin);
  XCompositeRedirectSubwindows(dpy_, root_, CompositeRedirectManual);
  overlay_ = XCompositeGetOverlayWindow(dpy_, root_);
  // The overlay shows the shell's frame but takes no input.
  XserverRegion empty = XFixesCreateRegion(dpy_, nullptr, 0);
  XFixesSetWindowShapeRegion(dpy_, overlay_, ShapeInput, 0, 0, empty);
  XFixesDestroyRegion(dpy_, empty);
  gc_ = XCreateGC(dpy_, overlay_, 0, nullptr);
  return true;
}

bool WM::setupFramebuffer() {
  Visual* vis = DefaultVisual(dpy_, screen_);
  int depth = DefaultDepth(dpy_, screen_);
  fbImage_ = XShmCreateImage(dpy_, vis, unsigned(depth), ZPixmap, nullptr, &fbShm_, unsigned(sw_), unsigned(sh_));
  if (!fbImage_ || fbImage_->bits_per_pixel != 32) {
    logf("need a 24/32-bit TrueColor screen (got %d bpp)", fbImage_ ? fbImage_->bits_per_pixel : 0);
    return false;
  }
  fbShm_.shmid = shmget(IPC_PRIVATE, size_t(fbImage_->bytes_per_line) * size_t(sh_), IPC_CREAT | 0600);
  fbShm_.shmaddr = fbImage_->data = static_cast<char*>(shmat(fbShm_.shmid, nullptr, 0));
  fbShm_.readOnly = False;
  if (!XShmAttach(dpy_, &fbShm_)) return false;
  XSync(dpy_, False);
  shmctl(fbShm_.shmid, IPC_RMID, nullptr);
  frame_.assign(size_t(sw_) * size_t(sh_) * 4, 0);
  return true;
}

void WM::setupShellWindow() {
  XSetWindowAttributes wa{};
  wa.override_redirect = True;
  wa.event_mask = ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
  shell_ = XCreateWindow(dpy_, root_, 0, 0, unsigned(sw_), unsigned(sh_), 0, 0, InputOnly, CopyFromParent,
                         CWOverrideRedirect | CWEventMask, &wa);
  XShapeCombineRectangles(dpy_, shell_, ShapeInput, 0, 0, nullptr, 0, ShapeSet, Unsorted);
  XMapWindow(dpy_, shell_);
}

void WM::setupInput() {
  int ev, err, major = 2, minor = 2;
  if (XQueryExtension(dpy_, "XInputExtension", &xiOpcode_, &ev, &err) && XIQueryVersion(dpy_, &major, &minor) == Success &&
      (major > 2 || (major == 2 && minor >= 2))) {
    VLOG("XInput %d.%d: multi-touch on the shell", major, minor);
  } else {
    xiOpcode_ = -1;
  }
  auto selectTouch = [&](Window w) {
    if (xiOpcode_ < 0) return;
    unsigned char mask[XIMaskLen(XI_LASTEVENT)] = {0};
    XISetMask(mask, XI_TouchBegin);
    XISetMask(mask, XI_TouchUpdate);
    XISetMask(mask, XI_TouchEnd);
    XIEventMask m{XIAllMasterDevices, int(sizeof mask), mask};
    XISelectEvents(dpy_, w, &m, 1);
  };
  selectTouch(shell_);
}

void WM::grabKeys() {
  auto grab = [&](KeySym sym, unsigned mods) {
    KeyCode kc = XKeysymToKeycode(dpy_, sym);
    if (!kc) return;
    for (unsigned extra : {0u, unsigned(LockMask), unsigned(Mod2Mask), unsigned(LockMask | Mod2Mask)})
      XGrabKey(dpy_, kc, mods | extra, root_, True, GrabModeAsync, GrabModeAsync);
  };
  grab(XK_Super_L, 0);
  grab(XK_Super_R, 0);
  grab(XK_Tab, Mod1Mask);
  grab(XK_F4, Mod1Mask);
  grab(XK_Up, Mod4Mask);
  grab(XK_Down, Mod4Mask);
  grab(XK_Left, Mod4Mask);
  grab(XK_Right, Mod4Mask);
}

Rect WM::workArea() const {
  Rect r;
  r.x = 0;
  r.y = P(topBar_ + titleBar_);
  r.w = sw_;
  r.h = sh_ - P(dock_) - r.y;
  return r;
}

// --------------------------------------------------------------- JSUI

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
  rt_ = jsui_create(&cfg);
  if (!rt_) return false;
  publishScreen();
  publishSystem();
  if (opt_.scanApps) {
    apps_ = scanApps();
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
                  ",\"dock\":" + wmjson::num(dock_) + ",\"titleBar\":" + wmjson::num(titleBar_) +
                  ",\"border\":" + wmjson::num(border_) + ",\"touch\":" + (opt_.mouse ? "false" : "true") +
                  ",\"multiTouch\":" + (xiOpcode_ >= 0 ? "true" : "false") + "}";
  jsui_set_state(rt_, "wm.screen", j.c_str());
}

void WM::publishApps() {
  std::string j = "[";
  for (size_t i = 0; i < apps_.size(); ++i) {
    const App& a = apps_[i];
    if (i) j += ',';
    j += "{\"id\":" + wmjson::quoted(a.id) + ",\"name\":" + wmjson::quoted(a.name) +
         ",\"comment\":" + wmjson::quoted(a.comment) + ",\"exec\":" + wmjson::quoted(a.exec) +
         ",\"icon\":" + wmjson::quoted(a.icon.empty() ? "" : "file://" + a.icon) +
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
  // A battery when the machine has one (a real 11" tablet); Xvfb in Docker has none.
  std::string battery = "null";
  if (DIR* d = opendir("/sys/class/power_supply")) {
    while (dirent* e = readdir(d)) {
      std::string base = std::string("/sys/class/power_supply/") + e->d_name;
      std::ifstream cap(base + "/capacity"), status(base + "/status");
      int pct = -1;
      std::string st;
      if (cap >> pct && pct >= 0) {
        status >> st;
        battery = "{\"percent\":" + std::to_string(pct) + ",\"charging\":" + (st == "Charging" ? "true" : "false") + "}";
        break;
      }
    }
    closedir(d);
  }
  std::string j = "{\"user\":" + wmjson::quoted(user) + ",\"hostname\":" + wmjson::quoted(host) +
                  ",\"battery\":" + battery + ",\"xtest\":" + (hasXTest_ ? "true" : "false") + "}";
  jsui_set_state(rt_, "wm.system", j.c_str());
}

void WM::onStateChange(const char* key, const char* json) {
  if (std::strcmp(key, "wm.settings") == 0) {
    wmjson::Value v = wmjson::parse(json);
    tabletMode_ = v["tabletMode"].truthy();
  }
}

void WM::writeAnchors(const wmjson::Value& v) {
  if (opt_.anchors.empty() || v.type != wmjson::Value::Object) return;
  // In screen px, for test drivers (xdotool) that click on shell elements.
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
  if (v["id"].type == wmjson::Value::Number) c = find(Window(v["id"].num()));
  if (n == "wm:shellRegions") {
    setShellRegions(v["rects"]);
  } else if (n == "wm:anchors") {
    writeAnchors(v);
  } else if (n == "wm:launch") {
    launch(v["exec"].str());
  } else if (n == "wm:focus" && c) {
    if (c->minimized) unminimize(c);
    raise(c);
    focus(c);
  } else if (n == "wm:move" && c) {
    if (c->maximized || c->snap) {
      // Dragging a maximized / snapped window out of place restores its size.
      c->maximized = false;
      c->snap = 0;
      int w = c->hasRestore ? c->restore.w : c->w, h = c->hasRestore ? c->restore.h : c->h;
      configure(c, P(v["x"].num()), P(v["y"].num()), w, h);
      setNetWmState(c);
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
    // Two windows side by side: {left, right, ratio?}.
    Client* l = find(Window(v["left"].num()));
    Client* r = find(Window(v["right"].num()));
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
    for (auto& [w, cl] : clients_)
      if (cl->managed && cl->snap && !cl->minimized) setSnap(cl, cl->snap);
  } else if (n == "wm:fullscreen" && c) {
    setFullscreen(c, v["on"].type == wmjson::Value::Null ? !c->fullscreen : v["on"].truthy());
  } else if (n == "wm:release") {
    Window w = Window(v["id"].num());
    if (!find(w)) {
      releaseAt_.erase(w);
      jsui_remove_texture(rt_, ("win:" + std::to_string(w)).c_str());
      jsui_remove_texture(rt_, ("icon:" + std::to_string(w)).c_str());
    }
  } else if (n == "wm:keyboard") {
    bool grab = v["grab"].truthy();
    if (grab && !keyboardGrabbed_) {
      keyboardGrabbed_ = XGrabKeyboard(dpy_, root_, True, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess;
    } else if (!grab && keyboardGrabbed_) {
      XUngrabKeyboard(dpy_, CurrentTime);
      keyboardGrabbed_ = false;
    }
  } else if (n == "wm:typeKey") {
    typeKey(v["key"].str(), int(v["mods"].num()));
  } else if (n == "wm:closeAll") {
    std::vector<Window> ws = stack_;
    for (Window w : ws)
      if (Client* cc = find(w)) close(cc);
  } else if (n == "wm:quit") {
    g_quit = 1;
  } else if (n == "wm:reloadApps") {
    apps_ = scanApps();
    publishApps();
  }
}

// --------------------------------------------------------------- clients

Client* WM::find(Window w) {
  auto it = clients_.find(w);
  return it == clients_.end() ? nullptr : it->second;
}

Client* WM::findByFrame(Window w) {
  for (auto& [win, c] : clients_)
    if (c->frame == w) return c;
  return nullptr;
}

void WM::scanExisting() {
  Window r, p, *children = nullptr;
  unsigned n = 0;
  if (!XQueryTree(dpy_, root_, &r, &p, &children, &n)) return;
  for (unsigned i = 0; i < n; ++i) {
    Window w = children[i];
    if (w == overlay_ || w == shell_ || w == check_) continue;
    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy_, w, &wa) || wa.c_class == InputOnly || wa.map_state != IsViewable) continue;
    if (wa.override_redirect) addPopup(w);
    else manage(w, true);
  }
  if (children) XFree(children);
}

void WM::readTitle(Client* c) {
  c->title.clear();
  Atom type;
  int format;
  unsigned long n, after;
  unsigned char* data = nullptr;
  if (XGetWindowProperty(dpy_, c->win, a_._NET_WM_NAME, 0, 1024, False, a_.UTF8_STRING, &type, &format, &n, &after,
                         &data) == Success &&
      data && n) {
    c->title.assign(reinterpret_cast<char*>(data), n);
  }
  if (data) XFree(data);
  if (c->title.empty()) {
    XTextProperty tp;
    if (XGetWMName(dpy_, c->win, &tp) && tp.value) {
      char** list = nullptr;
      int count = 0;
      if (Xutf8TextPropertyToTextList(dpy_, &tp, &list, &count) >= Success && count > 0 && list) {
        c->title = list[0];
        XFreeStringList(list);
      } else {
        c->title.assign(reinterpret_cast<char*>(tp.value), tp.nitems);
      }
      XFree(tp.value);
    }
  }
}

void WM::readNormalHints(Client* c) {
  XSizeHints h{};
  long supplied = 0;
  c->minW = c->minH = 1;
  c->maxW = c->maxH = 0;
  if (XGetWMNormalHints(dpy_, c->win, &h, &supplied)) {
    if (h.flags & PMinSize) {
      c->minW = std::max(1, h.min_width);
      c->minH = std::max(1, h.min_height);
    } else if (h.flags & PBaseSize) {
      c->minW = std::max(1, h.base_width);
      c->minH = std::max(1, h.base_height);
    }
    if (h.flags & PMaxSize) {
      c->maxW = h.max_width;
      c->maxH = h.max_height;
    }
  }
}

void WM::readIcon(Client* c) {
  Atom type;
  int format;
  unsigned long n, after;
  unsigned char* data = nullptr;
  c->hasIcon = false;
  if (XGetWindowProperty(dpy_, c->win, a_._NET_WM_ICON, 0, 1 << 22, False, XA_CARDINAL, &type, &format, &n, &after,
                         &data) != Success || !data || n < 2) {
    if (data) XFree(data);
    return;
  }
  // Several sizes back to back (w, h, w*h ARGB longs); take the one closest to 96 px.
  const long* p = reinterpret_cast<const long*>(data);
  size_t best = SIZE_MAX, i = 0;
  long bw = 0, bh = 0;
  while (i + 2 <= n) {
    long w = p[i], h = p[i + 1];
    if (w <= 0 || h <= 0 || i + 2 + size_t(w * h) > n) break;
    if (best == SIZE_MAX || std::labs(w - 96) < std::labs(bw - 96)) {
      best = i;
      bw = w;
      bh = h;
    }
    i += 2 + size_t(w * h);
  }
  if (best != SIZE_MAX) {
    std::vector<uint8_t> rgba(size_t(bw * bh) * 4);
    for (long k = 0; k < bw * bh; ++k) {
      uint32_t argb = uint32_t(p[best + 2 + size_t(k)]);
      rgba[size_t(k) * 4 + 0] = uint8_t(argb >> 16);
      rgba[size_t(k) * 4 + 1] = uint8_t(argb >> 8);
      rgba[size_t(k) * 4 + 2] = uint8_t(argb);
      rgba[size_t(k) * 4 + 3] = uint8_t(argb >> 24);
    }
    jsui_set_texture_pixels(rt_, ("icon:" + std::to_string(c->win)).c_str(), rgba.data(), int(bw), int(bh), 0,
                            JSUI_TEXTURE_UNPREMULTIPLIED);
    c->hasIcon = true;
  }
  XFree(data);
}

void WM::matchApp(Client* c) {
  c->appId.clear();
  std::string cls = lower(c->wmClass), inst = lower(c->wmInstance);
  for (const App& a : apps_) {
    if (!a.wmClass.empty() && (lower(a.wmClass) == cls || lower(a.wmClass) == inst)) {
      c->appId = a.id;
      return;
    }
  }
  for (const App& a : apps_) {
    std::string exe = a.exec.substr(0, a.exec.find(' '));
    exe = lower(baseName(exe));
    if (exe == inst || exe == cls || lower(a.id) == inst || lower(a.id) == cls) {
      c->appId = a.id;
      return;
    }
  }
}

void WM::readProps(Client* c) {
  readTitle(c);
  XClassHint ch{};
  if (XGetClassHint(dpy_, c->win, &ch)) {
    c->wmInstance = ch.res_name ? ch.res_name : "";
    c->wmClass = ch.res_class ? ch.res_class : "";
    if (ch.res_name) XFree(ch.res_name);
    if (ch.res_class) XFree(ch.res_class);
  }
  readNormalHints(c);
  Window tf = 0;
  c->transientFor = XGetTransientForHint(dpy_, c->win, &tf) ? tf : 0;

  Atom* protos = nullptr;
  int count = 0;
  c->deleteWindow = c->takeFocus = false;
  if (XGetWMProtocols(dpy_, c->win, &protos, &count)) {
    for (int i = 0; i < count; ++i) {
      if (protos[i] == a_.WM_DELETE_WINDOW) c->deleteWindow = true;
      if (protos[i] == a_.WM_TAKE_FOCUS) c->takeFocus = true;
    }
    XFree(protos);
  }
  c->acceptsFocus = true;
  if (XWMHints* hints = XGetWMHints(dpy_, c->win)) {
    if (hints->flags & InputHint) c->acceptsFocus = hints->input;
    XFree(hints);
  }

  Atom type;
  int format;
  unsigned long n, after;
  unsigned char* data = nullptr;
  c->type = c->transientFor ? WinType::Dialog : WinType::Normal;
  if (XGetWindowProperty(dpy_, c->win, a_._NET_WM_WINDOW_TYPE, 0, 16, False, XA_ATOM, &type, &format, &n, &after,
                         &data) == Success && data) {
    Atom* types = reinterpret_cast<Atom*>(data);
    for (unsigned long i = 0; i < n; ++i) {
      Atom t = types[i];
      if (t == a_._NET_WM_WINDOW_TYPE_NORMAL) c->type = WinType::Normal;
      else if (t == a_._NET_WM_WINDOW_TYPE_DIALOG) c->type = WinType::Dialog;
      else if (t == a_._NET_WM_WINDOW_TYPE_UTILITY || t == a_._NET_WM_WINDOW_TYPE_TOOLBAR || t == a_._NET_WM_WINDOW_TYPE_MENU)
        c->type = WinType::Utility;
      else if (t == a_._NET_WM_WINDOW_TYPE_SPLASH) c->type = WinType::Splash;
      else if (t == a_._NET_WM_WINDOW_TYPE_DOCK) c->type = WinType::Dock;
      else if (t == a_._NET_WM_WINDOW_TYPE_DESKTOP) c->type = WinType::Desktop;
      else if (t == a_._NET_WM_WINDOW_TYPE_NOTIFICATION) c->type = WinType::Notification;
      else continue;
      break;
    }
  }
  if (data) XFree(data);
  data = nullptr;

  // Motif hints: decorations == 0 asks for no frame (client-side decorations).
  c->decorated = c->type == WinType::Normal || c->type == WinType::Dialog || c->type == WinType::Utility;
  if (XGetWindowProperty(dpy_, c->win, a_._MOTIF_WM_HINTS, 0, 5, False, AnyPropertyType, &type, &format, &n, &after,
                         &data) == Success && data && n >= 3) {
    long* m = reinterpret_cast<long*>(data);
    if ((m[0] & 2) && m[2] == 0) c->decorated = false;
  }
  if (data) XFree(data);
  data = nullptr;

  c->pid = 0;
  if (XGetWindowProperty(dpy_, c->win, a_._NET_WM_PID, 0, 1, False, XA_CARDINAL, &type, &format, &n, &after,
                         &data) == Success && data && n == 1) {
    c->pid = *reinterpret_cast<long*>(data);
  }
  if (data) XFree(data);
  matchApp(c);
}

void WM::manage(Window w, bool existing) {
  if (find(w)) return;
  XWindowAttributes wa;
  if (!XGetWindowAttributes(dpy_, w, &wa) || wa.override_redirect) return;
  auto* c = new Client;
  c->win = w;
  c->x = wa.x;
  c->y = wa.y;
  c->w = std::max(1, wa.width);
  c->h = std::max(1, wa.height);
  c->bw = 0;
  c->depth = wa.depth;
  c->visual = wa.visual;
  clients_[w] = c;
  readProps(c);
  readIcon(c);

  XSelectInput(dpy_, w, PropertyChangeMask | FocusChangeMask);
  XSetWindowBorderWidth(dpy_, w, 0);
  XAddToSaveSet(dpy_, w);
  if (c->type == WinType::Dock || c->type == WinType::Desktop) {
    // Panels / desktops from other environments: shown as they are, undecorated.
    c->decorated = false;
  }
  if (c->decorated) {
    XSetWindowAttributes fa{};
    fa.override_redirect = True;
    fa.event_mask = ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
    c->frame = XCreateWindow(dpy_, root_, 0, 0, 1, 1, 0, 0, InputOnly, CopyFromParent, CWOverrideRedirect | CWEventMask,
                             &fa);
    if (xiOpcode_ >= 0) {
      unsigned char mask[XIMaskLen(XI_LASTEVENT)] = {0};
      XISetMask(mask, XI_TouchBegin);
      XISetMask(mask, XI_TouchUpdate);
      XISetMask(mask, XI_TouchEnd);
      XIEventMask m{XIAllMasterDevices, int(sizeof mask), mask};
      XISelectEvents(dpy_, c->frame, &m, 1);
    }
  }
  if (!existing) place(c);
  else configure(c, c->x, std::max(c->y, workArea().y), c->w, c->h);

  long extents[4] = {0, 0, c->decorated ? P(titleBar_) : 0, 0};
  XChangeProperty(dpy_, w, a_._NET_FRAME_EXTENTS, XA_CARDINAL, 32, PropModeReplace,
                  reinterpret_cast<unsigned char*>(extents), 4);
  stack_.push_back(w);
  c->mapped = true;
  c->viewable = true;
  c->mappedAt = nowMs();
  XMapWindow(dpy_, w);
  if (c->frame) XMapWindow(dpy_, c->frame);
  setWmState(c, NormalState);
  startCompositing(c);
  updateFrame(c);
  restack();
  updateClientList();
  publishWindow(c);
  publishStack();
  if (c->acceptsFocus || c->takeFocus) focus(c);
  logf("manage 0x%lx \"%s\" (%s) %dx%d", w, c->title.c_str(), c->wmClass.c_str(), c->w, c->h);
}

void WM::addPopup(Window w) {
  if (find(w)) return;
  XWindowAttributes wa;
  if (!XGetWindowAttributes(dpy_, w, &wa) || wa.c_class == InputOnly) return;
  auto* c = new Client;
  c->win = w;
  c->managed = false;
  c->decorated = false;
  c->x = wa.x;
  c->y = wa.y;
  c->w = std::max(1, wa.width);
  c->h = std::max(1, wa.height);
  c->bw = wa.border_width;
  c->depth = wa.depth;
  c->visual = wa.visual;
  c->mapped = c->viewable = true;
  clients_[w] = c;
  popups_.push_back(w);
  startCompositing(c);
  publishPopups();
}

void WM::unmanage(Client* c, bool destroyed) {
  Window w = c->win;
  VLOG("unmanage 0x%lx%s", w, destroyed ? " (destroyed)" : "");
  stopCompositing(c);
  if (c->frame) XDestroyWindow(dpy_, c->frame);
  if (!destroyed && c->managed) {
    setWmState(c, WithdrawnState);
    XRemoveFromSaveSet(dpy_, w);
  }
  if (mrWin_ == w) endMoveResize();
  clients_.erase(w);
  stack_.erase(std::remove(stack_.begin(), stack_.end(), w), stack_.end());
  bool popup = !c->managed;
  popups_.erase(std::remove(popups_.begin(), popups_.end(), w), popups_.end());
  std::string id = std::to_string(w);
  if (popup) {
    jsui_remove_texture(rt_, ("win:" + id).c_str());
    publishPopups();
  } else {
    // Keep the last frame until the shell's close animation released it.
    releaseAt_[w] = nowMs() + 3000;
    jsui_patch_state(rt_, "wm.windows", ("{\"" + id + "\":null}").c_str());
    publishStack();
    updateClientList();
    if (active_ == w) {
      active_ = 0;
      focusTop();
    }
  }
  delete c;
}

void WM::place(Client* c) {
  Rect wa = workArea();
  int w = c->w, h = c->h;
  // Keep everything on screen: an 11" screen has no room for 1000 px dialogs off the edge.
  w = std::min(w, wa.w);
  h = std::min(h, wa.h);
  if (c->decorated && c->type == WinType::Normal) {
    // Room for the title bar's touch buttons (xclock, xeyes start tiny).
    w = std::max(w, std::min(wa.w, P(280)));
    h = std::max(h, std::min(wa.h, P(160)));
  }
  int x, y;
  XSizeHints hints{};
  long supplied = 0;
  bool userPos = XGetWMNormalHints(dpy_, c->win, &hints, &supplied) && (hints.flags & USPosition);
  if (c->transientFor) {
    Client* parent = find(c->transientFor);
    if (parent) {
      x = parent->x + (parent->w - w) / 2;
      y = parent->y + (parent->h - h) / 2;
    } else {
      x = wa.x + (wa.w - w) / 2;
      y = wa.y + (wa.h - h) / 2;
    }
  } else if (userPos && c->x + w <= sw_ && c->y + h <= sh_) {
    x = c->x;
    y = std::max(c->y, wa.y);
  } else {
    int step = P(36);
    int off = (cascade_++ % 6) * step - step * 2;
    x = wa.x + (wa.w - w) / 2 + off;
    y = wa.y + (wa.h - h) / 2 + off;
  }
  x = std::clamp(x, wa.x, std::max(wa.x, wa.x + wa.w - w));
  y = std::clamp(y, wa.y, std::max(wa.y, wa.y + wa.h - h));
  configure(c, x, y, w, h);
  if (c->type == WinType::Splash || !c->decorated) return;
  if (tabletMode_ && c->type == WinType::Normal && !c->transientFor) setMaximized(c, true);
}

void WM::configure(Client* c, int x, int y, int w, int h) {
  w = std::max(w, c->minW);
  h = std::max(h, c->minH);
  if (c->maxW > 0) w = std::min(w, c->maxW);
  if (c->maxH > 0) h = std::min(h, c->maxH);
  w = std::max(1, w);
  h = std::max(1, h);
  bool resized = w != c->w || h != c->h;
  bool moved = x != c->x || y != c->y;
  c->x = x;
  c->y = y;
  c->w = w;
  c->h = h;
  XMoveResizeWindow(dpy_, c->win, x, y, unsigned(w), unsigned(h));
  if (!resized && moved) sendConfigureNotify(c);
  updateFrame(c);
  if (c->managed) publishWindow(c);
}

void WM::sendConfigureNotify(Client* c) {
  XConfigureEvent ce{};
  ce.type = ConfigureNotify;
  ce.display = dpy_;
  ce.event = c->win;
  ce.window = c->win;
  ce.x = c->x;
  ce.y = c->y;
  ce.width = c->w;
  ce.height = c->h;
  ce.border_width = 0;
  ce.above = None;
  ce.override_redirect = False;
  XSendEvent(dpy_, c->win, False, StructureNotifyMask, reinterpret_cast<XEvent*>(&ce));
}

void WM::updateFrame(Client* c) {
  if (!c->frame) return;
  bool show = c->viewable && !c->fullscreen;
  if (!show) {
    XUnmapWindow(dpy_, c->frame);
    return;
  }
  int tb = P(titleBar_), m = c->maximized ? 0 : P(border_);
  XMoveResizeWindow(dpy_, c->frame, c->x - m, c->y - tb, unsigned(c->w + 2 * m), unsigned(c->h + tb + m));
  XMapWindow(dpy_, c->frame);
}

void WM::restack() {
  // Top to bottom: popups, the shell's input window, then each client with its
  // frame right below it.
  std::vector<Window> order;
  for (auto it = popups_.rbegin(); it != popups_.rend(); ++it) order.push_back(*it);
  order.push_back(shell_);
  for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
    order.push_back(*it);
    if (Client* c = find(*it); c && c->frame) order.push_back(c->frame);
  }
  XRestackWindows(dpy_, order.data(), int(order.size()));
  stackDirty_ = false;
}

void WM::raise(Client* c) {
  if (!c || !c->managed) return;
  auto it = std::find(stack_.begin(), stack_.end(), c->win);
  if (it == stack_.end()) return;
  // Raise its transient dialogs with it, above it.
  stack_.erase(it);
  stack_.push_back(c->win);
  for (auto& [w, other] : clients_) {
    if (other->managed && other->transientFor == c->win) {
      stack_.erase(std::remove(stack_.begin(), stack_.end(), w), stack_.end());
      stack_.push_back(w);
    }
  }
  restack();
  publishStack();
  updateClientList();
}

void WM::installFocusGrab(Client* c, bool on) {
  if (!c->managed || on == c->focusGrab) return;
  if (on) XGrabButton(dpy_, AnyButton, AnyModifier, c->win, False, ButtonPressMask, GrabModeSync, GrabModeAsync, None, None);
  else XUngrabButton(dpy_, AnyButton, AnyModifier, c->win);
  c->focusGrab = on;
}

void WM::focus(Client* c) {
  Window prev = active_;
  if (!c) {
    active_ = 0;
    XSetInputFocus(dpy_, root_, RevertToPointerRoot, CurrentTime);
  } else {
    active_ = c->win;
    if (c->acceptsFocus) XSetInputFocus(dpy_, c->win, RevertToPointerRoot, CurrentTime);
    if (c->takeFocus) {
      XEvent ev{};
      ev.xclient.type = ClientMessage;
      ev.xclient.window = c->win;
      ev.xclient.message_type = a_.WM_PROTOCOLS;
      ev.xclient.format = 32;
      ev.xclient.data.l[0] = long(a_.WM_TAKE_FOCUS);
      ev.xclient.data.l[1] = CurrentTime;
      XSendEvent(dpy_, c->win, False, NoEventMask, &ev);
    }
  }
  for (auto& [w, cl] : clients_) installFocusGrab(cl, cl->managed && w != active_);
  XChangeProperty(dpy_, root_, a_._NET_ACTIVE_WINDOW, XA_WINDOW, 32, PropModeReplace,
                  reinterpret_cast<unsigned char*>(&active_), 1);
  if (Client* p = find(prev)) setNetWmState(p);
  if (c) setNetWmState(c);
  publishActive();
}

void WM::focusTop() {
  for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
    Client* c = find(*it);
    if (c && c->viewable && c->type != WinType::Dock && c->type != WinType::Desktop) {
      focus(c);
      return;
    }
  }
  focus(nullptr);
}

void WM::close(Client* c) {
  if (c->deleteWindow) {
    XEvent ev{};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = c->win;
    ev.xclient.message_type = a_.WM_PROTOCOLS;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = long(a_.WM_DELETE_WINDOW);
    ev.xclient.data.l[1] = CurrentTime;
    XSendEvent(dpy_, c->win, False, NoEventMask, &ev);
  } else {
    XKillClient(dpy_, c->win);
  }
}

void WM::minimize(Client* c) {
  if (c->minimized || !c->managed) return;
  capture(c);  // the dock / overview keep showing its last frame
  c->minimized = true;
  c->viewable = false;
  c->ignoreUnmaps++;
  XUnmapWindow(dpy_, c->win);
  updateFrame(c);
  setWmState(c, IconicState);
  setNetWmState(c);
  publishWindow(c);
  if (active_ == c->win) focusTop();
}

void WM::unminimize(Client* c) {
  if (!c->minimized) return;
  c->minimized = false;
  c->viewable = true;
  XMapWindow(dpy_, c->win);
  updateFrame(c);
  setWmState(c, NormalState);
  setNetWmState(c);
  publishWindow(c);
}

void WM::setMaximized(Client* c, bool on) {
  if (!c->decorated && on) return;
  if (on == c->maximized && !c->snap) return;
  if (on) {
    if (!c->maximized && !c->snap) {
      c->restore = {c->x, c->y, c->w, c->h};
      c->hasRestore = true;
    }
    c->snap = 0;
    c->maximized = true;
    Rect wa = workArea();
    configure(c, wa.x, wa.y, wa.w, wa.h);
  } else {
    c->maximized = false;
    if (c->hasRestore) configure(c, c->restore.x, c->restore.y, c->restore.w, c->restore.h);
  }
  setNetWmState(c);
  publishWindow(c);
}

void WM::setSnap(Client* c, int side) {
  if (side == 0) {
    c->snap = 0;
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
  Rect wa = workArea();
  // Split view: the left window gets splitRatio_ of the width, a 2 px seam between.
  int left = int(std::lround(wa.w * splitRatio_)) - P(2);
  int right0 = int(std::lround(wa.w * splitRatio_)) + P(2);
  configure(c, side == 1 ? wa.x : wa.x + right0, wa.y, side == 1 ? left : wa.w - right0, wa.h);
  setNetWmState(c);
  publishWindow(c);
}

void WM::setFullscreen(Client* c, bool on) {
  if (on == c->fullscreen) return;
  if (on) {
    if (!c->maximized && !c->snap) {
      c->restore = {c->x, c->y, c->w, c->h};
      c->hasRestore = true;
    }
    c->fullscreen = true;
    configure(c, 0, 0, sw_, sh_);
    raise(c);
  } else {
    c->fullscreen = false;
    if (c->maximized) {
      Rect wa = workArea();
      configure(c, wa.x, wa.y, wa.w, wa.h);
    } else if (c->hasRestore) {
      configure(c, c->restore.x, c->restore.y, c->restore.w, c->restore.h);
    }
  }
  updateFrame(c);
  setNetWmState(c);
  publishWindow(c);
}

void WM::setWmState(Client* c, long state) {
  long data[2] = {state, None};
  XChangeProperty(dpy_, c->win, a_.WM_STATE, a_.WM_STATE, 32, PropModeReplace, reinterpret_cast<unsigned char*>(data), 2);
}

void WM::setNetWmState(Client* c) {
  std::vector<Atom> st;
  if (c->fullscreen) st.push_back(a_._NET_WM_STATE_FULLSCREEN);
  if (c->maximized) {
    st.push_back(a_._NET_WM_STATE_MAXIMIZED_VERT);
    st.push_back(a_._NET_WM_STATE_MAXIMIZED_HORZ);
  }
  if (c->minimized) st.push_back(a_._NET_WM_STATE_HIDDEN);
  if (c->win == active_) st.push_back(a_._NET_WM_STATE_FOCUSED);
  XChangeProperty(dpy_, c->win, a_._NET_WM_STATE, XA_ATOM, 32, PropModeReplace,
                  reinterpret_cast<unsigned char*>(st.data()), int(st.size()));
}

void WM::updateClientList() {
  std::vector<Window> list;
  for (auto& [w, c] : clients_)
    if (c->managed) list.push_back(w);
  XChangeProperty(dpy_, root_, a_._NET_CLIENT_LIST, XA_WINDOW, 32, PropModeReplace,
                  reinterpret_cast<unsigned char*>(list.data()), int(list.size()));
  XChangeProperty(dpy_, root_, a_._NET_CLIENT_LIST_STACKING, XA_WINDOW, 32, PropModeReplace,
                  reinterpret_cast<unsigned char*>(stack_.data()), int(stack_.size()));
}

// ------------------------------------------------------------ compositing

void WM::startCompositing(Client* c) {
  if (!c->damage) {
    c->damage = XDamageCreate(dpy_, c->win, XDamageReportNonEmpty);
  }
  if (c->pixmap) XFreePixmap(dpy_, c->pixmap);
  c->pixmap = XCompositeNameWindowPixmap(dpy_, c->win);
  c->damaged = true;
  c->dmg = {0, 0, c->w + 2 * c->bw, c->h + 2 * c->bw};
}

void WM::stopCompositing(Client* c) {
  if (c->damage) XDamageDestroy(dpy_, c->damage);
  if (c->pixmap) XFreePixmap(dpy_, c->pixmap);
  c->damage = 0;
  c->pixmap = 0;
}

void WM::capture(Client* c) {
  if (!c->pixmap || !c->damaged) return;
  int pw = c->w + 2 * c->bw, ph = c->h + 2 * c->bw;
  bool full = c->texW != pw || c->texH != ph || !c->hasTexture;
  Rect r = full ? Rect{0, 0, pw, ph} : c->dmg;
  r.x = std::clamp(r.x, 0, pw);
  r.y = std::clamp(r.y, 0, ph);
  r.w = std::clamp(r.w, 0, pw - r.x);
  r.h = std::clamp(r.h, 0, ph - r.y);
  c->damaged = false;
  c->dmg = {};
  if (r.w <= 0 || r.h <= 0) return;

  size_t need = size_t(r.w) * size_t(r.h) * 4;
  if (need > capShmSize_) {
    if (capShm_.shmaddr) {
      XShmDetach(dpy_, &capShm_);
      XSync(dpy_, False);
      shmdt(capShm_.shmaddr);
    }
    capShmSize_ = std::max(need, size_t(sw_) * size_t(sh_) * 4);
    capShm_.shmid = shmget(IPC_PRIVATE, capShmSize_, IPC_CREAT | 0600);
    capShm_.shmaddr = static_cast<char*>(shmat(capShm_.shmid, nullptr, 0));
    capShm_.readOnly = False;
    XShmAttach(dpy_, &capShm_);
    XSync(dpy_, False);
    shmctl(capShm_.shmid, IPC_RMID, nullptr);
  }
  XImage* img = XShmCreateImage(dpy_, c->visual, unsigned(c->depth), ZPixmap, nullptr, &capShm_, unsigned(r.w),
                                unsigned(r.h));
  if (!img) return;
  img->data = capShm_.shmaddr;
  bool ok = img->bits_per_pixel == 32 && XShmGetImage(dpy_, c->pixmap, img, r.x, r.y, AllPlanes);
  if (ok) {
    if (full) {
      c->rgba.assign(size_t(pw) * size_t(ph) * 4, 0);
      c->texW = pw;
      c->texH = ph;
    }
    bool alpha = c->depth == 32;
    for (int y = 0; y < r.h; ++y) {
      const uint8_t* src = reinterpret_cast<const uint8_t*>(img->data) + size_t(y) * size_t(img->bytes_per_line);
      uint8_t* dst = c->rgba.data() + (size_t(r.y + y) * size_t(pw) + size_t(r.x)) * 4;
      for (int x = 0; x < r.w; ++x, src += 4, dst += 4) {
        dst[0] = src[2];
        dst[1] = src[1];
        dst[2] = src[0];
        dst[3] = alpha ? src[3] : 255;
      }
    }
    jsui_set_texture_pixels(rt_, ("win:" + std::to_string(c->win)).c_str(), c->rgba.data(), pw, ph, 0,
                            alpha ? 0 : JSUI_TEXTURE_OPAQUE);
    if (!c->hasTexture) {
      c->hasTexture = true;
      if (c->managed) publishWindow(c);
      else publishPopups();
    }
  }
  img->data = nullptr;
  XDestroyImage(img);
}

void WM::processDamage() {
  for (auto& [w, c] : clients_)
    if (c->damaged && c->viewable) capture(c);
}

void WM::present() {
  JsuiRect dirty{};
  if (!jsui_render_rgba_incremental(rt_, frame_.data(), sw_, sh_, size_t(sw_) * 4, &dirty)) return;
  if (dirty.width <= 0 || dirty.height <= 0) return;
  int x0 = std::max(0, dirty.x), y0 = std::max(0, dirty.y);
  int x1 = std::min(sw_, dirty.x + dirty.width), y1 = std::min(sh_, dirty.y + dirty.height);
  for (int y = y0; y < y1; ++y) {
    const uint8_t* src = frame_.data() + (size_t(y) * size_t(sw_) + size_t(x0)) * 4;
    uint8_t* dst = reinterpret_cast<uint8_t*>(fbImage_->data) + size_t(y) * size_t(fbImage_->bytes_per_line) + size_t(x0) * 4;
    for (int x = x0; x < x1; ++x, src += 4, dst += 4) {
      dst[0] = src[2];
      dst[1] = src[1];
      dst[2] = src[0];
      dst[3] = 255;
    }
  }
  XShmPutImage(dpy_, overlay_, gc_, fbImage_, x0, y0, x0, y0, unsigned(x1 - x0), unsigned(y1 - y0), False);
  XSync(dpy_, False);  // the shared buffer is ours again
}

// ------------------------------------------------------------- state out

std::string WM::windowJson(const Client* c) const {
  const char* type = "normal";
  switch (c->type) {
    case WinType::Dialog: type = "dialog"; break;
    case WinType::Utility: type = "utility"; break;
    case WinType::Splash: type = "splash"; break;
    case WinType::Dock: type = "dock"; break;
    case WinType::Desktop: type = "desktop"; break;
    case WinType::Notification: type = "notification"; break;
    default: break;
  }
  bool resizable = c->maxW == 0 || c->maxW != c->minW || c->maxH != c->minH;
  std::string j = "{\"id\":" + std::to_string(c->win) + ",\"title\":" + wmjson::quoted(c->title) +
                  ",\"wmClass\":" + wmjson::quoted(c->wmClass) + ",\"instance\":" + wmjson::quoted(c->wmInstance) +
                  ",\"appId\":" + wmjson::quoted(c->appId) + ",\"x\":" + wmjson::num(L(c->x)) +
                  ",\"y\":" + wmjson::num(L(c->y)) + ",\"w\":" + wmjson::num(L(c->w)) + ",\"h\":" + wmjson::num(L(c->h)) +
                  ",\"minW\":" + wmjson::num(L(c->minW)) + ",\"minH\":" + wmjson::num(L(c->minH)) +
                  ",\"restoreW\":" + wmjson::num(L(c->hasRestore ? c->restore.w : c->w)) +
                  ",\"restoreH\":" + wmjson::num(L(c->hasRestore ? c->restore.h : c->h)) +
                  ",\"type\":\"" + type + "\",\"decorated\":" + (c->decorated ? "true" : "false") +
                  ",\"resizable\":" + (resizable ? "true" : "false") +
                  ",\"transientFor\":" + std::to_string(c->transientFor) +
                  ",\"minimized\":" + (c->minimized ? "true" : "false") +
                  ",\"maximized\":" + (c->maximized ? "true" : "false") +
                  ",\"fullscreen\":" + (c->fullscreen ? "true" : "false") + ",\"snap\":\"" +
                  (c->snap == 1 ? "left" : c->snap == 2 ? "right" : "") + "\",\"icon\":" +
                  (c->hasIcon ? "true" : "false") + ",\"ready\":" + (c->hasTexture ? "true" : "false") +
                  ",\"pid\":" + std::to_string(c->pid) + ",\"mappedAt\":" + wmjson::num(c->mappedAt) + "}";
  return j;
}

void WM::publishWindow(const Client* c) {
  if (!c->managed) return;
  std::string j = "{\"" + std::to_string(c->win) + "\":" + windowJson(c) + "}";
  // Merge patch: replace the whole window object (null members never appear).
  jsui_patch_state(rt_, "wm.windows", j.c_str());
}

void WM::publishStack() {
  std::string j = "[";
  for (size_t i = 0; i < stack_.size(); ++i) {
    if (i) j += ',';
    j += std::to_string(stack_[i]);
  }
  j += "]";
  jsui_set_state(rt_, "wm.stack", j.c_str());
}

void WM::publishPopups() {
  std::string j = "{";
  bool first = true;
  for (Window w : popups_) {
    Client* c = find(w);
    if (!c || !c->viewable) continue;
    if (!first) j += ',';
    first = false;
    j += "\"" + std::to_string(w) + "\":{\"id\":" + std::to_string(w) + ",\"x\":" + wmjson::num(L(c->x)) +
         ",\"y\":" + wmjson::num(L(c->y)) + ",\"w\":" + wmjson::num(L(c->w + 2 * c->bw)) +
         ",\"h\":" + wmjson::num(L(c->h + 2 * c->bw)) + ",\"ready\":" + (c->hasTexture ? "true" : "false") + "}";
  }
  j += "}";
  jsui_set_state(rt_, "wm.popups", j.c_str());
}

void WM::publishActive() { jsui_set_state(rt_, "wm.active", std::to_string(active_).c_str()); }

// ------------------------------------------------------------------ input

void WM::setShellRegions(const wmjson::Value& rects) {
  std::vector<XRectangle> xr;
  for (size_t i = 0; i < rects.size(); ++i) {
    const wmjson::Value& r = rects[i];
    int x = P(r[size_t(0)].num()), y = P(r[size_t(1)].num());
    int w = P(r[size_t(0)].num() + r[size_t(2)].num()) - x, h = P(r[size_t(1)].num() + r[size_t(3)].num()) - y;
    if (w <= 0 || h <= 0) continue;
    xr.push_back({short(std::clamp(x, -32000, 32000)), short(std::clamp(y, -32000, 32000)),
                  static_cast<unsigned short>(std::min(w, 65535)), static_cast<unsigned short>(std::min(h, 65535))});
  }
  XShapeCombineRectangles(dpy_, shell_, ShapeInput, 0, 0, xr.data(), int(xr.size()), ShapeSet, Unsorted);
}

void WM::pointerEvent(int type, float x, float y, int button, unsigned state, Time) {
  float lx = float(x / scale_), ly = float(y / scale_);
  int mods = ((state & ShiftMask) ? JSUI_MOD_SHIFT : 0) | ((state & ControlMask) ? JSUI_MOD_CTRL : 0) |
             ((state & Mod1Mask) ? JSUI_MOD_ALT : 0) | ((state & Mod4Mask) ? JSUI_MOD_META : 0);
  jsui_set_event_time(rt_, nowMs());
  if (type == MotionNotify) {
    if (opt_.mouse) jsui_send_mouse_move(rt_, lx, ly);
    else if (pointerDown_) jsui_send_touch(rt_, 0, 1, lx, ly);
    return;
  }
  if (button == 4 || button == 5 || button == 6 || button == 7) {
    if (type == ButtonPress) {
      float dy = button == 4 ? -60.f : button == 5 ? 60.f : 0.f;
      float dx = button == 6 ? -60.f : button == 7 ? 60.f : 0.f;
      jsui_send_mouse_wheel_ex(rt_, lx, ly, dx, dy, JSUI_WHEEL_STEPS);
    }
    return;
  }
  bool down = type == ButtonPress;
  if (opt_.mouse || button != 1) {
    JsuiMouseButton b = button == 3 ? JSUI_MOUSE_RIGHT : button == 2 ? JSUI_MOUSE_MIDDLE : JSUI_MOUSE_LEFT;
    jsui_send_mouse_button(rt_, lx, ly, b, down ? 1 : 0, mods);
  } else {
    if (!down && !pointerDown_) return;
    // A release can get lost (its window went away mid-press): end that touch first.
    if (down && pointerDown_) jsui_send_touch(rt_, 0, 3, lx, ly);
    pointerDown_ = down;
    jsui_send_touch(rt_, 0, down ? 0 : 2, lx, ly);
  }
}

void WM::handleXi(XGenericEventCookie* cookie) {
  if (cookie->extension != xiOpcode_ || !XGetEventData(dpy_, cookie)) return;
  auto* e = static_cast<XIDeviceEvent*>(cookie->data);
  int phase = cookie->evtype == XI_TouchBegin ? 0 : cookie->evtype == XI_TouchUpdate ? 1 : 2;
  int id = 1 + (e->detail % 1000);
  jsui_set_event_time(rt_, nowMs());
  jsui_send_touch(rt_, id, phase, float(e->root_x / scale_), float(e->root_y / scale_));
  XFreeEventData(dpy_, cookie);
}

void WM::keyEvent(XKeyEvent& e) {
  char text[32] = {0};
  KeySym sym = 0;
  int len = XLookupString(&e, text, sizeof text - 1, &sym, nullptr);
  text[std::max(0, len)] = 0;
  bool down = e.type == KeyPress;
  bool shellOwnsKeys = keyboardGrabbed_;
  // Global shortcuts (grabbed on the root).
  if (!shellOwnsKeys || (e.state & Mod4Mask) || sym == XK_Super_L || sym == XK_Super_R) {
    if (!down) {
      if (sym == XK_Super_L || sym == XK_Super_R) jsui_post_message(rt_, "wm:hotkey", "{\"name\":\"super\"}");
      return;
    }
    Client* c = find(active_);
    if (sym == XK_Tab && (e.state & Mod1Mask)) jsui_post_message(rt_, "wm:hotkey", "{\"name\":\"switch\"}");
    else if (sym == XK_F4 && (e.state & Mod1Mask) && c) close(c);
    else if (sym == XK_Up && (e.state & Mod4Mask) && c) setMaximized(c, true);
    else if (sym == XK_Down && (e.state & Mod4Mask) && c) c->maximized || c->snap ? setMaximized(c, false) : minimize(c);
    else if (sym == XK_Left && (e.state & Mod4Mask) && c) setSnap(c, 1);
    else if (sym == XK_Right && (e.state & Mod4Mask) && c) setSnap(c, 2);
    if (!shellOwnsKeys) return;
  }
  DomKey dk = domKey(sym, text);
  int mods = ((e.state & ShiftMask) ? JSUI_MOD_SHIFT : 0) | ((e.state & ControlMask) ? JSUI_MOD_CTRL : 0) |
             ((e.state & Mod1Mask) ? JSUI_MOD_ALT : 0);
  jsui_send_key(rt_, dk.key, dk.code, down ? 1 : 0, 0, mods);
  if (down && len > 0 && static_cast<unsigned char>(text[0]) >= 0x20 && text[0] != 0x7f) jsui_send_text(rt_, text);
}

// The on-screen keyboard: into the shell's own text field while it has the
// keyboard, otherwise into the focused client with XTest.
void WM::typeKey(const std::string& key, int mods) {
  if (key.empty()) return;
  if (keyboardGrabbed_) {
    jsui_send_key(rt_, key.c_str(), nullptr, 1, 0, mods);
    if (key.size() <= 4 && static_cast<unsigned char>(key[0]) >= 0x20 && key != "Enter") jsui_send_text(rt_, key.c_str());
    jsui_send_key(rt_, key.c_str(), nullptr, 0, 0, mods);
    return;
  }
  if (!hasXTest_) return;
  static const std::map<std::string, KeySym> named = {
      {"Enter", XK_Return},     {"Backspace", XK_BackSpace}, {"Tab", XK_Tab},     {"Escape", XK_Escape},
      {"ArrowLeft", XK_Left},   {"ArrowRight", XK_Right},    {"ArrowUp", XK_Up},  {"ArrowDown", XK_Down},
      {" ", XK_space},          {"Delete", XK_Delete},       {"Home", XK_Home},   {"End", XK_End},
  };
  KeySym sym = NoSymbol;
  auto it = named.find(key);
  if (it != named.end()) sym = it->second;
  else {
    // One UTF-8 character.
    const unsigned char* p = reinterpret_cast<const unsigned char*>(key.c_str());
    unsigned cp = p[0];
    if (cp >= 0xF0) cp = ((p[0] & 7u) << 18) | ((p[1] & 63u) << 12) | ((p[2] & 63u) << 6) | (p[3] & 63u);
    else if (cp >= 0xE0) cp = ((p[0] & 15u) << 12) | ((p[1] & 63u) << 6) | (p[2] & 63u);
    else if (cp >= 0xC0) cp = ((p[0] & 31u) << 6) | (p[1] & 63u);
    sym = cp < 0x100 ? KeySym(cp) : KeySym(0x01000000 | cp);
  }
  KeyCode kc = XKeysymToKeycode(dpy_, sym);
  bool shift = false;
  if (kc) {
    if (XkbKeycodeToKeysym(dpy_, kc, 0, 0) != sym && XkbKeycodeToKeysym(dpy_, kc, 0, 1) == sym) shift = true;
  } else {
    // Not on the keyboard map: borrow a spare keycode for it.
    int minK, maxK;
    XDisplayKeycodes(dpy_, &minK, &maxK);
    int per = 0;
    KeySym* map = XGetKeyboardMapping(dpy_, KeyCode(minK), maxK - minK + 1, &per);
    for (int k = maxK; k >= minK && map; --k) {
      bool empty = true;
      for (int j = 0; j < per; ++j) empty = empty && map[(k - minK) * per + j] == NoSymbol;
      if (empty) {
        KeySym syms[2] = {sym, sym};
        XChangeKeyboardMapping(dpy_, k, 2, syms, 1);
        XSync(dpy_, False);
        kc = KeyCode(k);
        break;
      }
    }
    if (map) XFree(map);
    if (!kc) return;
  }
  KeyCode shiftKc = XKeysymToKeycode(dpy_, XK_Shift_L), ctrlKc = XKeysymToKeycode(dpy_, XK_Control_L);
  bool ctrl = mods & JSUI_MOD_CTRL;
  shift = shift || (mods & JSUI_MOD_SHIFT);
  if (ctrl) XTestFakeKeyEvent(dpy_, ctrlKc, True, 0);
  if (shift) XTestFakeKeyEvent(dpy_, shiftKc, True, 0);
  XTestFakeKeyEvent(dpy_, kc, True, 0);
  XTestFakeKeyEvent(dpy_, kc, False, 0);
  if (shift) XTestFakeKeyEvent(dpy_, shiftKc, False, 0);
  if (ctrl) XTestFakeKeyEvent(dpy_, ctrlKc, False, 0);
  XFlush(dpy_);
}

void WM::startMoveResize(Client* c, int direction, int rootX, int rootY) {
  if (!c || c->fullscreen) return;
  if (direction == 11) {  // _NET_WM_MOVERESIZE_CANCEL
    endMoveResize();
    return;
  }
  if (direction < 0 || direction > 8) return;
  if (XGrabPointer(dpy_, root_, False, ButtonReleaseMask | PointerMotionMask, GrabModeAsync, GrabModeAsync, None, None,
                   CurrentTime) != GrabSuccess)
    return;
  mrWin_ = c->win;
  mrDir_ = direction;
  mrStartX_ = rootX;
  mrStartY_ = rootY;
  mrStart_ = {c->x, c->y, c->w, c->h};
  raise(c);
  focus(c);
}

void WM::moveResizeMotion(int rootX, int rootY) {
  Client* c = find(mrWin_);
  if (!c) return endMoveResize();
  int dx = rootX - mrStartX_, dy = rootY - mrStartY_;
  Rect r = mrStart_;
  if (mrDir_ == 8) {
    r.x += dx;
    r.y += dy;
    c->maximized = false;
    c->snap = 0;
    configure(c, r.x, std::max(r.y, P(topBar_)), r.w, r.h);
    return;
  }
  // 0 top-left, 1 top, 2 top-right, 3 right, 4 bottom-right, 5 bottom, 6 bottom-left, 7 left
  bool left = mrDir_ == 0 || mrDir_ == 6 || mrDir_ == 7, right = mrDir_ == 2 || mrDir_ == 3 || mrDir_ == 4;
  bool top = mrDir_ == 0 || mrDir_ == 1 || mrDir_ == 2, bottom = mrDir_ == 4 || mrDir_ == 5 || mrDir_ == 6;
  if (left) {
    r.x += dx;
    r.w -= dx;
  }
  if (right) r.w += dx;
  if (top) {
    r.y += dy;
    r.h -= dy;
  }
  if (bottom) r.h += dy;
  c->maximized = false;
  c->snap = 0;
  configure(c, r.x, r.y, std::max(r.w, c->minW), std::max(r.h, c->minH));
}

void WM::endMoveResize() {
  if (!mrWin_) return;
  XUngrabPointer(dpy_, CurrentTime);
  mrWin_ = 0;
  mrDir_ = -1;
}

// ------------------------------------------------------------- event loop

void WM::handleEvent(XEvent& e) {
  if (e.type == damageEvent_ + XDamageNotify) {
    auto* de = reinterpret_cast<XDamageNotifyEvent*>(&e);
    Client* c = find(de->drawable);
    XserverRegion parts = XFixesCreateRegion(dpy_, nullptr, 0);
    XDamageSubtract(dpy_, de->damage, None, parts);
    if (c) {
      int n = 0;
      XRectangle bounds{};
      XRectangle* rects = XFixesFetchRegionAndBounds(dpy_, parts, &n, &bounds);
      if (rects) XFree(rects);
      Rect r{bounds.x, bounds.y, bounds.width, bounds.height};
      if (n > 0 && r.w > 0 && r.h > 0) {
        if (!c->damaged || c->dmg.w <= 0) c->dmg = r;
        else {
          int x0 = std::min(c->dmg.x, r.x), y0 = std::min(c->dmg.y, r.y);
          int x1 = std::max(c->dmg.x + c->dmg.w, r.x + r.w), y1 = std::max(c->dmg.y + c->dmg.h, r.y + r.h);
          c->dmg = {x0, y0, x1 - x0, y1 - y0};
        }
        c->damaged = true;
      }
    }
    XFixesDestroyRegion(dpy_, parts);
    return;
  }
  switch (e.type) {
    case MapRequest: {
      Window w = e.xmaprequest.window;
      if (Client* c = find(w)) {
        if (c->minimized) unminimize(c);
        raise(c);
        focus(c);
      } else {
        manage(w, false);
      }
      break;
    }
    case MapNotify: {
      Window w = e.xmap.window;
      if (w == overlay_ || w == shell_ || w == check_ || findByFrame(w)) break;
      if (e.xmap.override_redirect) {
        if (!find(w)) addPopup(w);
        restack();
      } else if (Client* c = find(w)) {
        c->mapped = true;
        startCompositing(c);  // a new pixmap each time it is mapped
      }
      break;
    }
    case UnmapNotify: {
      Client* c = find(e.xunmap.window);
      if (!c || e.xunmap.event != root_) break;
      if (!c->managed) {
        unmanage(c, false);
      } else if (c->ignoreUnmaps > 0) {
        c->ignoreUnmaps--;
      } else {
        unmanage(c, false);  // the client withdrew it
      }
      break;
    }
    case DestroyNotify: {
      if (Client* c = find(e.xdestroywindow.window)) unmanage(c, true);
      break;
    }
    case ConfigureRequest: {
      XConfigureRequestEvent& r = e.xconfigurerequest;
      Client* c = find(r.window);
      if (c && c->managed) {
        if (c->maximized || c->fullscreen || c->snap) {
          sendConfigureNotify(c);  // keep its place
        } else {
          int x = (r.value_mask & CWX) ? r.x : c->x, y = (r.value_mask & CWY) ? r.y : c->y;
          int w = (r.value_mask & CWWidth) ? r.width : c->w, h = (r.value_mask & CWHeight) ? r.height : c->h;
          Rect wa = workArea();
          y = std::max(y, wa.y);
          configure(c, x, y, std::min(w, sw_), std::min(h, sh_ - P(topBar_ + titleBar_)));
          sendConfigureNotify(c);
        }
        if ((r.value_mask & CWStackMode) && r.detail == Above) raise(c);
      } else {
        XWindowChanges wc{};
        wc.x = r.x;
        wc.y = r.y;
        wc.width = r.width;
        wc.height = r.height;
        wc.border_width = r.border_width;
        wc.sibling = r.above;
        wc.stack_mode = r.detail;
        XConfigureWindow(dpy_, r.window, unsigned(r.value_mask), &wc);
      }
      break;
    }
    case ConfigureNotify: {
      XConfigureEvent& ce = e.xconfigure;
      if (ce.window == root_) {
        if (ce.width != sw_ || ce.height != sh_) logf("screen resized to %dx%d (restart the WM to follow)", ce.width, ce.height);
        break;
      }
      Client* c = find(ce.window);
      if (!c || ce.event != root_) break;
      bool resized = ce.width != c->w || ce.height != c->h || ce.border_width != c->bw;
      if (!c->managed) {
        c->x = ce.x;
        c->y = ce.y;
        c->w = std::max(1, ce.width);
        c->h = std::max(1, ce.height);
        c->bw = ce.border_width;
        if (resized) startCompositing(c);
        publishPopups();
        stackDirty_ = true;
      } else if (resized) {
        c->w = std::max(1, ce.width);
        c->h = std::max(1, ce.height);
        startCompositing(c);
        updateFrame(c);
        publishWindow(c);
      }
      break;
    }
    case PropertyNotify: {
      Client* c = find(e.xproperty.window);
      if (!c || !c->managed) break;
      Atom a = e.xproperty.atom;
      if (a == XA_WM_NAME || a == a_._NET_WM_NAME) {
        readTitle(c);
        publishWindow(c);
      } else if (a == a_._NET_WM_ICON) {
        readIcon(c);
        publishWindow(c);
      } else if (a == XA_WM_NORMAL_HINTS) {
        readNormalHints(c);
        publishWindow(c);
      } else if (a == XA_WM_HINTS || a == a_.WM_PROTOCOLS) {
        readProps(c);
        publishWindow(c);
      }
      break;
    }
    case ClientMessage: {
      XClientMessageEvent& cm = e.xclient;
      Client* c = find(cm.window);
      if (!c) break;
      if (cm.message_type == a_._NET_ACTIVE_WINDOW) {
        if (c->minimized) unminimize(c);
        raise(c);
        focus(c);
      } else if (cm.message_type == a_._NET_CLOSE_WINDOW) {
        close(c);
      } else if (cm.message_type == a_.WM_CHANGE_STATE && cm.data.l[0] == IconicState) {
        minimize(c);
      } else if (cm.message_type == a_._NET_WM_STATE) {
        long action = cm.data.l[0];  // 0 remove, 1 add, 2 toggle
        for (int i = 1; i <= 2; ++i) {
          Atom p = Atom(cm.data.l[i]);
          if (p == a_._NET_WM_STATE_FULLSCREEN)
            setFullscreen(c, action == 2 ? !c->fullscreen : action == 1);
          else if (p == a_._NET_WM_STATE_MAXIMIZED_VERT || p == a_._NET_WM_STATE_MAXIMIZED_HORZ) {
            setMaximized(c, action == 2 ? !c->maximized : action == 1);
            break;  // both atoms come together
          } else if (p == a_._NET_WM_STATE_HIDDEN && action == 1) {
            minimize(c);
          }
        }
      } else if (cm.message_type == a_._NET_WM_MOVERESIZE) {
        startMoveResize(c, int(cm.data.l[2]), int(cm.data.l[0]), int(cm.data.l[1]));
      }
      break;
    }
    case ButtonPress:
    case ButtonRelease: {
      XButtonEvent& b = e.xbutton;
      if (mrWin_) {
        if (e.type == ButtonRelease) endMoveResize();
        break;
      }
      // A click on an unfocused client (our passive grab): focus, raise, replay.
      if (Client* c = find(b.window); c && c->managed && b.window != root_) {
        XAllowEvents(dpy_, ReplayPointer, b.time);
        if (e.type == ButtonPress) {
          if (c->minimized) unminimize(c);
          raise(c);
          focus(c);
        }
        break;
      }
      pointerEvent(e.type, float(b.x_root), float(b.y_root), int(b.button), b.state, b.time);
      break;
    }
    case MotionNotify: {
      // Coalesce: only the latest position matters.
      while (XCheckTypedEvent(dpy_, MotionNotify, &e)) {
      }
      if (mrWin_) {
        moveResizeMotion(e.xmotion.x_root, e.xmotion.y_root);
        break;
      }
      pointerEvent(MotionNotify, float(e.xmotion.x_root), float(e.xmotion.y_root), 0, e.xmotion.state, e.xmotion.time);
      break;
    }
    case KeyPress:
    case KeyRelease:
      keyEvent(e.xkey);
      break;
    case GenericEvent:
      handleXi(&e.xcookie);
      break;
    default:
      break;
  }
}

int WM::run() {
  if (!openDisplay()) return 1;
  if (!becomeWm()) return 1;
  if (!setupCompositing() || !setupFramebuffer()) return 1;
  setupShellWindow();
  setupInput();
  setupEwmh();
  grabKeys();
  if (!createRuntime()) return 1;
  scanExisting();
  restack();
  publishScreen();
  for (const auto& cmd : opt_.autostart) launch(cmd);

  const int xfd = ConnectionNumber(dpy_);
  double lastFrame = 0;
  const double frameMs = 1000.0 / 60.0;
  while (!g_quit) {
    while (XPending(dpy_)) {
      XEvent e;
      XNextEvent(dpy_, &e);
      handleEvent(e);
    }
    if (stackDirty_) restack();
    double now = nowMs();
    for (auto it = releaseAt_.begin(); it != releaseAt_.end();) {
      if (now >= it->second) {
        jsui_remove_texture(rt_, ("win:" + std::to_string(it->first)).c_str());
        jsui_remove_texture(rt_, ("icon:" + std::to_string(it->first)).c_str());
        it = releaseAt_.erase(it);
      } else {
        ++it;
      }
    }
    if (now - lastFrame >= frameMs - 0.5) {
      lastFrame = now;
      processDamage();
      jsui_tick(rt_, now);
      present();
      XFlush(dpy_);
    }
    // Sleep until X input, the next frame (while animating / damaged) or the next JS timer.
    double wait = 1000;
    bool damaged = false;
    for (auto& [w, c] : clients_) damaged = damaged || (c->damaged && c->viewable);
    if (jsui_wants_frame(rt_) || jsui_needs_redraw(rt_) || damaged || jsui_pending_loads(rt_) > 0)
      wait = std::max(0.0, lastFrame + frameMs - nowMs());
    else {
      double t = jsui_next_timer(rt_);
      if (std::isfinite(t)) wait = std::clamp(t - nowMs(), 0.0, 1000.0);
      // Due again after the frame interval at the earliest.
      wait = std::max(wait, lastFrame + frameMs - nowMs());
    }
    if (XPending(dpy_)) continue;
    pollfd pfd{xfd, POLLIN, 0};
    poll(&pfd, 1, int(std::ceil(std::max(0.0, wait))));
  }
  logf("quit");
  if (rt_) jsui_destroy(rt_);
  XCompositeReleaseOverlayWindow(dpy_, root_);
  XCloseDisplay(dpy_);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parseArgs(argc, argv, opt)) return 2;
  signal(SIGCHLD, SIG_IGN);  // launched apps are reaped automatically
  signal(SIGINT, [](int) { g_quit = 1; });
  signal(SIGTERM, [](int) { g_quit = 1; });
  WM wm(opt);
  return wm.run();
}
