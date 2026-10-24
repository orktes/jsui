#include "core/value.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace jsui {

bool Value::equals(const Value& b) const {
  const Value& a = *this;
  if (a.identity() && a.identity() == b.identity()) return true;
  if (a.isNull() || b.isNull()) return a.isNull() && b.isNull();
  if (a.isBool()) return b.isBool() && a.asBool() == b.asBool();
  if (a.isNumber()) return b.isNumber() && a.asNumber() == b.asNumber();
  if (a.isString()) return b.isString() && a.asString() == b.asString();
  if (a.isArray()) {
    if (!b.isArray() || a.asArray().size() != b.asArray().size()) return false;
    for (size_t i = 0; i < a.asArray().size(); i++)
      if (!a.asArray()[i].equals(b.asArray()[i])) return false;
    return true;
  }
  if (a.isObject()) {
    if (!b.isObject() || a.asObject().size() != b.asObject().size()) return false;
    auto ib = b.asObject().begin();
    for (auto& [k, v] : a.asObject()) {
      if (k != ib->first || !v.equals(ib->second)) return false;
      ++ib;
    }
    return true;
  }
  return false;
}

bool Value::assignReusing(const Value& src) {
  if (isNumber() && src.isNumber()) {
    double& d = std::get<2>(v_);
    if (d == src.asNumber()) return false;
    d = src.asNumber();
    return true;
  }
  if (identity() && identity() == src.identity()) return false;
  if (isArray() && src.isArray() && asArray().size() == src.asArray().size()) {
    if (std::get<4>(v_).use_count() > 1 && equals(src)) return false;
    Array& a = mutableArray();
    const Array& b = src.asArray();
    bool changed = false;
    for (size_t i = 0; i < a.size(); i++) changed |= a[i].assignReusing(b[i]);
    return changed;
  }
  if (isObject() && src.isObject() && asObject().size() == src.asObject().size()) {
    if (std::get<5>(v_).use_count() > 1 && equals(src)) return false;
    // Keys and values in one walk; on a key mismatch the entries changed so
    // far do not matter, the whole object is replaced.
    Object& a = mutableObject();
    auto ib = src.asObject().begin();
    bool changed = false;
    for (auto& entry : a) {
      if (entry.first != ib->first) {
        *this = src;
        return true;
      }
      changed |= entry.second.assignReusing(ib->second);
      ++ib;
    }
    return changed;
  }
  if (isString() && src.isString()) {
    std::string& s = std::get<3>(v_);
    if (s == src.asString()) return false;
    s = src.asString();
    return true;
  }
  if (equals(src)) return false;  // bools, null
  *this = src;
  return true;
}

namespace {

struct JsonParser {
  const std::string& s;
  size_t i = 0;
  std::string err;

  void ws() {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) i++;
  }
  bool fail(const char* m) {
    if (err.empty()) err = std::string(m) + " at " + std::to_string(i);
    return false;
  }
  static void appendUtf8(std::string& out, unsigned cp) {
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
  bool hex4(unsigned& cp) {
    if (i + 4 > s.size()) return fail("bad escape");
    cp = unsigned(std::strtoul(s.substr(i, 4).c_str(), nullptr, 16));
    i += 4;
    return true;
  }
  bool str(std::string& out) {
    i++;  // opening quote
    while (i < s.size() && s[i] != '"') {
      char c = s[i++];
      if (c != '\\') {
        out += c;
        continue;
      }
      if (i >= s.size()) return fail("bad escape");
      char e = s[i++];
      switch (e) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'u': {
          unsigned cp;
          if (!hex4(cp)) return false;
          if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {
            i += 2;
            unsigned lo;
            if (!hex4(lo)) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          appendUtf8(out, cp);
          break;
        }
        default: out += e;
      }
    }
    if (i >= s.size()) return fail("unterminated string");
    i++;
    return true;
  }
  bool value(Value& out) {
    ws();
    if (i >= s.size()) return fail("unexpected end");
    char c = s[i];
    if (c == '{') {
      i++;
      Value::Object o;
      ws();
      if (i < s.size() && s[i] == '}') {
        i++;
        out = Value(std::move(o));
        return true;
      }
      for (;;) {
        ws();
        if (i >= s.size() || s[i] != '"') return fail("expected key");
        std::string k;
        if (!str(k)) return false;
        ws();
        if (i >= s.size() || s[i] != ':') return fail("expected ':'");
        i++;
        Value v;
        if (!value(v)) return false;
        o[k] = std::move(v);
        ws();
        if (i < s.size() && s[i] == ',') {
          i++;
          continue;
        }
        if (i < s.size() && s[i] == '}') {
          i++;
          break;
        }
        return fail("expected ',' or '}'");
      }
      out = Value(std::move(o));
      return true;
    }
    if (c == '[') {
      i++;
      Value::Array a;
      ws();
      if (i < s.size() && s[i] == ']') {
        i++;
        out = Value(std::move(a));
        return true;
      }
      for (;;) {
        Value v;
        if (!value(v)) return false;
        a.push_back(std::move(v));
        ws();
        if (i < s.size() && s[i] == ',') {
          i++;
          continue;
        }
        if (i < s.size() && s[i] == ']') {
          i++;
          break;
        }
        return fail("expected ',' or ']'");
      }
      out = Value(std::move(a));
      return true;
    }
    if (c == '"') {
      std::string str_;
      if (!str(str_)) return false;
      out = Value(std::move(str_));
      return true;
    }
    if (s.compare(i, 4, "true") == 0) {
      i += 4;
      out = Value(true);
      return true;
    }
    if (s.compare(i, 5, "false") == 0) {
      i += 5;
      out = Value(false);
      return true;
    }
    if (s.compare(i, 4, "null") == 0) {
      i += 4;
      out = Value();
      return true;
    }
    char* end = nullptr;
    double d = std::strtod(s.c_str() + i, &end);
    if (end == s.c_str() + i) return fail("unexpected character");
    i = size_t(end - s.c_str());
    out = Value(d);
    return true;
  }
};

void writeString(std::string& out, const std::string& s) {
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out += char(c);
        }
    }
  }
  out += '"';
}

void write(std::string& out, const Value& v) {
  if (v.isNull()) {
    out += "null";
  } else if (v.isBool()) {
    out += v.asBool() ? "true" : "false";
  } else if (v.isNumber()) {
    double d = v.asNumber();
    if (!std::isfinite(d)) {
      out += "null";
    } else {
      char buf[32];
      std::snprintf(buf, sizeof buf, "%.17g", d);
      out += buf;
    }
  } else if (v.isString()) {
    writeString(out, v.asString());
  } else if (v.isArray()) {
    out += '[';
    bool first = true;
    for (auto& e : v.asArray()) {
      if (!first) out += ',';
      first = false;
      write(out, e);
    }
    out += ']';
  } else {
    out += '{';
    bool first = true;
    for (auto& [k, e] : v.asObject()) {
      if (!first) out += ',';
      first = false;
      writeString(out, k);
      out += ':';
      write(out, e);
    }
    out += '}';
  }
}

}  // namespace

Value Value::parseJson(const std::string& text, std::string* error) {
  JsonParser p{text};
  Value v;
  if (!p.value(v)) {
    if (error) *error = p.err;
    return Value();
  }
  return v;
}

std::string Value::toJson() const {
  std::string out;
  write(out, *this);
  return out;
}

}  // namespace jsui
