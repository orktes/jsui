// Helpers for render tests: build a node tree from a JSON description,
// paint it, read pixels, optionally save PNGs (JSUI_TEST_OUT=<dir>).
//
//   {"type": "View", "style": {...}, "props": {...}, "text": "...", "children": [...]}
// A Text's "text" becomes a RawText child.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "core/document.h"
#include "core/value.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#include "include/encode/SkPngEncoder.h"
#include "jsui/jsui.h"
#include "render/painter.h"
#include "runtime.h"

namespace rtest {

using namespace jsui;

inline Node* build(Document& doc, const Value& spec) {
  NodeType type = NodeType::View;
  if (spec["type"].isString()) nodeTypeFromName(spec["type"].asString(), &type);
  Node* n = doc.createNode(type);
  if (type == NodeType::RawText) {
    n->setText(spec["text"].asString());
    return n;
  }
  Value::Object props = spec["props"].asObject();
  if (spec.has("style")) props["style"] = spec["style"];
  n->setProps(Value(props));
  if (spec["text"].isString()) {
    Node* raw = doc.createNode(NodeType::RawText);
    raw->setText(spec["text"].asString());
    n->appendChild(raw);
  }
  for (auto& c : spec["children"].asArray()) {
    if (c.isString()) {
      Node* raw = doc.createNode(NodeType::RawText);
      raw->setText(c.asString());
      n->appendChild(raw);
    } else {
      n->appendChild(build(doc, c));
    }
  }
  return n;
}

struct Scene {
  JsuiConfig cfg;
  std::unique_ptr<Runtime> rt;
  int w, h;
  float scale;
  std::vector<uint32_t> px;  // RGBA8888 premul, as uint32 little endian (R in low byte)

  Scene(int width, int height, float dpr = 1) : w(width), h(height), scale(dpr) {
    jsui_config_init(&cfg);
    cfg.width = width;
    cfg.height = height;
    cfg.pixel_ratio = dpr;
    rt = std::make_unique<Runtime>(cfg, nullptr);
  }
  Document& doc() { return rt->doc; }

  Node* add(const std::string& json) {
    std::string err;
    Value v = Value::parseJson(json, &err);
    if (!err.empty()) std::fprintf(stderr, "bad test json: %s\n", err.c_str());
    Node* n = build(doc(), v);
    doc().root()->appendChild(n);
    return n;
  }

  void render() {
    int dw = int(w * scale + 0.5f), dh = int(h * scale + 0.5f);
    px.assign(size_t(dw) * dh, 0);
    rt->tick(0);
    rt->render(px.data(), dw, dh, size_t(dw) * 4);
  }

  int dw() const { return int(w * scale + 0.5f); }
  int dh() const { return int(h * scale + 0.5f); }
  // Device-pixel accessors (unpremultiplied would be lossy; tests use premul).
  uint32_t at(int x, int y) const { return px[size_t(y) * dw() + x]; }
  int r(int x, int y) const { return int(at(x, y) & 255); }
  int g(int x, int y) const { return int((at(x, y) >> 8) & 255); }
  int b(int x, int y) const { return int((at(x, y) >> 16) & 255); }
  int a(int x, int y) const { return int(at(x, y) >> 24); }

  // Counts pixels in a rect whose luminance is below `thresh` with alpha > 128.
  int darkPixels(int x0, int y0, int x1, int y1, int thresh = 100) const {
    int n = 0;
    for (int y = y0; y < y1; y++)
      for (int x = x0; x < x1; x++)
        if (a(x, y) > 128 && (r(x, y) + g(x, y) + b(x, y)) / 3 < thresh) n++;
    return n;
  }

  void save(const char* name) const {
    const char* dir = std::getenv("JSUI_TEST_OUT");
    if (!dir) return;
    SkPixmap pm(SkImageInfo::Make(dw(), dh(), kRGBA_8888_SkColorType, kPremul_SkAlphaType), px.data(), size_t(dw()) * 4);
    SkFILEWStream f((std::string(dir) + "/" + name + ".png").c_str());
    SkPngEncoder::Encode(&f, pm, {});
  }
};

// Damage-only repaints must match full repaints. The x86 Skia build
// rasterizes an anti-aliased edge identically under the damage clip, and is
// checked to the bit. The macOS arm64 build blends some edge pixels of a
// node painted whole inside the damage region a level or two apart from the
// full frame (CI, M1: e.g. a rounded box's corners, 40-70 px per frame at
// most 2 levels), so there a frame passes within that: at most 4 levels on
// at most a quarter percent of the pixels. Stale or missing paint differs
// by far more.
#if defined(__x86_64__) || defined(_M_X64)
constexpr bool kExactUnderClip = true;
#else
constexpr bool kExactUnderClip = false;
#endif

struct PixelDiff {
  size_t count = 0;  // pixels that differ at all
  int max = 0;       // largest channel difference
};

inline PixelDiff pixelDiff(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
  PixelDiff d;
  for (size_t k = 0; k < a.size(); k++) {
    if (a[k] == b[k]) continue;
    d.count++;
    for (int ch = 0; ch < 4; ch++)
      d.max = std::max(d.max, std::abs(int((a[k] >> (ch * 8)) & 255) - int((b[k] >> (ch * 8)) & 255)));
  }
  return d;
}

inline bool matchesFull(const PixelDiff& d, size_t pixels) {
  if (kExactUnderClip) return d.count == 0;
  return d.max <= 4 && d.count * 400 <= pixels;
}

}  // namespace rtest
