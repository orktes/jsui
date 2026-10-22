// jsui-record: render a scripted interaction session to video frames,
// headlessly, on a fixed frame clock.
//
//   jsui-record bundle.js --timeline session.json [--width 1280] [--height 720]
//               [--fps 60] [--scale 1] [--ffmpeg out.mp4] [--crf 20]
//               [--frames dir] [--stills dir] [--raw] [--no-overlay]
//               [--duration ms] [--font Family=path.ttf]
//
// The timeline is a JSON object {"steps": [...]} (or a bare array) of steps
// executed one after another on the simulated clock (frame n is at
// n * 1000 / fps ms; input is sent before the tick of the frame it falls on):
//
//   {"wait": 500}
//   {"caption": "Hover", "sub": "onHoverIn -> Animated.spring", "tag": "01"}
//   {"caption": null}                                  hide the caption
//   {"move": [x, y] | "#anchor", "dur": 400, "ease": "inOut"}   mouse move
//   {"click": true, "hold": 90}                        left click at the cursor
//   {"down": true} / {"up": true}                      left button
//   {"drag": "#anchor" | [x, y], "from": ..., "dur": 300, "ease": "linear"}
//                                                      mouse drag (button held)
//   {"touch": [x, y] | "#anchor", "to": ..., "dur": 120, "ease": "in",
//    "hold": 0, "release": true}                       touch drag; a quick
//                                                      release is a fling
//   {"tap": [x, y] | "#anchor", "hold": 80}            touch tap
//   {"wheel": [dx, dy], "count": 3, "interval": 120}   mouse wheel notches
//   {"key": "ArrowDown", "code": "ArrowDown", "mods": ["shift"], "hold": 80,
//    "count": 1, "interval": 250}                      key presses
//   {"type": "Hello", "interval": 90}                  typed text (key + text)
//   {"pad": 13, "hold": 90, "count": 1, "interval": 250}  gamepad button
//   {"axis": 1, "value": 0.8, "dur": 600}              gamepad stick, then 0
//   {"message": "name", "data": {...}}                 jsui_post_message
//   {"state": "key", "data": value}                    jsui_set_state (no data: remove)
//   {"state": "key", "patch": {...}}                   jsui_patch_state (merge patch)
//   {"cursor": "show" | "hide"}  {"leave": true}       overlay cursor / mouse leave
//   {"label": "..."}                                   legend text of the next input
//
// Every step may carry "label" (the input legend text, e.g. "Gamepad ↓") and
// "at" (absolute start time in ms; the clock never goes back). Anchors
// ("#name", or {"anchor": "name", "dx": 0, "dy": 0, "fx": 0.5, "fy": 0.5})
// are rectangles the app posts with
// jsui.postMessage('jsui-record:anchors', {name: {x, y, width, height}})
// (page coordinates), resolved when the step starts.
//
// Each frame is rendered with the CPU renderer, a cursor / touch / caption /
// input-legend overlay is drawn on top (--no-overlay skips it) and the frame
// is piped as raw RGBA to ffmpeg (--ffmpeg: H.264, yuv420p), written as PNGs
// (--frames dir), and/or written to stdout (--raw). --stills dir writes a PNG
// whenever a step has "still": "name".
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/value.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkData.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkBlurTypes.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkRRect.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTypeface.h"
#include "include/effects/SkGradientShader.h"
#include "include/encode/SkPngEncoder.h"
#include "include/ports/SkFontMgr_empty.h"
#include "jsui/jsui.h"
#include "text/embedded_fonts.h"

#ifdef _WIN32
// The ffmpeg pipe carries raw frames, so it must be opened in binary mode.
#define popen(cmd, mode) _popen(cmd, "wb")
#define pclose _pclose
#endif

using jsui::Value;

