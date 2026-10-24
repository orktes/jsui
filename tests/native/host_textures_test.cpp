// Host textures (jsui_set_texture / jsui_set_texture_pixels /
// jsui_texture_updated / jsui_remove_texture): Image nodes with a `texture`
// prop (HostTexture in @orktes/jsui-react) sized by and showing the host's
// pixels on the CPU raster, new contents repainting only those views, the
// GPU kinds left empty (with one warning) where a render cannot sample them,
// and the JS bridge (__jsui_native.texture, __jsui_onHostTexture). GL
// textures sampled by the Ganesh backend: gpu_test.cpp; Vulkan:
// vulkan_test.cpp.
#include <cstring>
#include <string>
#include <vector>

#include "jsui/jsui.h"
#include "render/painter.h"
#include "render_util.h"
#include "test.h"

using rtest::Scene;

namespace {

// w x h RGBA pixels: the left half `left`, the right half `right` (RGBA
// bytes as uint32 little endian), optionally with the top row red.
std::vector<uint32_t> halves(int w, int h, uint32_t left, uint32_t right) {
  std::vector<uint32_t> px(size_t(w) * h);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) px[size_t(y) * w + x] = x < w / 2 ? left : right;
  return px;
}

constexpr uint32_t kRed = 0xff0000ff, kBlue = 0xffff0000, kGreen = 0xff00ff00;

}  // namespace

TEST(host_texture_pixels_size_and_paint) {
  Scene s(200, 100);
  jsui::Node* tex = s.add(R"J({"type":"Image","style":{"alignSelf":"flex-start"},"props":{"texture":"cam"}})J");
  // Before the host sets it: no size, nothing drawn.
  s.render();
  CHECK_NEAR(tex->layoutRect().width, 0, 0.01);
  std::vector<uint32_t> px = halves(40, 20, kRed, kBlue);
  CHECK_EQ(s.rt->textures.setPixels("cam", px.data(), 40, 20, 0, 0), 1);
  CHECK(s.rt->needsRedraw());
  s.render();
  s.save("host_texture");
  jsui::Rect r = tex->layoutRect();
  CHECK_NEAR(r.width, 40, 0.01);  // intrinsic size: the texture's px
  CHECK_NEAR(r.height, 20, 0.01);
  CHECK_EQ(s.at(5, 10), kRed);
  CHECK_EQ(s.at(35, 10), kBlue);
  CHECK_EQ(s.a(45, 10), 0);
  // A bigger texture re-lays the view out.
  std::vector<uint32_t> big = halves(80, 40, kGreen, kGreen);
  CHECK_EQ(s.rt->textures.setPixels("cam", big.data(), 80, 40, 0, 0), 1);
  s.render();
  CHECK_NEAR(tex->layoutRect().width, 80, 0.01);
  CHECK_EQ(s.at(70, 30), kGreen);
  // Removed: the view is empty and unsized again.
  CHECK(s.rt->textures.remove("cam"));
  s.render();
  CHECK_NEAR(tex->layoutRect().width, 0, 0.01);
  CHECK_EQ(s.a(10, 10), 0);
}

TEST(host_texture_resize_mode_flags_and_rows) {
  Scene s(100, 100);
  s.add(R"J({"type":"Image","props":{"texture":"v","resizeMode":"stretch"},
             "style":{"position":"absolute","left":0,"top":0,"width":100,"height":50}})J");
  s.add(R"J({"type":"Image","props":{"texture":"v"},
             "style":{"position":"absolute","left":0,"top":50,"width":50,"height":50,"borderRadius":25}})J");
  // 4x2 with the top row red, the bottom row blue, given bottom row first
  // with a padded stride and a bottom-left origin.
  std::vector<uint32_t> rows(6 * 2);
  for (int x = 0; x < 6; x++) {
    rows[x] = kBlue;       // row 0: the bottom of the picture
    rows[6 + x] = kRed;
  }
  CHECK_EQ(s.rt->textures.setPixels("v", rows.data(), 4, 2, 6 * 4, JSUI_TEXTURE_BOTTOM_LEFT_ORIGIN), 1);
  s.render();
  CHECK_EQ(s.at(50, 5), kRed);    // stretched: top half red
  CHECK_EQ(s.at(50, 45), kBlue);  // bottom half blue
  CHECK_EQ(s.a(1, 51), 0);        // round clip: the corner is empty
  CHECK_EQ(s.a(25, 75), 255);
  // Straight alpha is premultiplied when drawn.
  std::vector<uint32_t> half(4 * 2, 0x80ffffffu);
  CHECK_EQ(s.rt->textures.setPixels("v", half.data(), 4, 2, 0, JSUI_TEXTURE_UNPREMULTIPLIED), 1);
  s.render();
  CHECK_NEAR(double(s.r(50, 25)), 128, 2);
  CHECK_NEAR(double(s.a(50, 25)), 128, 2);
  // Bad sizes are rejected.
  CHECK_EQ(s.rt->textures.setPixels("v", half.data(), 0, 2, 0, 0), -1);
  CHECK_EQ(s.rt->textures.setPixels("v", half.data(), 4, 2, 8, 0), -1);  // row_bytes < width * 4
}

