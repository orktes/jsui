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
  // Object member lookup; returns a null Value when missing.
  const Value& operator[](const std::string& key) const {
    static const Value null;
    if (!isObject()) return null;
    auto& o = *std::get<5>(v_);
    auto it = o.find(key);
    return it == o.end() ? null : it->second;
  }
  bool has(const std::string& key) const { return isObject() && std::get<5>(v_)->count(key) > 0; }

  // Minimal JSON support (tests, host messages).
  static Value parseJson(const std::string& text, std::string* error = nullptr);
  std::string toJson() const;

 private:
  std::variant<std::monostate, bool, double, std::string, std::shared_ptr<Array>, std::shared_ptr<Object>> v_;
};

}  // namespace jsui