namespace {

bool readFile(const std::string& path, std::string* out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

double ease(const std::string& name, double t) {
  t = std::clamp(t, 0.0, 1.0);
  if (name == "linear") return t;
  if (name == "in") return t * t;
  if (name == "in3") return t * t * t;
  if (name == "out") return 1 - (1 - t) * (1 - t);
  if (name == "out3") return 1 - std::pow(1 - t, 3);
  // "inOut" (default): smoothstep-like cubic
  return t < 0.5 ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2;
}

struct Rect {
  float x = 0, y = 0, w = 0, h = 0;
};

// ------------------------------------------------------------------ overlay

enum class Icon { None, Mouse, Touch, Gamepad, Keyboard, Wheel };

struct Overlay {
  sk_sp<SkTypeface> regular, semibold, bold;
  int width = 1280, height = 720;

  void init() {
    sk_sp<SkFontMgr> mgr = SkFontMgr_New_Custom_Empty();
    for (size_t i = 0; i < jsui::kEmbeddedFontCount; i++) {
      const jsui::EmbeddedFont& f = jsui::kEmbeddedFonts[i];
      if (f.italic) continue;
      auto tf = mgr->makeFromData(SkData::MakeWithoutCopy(f.data, f.size));
      if (f.weight == 400) regular = tf;
      else if (f.weight == 600) semibold = tf;
      else if (f.weight == 700) bold = tf;
    }
    if (!semibold) semibold = regular;
    if (!bold) bold = semibold;
  }

  static SkPaint fill(SkColor c, float alpha = 1) {
    SkPaint p;
    p.setAntiAlias(true);
    p.setColor(c);
    p.setAlphaf(p.getAlphaf() * alpha);
    return p;
  }

  float textWidth(const std::string& s, const sk_sp<SkTypeface>& tf, float size) const {
    SkFont font(tf, size);
    font.setSubpixel(true);
    return font.measureText(s.data(), s.size(), SkTextEncoding::kUTF8);
  }

  void text(SkCanvas* c, const std::string& s, float x, float y, const sk_sp<SkTypeface>& tf, float size, SkColor color,
            float alpha = 1) const {
    SkFont font(tf, size);
    font.setSubpixel(true);
    font.setEdging(SkFont::Edging::kAntiAlias);
    c->drawSimpleText(s.data(), s.size(), SkTextEncoding::kUTF8, x, y, font, fill(color, alpha));
  }

  // Classic arrow pointer with its tip at (x, y).
  void cursor(SkCanvas* c, float x, float y, float alpha, bool pressed) const {
    if (alpha <= 0) return;
    SkPath p;
    const float pts[][2] = {{0, 0}, {0, 18.5f}, {4.4f, 14.4f}, {7.4f, 21.2f}, {10.6f, 19.8f}, {7.7f, 13.2f}, {13.4f, 13.2f}};
    p.moveTo(pts[0][0], pts[0][1]);
    for (int i = 1; i < 7; i++) p.lineTo(pts[i][0], pts[i][1]);
    p.close();
    c->save();
    c->translate(x, y);
    float s = pressed ? 1.12f : 1.25f;
    c->scale(s, s);
    SkPaint shadow = fill(SK_ColorBLACK, 0.45f * alpha);
    shadow.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 2.2f));
    c->save();
    c->translate(1.2f, 2.0f);
    c->drawPath(p, shadow);
    c->restore();
    c->drawPath(p, fill(SK_ColorWHITE, alpha));
    SkPaint stroke = fill(0xff111318, alpha);
    stroke.setStyle(SkPaint::kStroke_Style);
    stroke.setStrokeWidth(1.3f);
    stroke.setStrokeJoin(SkPaint::kRound_Join);
    c->drawPath(p, stroke);
    c->restore();
  }

  void ring(SkCanvas* c, float x, float y, float r, float width, SkColor color, float alpha) const {
    SkPaint p = fill(color, alpha);
    p.setStyle(SkPaint::kStroke_Style);
    p.setStrokeWidth(width);
    c->drawCircle(x, y, r, p);
  }

