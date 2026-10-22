// SDL keyboard / controller / cursor mapping to the JSUI (DOM-style) API.
#pragma once

#include <SDL.h>

#include <string>

namespace viewer {

// DOM KeyboardEvent.code for a physical key ("KeyA", "ArrowLeft", ...), or nullptr.
const char* domCode(SDL_Scancode sc);
// DOM KeyboardEvent.key for a key press ("a", "A", "Enter", "Shift", ...).
// Printable keys follow the US layout for shifted symbols.
std::string domKey(SDL_Keycode key, Uint16 sdlMods);
// JSUI_MOD_* bits.
int jsuiMods(Uint16 sdlMods);
// W3C standard gamepad button index, or -1.
int gamepadButton(SDL_GameControllerButton b);
// CSS cursor name -> SDL system cursor; returns false for "none".
bool systemCursor(const std::string& css, SDL_SystemCursor* out);

}  // namespace viewer
