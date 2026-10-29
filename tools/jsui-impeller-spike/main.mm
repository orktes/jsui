// jsui-impeller-spike: the same JSUI frames through Skia Graphite and through
// Flutter's Impeller (its standalone SDK), on Metal, in a window.
//
//   jsui-impeller-spike --backend graphite|impeller [--frames N] [--settle-ms MS]
//                       [--width W] [--height H] [--scale S] [--hover] [--vsync]
//                       [--cache-dir DIR] [--no-cache-file] [--no-precompile]
//                       [--json out.json] [--png out.png] bundle.js [bundle.js ...]
//
// Each bundle is one "screen": a fresh runtime that loads it and runs
// --frames frames of simulated 60 Hz time from t = 0 (its entrance
// animations included), rendering every frame. The GPU context is created
// once, before the first screen, and kept, so later screens are the
// "navigate to a screen this app has not drawn yet" case.
//
// Both backends draw what JSUI records for a GPU frame (Surface::recordFrame,
// the JS-thread path native hosts use by default), a full repaint every frame:
//   graphite  jsui's Graphite Metal context (the production path: compile
//             pool, pipeline cache file, jsui_gpu_prepare_target's common
//             precompile) replays it into the drawable's texture; our queue
//             presents.
//   impeller  the recording is replayed into ImpellerCanvas (an SkCanvas that
//             issues Impeller display list calls), the display list drawn
//             into the drawable with ImpellerSurfaceDrawDisplayList and
//             presented by Impeller.
// The window's CAMetalLayer has 3 drawables and, without --vsync, does not
// wait for the display: nextDrawable blocks only while the GPU still holds
// all three, so the present-to-present interval is the frame cost, CPU or
// GPU bound, the same way for both backends.
//
// Pipeline state (first-launch vs later launches): --cache-dir DIR puts
// JSUI's pipeline cache file there (a fresh directory: first launch; the same
// one again: later launch); --no-cache-file and --no-precompile turn off
// the cache file and the common-pipeline precompile (Graphite with neither
// is roughly Skia without warm-up). Impeller keeps no cache of its own; the
// OS Metal shader cache ($(getconf DARWIN_USER_CACHE_DIR)/com.apple.metal)
// is cleared by run.sh for cold runs, for both.
//
// Reported per screen and overall: present interval p50 / p95 / p99 / max,
// frames over 16.7 / 33 ms, stall time (sum of interval - 16.7 ms), CPU
// record and submit ms, context creation ms, memory (phys_footprint, peak),
// and for impeller what it could not translate.
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <mach/mach.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "include/core/SkData.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkStream.h"
#include "include/encode/SkPngEncoder.h"
#include "input/input.h"
#include "jsui/jsui.h"
#include "render/gpu.h"
#include "render/pipeline_cache.h"
#include "runtime.h"
#include "surface.h"

#if defined(JSUI_IMPELLER_SPIKE)
#include "impeller_canvas.h"
#endif

namespace {

using Clock = std::chrono::steady_clock;
double ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

struct Options {
  std::string backend = "graphite";
  int frames = 300;
  double settleMs = 0;
  int width = 1280, height = 720;
  float scale = 1;
  bool hover = false, vsync = false, noCacheFile = false, noPrecompile = false;
  std::string cacheDir, json, png;
  std::vector<std::string> bundles;
};

struct Frame {
  double interval = 0, record = 0, submit = 0;
};

struct Memory {
  double footprint = 0, peak = 0;  // MB
};

Memory memory() {
  task_vm_info_data_t info{};
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  Memory m;
  if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
    m.footprint = double(info.phys_footprint) / (1024 * 1024);
    m.peak = double(info.ledger_phys_footprint_peak) / (1024 * 1024);
  }
  return m;
}

double pct(std::vector<double> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  size_t i = size_t(std::ceil(p * double(v.size())));
  return v[std::min(i ? i - 1 : 0, v.size() - 1)];
}