  void touch(SkCanvas* c, float x, float y, float alpha, float pressScale) const {
    if (alpha <= 0) return;
    SkPaint glow = fill(0xffffffff, 0.18f * alpha);
    glow.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 8));
    c->drawCircle(x, y, 26 * pressScale, glow);
    c->drawCircle(x, y, 20 * pressScale, fill(0xffffffff, 0.30f * alpha));
    ring(c, x, y, 20 * pressScale, 2.5f, 0xffffffff, 0.85f * alpha);
  }

  // Small vector icons for the input legend (in a 22 x 22 box).
  void icon(SkCanvas* c, Icon ic, float x, float y, SkColor color, float a) const {
    SkPaint s = fill(color, a);
    s.setStyle(SkPaint::kStroke_Style);
    s.setStrokeWidth(1.8f);
    s.setStrokeCap(SkPaint::kRound_Cap);
    SkPaint f = fill(color, a);
    switch (ic) {
      case Icon::Mouse:
      case Icon::Wheel: {
        c->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(x + 5, y + 1, 12, 20), 6, 6), s);
        c->drawLine(x + 11, y + 4.5f, x + 11, y + (ic == Icon::Wheel ? 10.5f : 8.5f), s);
        if (ic == Icon::Wheel) {
          c->drawLine(x + 8.5f, y + 14, x + 11, y + 16.5f, s);
          c->drawLine(x + 13.5f, y + 14, x + 11, y + 16.5f, s);
        }
        break;
      }
      case Icon::Touch: {
        c->drawCircle(x + 11, y + 11, 8.5f, s);
        c->drawCircle(x + 11, y + 11, 4, f);
        break;
      }
      case Icon::Gamepad: {
        SkPath p;
        p.addRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(x, y + 5, 22, 13), 6.5f, 6.5f));
        c->drawPath(p, s);
        c->drawLine(x + 5, y + 11.5f, x + 9, y + 11.5f, s);
        c->drawLine(x + 7, y + 9.5f, x + 7, y + 13.5f, s);
        c->drawCircle(x + 15, y + 10, 1.3f, f);
        c->drawCircle(x + 17.5f, y + 12.8f, 1.3f, f);
        break;
      }
      case Icon::Keyboard: {
        c->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(x, y + 4, 22, 14), 3, 3), s);
        for (int i = 0; i < 4; i++) c->drawCircle(x + 4.5f + i * 4.3f, y + 8.5f, 0.9f, f);
        c->drawLine(x + 6, y + 13.5f, x + 16, y + 13.5f, s);
        break;
      }
      case Icon::None: break;
    }
  }

  // Caption: bottom-left glass card with a tag, a title and a subtitle;
  // the input legend chip sits to its right.
  void caption(SkCanvas* c, const std::string& tag, const std::string& title, const std::string& sub, float alpha,
               Icon ic, const std::string& legend, float legendAlpha) const {
    float x = 24, h = 62;
    float y = height - 24 - h;
    float pad = 16;
    float tagW = tag.empty() ? 0 : textWidth(tag, bold, 13) + 16;
    float titleW = textWidth(title, bold, 19);
    float subW = textWidth(sub, regular, 14);
    float w = pad * 2 + (tagW > 0 ? tagW + 12 : 0) + std::max(titleW, subW);
    if (alpha > 0 && !title.empty()) {
      c->save();
      float slide = (1 - alpha) * 10;
      c->translate(0, slide);
      SkRRect card = SkRRect::MakeRectXY(SkRect::MakeXYWH(x, y, w, h), 14, 14);
      SkPaint shadow = fill(SK_ColorBLACK, 0.55f * alpha);
      shadow.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 10));
      c->drawRRect(card.makeOffset(0, 6), shadow);
      c->drawRRect(card, fill(0xff090c16, 0.86f * alpha));
      SkPaint border = fill(0xffffffff, 0.14f * alpha);
      border.setStyle(SkPaint::kStroke_Style);
      border.setStrokeWidth(1);
      c->drawRRect(card, border);
      // accent bar
      c->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(x, y + 14, 3, h - 28), 1.5f, 1.5f), fill(0xfff2c35b, alpha));
      float tx = x + pad;
      if (tagW > 0) {
        SkRRect chip = SkRRect::MakeRectXY(SkRect::MakeXYWH(tx, y + (h - 26) / 2, tagW, 26), 7, 7);
        c->drawRRect(chip, fill(0xfff2c35b, 0.95f * alpha));
        text(c, tag, tx + 8, y + h / 2 + 4.5f, bold, 13, 0xff1d1405, alpha);
        tx += tagW + 12;
      }
      text(c, title, tx, y + 27, bold, 19, 0xfff4f6ff, alpha);
      text(c, sub, tx, y + 47, regular, 14, 0xffa9b4d4, alpha);
      c->restore();
    }
    if (legendAlpha > 0 && !legend.empty()) {
      float lx = (alpha > 0 && !title.empty()) ? x + w + 12 : x;
      float lw = 16 + 22 + 10 + textWidth(legend, semibold, 15) + 16;
      float lh = 42;
      float ly = y + (h - lh) / 2;
      SkRRect chip = SkRRect::MakeRectXY(SkRect::MakeXYWH(lx, ly, lw, lh), 21, 21);
      SkPaint shadow = fill(SK_ColorBLACK, 0.5f * legendAlpha);
      shadow.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 8));
      c->drawRRect(chip.makeOffset(0, 4), shadow);
      c->drawRRect(chip, fill(0xff1b2238, 0.92f * legendAlpha));
      SkPaint border = fill(0xff4fd1ff, 0.55f * legendAlpha);
      border.setStyle(SkPaint::kStroke_Style);
      border.setStrokeWidth(1.2f);
      c->drawRRect(chip, border);
      icon(c, ic, lx + 16, ly + 10, 0xff9fe6ff, legendAlpha);
      text(c, legend, lx + 16 + 22 + 10, ly + 26.5f, semibold, 15, 0xffeef2ff, legendAlpha);
    }
  }
};

// ----------------------------------------------------------------- recorder

struct Options {
  std::string bundle, timeline, ffmpegOut, framesDir, stillsDir;
  int width = 1280, height = 720, fps = 60, crf = 20;
  float scale = 1;
  bool raw = false, overlay = true;
  double duration = -1;  // ms; default: end of the timeline + 500
  std::vector<std::pair<std::string, std::string>> fonts;
  std::string preset = "slow";
};

struct Recorder {
  Options opt;
  JsuiRuntime* rt = nullptr;
  Overlay ov;
  std::map<std::string, Rect> anchors;
  std::vector<Value> steps;

  // Input state.
  float mx = -100, my = -100;  // cursor
  bool mouseDown = false;
  bool touchActive = false;
  float tx = 0, ty = 0;
  double touchEnd = -1e9;
  struct Trail {
    float x, y;
    double t;
  };
  std::vector<Trail> trail;
  struct Ripple {
    float x, y;
    double t;
  };
  std::vector<Ripple> ripples;
  bool cursorVisible = true;
  double cursorChange = -1e9;

  // Caption / legend.
  std::string capTag, capTitle, capSub;
  double capSince = -1e9;
  bool capOn = false;
  double capOffAt = -1e9;
  std::string prevTag, prevTitle, prevSub;
  std::string legend, pendingLabel;
  Icon legendIcon = Icon::None;
  double legendUntil = -1e9, legendSince = -1e9;

  // Execution.
  size_t index = 0;
  double stepStart = 0;
  bool stepBegun = false;
  // Per-step scratch.
  float fromX = 0, fromY = 0, toX = 0, toY = 0;
  int sentCount = 0;
  bool phaseA = false, phaseB = false;
  std::string stillRequest;

  static void onMessage(void* user, const char* name, const char* json) {
    auto* r = static_cast<Recorder*>(user);
    if (std::strcmp(name, "jsui-record:anchors") != 0 || !json) return;
    Value v = Value::parseJson(json);
    for (auto& [k, rv] : v.asObject()) {
      Rect rc;
      rc.x = float(rv["x"].asNumber());
      rc.y = float(rv["y"].asNumber());
      rc.w = float(rv["width"].asNumber());
      rc.h = float(rv["height"].asNumber());
      r->anchors[k] = rc;
    }
  }

