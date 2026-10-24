// Precompiled bundles (src/js/bytecode.h): jsui_compile_bundle output loads
// like source, a bundle from another QuickJS falls back to its source, and
// broken containers fail cleanly.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "js/bytecode.h"
#include "jsui/jsui.h"
#include "test.h"

namespace {

// Paints the viewport in `color` (a CSS color) once evaluated.
std::string paintBundle(const char* color) {
  return std::string(
             "const n = __jsui_native; const v = n.createNode('View');"
             "function paint(c) { n.setProps(v, {style: {flex: 1, backgroundColor: c}}); }"
             "paint('") +
         color + "'); n.appendChild(n.rootId, v);";
}

std::string compile(const std::string& src, int keepSource = 1, const char* sourceUrl = nullptr) {
  void* data = nullptr;
  size_t size = 0;
  char* err = nullptr;
  int r = jsui_compile_bundle(src.data(), src.size(), "bc.js", keepSource, sourceUrl, &data, &size, &err);
  CHECK_EQ(r, 0);
  CHECK(err == nullptr);
  std::string out(static_cast<const char*>(data), size);
  jsui_free(data);
  return out;
}

struct Rt {
  JsuiRuntime* rt;
  std::vector<std::string> warnings;
  Rt() {
    JsuiConfig cfg;
    jsui_config_init(&cfg);
    cfg.width = 4;
    cfg.height = 4;
    cfg.log_user = this;
    cfg.log = [](void* user, JsuiLogLevel level, const char* msg) {
      if (level == JSUI_LOG_WARN) static_cast<Rt*>(user)->warnings.push_back(msg);
    };
    rt = jsui_create(&cfg);
  }
  ~Rt() { jsui_destroy(rt); }
  int load(const std::string& bundle, const char* name = "bc.js") {
    return jsui_load_bundle_source(rt, bundle.data(), bundle.size(), name);
  }
  // RGBA of the top-left pixel.
  std::vector<int> pixel() {
    jsui_tick(rt, 0);
    std::vector<uint8_t> px(4 * 4 * 4);
    jsui_render_rgba(rt, px.data(), 4, 4, 16);
    return {px[0], px[1], px[2], px[3]};
  }
};

// The container with its key replaced (same length keeps the layout simple).
std::string withForeignKey(std::string bundle) {
  uint32_t keyLen = 0;
  std::memcpy(&keyLen, bundle.data() + 8, 4);  // little-endian hosts
  for (uint32_t i = 0; i < keyLen; i++) bundle[12 + i] = 'x';
  return bundle;
}

}  // namespace

TEST(bytecode_bundle_runs) {
  std::string src = paintBundle("#ff0000");
  std::string bc = compile(src);
  CHECK(jsui::isBundleContainer(bc));
  jsui::BundleParts parts;
  CHECK(jsui::parseBundleContainer(bc, parts));
  CHECK_EQ(std::string(parts.key), std::string(jsui_bytecode_key()));
  CHECK_EQ(std::string(parts.source), src);
  CHECK(!parts.bytecode.empty());
  Rt t;
  CHECK_EQ(t.load(bc), 0);
  CHECK_EQ(t.pixel()[0], 255);
  CHECK(t.warnings.empty());
}

TEST(bytecode_bundle_without_source_runs) {
  std::string bc = compile(paintBundle("#00ff00"), 0);
  jsui::BundleParts parts;
  CHECK(jsui::parseBundleContainer(bc, parts));
  CHECK(parts.source.empty());
  Rt t;
  CHECK_EQ(t.load(bc), 0);
  CHECK_EQ(t.pixel()[1], 255);
}

TEST(bytecode_keeps_stack_trace_lines) {
  std::string bc = compile("var a = 1;\nvar b = 2;\nthrow new Error('boom');\n");
  Rt t;
  CHECK(t.load(bc) != 0);
  std::string err = jsui_last_error(t.rt) ? jsui_last_error(t.rt) : "";
  CHECK(err.find("boom") != std::string::npos);
  CHECK(err.find("bc.js:3") != std::string::npos);
}

