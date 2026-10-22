// SKELETON: hit testing and event routing are implemented on top of this.
#include "input/input.h"

#include "runtime.h"

namespace jsui {

InputManager::InputManager(Runtime& rt) : rt_(rt) {}
InputManager::~InputManager() = default;

bool InputManager::mouseMove(float, float) { return false; }
bool InputManager::mouseButton(float, float, int, bool, int) { return false; }
bool InputManager::mouseWheel(float, float, float, float) { return false; }
bool InputManager::mouseLeave() { return false; }
bool InputManager::touch(int, int, float, float) { return false; }
bool InputManager::key(const char*, const char*, bool, bool, int) { return false; }
bool InputManager::text(const char*) { return false; }
bool InputManager::gamepadButton(int, int, bool, float) { return false; }
bool InputManager::gamepadAxis(int, int, float) { return false; }
void InputManager::gamepadConnected(int, bool, const char*) {}
Node* InputManager::hitTest(float, float) { return nullptr; }
void InputManager::focus(Node* n) { focused_ = n; }
void InputManager::nodeRemoved(Node* n) {
  if (focused_ == n) focused_ = nullptr;
  if (hovered_ == n) hovered_ = nullptr;
}
void InputManager::tick(double) {}

}  // namespace jsui
