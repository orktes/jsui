#include <cstring>
#include <vector>

#include "core/document.h"
#include "core/value.h"
#include "jsui/jsui.h"
#include "platform/loader.h"
#include "test.h"

using namespace jsui;

TEST(value_json_roundtrip) {
  std::string err;
  Value v = Value::parseJson(R"({"a":[1,2,{"b":"xé"}],"c":true,"d":null})", &err);
  CHECK(err.empty());
  CHECK_EQ(v["a"].asArray().size(), size_t(3));
  CHECK_EQ(v["a"].asArray()[2]["b"].asString(), std::string("x\xc3\xa9"));
  CHECK_EQ(Value::parseJson(v.toJson()).toJson(), v.toJson());
}

TEST(resolve_urls) {
  CHECK_EQ(ResourceLoader::resolve("http://localhost:5173/app/bundle.js", "img/a.png"),
           std::string("http://localhost:5173/app/img/a.png"));
  CHECK_EQ(ResourceLoader::resolve("http://localhost:5173/app/bundle.js", "/b.png"),
           std::string("http://localhost:5173/b.png"));
  CHECK_EQ(ResourceLoader::resolve("/x/y/bundle.js", "../a.ttf"), std::string("/x/a.ttf"));
  CHECK_EQ(ResourceLoader::resolve("bundle.js", "a.png"), std::string("a.png"));
  CHECK_EQ(ResourceLoader::resolve("C:\\ui\\bundle.js", "a.png"), std::string("C:\\ui\\a.png"));
  CHECK_EQ(ResourceLoader::resolve("http://localhost:5173/", "C:\\ui\\a.png"), std::string("C:\\ui\\a.png"));
}

TEST(layout_basic) {
  Document doc;
  doc.setViewport(200, 100, 1);
  Node* a = doc.createNode(NodeType::View);
  a->setProps(Value::parseJson(R"({"style":{"width":50,"height":20,"margin":10}})"));
  doc.root()->appendChild(a);
  doc.layout();
  Rect r = a->absoluteRect();
  CHECK_NEAR(r.x, 10, 0.01);
  CHECK_NEAR(r.width, 50, 0.01);
}

TEST(capi_renders_bundle) {
  JsuiConfig cfg;
  jsui_config_init(&cfg);
  cfg.width = 4;
  cfg.height = 4;
  JsuiRuntime* rt = jsui_create(&cfg);
  const char* src =
      "const n = __jsui_native; const v = n.createNode('View');"
      "n.setProps(v, {style: {flex: 1, backgroundColor: '#ff0000'}});"
      "n.appendChild(n.rootId, v);";
  CHECK_EQ(jsui_load_bundle_source(rt, src, std::strlen(src), "smoke.js"), 0);
  jsui_tick(rt, 0);
  std::vector<uint8_t> px(4 * 4 * 4);
  jsui_render_rgba(rt, px.data(), 4, 4, 16);
  CHECK_EQ(int(px[0]), 255);
  CHECK_EQ(int(px[1]), 0);
  CHECK_EQ(int(px[3]), 255);
  jsui_destroy(rt);
}
