// Order-preserving JSON document for the shared host state
// (jsui_set_state / jsui_patch_state, see docs/ARCHITECTURE.md "Game data").
//
// Unlike Value (whose objects are std::map), objects keep their member order
// and numbers keep their source text, so a value the embedder sets is handed
// to JS exactly as written, and an RFC 7386 merge patch changes only what it
// names: new members are appended, existing ones stay where they are.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jsui {

class Json {
 public:
  enum class Kind : uint8_t { Null, Bool, Number, String, Array, Object };
  using Member = std::pair<std::string, Json>;

  Json() = default;

  // Strict RFC 8259 parse of a whole document (surrounding whitespace allowed).
  // Nesting deeper than kMaxDepth is rejected so hostile input cannot
  // overflow the stack. On failure `out` is unchanged and `error` (optional)
  // says what and where.
  static constexpr int kMaxDepth = 512;
  static bool parse(std::string_view text, Json* out, std::string* error = nullptr);

  // Compact serialization (no whitespace). Numbers are written as parsed.
  std::string dump() const;
  void dump(std::string& out) const;

  // RFC 7386 JSON Merge Patch: applies `patch` to this value in place.
  // Returns true when the value changed.
  bool mergePatch(const Json& patch);

  bool operator==(const Json& o) const;
  bool operator!=(const Json& o) const { return !(*this == o); }

  Kind kind() const { return kind_; }
  bool isNull() const { return kind_ == Kind::Null; }
  bool isObject() const { return kind_ == Kind::Object; }
  // Bool value; string contents; number source text.
  bool boolean() const { return bool_; }
  const std::string& text() const { return text_; }
  const std::vector<Json>& items() const { return items_; }
  const std::vector<Member>& members() const { return members_; }
  // Object member, or nullptr.
  const Json* find(std::string_view key) const;

 private:
  friend struct JsonReader;
  Kind kind_ = Kind::Null;
  bool bool_ = false;
  std::string text_;
  std::vector<Json> items_;
  std::vector<Member> members_;
};

}  // namespace jsui
