#include "keymap.h"

#include <cstdio>

using namespace godot;

namespace jsui_godot {

std::string utf8(char32_t cp) {
  std::string out;
  if (cp < 0x80) {
    out += char(cp);
  } else if (cp < 0x800) {
    out += char(0xC0 | (cp >> 6));
    out += char(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    if (cp >= 0xD800 && cp <= 0xDFFF) return {};
    out += char(0xE0 | (cp >> 12));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  } else if (cp <= 0x10FFFF) {
    out += char(0xF0 | (cp >> 18));
    out += char(0x80 | ((cp >> 12) & 0x3F));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  }
  return out;
}

const char* domCode(Key k, KeyLocation loc) {
  static char buf[16];
  const bool right = loc == KEY_LOCATION_RIGHT;
  if (k >= KEY_A && k <= KEY_Z) {
    std::snprintf(buf, sizeof buf, "Key%c", char('A' + (k - KEY_A)));
    return buf;
  }
  if (k >= KEY_0 && k <= KEY_9) {
    std::snprintf(buf, sizeof buf, "Digit%c", char('0' + (k - KEY_0)));
    return buf;
  }
  if (k >= KEY_F1 && k <= KEY_F24) {
    std::snprintf(buf, sizeof buf, "F%d", int(1 + (k - KEY_F1)));
    return buf;
  }
  if (k >= KEY_KP_0 && k <= KEY_KP_9) {
    std::snprintf(buf, sizeof buf, "Numpad%c", char('0' + (k - KEY_KP_0)));
    return buf;
  }
  switch (k) {
    case KEY_ENTER: return "Enter";
    case KEY_KP_ENTER: return "NumpadEnter";
    case KEY_ESCAPE: return "Escape";
    case KEY_BACKSPACE: return "Backspace";
    case KEY_TAB:
    case KEY_BACKTAB: return "Tab";
    case KEY_SPACE: return "Space";
    case KEY_MINUS: return "Minus";
    case KEY_EQUAL: return "Equal";
    case KEY_BRACKETLEFT: return "BracketLeft";
    case KEY_BRACKETRIGHT: return "BracketRight";
    case KEY_BACKSLASH: return "Backslash";
    case KEY_SECTION: return "IntlBackslash";
    case KEY_SEMICOLON: return "Semicolon";
    case KEY_APOSTROPHE: return "Quote";
    case KEY_QUOTELEFT: return "Backquote";
    case KEY_COMMA: return "Comma";
    case KEY_PERIOD: return "Period";
    case KEY_SLASH: return "Slash";
    case KEY_CAPSLOCK: return "CapsLock";
    case KEY_PRINT: return "PrintScreen";
    case KEY_SCROLLLOCK: return "ScrollLock";
    case KEY_PAUSE: return "Pause";
    case KEY_INSERT: return "Insert";
    case KEY_HOME: return "Home";
    case KEY_PAGEUP: return "PageUp";
    case KEY_DELETE: return "Delete";
    case KEY_END: return "End";
    case KEY_PAGEDOWN: return "PageDown";
    case KEY_RIGHT: return "ArrowRight";
    case KEY_LEFT: return "ArrowLeft";
    case KEY_DOWN: return "ArrowDown";
    case KEY_UP: return "ArrowUp";
    case KEY_NUMLOCK: return "NumLock";
    case KEY_KP_DIVIDE: return "NumpadDivide";
    case KEY_KP_MULTIPLY: return "NumpadMultiply";
    case KEY_KP_SUBTRACT: return "NumpadSubtract";
    case KEY_KP_ADD: return "NumpadAdd";
    case KEY_KP_PERIOD: return "NumpadDecimal";
    case KEY_MENU: return "ContextMenu";
    case KEY_SHIFT: return right ? "ShiftRight" : "ShiftLeft";
    case KEY_CTRL: return right ? "ControlRight" : "ControlLeft";
    case KEY_ALT: return right ? "AltRight" : "AltLeft";
    case KEY_META: return right ? "MetaRight" : "MetaLeft";
    case KEY_BACK: return "BrowserBack";
    case KEY_FORWARD: return "BrowserForward";
    default: return nullptr;
  }
}

std::string domKey(Key k, char32_t unicode, bool shift) {
  // Printable keys: the produced character (already shifted / layout mapped).
  if (unicode >= 0x20 && unicode != 0x7F) return utf8(unicode);
  if (k >= KEY_F1 && k <= KEY_F35) return "F" + std::to_string(int(1 + (k - KEY_F1)));
  switch (k) {
    case KEY_ENTER:
    case KEY_KP_ENTER: return "Enter";
    case KEY_ESCAPE: return "Escape";
    case KEY_BACKSPACE: return "Backspace";
    case KEY_TAB:
    case KEY_BACKTAB: return "Tab";
    case KEY_SPACE: return " ";
    case KEY_INSERT: return "Insert";
    case KEY_DELETE: return "Delete";
    case KEY_PAUSE: return "Pause";
    case KEY_PRINT: return "PrintScreen";
    case KEY_CLEAR: return "Clear";
    case KEY_HOME: return "Home";
    case KEY_END: return "End";
    case KEY_LEFT: return "ArrowLeft";
    case KEY_UP: return "ArrowUp";
    case KEY_RIGHT: return "ArrowRight";
    case KEY_DOWN: return "ArrowDown";
    case KEY_PAGEUP: return "PageUp";
    case KEY_PAGEDOWN: return "PageDown";
    case KEY_SHIFT: return "Shift";
    case KEY_CTRL: return "Control";
    case KEY_META: return "Meta";
    case KEY_ALT: return "Alt";
    case KEY_CAPSLOCK: return "CapsLock";
    case KEY_NUMLOCK: return "NumLock";
    case KEY_SCROLLLOCK: return "ScrollLock";
    case KEY_MENU: return "ContextMenu";
    case KEY_HELP: return "Help";
    case KEY_BACK: return "BrowserBack";
    case KEY_FORWARD: return "BrowserForward";
    case KEY_REFRESH: return "BrowserRefresh";
    case KEY_VOLUMEDOWN: return "AudioVolumeDown";
    case KEY_VOLUMEUP: return "AudioVolumeUp";
    case KEY_VOLUMEMUTE: return "AudioVolumeMute";
    case KEY_MEDIAPLAY: return "MediaPlayPause";
    case KEY_MEDIASTOP: return "MediaStop";
    case KEY_MEDIANEXT: return "MediaTrackNext";
    case KEY_MEDIAPREVIOUS: return "MediaTrackPrevious";
    default: break;
  }
  // Key releases and Ctrl/Cmd chords carry no unicode: fall back to the
  // logical key, whose Latin-1 codes equal the (upper-case) character.
  if (k >= KEY_KP_0 && k <= KEY_KP_9) return std::string(1, char('0' + (k - KEY_KP_0)));
  switch (k) {
    case KEY_KP_MULTIPLY: return "*";
    case KEY_KP_DIVIDE: return "/";
    case KEY_KP_SUBTRACT: return "-";
    case KEY_KP_ADD: return "+";
    case KEY_KP_PERIOD: return ".";
    default: break;
  }
  if (k > KEY_SPACE && k <= 0xFF) {
    char32_t c = char32_t(k);
    if (c >= 'A' && c <= 'Z' && !shift) c = c - 'A' + 'a';
    return utf8(c);
  }
  return "Unidentified";
}

int gamepadButton(JoyButton b) {
  switch (b) {
    case JOY_BUTTON_A: return 0;
    case JOY_BUTTON_B: return 1;
    case JOY_BUTTON_X: return 2;
    case JOY_BUTTON_Y: return 3;
    case JOY_BUTTON_LEFT_SHOULDER: return 4;
    case JOY_BUTTON_RIGHT_SHOULDER: return 5;
    case JOY_BUTTON_BACK: return 8;
    case JOY_BUTTON_START: return 9;
    case JOY_BUTTON_LEFT_STICK: return 10;
    case JOY_BUTTON_RIGHT_STICK: return 11;
    case JOY_BUTTON_DPAD_UP: return 12;
    case JOY_BUTTON_DPAD_DOWN: return 13;
    case JOY_BUTTON_DPAD_LEFT: return 14;
    case JOY_BUTTON_DPAD_RIGHT: return 15;
    case JOY_BUTTON_GUIDE: return 16;
    default: return -1;  // triggers arrive as axes (see JsuiView)
  }
}

}  // namespace jsui_godot
