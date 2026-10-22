#include "keymap.h"

#include <cctype>
#include <cstdio>
#include <cstring>

#include "jsui/jsui.h"

namespace viewer {

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
    case SDL_SCANCODE_NONUSBACKSLASH: return "IntlBackslash";
    case SDL_SCANCODE_SEMICOLON: return "Semicolon";
    case SDL_SCANCODE_APOSTROPHE: return "Quote";
    case SDL_SCANCODE_GRAVE: return "Backquote";
    case SDL_SCANCODE_COMMA: return "Comma";
    case SDL_SCANCODE_PERIOD: return "Period";
    case SDL_SCANCODE_SLASH: return "Slash";
    case SDL_SCANCODE_CAPSLOCK: return "CapsLock";
    case SDL_SCANCODE_PRINTSCREEN: return "PrintScreen";
    case SDL_SCANCODE_SCROLLLOCK: return "ScrollLock";
    case SDL_SCANCODE_PAUSE: return "Pause";
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
    case SDL_SCANCODE_NUMLOCKCLEAR: return "NumLock";
    case SDL_SCANCODE_KP_DIVIDE: return "NumpadDivide";
    case SDL_SCANCODE_KP_MULTIPLY: return "NumpadMultiply";
    case SDL_SCANCODE_KP_MINUS: return "NumpadSubtract";
    case SDL_SCANCODE_KP_PLUS: return "NumpadAdd";
    case SDL_SCANCODE_KP_ENTER: return "NumpadEnter";
    case SDL_SCANCODE_KP_0: return "Numpad0";
    case SDL_SCANCODE_KP_PERIOD: return "NumpadDecimal";
    case SDL_SCANCODE_KP_EQUALS: return "NumpadEqual";
    case SDL_SCANCODE_APPLICATION: return "ContextMenu";
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

std::string domKey(SDL_Keycode key, Uint16 mods) {
  bool shift = (mods & KMOD_SHIFT) != 0;
  bool caps = (mods & KMOD_CAPS) != 0;
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
  if (key >= SDLK_KP_1 && key <= SDLK_KP_9) return std::string(1, char('1' + (key - SDLK_KP_1)));
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
    case SDLK_NUMLOCKCLEAR: return "NumLock";
    case SDLK_SCROLLLOCK: return "ScrollLock";
    case SDLK_PAUSE: return "Pause";
    case SDLK_PRINTSCREEN: return "PrintScreen";
    case SDLK_APPLICATION: return "ContextMenu";
    case SDLK_KP_0: return "0";
    case SDLK_KP_PERIOD: return ".";
    case SDLK_KP_DIVIDE: return "/";
    case SDLK_KP_MULTIPLY: return "*";
    case SDLK_KP_MINUS: return "-";
    case SDLK_KP_PLUS: return "+";
    case SDLK_AC_BACK: return "BrowserBack";
    default: return "Unidentified";
  }
}

int jsuiMods(Uint16 m) {
  return ((m & KMOD_SHIFT) ? JSUI_MOD_SHIFT : 0) | ((m & KMOD_CTRL) ? JSUI_MOD_CTRL : 0) |
         ((m & KMOD_ALT) ? JSUI_MOD_ALT : 0) | ((m & KMOD_GUI) ? JSUI_MOD_META : 0);
}

int gamepadButton(SDL_GameControllerButton b) {
  switch (b) {
    case SDL_CONTROLLER_BUTTON_A: return 0;
    case SDL_CONTROLLER_BUTTON_B: return 1;
    case SDL_CONTROLLER_BUTTON_X: return 2;
    case SDL_CONTROLLER_BUTTON_Y: return 3;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return 4;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return 5;
    case SDL_CONTROLLER_BUTTON_BACK: return 8;
    case SDL_CONTROLLER_BUTTON_START: return 9;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return 10;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return 11;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return 12;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return 13;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return 14;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return 15;
    case SDL_CONTROLLER_BUTTON_GUIDE: return 16;
    default: return -1;
  }
}

bool systemCursor(const std::string& c, SDL_SystemCursor* out) {
  if (c == "none") return false;
  SDL_SystemCursor s = SDL_SYSTEM_CURSOR_ARROW;
  if (c == "pointer") s = SDL_SYSTEM_CURSOR_HAND;
  else if (c == "text" || c == "vertical-text") s = SDL_SYSTEM_CURSOR_IBEAM;
  else if (c == "crosshair") s = SDL_SYSTEM_CURSOR_CROSSHAIR;
  else if (c == "wait") s = SDL_SYSTEM_CURSOR_WAIT;
  else if (c == "progress") s = SDL_SYSTEM_CURSOR_WAITARROW;
  else if (c == "move" || c == "grab" || c == "grabbing" || c == "all-scroll") s = SDL_SYSTEM_CURSOR_SIZEALL;
  else if (c == "not-allowed" || c == "no-drop") s = SDL_SYSTEM_CURSOR_NO;
  else if (c == "ew-resize" || c == "col-resize" || c == "e-resize" || c == "w-resize") s = SDL_SYSTEM_CURSOR_SIZEWE;
  else if (c == "ns-resize" || c == "row-resize" || c == "n-resize" || c == "s-resize") s = SDL_SYSTEM_CURSOR_SIZENS;
  else if (c == "nesw-resize" || c == "ne-resize" || c == "sw-resize") s = SDL_SYSTEM_CURSOR_SIZENESW;
  else if (c == "nwse-resize" || c == "nw-resize" || c == "se-resize") s = SDL_SYSTEM_CURSOR_SIZENWSE;
  *out = s;
  return true;
}

}  // namespace viewer
