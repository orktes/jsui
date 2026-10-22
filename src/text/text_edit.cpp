#include "text/text_edit.h"

#include <algorithm>

#include "core/document.h"

namespace jsui::edit {

// ------------------------------------------------------------------ UTF

std::u16string toUtf16(const std::string& s) {
  std::u16string out;
  out.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    unsigned char c = (unsigned char)s[i++];
    uint32_t cp;
    if (c < 0x80) {
      cp = c;
    } else {
      int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
      if (extra < 0) {
        cp = 0xFFFD;
      } else {
        cp = c & (0x3F >> extra);
        bool ok = true;
        for (int k = 0; k < extra; k++) {
          if (i >= s.size() || ((unsigned char)s[i] & 0xC0) != 0x80) {
            ok = false;
            break;
          }
          cp = (cp << 6) | ((unsigned char)s[i++] & 0x3F);
        }
        if (!ok || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
      }
    }
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out.push_back(char16_t(0xD800 + (cp >> 10)));
      out.push_back(char16_t(0xDC00 + (cp & 0x3FF)));
    } else {
      out.push_back(char16_t(cp));
    }
  }
  return out;
}

std::string toUtf8(const std::u16string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    uint32_t cp = s[i];
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
      i++;
    } else if (cp >= 0xD800 && cp <= 0xDFFF) {
      cp = 0xFFFD;  // lone surrogate
    }
    if (cp < 0x80) {
      out.push_back(char(cp));
    } else if (cp < 0x800) {
      out.push_back(char(0xC0 | (cp >> 6)));
      out.push_back(char(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(char(0xE0 | (cp >> 12)));
      out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(char(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(char(0xF0 | (cp >> 18)));
      out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(char(0x80 | (cp & 0x3F)));
    }
  }
  return out;
}

namespace {

// Code point at i (i at a code point start) and its length in code units.
uint32_t cpAt(const std::u16string& s, int i, int* len) {
  uint32_t c = s[size_t(i)];
  if (c >= 0xD800 && c <= 0xDBFF && size_t(i) + 1 < s.size() && s[size_t(i) + 1] >= 0xDC00 &&
      s[size_t(i) + 1] <= 0xDFFF) {
    *len = 2;
    return 0x10000 + ((c - 0xD800) << 10) + (s[size_t(i) + 1] - 0xDC00);
  }
  *len = 1;
  return c;
}

bool extendsCluster(uint32_t c) {
  return (c >= 0x0300 && c <= 0x036F) || (c >= 0x0483 && c <= 0x0489) || (c >= 0x0591 && c <= 0x05BD) ||
         (c >= 0x0610 && c <= 0x061A) || (c >= 0x064B && c <= 0x065F) || (c >= 0x0E31 && c <= 0x0E3A && c != 0x0E32 && c != 0x0E33) ||
         (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x1DC0 && c <= 0x1DFF) || (c >= 0x20D0 && c <= 0x20FF) ||
         (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xFE20 && c <= 0xFE2F) || (c >= 0x1F3FB && c <= 0x1F3FF) ||
         (c >= 0xE0020 && c <= 0xE007F) || (c >= 0xE0100 && c <= 0xE01EF) || c == 0x200C;
}

bool isRegional(uint32_t c) { return c >= 0x1F1E6 && c <= 0x1F1FF; }

enum class CharClass { Space, Word, Punct };

CharClass classOf(uint32_t c) {
  if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0xA0 || c == 0x3000 || (c >= 0x2000 && c <= 0x200B) ||
      c == 0x202F || c == 0x205F || c == 0x1680)
    return CharClass::Space;
  if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_') return CharClass::Word;
  if (c < 0x80) return CharClass::Punct;
  if ((c >= 0xA1 && c <= 0xBF && c != 0xAA && c != 0xB5 && c != 0xBA) || c == 0xD7 || c == 0xF7) return CharClass::Punct;
  if ((c >= 0x2010 && c <= 0x2BFF) || (c >= 0x3001 && c <= 0x303F) || (c >= 0xFE30 && c <= 0xFE4F) ||
      (c >= 0xFF01 && c <= 0xFF0F) || (c >= 0xFF1A && c <= 0xFF20) || (c >= 0x1F000 && c <= 0x1FAFF))
    return CharClass::Punct;
  return CharClass::Word;
}

CharClass classAt(const std::u16string& s, int i) {
  int len;
  return classOf(cpAt(s, i, &len));
}

}  // namespace

int nextBoundary(const std::u16string& s, int i) {
  int n = int(s.size());
  if (i >= n) return n;
  if (i < 0) return 0;
  int len;
  uint32_t c = cpAt(s, i, &len);
  int j = i + len;
  if (c == '\r' && j < n && s[size_t(j)] == '\n') return j + 1;
  if (isRegional(c) && j < n) {
    uint32_t d = cpAt(s, j, &len);
    if (isRegional(d)) return j + len;
  }
  while (j < n) {
    uint32_t d = cpAt(s, j, &len);
    if (extendsCluster(d)) {
      j += len;
    } else if (d == 0x200D && j + len < n) {  // ZWJ joins the next code point
      j += len;
      cpAt(s, j, &len);
      j += len;
    } else {
      break;
    }
  }
  return j;
}

int prevBoundary(const std::u16string& s, int i) {
  if (i <= 0) return 0;
  int b = 0;
  for (;;) {
    int nb = nextBoundary(s, b);
    if (nb >= i || nb == b) return b;
    b = nb;
  }
}

int nextWordEnd(const std::u16string& s, int i) {
  int n = int(s.size());
  while (i < n && classAt(s, i) != CharClass::Word) i = nextBoundary(s, i);
  while (i < n && classAt(s, i) == CharClass::Word) i = nextBoundary(s, i);
  return i;
}

int prevWordStart(const std::u16string& s, int i) {
  while (i > 0) {
    int p = prevBoundary(s, i);
    if (classAt(s, p) == CharClass::Word) break;
    i = p;
  }
  while (i > 0) {
    int p = prevBoundary(s, i);
    if (classAt(s, p) != CharClass::Word) break;
    i = p;
  }
  return i;
}

void wordAt(const std::u16string& s, int i, int* start, int* end) {
  int n = int(s.size());
  if (n == 0) {
    *start = *end = 0;
    return;
  }
  if (i >= n) i = prevBoundary(s, n);
  i = std::max(0, i);
  CharClass k = classAt(s, i);
  int a = i, b = nextBoundary(s, i);
  if (k == CharClass::Punct) {  // punctuation selects one character
    *start = a;
    *end = b;
    return;
  }
  while (a > 0) {
    int p = prevBoundary(s, a);
    if (classAt(s, p) != k) break;
    a = p;
  }
  while (b < n && classAt(s, b) == k) b = nextBoundary(s, b);
  *start = a;
  *end = b;
}

// ---------------------------------------------------------------- state

EditState* state(Node* n) {
  if (auto* s = peek(n)) return s;
  auto fresh = std::make_unique<EditState>();
  const Value& p = n->props();
  const Value& v = p["value"].isString() ? p["value"] : p["defaultValue"];
  if (p["value"].isString()) fresh->valueProp = p["value"].asString();
  if (v.isString()) fresh->value = toUtf16(v.asString());
  else if (v.isNumber()) fresh->value = toUtf16(Value(v).toJson());
  fresh->anchor = fresh->focus = int(fresh->value.size());
  EditState* s = fresh.get();
  n->editState = std::move(fresh);
  return s;
}

void contentChanged(Node* n) {
  n->markDirty();  // drops the paragraph, dirties layout
  if (n->yoga() && YGNodeHasMeasureFunc(n->yoga())) YGNodeMarkDirty(n->yoga());
}

void setValue(Node* n, EditState* s, std::u16string value) {
  if (value == s->value) return;
  s->value = std::move(value);
  int len = int(s->value.size());
  s->anchor = std::clamp(s->anchor, 0, len);
  s->focus = std::clamp(s->focus, 0, len);
  contentChanged(n);
}

void syncProps(Node* n) {
  EditState* s = peek(n);
  if (!s) {
    state(n);
    return;
  }
  const Value& p = n->props();
  if (!p["value"].isString()) return;
  const Value& count = p["mostRecentEventCount"];
  bool propChanged = p["value"].asString() != s->valueProp;
  s->valueProp = p["value"].asString();
  if (count.isNumber()) {
    // JS has not seen every native edit yet: this render is stale.
    if (count.asNumber() < s->eventCount) return;
  } else if (!propChanged) {
    return;  // no count: only a new value replaces the edited text
  }
  std::u16string v = toUtf16(p["value"].asString());
  if (v == s->value) return;
  bool sameLength = v.size() == s->value.size();
  s->value = std::move(v);
  int len = int(s->value.size());
  if (sameLength) {
    // e.g. a transform (upper-casing) of what was typed: keep the caret.
    s->anchor = std::min(s->anchor, len);
    s->focus = std::min(s->focus, len);
  } else {
    s->anchor = s->focus = len;  // like setting input.value on the web
  }
  s->undo.clear();
  s->redo.clear();
  s->coalesceTyping = false;
  s->hasGoalX = false;
  contentChanged(n);
}

}  // namespace jsui::edit
