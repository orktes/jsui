// Godot input -> DOM KeyboardEvent / W3C Gamepad "standard" mapping.
#pragma once

#include <godot_cpp/classes/global_constants.hpp>

#include <string>

namespace jsui_godot {

// DOM `code` (physical key, US layout) for a Godot physical keycode, or
// nullptr when there is no equivalent. `location` disambiguates left/right
// modifiers (KeyLocation, Godot 4.3+).
const char* domCode(godot::Key physical, godot::KeyLocation location);

// DOM `key` (logical key). `unicode` is the character Godot produced for the
// press (0 for releases and non-printing keys); `keycode` is the layout-
// mapped, unshifted logical key.
std::string domKey(godot::Key keycode, char32_t unicode, bool shift);

// Godot JoyButton (SDL layout) -> standard-mapping button index, or -1.
int gamepadButton(godot::JoyButton button);

// UTF-8 encoding of one code point.
std::string utf8(char32_t cp);

}  // namespace jsui_godot