  bool resolve(const Value& target, float* x, float* y) {
    std::string name;
    float dx = 0, dy = 0, fx = 0.5f, fy = 0.5f;
    if (target.isArray() && target.asArray().size() >= 2) {
      *x = float(target.asArray()[0].asNumber());
      *y = float(target.asArray()[1].asNumber());
      return true;
    }
    if (target.isString()) name = target.asString();
    else if (target.isObject()) {
      name = target["anchor"].asString();
      dx = float(target["dx"].asNumber());
      dy = float(target["dy"].asNumber());
      fx = float(target["fx"].asNumber(0.5));
      fy = float(target["fy"].asNumber(0.5));
    }
    if (!name.empty() && name[0] == '#') name = name.substr(1);
    auto it = anchors.find(name);
    if (it == anchors.end()) {
      std::fprintf(stderr, "jsui-record: unknown anchor '%s' (step %zu)\n", name.c_str(), index);
      *x = mx;
      *y = my;
      return false;
    }
    *x = it->second.x + it->second.w * fx + dx;
    *y = it->second.y + it->second.h * fy + dy;
    return true;
  }

  static int mods(const Value& v) {
    int m = 0;
    for (auto& e : v.asArray()) {
      const std::string& s = e.asString();
      if (s == "shift") m |= JSUI_MOD_SHIFT;
      else if (s == "ctrl") m |= JSUI_MOD_CTRL;
      else if (s == "alt") m |= JSUI_MOD_ALT;
      else if (s == "meta") m |= JSUI_MOD_META;
    }
    return m;
  }

  void setLegend(const Value& step, Icon ic, const std::string& fallback, double now, double dur) {
    std::string l = step["label"].isString() ? step["label"].asString() : (!pendingLabel.empty() ? pendingLabel : fallback);
    pendingLabel.clear();
    if (l.empty()) return;
    if (l != legend || now > legendUntil) legendSince = now;
    legend = l;
    legendIcon = ic;
    legendUntil = std::max(legendUntil, now + dur + 900);
  }

  void mouseMove(float x, float y) {
    mx = x;
    my = y;
    jsui_send_mouse_move(rt, x, y);
  }

  // Step duration (ms) for scheduling.
  static double duration(const Value& s) {
    if (s.has("wait")) return s["wait"].asNumber();
    if (s.has("move") || s.has("drag")) return s["dur"].asNumber(400);
    if (s.has("touch")) return s["dur"].asNumber(150) + s["hold"].asNumber(0);
    if (s.has("tap")) return s["hold"].asNumber(80);
    if (s.has("click")) return s["hold"].asNumber(90);
    if (s.has("wheel")) return std::max(0.0, s["count"].asNumber(1) - 1) * s["interval"].asNumber(120);
    if (s.has("key") || s.has("pad")) {
      double n = s["count"].asNumber(1);
      return (n - 1) * s["interval"].asNumber(250) + s["hold"].asNumber(80);
    }
    if (s.has("type")) {
      std::string t = s["type"].asString();
      size_t n = 0;
      for (unsigned char ch : t) n += (ch & 0xC0) != 0x80;
      return double(n) * s["interval"].asNumber(90);
    }
    if (s.has("axis")) return s["dur"].asNumber(500);
    return 0;
  }

  // Characters of a UTF-8 string.
  static std::vector<std::string> chars(const std::string& t) {
    std::vector<std::string> out;
    for (size_t i = 0; i < t.size();) {
      size_t n = 1;
      unsigned char c = (unsigned char)t[i];
      if (c >= 0xF0) n = 4;
      else if (c >= 0xE0) n = 3;
      else if (c >= 0xC0) n = 2;
      out.push_back(t.substr(i, n));
      i += n;
    }
    return out;
  }

  void sendKey(const std::string& key, const std::string& code, bool down, int m) {
    jsui_send_key(rt, key.c_str(), code.empty() ? nullptr : code.c_str(), down ? 1 : 0, 0, m);
  }

