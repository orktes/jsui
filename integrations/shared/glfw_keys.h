// GLFW key codes -> the DOM-style key names of jsui_send_key, for the engine
// examples. raylib's KEY_* and sokol_app's SAPP_KEYCODE_* use the same values
// as GLFW, so both share this table.
#pragma once

#include <string>

#include "jsui/jsui.h"

namespace jsui_example {

// DOM KeyboardEvent.code of a GLFW key ("KeyA", "ArrowLeft", ...), or nullptr.
inline const char* glfwDomCode(int k) {
  static const char* kDigits[] = {"Digit0", "Digit1", "Digit2", "Digit3", "Digit4",
                                  "Digit5", "Digit6", "Digit7", "Digit8", "Digit9"};
  static const char* kLetters[] = {"KeyA", "KeyB", "KeyC", "KeyD", "KeyE", "KeyF", "KeyG", "KeyH", "KeyI",
                                   "KeyJ", "KeyK", "KeyL", "KeyM", "KeyN", "KeyO", "KeyP", "KeyQ", "KeyR",
                                   "KeyS", "KeyT", "KeyU", "KeyV", "KeyW", "KeyX", "KeyY", "KeyZ"};
  static const char* kF[] = {"F1",  "F2",  "F3",  "F4",  "F5",  "F6",  "F7",  "F8",  "F9",  "F10", "F11", "F12", "F13",
                             "F14", "F15", "F16", "F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24", "F25"};
  static const char* kNumpad[] = {"Numpad0", "Numpad1", "Numpad2", "Numpad3", "Numpad4",
                                  "Numpad5", "Numpad6", "Numpad7", "Numpad8", "Numpad9"};
  if (k >= 48 && k <= 57) return kDigits[k - 48];
  if (k >= 65 && k <= 90) return kLetters[k - 65];
  if (k >= 290 && k <= 314) return kF[k - 290];
  if (k >= 320 && k <= 329) return kNumpad[k - 320];
  switch (k) {
    case 32: return "Space";
    case 39: return "Quote";
    case 44: return "Comma";
    case 45: return "Minus";
    case 46: return "Period";
    case 47: return "Slash";
    case 59: return "Semicolon";
    case 61: return "Equal";
    case 91: return "BracketLeft";
    case 92: return "Backslash";
    case 93: return "BracketRight";
    case 96: return "Backquote";
    case 256: return "Escape";
    case 257: return "Enter";
    case 258: return "Tab";
    case 259: return "Backspace";
    case 260: return "Insert";
    case 261: return "Delete";
    case 262: return "ArrowRight";
    case 263: return "ArrowLeft";
    case 264: return "ArrowDown";
    case 265: return "ArrowUp";
    case 266: return "PageUp";
    case 267: return "PageDown";
    case 268: return "Home";
    case 269: return "End";
    case 280: return "CapsLock";
    case 281: return "ScrollLock";
    case 282: return "NumLock";
    case 283: return "PrintScreen";
    case 284: return "Pause";
    case 330: return "NumpadDecimal";
    case 331: return "NumpadDivide";
    case 332: return "NumpadMultiply";
    case 333: return "NumpadSubtract";
    case 334: return "NumpadAdd";
    case 335: return "NumpadEnter";
    case 336: return "NumpadEqual";
    case 340: return "ShiftLeft";
    case 341: return "ControlLeft";
    case 342: return "AltLeft";
    case 343: return "MetaLeft";
    case 344: return "ShiftRight";
    case 345: return "ControlRight";
    case 346: return "AltRight";
    case 347: return "MetaRight";
    case 348: return "ContextMenu";
    default: return nullptr;
  }
}

// DOM KeyboardEvent.key of a GLFW key press ("a", "A", "Enter", "Shift", ...).
// Printable keys follow the US layout for shifted symbols; the text itself is
// sent separately (jsui_send_text), as browsers do.
inline std::string glfwDomKey(int k, bool shift) {
  if (k >= 65 && k <= 90) return std::string(1, char(shift ? k : k + 32));
  if (k >= 32 && k <= 96) {
    static const char kPlain[] = " '"
                                 ",-./0123456789;=[\\]`";
    static const char kShift[] = " \""
                                 "<_>?)!@#$%^&*(:+{|}~";
    const char* chars = shift ? kShift : kPlain;
    static const int kCodes[] = {32, 39, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 59, 61, 91, 92, 93, 96};
    for (int i = 0; i < int(sizeof(kCodes) / sizeof(kCodes[0])); i++)
      if (kCodes[i] == k) return std::string(1, chars[i]);
  }
  if (k >= 320 && k <= 329) return std::string(1, char('0' + k - 320));
  switch (k) {
    case 256: return "Escape";
    case 257:
    case 335: return "Enter";
    case 258: return "Tab";
    case 259: return "Backspace";
    case 260: return "Insert";
    case 261: return "Delete";
    case 262: return "ArrowRight";
    case 263: return "ArrowLeft";
    case 264: return "ArrowDown";
    case 265: return "ArrowUp";
    case 266: return "PageUp";
    case 267: return "PageDown";
    case 268: return "Home";
    case 269: return "End";
    case 280: return "CapsLock";
    case 281: return "ScrollLock";
    case 282: return "NumLock";
    case 283: return "PrintScreen";
    case 284: return "Pause";
    case 330: return ".";
    case 331: return "/";
    case 332: return "*";
    case 333: return "-";
    case 334: return "+";
    case 336: return "=";
    case 340:
    case 344: return "Shift";
    case 341:
    case 345: return "Control";
    case 342:
    case 346: return "Alt";
    case 343:
    case 347: return "Meta";
    case 348: return "ContextMenu";
    default: break;
  }
  if (k >= 290 && k <= 314) return "F" + std::to_string(k - 289);
  return "Unidentified";
}

// UTF-8 of a Unicode code point (text input).
inline std::string utf8(unsigned cp) {
  std::string s;
  if (cp < 0x80) {
    s += char(cp);
  } else if (cp < 0x800) {
    s += char(0xC0 | (cp >> 6));
    s += char(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    s += char(0xE0 | (cp >> 12));
    s += char(0x80 | ((cp >> 6) & 0x3F));
    s += char(0x80 | (cp & 0x3F));
  } else {
    s += char(0xF0 | (cp >> 18));
    s += char(0x80 | ((cp >> 12) & 0x3F));
    s += char(0x80 | ((cp >> 6) & 0x3F));
    s += char(0x80 | (cp & 0x3F));
  }
  return s;
}

}  // namespace jsui_example
