// End-to-end hot updates: a real @orktes/jsui-react app (packages/cli/test/
// e2e/hmr) served by `jsui dev`, loaded in dev mode over http. The test
// copies steps/<n>/* (written by e2e.test.js) over the app and checks what
// renders: component edits keep state, a data module edit propagates, build
// and render errors reach jsui_last_error and clear on the fix, and an entry
// edit reloads the bundle. Run by the @orktes/jsui-cli test suite with
// JSUI_REACT_HMR_URL=<bundle url> JSUI_REACT_HMR_DIR=<app dir>; skipped
// without them.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "jsui/jsui.h"
#include "test.h"

namespace {

struct HmrE2E {
  std::vector<std::pair<std::string, std::string>> messages;
  std::vector<std::string> logs;
  JsuiRuntime* rt = nullptr;
  double now = 0;

  explicit HmrE2E(const char* url) {
    JsuiConfig cfg;
    jsui_config_init(&cfg);
    cfg.width = 320;
    cfg.height = 120;
    cfg.dev_mode = 1;
    cfg.on_message = [](void* u, const char* name, const char* json) {
      static_cast<HmrE2E*>(u)->messages.emplace_back(name, json ? json : "");
    };
    cfg.message_user = this;
    cfg.log = [](void* u, JsuiLogLevel, const char* msg) { static_cast<HmrE2E*>(u)->logs.push_back(msg); };
    cfg.log_user = this;
    rt = jsui_create(&cfg);
    jsui_load_bundle_url(rt, url);
  }
  ~HmrE2E() { jsui_destroy(rt); }

  // Ticks in real time (the dev server rebuilds and the runtime polls every
  // 500 ms) until pred() or the budget runs out.
  bool until(const std::function<bool()>& pred, int budgetMs = 15000) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < end) {
      now = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      jsui_tick(rt, now);
      if (pred()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    return pred();
  }
  std::string last(const std::string& name) const {
    for (auto it = messages.rbegin(); it != messages.rend(); ++it)
      if (it->first == name) return it->second;
    return "";
  }
  int count(const std::string& name) const {
    int n = 0;
    for (auto& m : messages) n += m.first == name;
    return n;
  }
  std::string error() const {
    const char* e = jsui_last_error(rt);
    return e ? e : "";
  }
  bool renders(const std::string& text) {
    bool ok = until([&] { return last("render") == "\"" + text + "\""; });
    if (!ok) {
      std::fprintf(stderr, "  expected render %s, last %s, error: %s\n", text.c_str(), last("render").c_str(),
                   error().c_str());
      for (auto& l : logs) std::fprintf(stderr, "    log: %s\n", l.c_str());
    }
    return ok;
  }
  void bump() { jsui_post_message(rt, "bump", "null"); }
};

void applyStep(const std::string& dir, int n) {
  namespace fs = std::filesystem;
  fs::path step = fs::path(dir) / "steps" / std::to_string(n);
  // Distinct mtimes for the watcher: let the previous rebuild settle.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  for (auto& e : fs::directory_iterator(step))
    fs::copy_file(e.path(), fs::path(dir) / e.path().filename(), fs::copy_options::overwrite_existing);
}

bool has(const std::string& s, const std::string& needle) { return s.find(needle) != std::string::npos; }

}  // namespace

TEST(react_hmr_e2e) {
  const char* url = std::getenv("JSUI_REACT_HMR_URL");
  const char* dir = std::getenv("JSUI_REACT_HMR_DIR");
  if (!url || !dir) {
    std::printf("  (skipped: set JSUI_REACT_HMR_URL and JSUI_REACT_HMR_DIR, see packages/cli/test)\n");
    return;
  }
  HmrE2E t(url);
  CHECK(t.renders("v1 a 0"));
  for (int i = 0; i < 3; i++) t.bump();
  CHECK(t.renders("v1 a 3"));

  // A component edit re-renders in place: state kept, no reload.
  applyStep(dir, 1);
  CHECK(t.renders("v2 a 3"));
  CHECK_EQ(t.count("boot"), 1);

  // A data module: its importer (a component file) re-runs.
  applyStep(dir, 2);
  CHECK(t.renders("v2 b 3"));

  // A syntax error shows on the overlay; the fix clears it and applies.
  applyStep(dir, 3);
  CHECK(t.until([&] { return has(t.error(), "Build failed") && has(t.error(), "App.jsx"); }));
  applyStep(dir, 4);
  CHECK(t.renders("v3 b 3"));
  CHECK(t.error().empty());

  // A render error unmounts the root; the fix mounts it again (state is
  // lost with the unmounted tree, host messages still arrive).
  applyStep(dir, 5);
  CHECK(t.until([&] { return has(t.error(), "render boom"); }));
  applyStep(dir, 6);
  CHECK(t.renders("v5 b 0"));
  t.bump();
  CHECK(t.renders("v5 b 1"));
  CHECK_EQ(t.count("boot"), 1);

  // The entry module: nothing above it can take the update, so it reloads.
  applyStep(dir, 7);
  CHECK(t.until([&] { return t.count("boot") == 2; }));
  CHECK(t.renders("v5 b 0"));
}