struct Summary {
  double p50 = 0, p95 = 0, p99 = 0, max = 0, stall = 0, record = 0, submit = 0, recordP99 = 0, submitP99 = 0;
  int over16 = 0, over33 = 0, frames = 0;
};

Summary summarize(const std::vector<Frame>& fs) {
  Summary s;
  std::vector<double> iv, rec, sub;
  for (const Frame& f : fs) {
    iv.push_back(f.interval);
    rec.push_back(f.record);
    sub.push_back(f.submit);
    if (f.interval > 1000.0 / 60 + 0.5) s.over16++;
    if (f.interval > 1000.0 / 30) s.over33++;
    s.stall += std::max(0.0, f.interval - 1000.0 / 60);
    s.record += f.record;
    s.submit += f.submit;
  }
  s.frames = int(fs.size());
  if (!fs.empty()) {
    s.record /= fs.size();
    s.submit /= fs.size();
  }
  s.p50 = pct(iv, 0.5);
  s.p95 = pct(iv, 0.95);
  s.p99 = pct(iv, 0.99);
  s.max = iv.empty() ? 0 : *std::max_element(iv.begin(), iv.end());
  s.recordP99 = pct(rec, 0.99);
  s.submitP99 = pct(sub, 0.99);
  return s;
}

std::string summaryJson(const Summary& s) {
  char buf[512];
  std::snprintf(buf, sizeof buf,
                "{\"frames\":%d,\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f,\"max\":%.3f,\"over16\":%d,\"over33\":%d,"
                "\"stall_ms\":%.1f,\"record_ms\":%.3f,\"record_p99\":%.3f,\"submit_ms\":%.3f,\"submit_p99\":%.3f}",
                s.frames, s.p50, s.p95, s.p99, s.max, s.over16, s.over33, s.stall, s.record, s.recordP99, s.submit,
                s.submitP99);
  return buf;
}

void printSummary(const char* name, const Summary& s) {
  std::printf("%-28s p50 %6.2f  p95 %6.2f  p99 %6.2f  max %7.2f ms  >16.7 %3d  >33 %3d  stall %7.1f ms  "
              "record %5.2f  submit %5.2f (p99 %6.2f)\n",
              name, s.p50, s.p95, s.p99, s.max, s.over16, s.over33, s.stall, s.record, s.submit, s.submitP99);
}

void pumpEvents() {
  @autoreleasepool {
    for (NSEvent* ev; (ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                untilDate:nil
                                                   inMode:NSDefaultRunLoopMode
                                                  dequeue:YES]);) {
      [NSApp sendEvent:ev];
    }
  }
}

std::unique_ptr<jsui::Runtime> makeRuntime(const Options& o, const std::string& bundle) {
  JsuiConfig cfg;
  jsui_config_init(&cfg);
  cfg.audio_disabled = 1;
  cfg.width = int(o.width / o.scale);
  cfg.height = int(o.height / o.scale);
  cfg.pixel_ratio = o.scale;
  auto rt = std::make_unique<jsui::Runtime>(cfg, nullptr);
  rt->loadUrl(bundle);
  // Images and fonts the bundle loads at start, without advancing time.
  rt->tick(0);
  for (int round = 0; round < 5000 && rt->loader.pending() > 0; round++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    rt->tick(0);
  }
  for (double t = 0; t < o.settleMs; t += 1000.0 / 60) rt->tick(t);
  return rt;
}