  // Runs the active step at local time u (ms since its start); returns true
  // when it is complete.
  bool run(const Value& s, double u, double now) {
    double dur = duration(s);
    bool first = !stepBegun;
    stepBegun = true;
    if (first && s["still"].isString()) stillRequest = s["still"].asString();
    if (s.has("caption")) {
      if (s["caption"].isNull()) {
        if (capOn) capOffAt = now;
        capOn = false;
      } else {
        prevTag = capTag;
        prevTitle = capTitle;
        prevSub = capSub;
        capTag = s["tag"].asString();
        capTitle = s["caption"].asString();
        capSub = s["sub"].asString();
        capSince = now;
        capOn = true;
      }
      return true;
    }
    if (s.has("label") && s.asObject().size() == 1) {
      pendingLabel = s["label"].asString();
      return true;
    }
    if (s.has("cursor")) {
      bool show = s["cursor"].asString() != "hide";
      if (show != cursorVisible) cursorChange = now;
      cursorVisible = show;
      return true;
    }
    if (s.has("message")) {
      std::string json = s.has("data") ? s["data"].toJson() : "null";
      jsui_post_message(rt, s["message"].asString().c_str(), json.c_str());
      return true;
    }
    if (s.has("state")) {
      std::string key = s["state"].asString();
      if (s.has("patch")) jsui_patch_state(rt, key.c_str(), s["patch"].toJson().c_str());
      else jsui_set_state(rt, key.c_str(), s.has("data") ? s["data"].toJson().c_str() : nullptr);
      return true;
    }
    if (s.has("leave")) {
      jsui_send_mouse_leave(rt);
      return true;
    }
    if (s.has("wait")) return u >= dur;
    if (s.has("move")) {
      if (first) {
        fromX = mx;
        fromY = my;
        resolve(s["move"], &toX, &toY);
        if (!cursorVisible) {
          cursorVisible = true;
          cursorChange = now;
        }
        if (fromX < -50) {
          fromX = toX;
          fromY = toY;
        }
        setLegend(s, Icon::Mouse, "", now, dur);
      }
      double e = dur > 0 ? ease(s["ease"].asString(), u / dur) : 1;
      mouseMove(float(fromX + (toX - fromX) * e), float(fromY + (toY - fromY) * e));
      return u >= dur;
    }
    if (s.has("click")) {
      if (first) {
        if (s["click"].isArray() || s["click"].isString() || s["click"].isObject()) {
          float x, y;
          resolve(s["click"], &x, &y);
          mouseMove(x, y);
        }
        jsui_send_mouse_button(rt, mx, my, JSUI_MOUSE_LEFT, 1, mods(s["mods"]));
        mouseDown = true;
        ripples.push_back({mx, my, now});
        setLegend(s, Icon::Mouse, "Click", now, dur);
      }
      if (u >= dur) {
        jsui_send_mouse_button(rt, mx, my, JSUI_MOUSE_LEFT, 0, mods(s["mods"]));
        mouseDown = false;
        return true;
      }
      return false;
    }
    if (s.has("down") || s.has("up")) {
      bool down = s.has("down");
      jsui_send_mouse_button(rt, mx, my, JSUI_MOUSE_LEFT, down ? 1 : 0, mods(s["mods"]));
      mouseDown = down;
      if (down) ripples.push_back({mx, my, now});
      return true;
    }
    if (s.has("drag")) {
      if (first) {
        if (s.has("from")) {
          resolve(s["from"], &fromX, &fromY);
          mouseMove(fromX, fromY);
        } else {
          fromX = mx;
          fromY = my;
        }
        resolve(s["drag"], &toX, &toY);
        jsui_send_mouse_button(rt, mx, my, JSUI_MOUSE_LEFT, 1, 0);
        mouseDown = true;
        setLegend(s, Icon::Mouse, "Drag", now, dur);
      }
      double e = dur > 0 ? ease(s["ease"].asString(), u / dur) : 1;
      mouseMove(float(fromX + (toX - fromX) * e), float(fromY + (toY - fromY) * e));
      if (u >= dur) {
        jsui_send_mouse_button(rt, mx, my, JSUI_MOUSE_LEFT, 0, 0);
        mouseDown = false;
        return true;
      }
      return false;
    }
    if (s.has("touch")) {
      double moveDur = s["dur"].asNumber(150);
      if (first) {
        resolve(s["touch"], &fromX, &fromY);
        if (s.has("to")) resolve(s["to"], &toX, &toY);
        else {
          toX = fromX + float(s["dx"].asNumber());
          toY = fromY + float(s["dy"].asNumber());
        }
        tx = fromX;
        ty = fromY;
        jsui_send_touch(rt, 0, 0, tx, ty);
        touchActive = true;
        trail.clear();
        trail.push_back({tx, ty, now});
        if (cursorVisible) cursorChange = now;
        cursorVisible = false;
        setLegend(s, Icon::Touch, "Touch drag", now, dur);
      }
      double e = moveDur > 0 ? ease(s.has("ease") ? s["ease"].asString() : std::string("linear"), u / moveDur) : 1;
      float nx = float(fromX + (toX - fromX) * e), ny = float(fromY + (toY - fromY) * e);
      if (!first && (nx != tx || ny != ty)) {
        tx = nx;
        ty = ny;
        jsui_send_touch(rt, 0, 1, tx, ty);
        trail.push_back({tx, ty, now});
      }
      if (u >= dur) {
        if (s["release"].asBool(true)) {
          jsui_send_touch(rt, 0, 2, tx, ty);
          touchActive = false;
          touchEnd = now;
        }
        return true;
      }
      return false;
    }
    if (s.has("tap")) {
      if (first) {
        resolve(s["tap"], &tx, &ty);
        jsui_send_touch(rt, 0, 0, tx, ty);
        touchActive = true;
        trail.clear();
        if (cursorVisible) cursorChange = now;
        cursorVisible = false;
        setLegend(s, Icon::Touch, "Tap", now, dur);
      }
      if (u >= dur) {
        jsui_send_touch(rt, 0, 2, tx, ty);
        touchActive = false;
        touchEnd = now;
        return true;
      }
      return false;
    }
    if (s.has("wheel")) {
      if (first) {
        sentCount = 0;
        float dy = float(s["wheel"].asArray().size() > 1 ? s["wheel"].asArray()[1].asNumber() : 0);
        setLegend(s, Icon::Wheel, dy >= 0 ? "Wheel \xE2\x86\x93" : "Wheel \xE2\x86\x91", now, dur);
      }
      int count = int(s["count"].asNumber(1));
      double interval = s["interval"].asNumber(120);
      const auto& w = s["wheel"].asArray();
      float dx = w.size() > 0 ? float(w[0].asNumber()) : 0, dy = w.size() > 1 ? float(w[1].asNumber()) : 0;
      while (sentCount < count && u >= sentCount * interval) {
        jsui_send_mouse_wheel(rt, mx, my, dx, dy);
        sentCount++;
      }
      return sentCount >= count;
    }
    if (s.has("key") || s.has("pad")) {
      bool pad = s.has("pad");
      int count = int(s["count"].asNumber(1));
      double interval = s["interval"].asNumber(250), hold = s["hold"].asNumber(80);
      std::string key = pad ? "" : s["key"].asString();
      std::string code = s["code"].isString() ? s["code"].asString() : key;
      int button = int(s["pad"].asNumber());
      int m = mods(s["mods"]);
      if (first) {
        sentCount = 0;
        phaseA = false;
        if (pad) {
          static const char* names[] = {"A", "B", "X", "Y", "LB", "RB", "LT", "RT", "View", "Menu", "LS", "RS",
                                        "D-pad \xE2\x86\x91", "D-pad \xE2\x86\x93", "D-pad \xE2\x86\x90",
                                        "D-pad \xE2\x86\x92", "Home"};
          std::string n = button >= 0 && button <= 16 ? names[button] : std::to_string(button);
          setLegend(s, Icon::Gamepad, "Gamepad " + n, now, dur);
        } else {
          std::string n = key == " " ? "Space" : key;
          if (n == "ArrowDown") n = "\xE2\x86\x93";
          else if (n == "ArrowUp") n = "\xE2\x86\x91";
          else if (n == "ArrowLeft") n = "\xE2\x86\x90";
          else if (n == "ArrowRight") n = "\xE2\x86\x92";
          std::string pre = (m & JSUI_MOD_CTRL ? "Ctrl+" : std::string()) + (m & JSUI_MOD_SHIFT ? "Shift+" : "");
          setLegend(s, Icon::Keyboard, "Key " + pre + n, now, dur);
        }
      }
      // Press k at k * interval, release at k * interval + hold.
      while (true) {
        int k = sentCount / 2;
        bool release = sentCount % 2 == 1;
        if (k >= count) break;
        double at = k * interval + (release ? hold : 0);
        if (u < at) break;
        if (pad) jsui_send_gamepad_button(rt, 0, button, release ? 0 : 1, release ? 0.f : 1.f);
        else {
          sendKey(key, code, !release, m);
          if (!release && s["text"].isString()) jsui_send_text(rt, s["text"].asString().c_str());
        }
        sentCount++;
      }
      return sentCount >= count * 2;
    }
    if (s.has("type")) {
      auto cs = chars(s["type"].asString());
      double interval = s["interval"].asNumber(90);
      if (first) {
        sentCount = 0;
        setLegend(s, Icon::Keyboard, "Typing", now, dur);
      }
      while (sentCount < int(cs.size()) && u >= sentCount * interval) {
        const std::string& ch = cs[size_t(sentCount)];
        std::string code = ch.size() == 1 && std::isalpha((unsigned char)ch[0])
                               ? std::string("Key") + char(std::toupper((unsigned char)ch[0]))
                               : ch == " " ? "Space" : "";
        sendKey(ch, code, true, 0);
        jsui_send_text(rt, ch.c_str());
        sendKey(ch, code, false, 0);
        sentCount++;
      }
      return sentCount >= int(cs.size()) && u >= dur;
    }
    if (s.has("axis")) {
      int axis = int(s["axis"].asNumber());
      if (first) {
        jsui_send_gamepad_axis(rt, 0, axis, float(s["value"].asNumber()));
        float v = float(s["value"].asNumber());
        const char* dir = axis % 2 == 1 ? (v > 0 ? "\xE2\x86\x93" : "\xE2\x86\x91") : (v > 0 ? "\xE2\x86\x92" : "\xE2\x86\x90");
        setLegend(s, Icon::Gamepad, std::string(axis >= 2 ? "Right stick " : "Left stick ") + dir, now, dur);
      }
      if (u >= dur) {
        jsui_send_gamepad_axis(rt, 0, axis, 0);
        return true;
      }
      return false;
    }
    std::fprintf(stderr, "jsui-record: unknown step %zu: %s\n", index, s.toJson().c_str());
    return true;
  }