// New contents repaint just the views showing the texture: an incremental
// CPU frame's dirty rect is the view.
TEST(host_texture_update_repaints_only_its_views) {
  Scene s(200, 100);
  s.add(R"J({"style":{"position":"absolute","left":0,"top":0,"width":200,"height":100,"backgroundColor":"#202020"}})J");
  s.add(R"J({"type":"Image","props":{"texture":"cam"},
             "style":{"position":"absolute","left":120,"top":30,"width":40,"height":20}})J");
  std::vector<uint32_t> px = halves(40, 20, kRed, kRed);
  s.rt->textures.setPixels("cam", px.data(), 40, 20, 0, 0);
  int dw = s.dw(), dh = s.dh();
  std::vector<uint32_t> buf(size_t(dw) * dh, 0);
  s.rt->tick(0);
  s.rt->renderIncremental(buf.data(), dw, dh, size_t(dw) * 4);
  CHECK(!s.rt->needsRedraw());
  px = halves(40, 20, kGreen, kGreen);
  s.rt->textures.setPixels("cam", px.data(), 40, 20, 0, 0);
  CHECK(s.rt->needsRedraw());
  s.rt->tick(16);
  SkIRect dirty = s.rt->renderIncremental(buf.data(), dw, dh, size_t(dw) * 4);
  CHECK(dirty.contains(SkIRect::MakeXYWH(120, 30, 40, 20)));
  CHECK(dirty.width() <= 42 && dirty.height() <= 22);
  CHECK_EQ(buf[size_t(40) * dw + 140], kGreen);
  CHECK(jsui::lastPaintStats(s.doc()).nodesPainted <= 3);  // root, backdrop, the view
  // An update of an unrelated texture repaints nothing.
  s.rt->textures.setPixels("other", px.data(), 40, 20, 0, 0);
  CHECK(!s.rt->needsRedraw());
}

