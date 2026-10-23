// jsui-render: run a bundle headlessly and write the frame as PNG.
//
//   jsui-render bundle.js -o out.png [--width 800] [--height 600] [--scale 1]
//               [--wait-ms 200] [--font Family=path.ttf ...]
//
// Time is simulated: the runtime is ticked in 16ms steps until --wait-ms has
// elapsed and there are no in-flight resource loads (images, fonts, fetch).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "jsui/jsui.h"

static bool readFile(const std::string& path, std::string* out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

int main(int argc, char** argv) {
  std::string bundle, outPath = "out.png";
  int width = 800, height = 600;
  float scale = 1;
  double waitMs = 200;
  std::vector<std::pair<std::string, std::string>> fonts;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", a.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "-o" || a == "--out") outPath = next();
    else if (a == "--width") width = std::atoi(next().c_str());
    else if (a == "--height") height = std::atoi(next().c_str());
    else if (a == "--scale") scale = float(std::atof(next().c_str()));
    else if (a == "--wait-ms") waitMs = std::atof(next().c_str());
    else if (a == "--font") {
      std::string v = next();
      auto eq = v.find('=');
      if (eq == std::string::npos) {
        std::fprintf(stderr, "--font expects Family=path\n");
        return 2;
      }
      fonts.emplace_back(v.substr(0, eq), v.substr(eq + 1));
    } else if (a == "-h" || a == "--help") {
      std::printf("usage: jsui-render <bundle.js|url> -o out.png [--width W] [--height H] [--scale S] "
                  "[--wait-ms MS] [--font Family=path]\n");
      return 0;
    } else bundle = a;
  }
  if (bundle.empty()) {
    std::fprintf(stderr, "no bundle given (see --help)\n");
    return 2;
  }

  JsuiConfig cfg;
  jsui_config_init(&cfg);
  cfg.width = width;
  cfg.height = height;
  cfg.pixel_ratio = scale;
  JsuiRuntime* rt = jsui_create(&cfg);

  for (auto& [family, path] : fonts) {
    std::string data;
    if (!readFile(path, &data) || jsui_register_font(rt, family.c_str(), data.data(), data.size(), 0, -1) != 0) {
      std::fprintf(stderr, "failed to load font %s\n", path.c_str());
      return 1;
    }
  }

  jsui_load_bundle_url(rt, bundle.c_str());
  // Loads complete on worker threads: wait (simulated time standing still)
  // until they have landed, bounded by 10 s of real time.
  double t = 0;
  auto settle = [&] {
    for (int round = 0; round < 1000 && jsui_pending_loads(rt) > 0; round++) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      jsui_tick(rt, t);
    }
  };
  jsui_tick(rt, t);
  settle();  // the bundle
  for (int i = 0; t <= waitMs || i < 2; i++, t += 16) {
    jsui_tick(rt, t);
    if (jsui_pending_loads(rt) > 0) settle();  // images, fonts, fetch()
  }
  for (int i = 0; i < 2; i++, t += 16) jsui_tick(rt, t);

  int status = 0;
  if (const char* err = jsui_last_error(rt)) {
    std::fprintf(stderr, "error: %s\n", err);
    status = 1;
  }
  void* png = nullptr;
  size_t size = 0;
  if (jsui_render_png(rt, &png, &size) != 0) {
    std::fprintf(stderr, "png encode failed\n");
    return 1;
  }
  FILE* f = std::fopen(outPath.c_str(), "wb");
  if (!f) {
    std::fprintf(stderr, "cannot write %s\n", outPath.c_str());
    return 1;
  }
  std::fwrite(png, 1, size, f);
  std::fclose(f);
  jsui_free(png);
  jsui_destroy(rt);
  return status;
}