  // Advances the timeline to `now`.
  void advance(double now) {
    while (index < steps.size()) {
      const Value& s = steps[index];
      if (!stepBegun && s["at"].isNumber()) stepStart = std::max(stepStart, s["at"].asNumber());
      if (now < stepStart) return;
      double u = now - stepStart;
      if (!run(s, u, now)) return;
      stepStart += duration(s);
      index++;
      stepBegun = false;
    }
  }

  double timelineEnd() const {
    double t = 0;
    for (auto& s : steps) {
      if (s["at"].isNumber()) t = std::max(t, s["at"].asNumber());
      t += duration(s);
    }
    return t;
  }

  void drawOverlay(SkCanvas* c, double now) {
    // Caption (cross-fade on change).
    float capA = capOn ? float(std::clamp((now - capSince) / 220.0, 0.0, 1.0))
                       : float(1 - std::clamp((now - capOffAt) / 220.0, 0.0, 1.0));
    float legA = 0;
    if (now < legendUntil) legA = float(std::clamp((now - legendSince) / 120.0, 0.0, 1.0));
    else legA = float(1 - std::clamp((now - legendUntil) / 300.0, 0.0, 1.0));
    ov.caption(c, capTag, capTitle, capSub, capA, legendIcon, legend, legA);

    // Click ripples.
    for (auto& r : ripples) {
      double a = (now - r.t) / 420.0;
      if (a < 0 || a > 1) continue;
      ov.ring(c, r.x, r.y, float(8 + 26 * a), 2.5f, 0xfff2c35b, float(0.9 * (1 - a)));
    }
    ripples.erase(std::remove_if(ripples.begin(), ripples.end(), [&](const Ripple& r) { return now - r.t > 500; }),
                  ripples.end());
    // Touch trail + finger.
    trail.erase(std::remove_if(trail.begin(), trail.end(), [&](const Trail& t) { return now - t.t > 260; }), trail.end());
    for (size_t i = 0; i < trail.size(); i++) {
      double a = 1 - (now - trail[i].t) / 260.0;
      c->drawCircle(trail[i].x, trail[i].y, float(5 + 9 * a), Overlay::fill(0xffffffff, float(0.22 * a)));
    }
    if (touchActive) ov.touch(c, tx, ty, 1, 1);
    else if (now - touchEnd < 220) {
      float a = float(1 - (now - touchEnd) / 220.0);
      ov.touch(c, tx, ty, a, 1 + (1 - a) * 0.4f);
    }
    // Mouse cursor (fades when hidden / shown).
    float cf = float(std::clamp((now - cursorChange) / 180.0, 0.0, 1.0));
    float ca = cursorVisible ? cf : 1 - cf;
    if (mx > -50) ov.cursor(c, mx, my, ca, mouseDown);
  }
};

