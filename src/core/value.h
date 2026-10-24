// A small dynamically typed value used to move props/styles from the JS
// bridge into the core. Keeps the core independent of QuickJS so it can be
// unit-tested (and fed from JSON) directly.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace jsui {

class Value {
 public:
  using Array = std::vector<Value>;
  using Object = std::map<std::string, Value>;

  Value() = default;
  Value(std::nullptr_t) {}
  Value(bool b) : v_(b) {}
  Value(double d) : v_(d) {}
  Value(int i) : v_(static_cast<double>(i)) {}
  Value(const char* s) : v_(std::string(s)) {}
  Value(std::string s) : v_(std::move(s)) {}
  Value(Array a) : v_(std::make_shared<Array>(std::move(a))) {}
  Value(Object o) : v_(std::make_shared<Object>(std::move(o))) {}

  bool isNull() const { return v_.index() == 0; }
  bool isBool() const { return v_.index() == 1; }
  bool isNumber() const { return v_.index() == 2; }
  bool isString() const { return v_.index() == 3; }
  bool isArray() const { return v_.index() == 4; }
  bool isObject() const { return v_.index() == 5; }

  bool asBool(bool def = false) const { return isBool() ? std::get<1>(v_) : def; }
  double asNumber(double def = 0) const { return isNumber() ? std::get<2>(v_) : def; }
  const std::string& asString() const {
    static const std::string empty;
    return isString() ? std::get<3>(v_) : empty;
  }
  const Array& asArray() const {
    static const Array empty;
    return isArray() ? *std::get<4>(v_) : empty;
  }
  const Object& asObject() const {
    static const Object empty;
    return isObject() ? *std::get<5>(v_) : empty;
  }
  // Copy-on-write access to the object (a non-object becomes an empty
  // object). Other Values sharing the container keep the old contents.
  Object& mutableObject() {
    if (!isObject()) v_ = std::make_shared<Object>();
    else if (std::get<5>(v_).use_count() > 1) v_ = std::make_shared<Object>(*std::get<5>(v_));
    return *std::get<5>(v_);
  }
  // Copy-on-write access to the array (a non-array becomes an empty array).
  Array& mutableArray() {
    if (!isArray()) v_ = std::make_shared<Array>();
    else if (std::get<4>(v_).use_count() > 1) v_ = std::make_shared<Array>(*std::get<4>(v_));
    return *std::get<4>(v_);
  }
  // Sets a string value, reusing this value's string buffer when it holds one.
  void setString(const std::string& s) {
    if (isString()) std::get<3>(v_) = s;
    else v_ = s;
  }
  // Makes this value equal to `src` and returns whether it changed, in one
  // walk: its own containers and string buffers are reused where they are
  // not shared and have src's shape (same array length, same object keys);
  // a shared container is only copied when something in it changes, and a
  // shape mismatch shares src's container instead. A per-frame write of a
  // same-shaped value (an Animated transform into a view's style) then
  // allocates nothing.
  bool assignReusing(const Value& src);
  // Deep equality (shared containers compare by identity first).
  bool equals(const Value& o) const;
  // Object member lookup; returns a null Value when missing.
  const Value& operator[](const std::string& key) const {
    static const Value null;
    if (!isObject()) return null;
    auto& o = *std::get<5>(v_);
    auto it = o.find(key);
    return it == o.end() ? null : it->second;
  }
  // The shared container of an array / object (nullptr otherwise): two
  // Values with the same identity are equal without a deep comparison.
  const void* identity() const {
    if (isArray()) return std::get<4>(v_).get();
    if (isObject()) return std::get<5>(v_).get();
    return nullptr;
  }
  bool has(const std::string& key) const { return isObject() && std::get<5>(v_)->count(key) > 0; }

  // Minimal JSON support (tests, host messages).
  static Value parseJson(const std::string& text, std::string* error = nullptr);
  std::string toJson() const;

 private:
  std::variant<std::monostate, bool, double, std::string, std::shared_ptr<Array>, std::shared_ptr<Object>> v_;
};

}  // namespace jsui