// A GPU texture on the CPU raster: the view keeps its size, draws nothing
// and the runtime warns once.
TEST(host_texture_gpu_kind_on_cpu_warns_once) {
  std::vector<std::string> warnings;
  JsuiConfig cfg;
  jsui_config_init(&cfg);
  cfg.width = 100;
  cfg.height = 60;
  cfg.log = [](void* u, JsuiLogLevel level, const char* msg) {
    if (level == JSUI_LOG_WARN) static_cast<std::vector<std::string>*>(u)->push_back(msg);
  };
  cfg.log_user = &warnings;
  JsuiRuntime* rt = jsui_create(&cfg);
  const char* src =
      "const n = __jsui_native; const v = n.createNode('Image');"
      "n.setProps(v, {texture: 'gpu', style: {position: 'absolute', left: 0, top: 0}});"
      "n.appendChild(n.rootId, v);";
  CHECK_EQ(jsui_load_bundle_source(rt, src, std::strlen(src), "tex.js"), 0);
  JsuiTexture t{};
  t.kind = JSUI_TEXTURE_VULKAN;
  t.handle = reinterpret_cast<void*>(uintptr_t(0x1234));
  t.width = 30;
  t.height = 20;
  CHECK_EQ(jsui_set_texture(rt, "gpu", &t), 1);
  CHECK_EQ(jsui_set_texture(rt, "gpu", &t), 0);  // same texture
  CHECK_EQ(jsui_texture_updated(rt, "gpu"), 1);
  CHECK_EQ(jsui_texture_updated(rt, "nope"), -1);
  JsuiTexture bad = t;
  bad.handle = nullptr;
  CHECK_EQ(jsui_set_texture(rt, "gpu", &bad), -1);
  bad = t;
  bad.kind = JSUI_TEXTURE_PIXELS;
  CHECK_EQ(jsui_set_texture(rt, "gpu", &bad), -1);
  std::vector<uint32_t> px(100 * 60, 0);
  for (int i = 0; i < 2; i++) {
    jsui_tick(rt, 16.0 * i);
    jsui_render_rgba(rt, px.data(), 100, 60, 400);
    jsui_texture_updated(rt, "gpu");
  }
  CHECK_EQ(px[size_t(10) * 100 + 10], 0u);
  CHECK_EQ(int(warnings.size()), 1);
  CHECK(!warnings.empty() && warnings[0].find("\"gpu\"") != std::string::npos);
  jsui::Node* node = jsui::internalRuntime(rt)->doc.root()->children()[0];
  CHECK_NEAR(node->layoutRect().width, 30, 0.01);
  CHECK_EQ(jsui_remove_texture(rt, "gpu"), 1);
  CHECK_EQ(jsui_remove_texture(rt, "gpu"), 0);
  CHECK_EQ(jsui_set_texture(rt, nullptr, &t), -1);
  jsui_destroy(rt);
}

// JS sees sizes (useHostTexture) and gets one delivery per tick for textures
// set, resized or removed, but none for new contents.
TEST(host_texture_js_bridge) {
  std::vector<std::string> messages;
  JsuiConfig cfg;
  jsui_config_init(&cfg);
  cfg.width = 100;
  cfg.height = 60;
  cfg.on_message = [](void* u, const char* name, const char* json) {
    static_cast<std::vector<std::string>*>(u)->push_back(std::string(name) + "=" + (json ? json : ""));
  };
  cfg.message_user = &messages;
  JsuiRuntime* rt = jsui_create(&cfg);
  const char* src = R"JS(
    const N = __jsui_native;
    globalThis.__jsui_onHostTexture = (keys) =>
      N.postMessage('changed', JSON.stringify(keys.map((k) => [k, N.texture(k) ? N.texture(k).slice(0, 2) : null])));
    globalThis.__jsui_onHostMessage = (name) => {
      if (name === 'keys') N.postMessage('keys', JSON.stringify(N.textureKeys()));
    };
  )JS";
  std::vector<uint32_t> px(4 * 4, kRed);
  jsui_set_texture_pixels(rt, "early", px.data(), 4, 4, 0, 0);  // before the bundle: no delivery
  CHECK_EQ(jsui_load_bundle_source(rt, src, std::strlen(src), "texjs.js"), 0);
  auto take = [&] {
    std::string out;
    for (auto& m : messages) out += (out.empty() ? "" : " ") + m;
    messages.clear();
    return out;
  };
  jsui_tick(rt, 0);
  CHECK_EQ(take(), std::string());
  jsui_set_texture_pixels(rt, "a", px.data(), 4, 4, 0, 0);
  CHECK(jsui_wants_frame(rt));
  jsui_tick(rt, 16);
  CHECK_EQ(take(), std::string("changed=[[\"a\",[4,4]]]"));
  jsui_set_texture_pixels(rt, "a", px.data(), 4, 4, 0, 0);  // new contents, same size
  jsui_texture_updated(rt, "a");
  jsui_tick(rt, 32);
  CHECK_EQ(take(), std::string());
  jsui_set_texture_pixels(rt, "a", px.data(), 2, 8, 0, 0);
  jsui_remove_texture(rt, "early");
  jsui_tick(rt, 48);
  CHECK_EQ(take(), std::string("changed=[[\"a\",[2,8]],[\"early\",null]]"));
  jsui_post_message(rt, "keys", "null");
  jsui_tick(rt, 64);
  CHECK_EQ(take(), std::string("keys=[\"a\"]"));
  jsui_destroy(rt);
}