bool writePng(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLTexture> tex, const std::string& path) {
  const NSUInteger w = tex.width, h = tex.height, rb = w * 4;
  id<MTLBuffer> buf = [device newBufferWithLength:rb * h options:MTLResourceStorageModeShared];
  id<MTLCommandBuffer> cb = [queue commandBuffer];
  id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
  [blit copyFromTexture:tex
               sourceSlice:0
               sourceLevel:0
              sourceOrigin:MTLOriginMake(0, 0, 0)
                sourceSize:MTLSizeMake(w, h, 1)
                  toBuffer:buf
         destinationOffset:0
    destinationBytesPerRow:rb
  destinationBytesPerImage:rb * h];
  [blit endEncoding];
  [cb commit];
  [cb waitUntilCompleted];
  SkPixmap pm(SkImageInfo::Make(int(w), int(h), kBGRA_8888_SkColorType, kPremul_SkAlphaType), buf.contents, rb);
  SkFILEWStream out(path.c_str());
  return out.isValid() && SkPngEncoder::Encode(&out, pm, {});
}

int usage() {
  std::fprintf(stderr,
               "usage: jsui-impeller-spike --backend graphite|impeller [--frames N] [--settle-ms MS] [--width W] "
               "[--height H] [--scale S] [--hover] [--vsync] [--cache-dir DIR] [--no-cache-file] "
               "[--no-precompile] [--json out.json] [--png out.png] bundle.js ...\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  @autoreleasepool {
    const auto t0 = Clock::now();
    Options o;
    for (int i = 1; i < argc; i++) {
      std::string a = argv[i];
      auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
      if (a == "--backend") o.backend = next();
      else if (a == "--frames") o.frames = std::atoi(next().c_str());
      else if (a == "--settle-ms") o.settleMs = std::atof(next().c_str());
      else if (a == "--width") o.width = std::atoi(next().c_str());
      else if (a == "--height") o.height = std::atoi(next().c_str());
      else if (a == "--scale") o.scale = float(std::atof(next().c_str()));
      else if (a == "--hover") o.hover = true;
      else if (a == "--vsync") o.vsync = true;
      else if (a == "--cache-dir") o.cacheDir = next();
      else if (a == "--no-cache-file") o.noCacheFile = true;
      else if (a == "--no-precompile") o.noPrecompile = true;
      else if (a == "--json") o.json = next();
      else if (a == "--png") o.png = next();
      else if (a.rfind("--", 0) == 0) return usage();
      else o.bundles.push_back(a);
    }
    if (o.bundles.empty() || (o.backend != "graphite" && o.backend != "impeller")) return usage();
#if !defined(JSUI_IMPELLER_SPIKE)
    if (o.backend == "impeller") {
      std::fprintf(stderr, "built without the Impeller SDK (-DJSUI_IMPELLER_SDK=<dir>)\n");
      return 1;
    }
#endif
    if (!o.cacheDir.empty()) jsui::pipeline_cache::setCacheDir(o.cacheDir);
    if (o.noCacheFile) jsui::pipeline_cache::setNoDefaultFile(true);
    if (o.noPrecompile) jsui::pipeline_cache::setNoCommonPrecompile(true);

    // Window + layer.
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp finishLaunching];
    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, o.width, o.height)
                                                   styleMask:NSWindowStyleMaskTitled
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    const CGFloat backing = window.backingScaleFactor;
    [window setContentSize:NSMakeSize(o.width / backing, o.height / backing)];
    window.title = [NSString stringWithFormat:@"jsui-impeller-spike (%s)", o.backend.c_str()];
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> queue = [device newCommandQueue];
    CAMetalLayer* layer = [CAMetalLayer layer];
    layer.device = device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = NO;
    layer.maximumDrawableCount = 3;
    layer.displaySyncEnabled = o.vsync;
    layer.contentsScale = backing;
    layer.drawableSize = CGSizeMake(o.width, o.height);
    window.contentView.wantsLayer = YES;
    window.contentView.layer = layer;
    [window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    pumpEvents();
    const Memory memStart = memory();

    // GPU context.
    const auto c0 = Clock::now();
    JsuiGpuContext* gctx = nullptr;
#if defined(JSUI_IMPELLER_SPIKE)
    ImpellerContext ictx = nullptr;
    std::unique_ptr<spike::ImpellerResources> res;
    spike::Stats istats;
#endif
    if (o.backend == "graphite") {
      gctx = jsui::graphite_gpu::createMetal((__bridge void*)device, (__bridge void*)queue);
      if (!gctx) {
        std::fprintf(stderr, "Graphite Metal context creation failed\n");
        return 1;
      }
      // What hosts do once the target format is known (common precompile).
      jsui::graphite_gpu::prepareTarget(gctx, unsigned(MTLPixelFormatBGRA8Unorm));
    } else {
#if defined(JSUI_IMPELLER_SPIKE)
      ictx = ImpellerContextCreateMetalNew(IMPELLER_VERSION);
      if (!ictx) {
        std::fprintf(stderr, "Impeller Metal context creation failed (library version %u, header %u)\n",
                     ImpellerGetVersion(), unsigned(IMPELLER_VERSION));
        return 1;
      }
      res = std::make_unique<spike::ImpellerResources>(ictx);
#endif
    }
    const double contextMs = ms(c0, Clock::now());
    const Memory memContext = memory();

    std::vector<Frame> all;
    std::vector<std::pair<std::string, Summary>> screens;
    double firstFrameMs = 0;  // process start -> first present
    id<MTLTexture> lastTex = nil;
    for (const std::string& bundle : o.bundles) {
      auto rt = makeRuntime(o, bundle);
      if (!rt->lastError.empty()) {
        std::fprintf(stderr, "%s: %s\n", bundle.c_str(), rt->lastError.c_str());
        return 1;
      }
      std::vector<Frame> fs;
      auto prev = Clock::now();
      double t = o.settleMs;
      for (int i = 0; i < o.frames; i++) {
        pumpEvents();
        t += 1000.0 / 60;
        if (o.hover) {
          float y = float(80 + std::fmod(i * 7.0, double(o.height / o.scale - 160)));
          float x = float(60 + 40 * std::sin(i * 0.05));
          rt->input->mouseMove(x, y);
        }
        rt->tick(t);
        Frame f;
        @autoreleasepool {
          id<CAMetalDrawable> drawable = [layer nextDrawable];
          if (!drawable) {
            std::fprintf(stderr, "no drawable\n");
            return 1;
          }
          lastTex = drawable.texture;
          const auto a = Clock::now();
          if (gctx) {
            JsuiGpuTarget target{};
            target.texture = (__bridge void*)drawable.texture;
            target.width = o.width;
            target.height = o.height;
            std::unique_ptr<jsui::Surface::RecordedFrame> rec;
            jsui::graphite_gpu::record(rt->main(), gctx, target, &rec);
            const auto b = Clock::now();
            JsuiRect dirty{};
            if (jsui::graphite_gpu::render(rt->main(), gctx, target, &dirty, rec.get()) != 0) {
              std::fprintf(stderr, "Graphite render failed\n");
              return 1;
            }
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            [cb presentDrawable:drawable];
            [cb commit];
            const auto c = Clock::now();
            f.record = ms(a, b);
            f.submit = ms(b, c);
          }
#if defined(JSUI_IMPELLER_SPIKE)
          else {
            jsui::Surface::CanvasChoice choice;
            choice.clear = true;
            auto rec = rt->main().recordFrame(nullptr, o.width, o.height, choice, /*growCurves=*/true, /*gpu=*/true);
            const auto b = Clock::now();
            ImpellerRect cull{0, 0, float(o.width), float(o.height)};
            ImpellerDisplayListBuilder builder = ImpellerDisplayListBuilderNew(&cull);
            {
              spike::ImpellerCanvas canvas(o.width, o.height, builder, *res);
              rt->main().replayFrame(*rec, &canvas);
              istats.add(canvas.stats());
            }
            ImpellerDisplayList dl = ImpellerDisplayListBuilderCreateDisplayListNew(builder);
            ImpellerDisplayListBuilderRelease(builder);
            ImpellerSurface surface = ImpellerSurfaceCreateWrappedMetalDrawableNew(ictx, (__bridge void*)drawable);
            if (!surface || !dl || !ImpellerSurfaceDrawDisplayList(surface, dl) || !ImpellerSurfacePresent(surface)) {
              std::fprintf(stderr, "Impeller draw failed\n");
              return 1;
            }
            ImpellerSurfaceRelease(surface);
            ImpellerDisplayListRelease(dl);
            res->endFrame();
            const auto c = Clock::now();
            f.record = ms(a, b);
            f.submit = ms(b, c);
          }
#endif
        }
        const auto now = Clock::now();
        f.interval = ms(prev, now);
        prev = now;
        if (all.empty() && fs.empty()) firstFrameMs = ms(t0, now);
        fs.push_back(f);
      }
      // The first interval of a screen includes loading it (not a frame).
      if (!fs.empty()) fs.front().interval = fs.size() > 1 ? fs[1].interval : 0;
      Summary s = summarize(fs);
      printSummary(bundle.substr(bundle.find_last_of('/') + 1).c_str(), s);
      screens.push_back({bundle, s});
      all.insert(all.end(), fs.begin(), fs.end());
    }
    const Summary total = summarize(all);
    printSummary("all", total);
    const Memory memEnd = memory();
    std::printf("context %.1f ms, first present %.1f ms after start; memory MB: start %.1f, after context %.1f, "
                "end %.1f, peak %.1f\n",
                contextMs, firstFrameMs, memStart.footprint, memContext.footprint, memEnd.footprint, memEnd.peak);
    std::string unsupported;
#if defined(JSUI_IMPELLER_SPIKE)
    if (ictx) {
      std::printf("impeller: %d draws (%d text runs, %d images, %d paths), %d layers, %d texture uploads, %d paragraph "
                  "builds\n",
                  istats.draws, istats.textRuns, istats.images, istats.paths, istats.layers, istats.textureUploads,
                  istats.paragraphBuilds);
      for (auto& [k, v] : istats.unsupported) {
        std::printf("  not translated: %-32s %d\n", k.c_str(), v);
        unsupported += (unsupported.empty() ? "" : ",") + ("\"" + k + "\":" + std::to_string(v));
      }
    }
#endif
    if (!o.png.empty() && lastTex) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));  // Impeller's queue is not ours
      if (!writePng(device, queue, lastTex, o.png)) std::fprintf(stderr, "could not write %s\n", o.png.c_str());
    }
    if (!o.json.empty()) {
      std::ostringstream js;
      js << "{\"backend\":\"" << o.backend << "\",\"width\":" << o.width << ",\"height\":" << o.height
         << ",\"no_cache_file\":" << (o.noCacheFile ? "true" : "false")
         << ",\"no_precompile\":" << (o.noPrecompile ? "true" : "false") << ",\"context_ms\":" << contextMs
         << ",\"first_present_ms\":" << firstFrameMs << ",\"mem_start_mb\":" << memStart.footprint
         << ",\"mem_context_mb\":" << memContext.footprint << ",\"mem_end_mb\":" << memEnd.footprint
         << ",\"mem_peak_mb\":" << memEnd.peak << ",\"all\":" << summaryJson(total) << ",\"screens\":[";
      for (size_t i = 0; i < screens.size(); i++) {
        js << (i ? "," : "") << "{\"bundle\":\"" << screens[i].first << "\",\"summary\":" << summaryJson(screens[i].second)
           << "}";
      }
      js << "],\"unsupported\":{" << unsupported << "},\"intervals\":[";
      for (size_t i = 0; i < all.size(); i++) js << (i ? "," : "") << all[i].interval;
      js << "]}\n";
      std::ofstream(o.json) << js.str();
    }
    if (gctx) jsui::graphite_gpu::destroy(gctx);
#if defined(JSUI_IMPELLER_SPIKE)
    res.reset();
    if (ictx) ImpellerContextRelease(ictx);
#endif
    return 0;
  }
}
