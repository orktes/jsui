#include "core/json.h"

#include <algorithm>
#include <unordered_map>

namespace jsui {

struct JsonReader {
  std::string_view s;
  size_t i = 0;
  std::string err;

  bool fail(const char* m) {
    if (err.empty()) err = std::string(m) + " at offset " + std::to_string(i);
    return false;
  }
  void ws() {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) i++;
  }
  bool lit(std::string_view word) {
    if (s.substr(i, word.size()) != word) return fail("invalid literal");
    i += word.size();
    return true;
  }
  static void utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
      out += char(cp);
    } else if (cp < 0x800) {
      out += char(0xC0 | (cp >> 6));
      out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += char(0xE0 | (cp >> 12));
      out += char(0x80 | ((cp >> 6) & 0x3F));
      out += char(0x80 | (cp & 0x3F));
    } else {
      out += char(0xF0 | (cp >> 18));
      out += char(0x80 | ((cp >> 12) & 0x3F));
      out += char(0x80 | ((cp >> 6) & 0x3F));
      out += char(0x80 | (cp & 0x3F));
    }
  }
  bool hex4(uint32_t& cp) {
    if (i + 4 > s.size()) return fail("truncated \\u escape");
    cp = 0;
    for (int k = 0; k < 4; k++) {
      char c = s[i++];
      cp <<= 4;
      if (c >= '0' && c <= '9') cp |= uint32_t(c - '0');
      else if (c >= 'a' && c <= 'f') cp |= uint32_t(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') cp |= uint32_t(c - 'A' + 10);
      else return fail("bad \\u escape");
    }
    return true;
  }
  bool string(std::string& out) {
    i++;  // opening quote
    for (;;) {
      size_t start = i;
      while (i < s.size() && s[i] != '"' && s[i] != '\\' && (unsigned char)s[i] >= 0x20) i++;
      out.append(s.data() + start, i - start);
      if (i >= s.size()) return fail("unterminated string");
      char c = s[i];
      if (c == '"') {
        i++;
        return true;
      }
      if ((unsigned char)c < 0x20) return fail("control character in string");
      i++;  // backslash
      if (i >= s.size()) return fail("unterminated string");
      char e = s[i++];
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          uint32_t cp;
          if (!hex4(cp)) return false;
          if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
            size_t save = i;
            i += 2;
            uint32_t lo;
            if (!hex4(lo)) return false;
            if (lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            else i = save;  // lone high surrogate: keep both escapes separately
          }
          // Lone surrogates become U+FFFD (they are not representable in UTF-8).
          if (cp >= 0xD800 && cp < 0xE000) cp = 0xFFFD;
          utf8(out, cp);
          break;
        }
        default: return fail("bad escape");
      }
    }
  }
  bool number(std::string& out) {
    size_t start = i;
    if (i < s.size() && s[i] == '-') i++;
    if (i >= s.size()) return fail("bad number");
    if (s[i] == '0') {
      i++;
    } else if (s[i] >= '1' && s[i] <= '9') {
      while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
    } else {
      return fail("bad number");
    }
    if (i < s.size() && s[i] == '.') {
      i++;
      size_t d = i;
      while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
      if (i == d) return fail("bad number");
    }
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
      i++;
      if (i < s.size() && (s[i] == '+' || s[i] == '-')) i++;
      size_t d = i;
      while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
      if (i == d) return fail("bad number");
    }
    out.assign(s.data() + start, i - start);
    return true;
  }
  bool value(Json& v, int depth) {
    if (depth > Json::kMaxDepth) return fail("nesting too deep");
    ws();
    if (i >= s.size()) return fail("unexpected end");
    char c = s[i];
    switch (c) {
      case 'n': v.kind_ = Json::Kind::Null; return lit("null");
      case 't': v.kind_ = Json::Kind::Bool; v.bool_ = true; return lit("true");
      case 'f': v.kind_ = Json::Kind::Bool; v.bool_ = false; return lit("false");
      case '"': v.kind_ = Json::Kind::String; return string(v.text_);
      case '[': {
        v.kind_ = Json::Kind::Array;
        i++;
        ws();
        if (i < s.size() && s[i] == ']') {
          i++;
          return true;
        }
        for (;;) {
          v.items_.emplace_back();
          if (!value(v.items_.back(), depth + 1)) return false;
          ws();
          if (i < s.size() && s[i] == ',') {
            i++;
            continue;
          }
          if (i < s.size() && s[i] == ']') {
            i++;
            return true;
          }
          return fail("expected ',' or ']'");
        }
      }
      case '{': {
        v.kind_ = Json::Kind::Object;
        i++;
        ws();
        if (i < s.size() && s[i] == '}') {
          i++;
          return true;
        }
        for (;;) {
          ws();
          if (i >= s.size() || s[i] != '"') return fail("expected member name");
          std::string key;
          if (!string(key)) return false;
          ws();
          if (i >= s.size() || s[i] != ':') return fail("expected ':'");
          i++;
          Json member;
          if (!value(member, depth + 1)) return false;
          // Duplicate names: the last one wins, like JSON.parse.
          bool dup = false;
          for (auto& m : v.members_)
            if (m.first == key) {
              m.second = std::move(member);
              dup = true;
              break;
            }
          if (!dup) v.members_.emplace_back(std::move(key), std::move(member));
          ws();
          if (i < s.size() && s[i] == ',') {
            i++;
            continue;
          }
          if (i < s.size() && s[i] == '}') {
            i++;
            return true;
          }
          return fail("expected ',' or '}'");
        }
      }
      default:
        if (c == '-' || (c >= '0' && c <= '9')) {
          v.kind_ = Json::Kind::Number;
          return number(v.text_);
        }
        return fail("unexpected character");
    }
  }
};

