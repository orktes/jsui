// Turns embedder input into DOM-like events: hit testing, pointer capture,
// hover (enter/leave), focus, keyboard routing and gamepad focus navigation.
#pragma once

#include <string>

#include "core/node.h"

namespace jsui {

class Runtime;

class InputManager {
 public:
  explicit InputManager(Runtime& rt);
  ~InputManager();

  bool mouseMove(float x, float y);
  bool mouseButton(float x, float y, int button, bool down, int mods);
  bool mouseWheel(float x, float y, float dx, float dy);
  bool mouseLeave();
  bool touch(int id, int phase, float x, float y);
  bool key(const char* key, const char* code, bool down, bool repeat, int mods);
  bool text(const char* utf8);
  bool gamepadButton(int pad, int button, bool down, float value);
  bool gamepadAxis(int pad, int axis, float value);
  void gamepadConnected(int pad, bool connected, const char* id);

  // Topmost node accepting pointer events at (x, y), honoring transforms,
  // overflow clipping, zIndex and pointerEvents.
  Node* hitTest(float x, float y);

  Node* focused() const { return focused_; }
  void focus(Node* n);
  // Called before a node is destroyed so dangling pointers are dropped.
  void nodeRemoved(Node* n);
  // Per-tick work (stick repeat, hover refresh after layout changes).
  void tick(double nowMs);

 private:
  Runtime& rt_;
  Node* focused_ = nullptr;
  Node* hovered_ = nullptr;
};

}  // namespace jsui