bool writePng(const std::string& path, const SkPixmap& pm) {
  SkFILEWStream out(path.c_str());
  if (!out.isValid()) return false;
  SkPngEncoder::Options o;
  o.fZLibLevel = 4;
  return SkPngEncoder::Encode(&out, pm, o);
}

void usage() {
  std::printf(
      "usage: jsui-record <bundle.js> --timeline session.json [--width 1280] [--height 720] [--fps 60]\n"
      "                   [--scale 1] [--ffmpeg out.mp4] [--crf 20] [--preset slow] [--frames dir]\n"
      "                   [--stills dir] [--raw] [--no-overlay] [--duration ms] [--font Family=path]\n"
      "  Renders a scripted input session headlessly at a fixed frame rate (see the header of\n"
      "  tools/jsui-record/main.cpp for the timeline format).\n");
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", a.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--timeline") o.timeline = next();
    else if (a == "--width") o.width = std::atoi(next().c_str());
    else if (a == "--height") o.height = std::atoi(next().c_str());
    else if (a == "--fps") o.fps = std::atoi(next().c_str());
    else if (a == "--scale") o.scale = float(std::atof(next().c_str()));
    else if (a == "--ffmpeg") o.ffmpegOut = next();
    else if (a == "--crf") o.crf = std::atoi(next().c_str());
    else if (a == "--preset") o.preset = next();
    else if (a == "--frames") o.framesDir = next();
    else if (a == "--stills") o.stillsDir = next();
    else if (a == "--raw") o.raw = true;
    else if (a == "--no-overlay") o.overlay = false;
    else if (a == "--duration") o.duration = std::atof(next().c_str());
    else if (a == "--font") {
      std::string v = next();
      auto eq = v.find('=');
      if (eq == std::string::npos) {
        std::fprintf(stderr, "--font expects Family=path\n");
        return 2;
      }
      o.fonts.emplace_back(v.substr(0, eq), v.substr(eq + 1));
    } else if (a == "-h" || a == "--help") {
      usage();
      return 0;
    } else if (!a.empty() && a[0] == '-') {
      std::fprintf(stderr, "unknown option %s\n", a.c_str());
      return 2;
    } else o.bundle = a;
  }
  if (o.bundle.empty() || o.timeline.empty() || o.fps <= 0 || o.width <= 0 || o.height <= 0) {
    usage();
    return 2;
  }

  for (const std::string* dir : {&o.framesDir, &o.stillsDir}) {
    std::error_code ec;
    if (!dir->empty()) std::filesystem::create_directories(*dir, ec);
    if (ec) {
      std::fprintf(stderr, "cannot create %s: %s\n", dir->c_str(), ec.message().c_str());
      return 1;
    }
  }

  Recorder r;
  r.opt = o;
  std::string tl;
  if (!readFile(o.timeline, &tl)) {
    std::fprintf(stderr, "cannot read %s\n", o.timeline.c_str());
    return 1;
  }
  std::string err;
  Value doc = Value::parseJson(tl, &err);
  if (!err.empty()) {
    std::fprintf(stderr, "%s: %s\n", o.timeline.c_str(), err.c_str());
    return 1;
  }
  r.steps = doc.isArray() ? doc.asArray() : doc["steps"].asArray();

  JsuiConfig cfg;
  jsui_config_init(&cfg);
  cfg.width = o.width;
  cfg.height = o.height;
  cfg.pixel_ratio = o.scale;
  cfg.on_message = &Recorder::onMessage;
  cfg.message_user = &r;
  r.rt = jsui_create(&cfg);
  for (auto& [family, path] : o.fonts) {
    std::string data;
    if (!readFile(path, &data) || jsui_register_font(r.rt, family.c_str(), data.data(), data.size(), 400, 0) != 0) {
      std::fprintf(stderr, "failed to load font %s\n", path.c_str());
      return 1;
    }
  }
  jsui_load_bundle_url(r.rt, o.bundle.c_str());
  // The bundle and its assets load on worker threads: wait for them with the
  // clock standing still at 0 (bounded by ~10 s of real time).
  auto settle = [&](double t) {
    for (int round = 0; round < 5000 && jsui_pending_loads(r.rt) > 0; round++) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      jsui_tick(r.rt, t);
    }
  };
  jsui_tick(r.rt, 0);
  settle(0);
  jsui_tick(r.rt, 0);
  settle(0);
  if (const char* e = jsui_last_error(r.rt)) {
    std::fprintf(stderr, "error: %s\n", e);
    return 1;
  }
  r.ov.width = o.width;
  r.ov.height = o.height;
  if (o.overlay) r.ov.init();

  int pw = int(std::lround(o.width * o.scale)), ph = int(std::lround(o.height * o.scale));
  double endMs = o.duration > 0 ? o.duration : r.timelineEnd() + 500;
  long frames = long(std::ceil(endMs * o.fps / 1000.0));

  FILE* ff = nullptr;
  if (!o.ffmpegOut.empty()) {
    char cmd[2048];
    std::snprintf(cmd, sizeof cmd,
                  "ffmpeg -hide_banner -loglevel error -y -f rawvideo -pix_fmt rgba -s %dx%d -r %d -i - "
                  "-c:v libx264 -preset %s -crf %d -pix_fmt yuv420p -movflags +faststart \"%s\"",
                  pw, ph, o.fps, o.preset.c_str(), o.crf, o.ffmpegOut.c_str());
    ff = popen(cmd, "w");
    if (!ff) {
      std::fprintf(stderr, "cannot start ffmpeg\n");
      return 1;
    }
  }

  std::vector<uint8_t> canvasPx(size_t(pw) * ph * 4), outPx(size_t(pw) * ph * 4);
  SkImageInfo info = SkImageInfo::Make(pw, ph, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  auto surface = SkSurfaces::WrapPixels(info, outPx.data(), size_t(pw) * 4);
  int status = 0;
  for (long f = 0; f < frames; f++) {
    double now = double(f) * 1000.0 / o.fps;
    r.advance(now);
    jsui_tick(r.rt, now);
    if (jsui_pending_loads(r.rt) > 0) settle(now);
    jsui_render_rgba_incremental(r.rt, canvasPx.data(), pw, ph, size_t(pw) * 4, nullptr);
    // Opaque frame: the UI over black.
    for (size_t i = 0; i < outPx.size(); i += 4) {
      outPx[i] = canvasPx[i];
      outPx[i + 1] = canvasPx[i + 1];
      outPx[i + 2] = canvasPx[i + 2];
      outPx[i + 3] = 255;
    }
    if (o.overlay) {
      SkCanvas* c = surface->getCanvas();
      c->save();
      c->scale(o.scale, o.scale);
      r.drawOverlay(c, now);
      c->restore();
    }
    SkPixmap pm(info, outPx.data(), size_t(pw) * 4);
    if (ff && std::fwrite(outPx.data(), 1, outPx.size(), ff) != outPx.size()) {
      std::fprintf(stderr, "ffmpeg pipe closed\n");
      status = 1;
      break;
    }
    if (o.raw) std::fwrite(outPx.data(), 1, outPx.size(), stdout);
    if (!o.framesDir.empty()) {
      char name[64];
      std::snprintf(name, sizeof name, "/frame-%05ld.png", f);
      if (!writePng(o.framesDir + name, pm)) {
        std::fprintf(stderr, "cannot write %s%s\n", o.framesDir.c_str(), name);
        status = 1;
        break;
      }
    }
    if (!r.stillRequest.empty()) {
      std::string still = o.stillsDir + "/" + r.stillRequest + ".png";
      if (!o.stillsDir.empty() && !writePng(still, pm)) {
        std::fprintf(stderr, "cannot write %s\n", still.c_str());
        status = 1;
        break;
      }
      r.stillRequest.clear();
    }
    if (const char* e = jsui_last_error(r.rt)) {
      std::fprintf(stderr, "error at %.0f ms: %s\n", now, e);
      status = 1;
      break;
    }
    if (f % (o.fps * 5) == 0) std::fprintf(stderr, "jsui-record: %.0f / %.0f ms\n", now, endMs);
  }
  if (ff && pclose(ff) != 0) {
    std::fprintf(stderr, "ffmpeg failed\n");
    status = 1;
  }
  if (r.index < r.steps.size()) std::fprintf(stderr, "jsui-record: timeline cut at step %zu of %zu\n", r.index, r.steps.size());
  std::fprintf(stderr, "jsui-record: %ld frames (%.1f s at %d fps)\n", frames, endMs / 1000, o.fps);
  jsui_destroy(r.rt);
  return status;
}