bool Json::parse(std::string_view text, Json* out, std::string* error) {
  JsonReader r{text};
  Json v;
  bool ok = r.value(v, 0);
  if (ok) {
    r.ws();
    if (r.i != text.size()) ok = r.fail("trailing characters");
  }
  if (!ok) {
    if (error) *error = r.err;
    return false;
  }
  if (out) *out = std::move(v);
  return true;
}

namespace {

void dumpString(std::string& out, const std::string& s) {
  static const char* hex = "0123456789abcdef";
  out += '"';
  size_t start = 0;
  for (size_t i = 0; i < s.size(); i++) {
    unsigned char c = (unsigned char)s[i];
    if (c >= 0x20 && c != '"' && c != '\\') continue;
    out.append(s, start, i - start);
    start = i + 1;
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        out += "\\u00";
        out += hex[c >> 4];
        out += hex[c & 15];
    }
  }
  out.append(s, start, std::string::npos);
  out += '"';
}

}  // namespace

void Json::dump(std::string& out) const {
  switch (kind_) {
    case Kind::Null: out += "null"; break;
    case Kind::Bool: out += bool_ ? "true" : "false"; break;
    case Kind::Number: out += text_; break;
    case Kind::String: dumpString(out, text_); break;
    case Kind::Array:
      out += '[';
      for (size_t k = 0; k < items_.size(); k++) {
        if (k) out += ',';
        items_[k].dump(out);
      }
      out += ']';
      break;
    case Kind::Object:
      out += '{';
      for (size_t k = 0; k < members_.size(); k++) {
        if (k) out += ',';
        dumpString(out, members_[k].first);
        out += ':';
        members_[k].second.dump(out);
      }
      out += '}';
      break;
  }
}

std::string Json::dump() const {
  std::string out;
  dump(out);
  return out;
}

const Json* Json::find(std::string_view key) const {
  if (kind_ != Kind::Object) return nullptr;
  for (auto& m : members_)
    if (m.first == key) return &m.second;
  return nullptr;
}

bool Json::operator==(const Json& o) const {
  if (kind_ != o.kind_) return false;
  switch (kind_) {
    case Kind::Null: return true;
    case Kind::Bool: return bool_ == o.bool_;
    case Kind::Number:
    case Kind::String: return text_ == o.text_;
    case Kind::Array: return items_ == o.items_;
    case Kind::Object: return members_ == o.members_;
  }
  return false;
}

bool Json::mergePatch(const Json& patch) {
  if (patch.kind_ != Kind::Object) {
    if (*this == patch) return false;
    *this = patch;
    return true;
  }
  bool changed = false;
  if (kind_ != Kind::Object) {
    *this = Json();
    kind_ = Kind::Object;
    changed = true;
  }
  // Big objects (inventories keyed by item id) get an index so a patch that
  // names k members costs O(n + k), not O(n * k).
  std::unordered_map<std::string_view, size_t> index;
  bool indexed = members_.size() > 16 && patch.members_.size() > 2;
  auto rebuild = [&] {
    index.clear();
    for (size_t k = 0; k < members_.size(); k++) index.emplace(members_[k].first, k);
  };
  if (indexed) rebuild();
  auto lookup = [&](const std::string& key) -> long {
    if (indexed) {
      auto it = index.find(key);
      return it == index.end() ? -1 : long(it->second);
    }
    for (size_t k = 0; k < members_.size(); k++)
      if (members_[k].first == key) return long(k);
    return -1;
  };
  bool removed = false;
  for (auto& [key, value] : patch.members_) {
    long at = lookup(key);
    if (value.kind_ == Kind::Null) {
      if (at < 0) continue;
      // Tombstone now, compact once at the end (keeps the index valid).
      members_[size_t(at)].second.kind_ = Kind::Null;
      members_[size_t(at)].second.text_ = "\x01removed";
      if (indexed) index.erase(members_[size_t(at)].first);
      removed = true;
      changed = true;
      continue;
    }
    if (at >= 0) {
      if (members_[size_t(at)].second.mergePatch(value)) changed = true;
      continue;
    }
    Json fresh;
    fresh.mergePatch(value);  // drops nulls nested in the added value
    size_t before = members_.capacity();
    members_.emplace_back(key, std::move(fresh));
    // Reallocation moved the keys the index views point at.
    if (indexed) {
      if (members_.capacity() != before) rebuild();
      else index.emplace(members_.back().first, members_.size() - 1);
    }
    changed = true;
  }
  if (removed) {
    members_.erase(std::remove_if(members_.begin(), members_.end(),
                                  [](const Member& m) {
                                    return m.second.kind_ == Kind::Null && m.second.text_ == "\x01removed";
                                  }),
                   members_.end());
  }
  return changed;
}

}  // namespace jsui
