// A small JSON reader and writer for the window manager's host messages.
// Messages from the shell are tiny ({"id": 4194311, "x": 120, "y": 80},
// {"rects": [[0, 0, 1280, 40]]}), so a plain recursive-descent parser over
// std::string / std::vector is plenty.
#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace wmjson {

struct Value {
  enum Type { Null, Bool, Number, String, Array, Object } type = Null;
  bool b = false;
  double n = 0;
  std::string s;
  std::vector<Value> a;
  std::map<std::string, Value> o;

  const Value& operator[](const char* key) const {
    static const Value none;
    if (type != Object) return none;
    auto it = o.find(key);
    return it == o.end() ? none : it->second;
  }
  const Value& operator[](size_t i) const {
    static const Value none;
    return type == Array && i < a.size() ? a[i] : none;
  }
  double num(double fallback = 0) const { return type == Number ? n : type == Bool ? (b ? 1 : 0) : fallback; }
  bool truthy() const { return type == Bool ? b : type == Number ? n != 0 : type == String ? !s.empty() : type != Null; }
  const std::string& str() const {
    static const std::string empty;
    return type == String ? s : empty;
  }
  size_t size() const { return type == Array ? a.size() : type == Object ? o.size() : 0; }
};

class Parser {
 public:
  explicit Parser(const char* p) : p_(p ? p : "") {}
  Value parse() {
    Value v;
    ws();
    value(v);
    return v;
  }

 private:
  const char* p_;
  void ws() {
    while (*p_ == ' ' || *p_ == '\n' || *p_ == '\r' || *p_ == '\t') ++p_;
  }
  void value(Value& v) {
    ws();
    switch (*p_) {
      case '{': object(v); break;
      case '[': array(v); break;
      case '"': v.type = Value::String; string(v.s); break;
      case 't': v.type = Value::Bool; v.b = true; p_ += std::strncmp(p_, "true", 4) == 0 ? 4 : 1; break;
      case 'f': v.type = Value::Bool; v.b = false; p_ += std::strncmp(p_, "false", 5) == 0 ? 5 : 1; break;
      case 'n': v.type = Value::Null; p_ += std::strncmp(p_, "null", 4) == 0 ? 4 : 1; break;
      case '\0': break;
      default: {
        char* end = nullptr;
        v.n = std::strtod(p_, &end);
        if (end == p_) {
          ++p_;
          return;
        }
        v.type = Value::Number;
        p_ = end;
      }
    }
  }
  void object(Value& v) {
    v.type = Value::Object;
    ++p_;
    for (;;) {
      ws();
      if (*p_ == '}') {
        ++p_;
        return;
      }
      if (*p_ != '"') return;
      std::string key;
      string(key);
      ws();
      if (*p_ == ':') ++p_;
      value(v.o[key]);
      ws();
      if (*p_ == ',') ++p_;
      else if (*p_ == '}') {
        ++p_;
        return;
      } else return;
    }
  }
  void array(Value& v) {
    v.type = Value::Array;
    ++p_;
    for (;;) {
      ws();
      if (*p_ == ']') {
        ++p_;
        return;
      }
      if (!*p_) return;
      v.a.emplace_back();
      value(v.a.back());
      ws();
      if (*p_ == ',') ++p_;
      else if (*p_ == ']') {
        ++p_;
        return;
      } else return;
    }
  }
  static void utf8(std::string& out, unsigned cp) {
    if (cp < 0x80) out += char(cp);
    else if (cp < 0x800) {
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
  unsigned hex4() {
    unsigned v = 0;
    for (int i = 0; i < 4 && *p_; ++i, ++p_) {
      char c = *p_;
      v = v * 16 + unsigned(c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0');
    }
    return v;
  }
  void string(std::string& out) {
    ++p_;
    while (*p_ && *p_ != '"') {
      if (*p_ == '\\') {
        ++p_;
        char c = *p_++;
        switch (c) {
          case 'n': out += '\n'; break;
          case 't': out += '\t'; break;
          case 'r': out += '\r'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'u': {
            unsigned cp = hex4();
            if (cp >= 0xD800 && cp < 0xDC00 && p_[0] == '\\' && p_[1] == 'u') {
              p_ += 2;
              unsigned lo = hex4();
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
            utf8(out, cp);
            break;
          }
          case '\0': return;
          default: out += c;
        }
      } else {
        out += *p_++;
      }
    }
    if (*p_ == '"') ++p_;
  }
};

inline Value parse(const char* json) { return Parser(json).parse(); }

// Appends `s` as a JSON string literal. Invalid UTF-8 (window titles come from
// arbitrary clients) is replaced byte by byte with U+FFFD.
inline void quote(std::string& out, const std::string& s) {
  out += '"';
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
  const unsigned char* end = p + s.size();
  while (p < end) {
    unsigned char c = *p;
    if (c == '"' || c == '\\') {
      out += '\\';
      out += char(c);
      ++p;
    } else if (c < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof buf, "\\u%04x", c);
      out += buf;
      ++p;
    } else if (c < 0x80) {
      out += char(c);
      ++p;
    } else {
      int len = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
      bool ok = len > 0 && p + len <= end;
      for (int i = 1; ok && i < len; ++i) ok = (p[i] & 0xC0) == 0x80;
      if (ok) {
        out.append(reinterpret_cast<const char*>(p), size_t(len));
        p += len;
      } else {
        out += "\xEF\xBF\xBD";
        ++p;
      }
    }
  }
  out += '"';
}

inline std::string quoted(const std::string& s) {
  std::string out;
  quote(out, s);
  return out;
}

// Numbers with up to 2 decimals (logical px), without trailing zeros.
inline std::string num(double v) {
  if (!std::isfinite(v)) return "0";
  char buf[32];
  double r = std::round(v * 100) / 100;
  if (r == std::floor(r)) std::snprintf(buf, sizeof buf, "%.0f", r);
  else std::snprintf(buf, sizeof buf, "%.2f", r);
  return buf;
}

}  // namespace wmjson