TEST(bytecode_from_other_quickjs_runs_source) {
  std::string bc = withForeignKey(compile(paintBundle("#0000ff")));
  Rt t;
  CHECK_EQ(t.load(bc), 0);
  CHECK_EQ(t.pixel()[2], 255);
  CHECK_EQ(t.warnings.size(), size_t(1));
  CHECK(t.warnings[0].find("another JSUI runtime") != std::string::npos);
}

TEST(bytecode_from_other_quickjs_without_source_fails) {
  std::string bc = withForeignKey(compile(paintBundle("#0000ff"), 0));
  Rt t;
  CHECK(t.load(bc) != 0);
  std::string err = jsui_last_error(t.rt) ? jsui_last_error(t.rt) : "";
  CHECK(err.find("another JSUI runtime") != std::string::npos);
}

TEST(bytecode_corrupt_falls_back_or_fails) {
  std::string bc = compile(paintBundle("#ff0000"));
  jsui::BundleParts parts;
  CHECK(jsui::parseBundleContainer(bc, parts));
  // Flip a byte inside the bytecode: its checksum catches it, the source runs.
  size_t at = size_t(parts.bytecode.data() - bc.data()) + parts.bytecode.size() / 2;
  std::string bad = bc;
  bad[at] = char(bad[at] ^ 0x5a);
  {
    Rt t;
    CHECK_EQ(t.load(bad), 0);
    CHECK_EQ(t.pixel()[0], 255);
    CHECK_EQ(t.warnings.size(), size_t(1));
  }
  // Truncated inside the header.
  Rt t;
  CHECK(t.load(bc.substr(0, 14)) != 0);
}

TEST(bytecode_compile_reports_syntax_errors) {
  const char* src = "let x = ;";
  void* data = reinterpret_cast<void*>(1);
  size_t size = 1;
  char* err = nullptr;
  CHECK_EQ(jsui_compile_bundle(src, std::strlen(src), "broken.js", 1, nullptr, &data, &size, &err), -1);
  CHECK(data == nullptr);
  CHECK(err != nullptr);
  CHECK(std::string(err).find("SyntaxError") != std::string::npos);
  CHECK(std::string(err).find("broken.js") != std::string::npos);
  jsui_free(err);
}

TEST(bytecode_split_source_file) {
  namespace fs = std::filesystem;
  fs::path dir = fs::temp_directory_path() / "jsui-bytecode-split";
  fs::create_directories(dir);
  std::string src = paintBundle("#0000ff");
  std::ofstream(dir / "bundle.source.js", std::ios::binary) << src;
  std::string bc = compile(src, 0, "bundle.source.js");
  jsui::BundleParts parts;
  CHECK(jsui::parseBundleContainer(bc, parts));
  CHECK_EQ(std::string(parts.sourceUrl), std::string("bundle.source.js"));
  CHECK(parts.source.empty());
  std::string name = (dir / "bundle.js").string();
  {
    // Same runtime: the bytecode runs, the source file is never read.
    Rt t;
    CHECK_EQ(t.load(bc, name.c_str()), 0);
    CHECK_EQ(t.pixel()[2], 255);
    CHECK(t.warnings.empty());
  }
  // Another runtime's bytecode: the separate source file runs.
  Rt t;
  CHECK_EQ(t.load(withForeignKey(bc), name.c_str()), 0);
  std::vector<int> px;
  for (int i = 0; i < 3000; i++) {  // the loader reads the file asynchronously
    px = t.pixel();
    if (px[2] == 255) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK_EQ(px[2], 255);
  CHECK_EQ(t.warnings.size(), size_t(1));
  CHECK(t.warnings[0].find("bundle.source.js") != std::string::npos);
  fs::remove_all(dir);
}
