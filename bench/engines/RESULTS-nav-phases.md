# Showcase navigation by phase: Graphite vs CPU (jsui-nav, c50e923)

`tools/jsui-bench/nav.cpp` from `claude/project-thread-r58hpd` c50e923 (main 0a19312 with #100's QuickJS patches,
plus the extended nav tool), Release, `JSUI_BUILD_TOOLS=ON`, Godot off. The default prebuilt Skia (aseprite m124)
has no Graphite Metal (`JSUI_GRAPHITE_METAL: OFF`, so `--backend graphite` can't run). So the main build uses the
locally built Skia m157 (`scripts/build-skia-desktop.sh` output, `JSUI_SKIA_DIR=~/Development/jsui-bench/third_party/skia-desktop`,
`skia_enable_graphite = true`). The `cpu-pr2-m124` rows are the same source with the default m124 prebuilt, for reference.

Bundle: `node packages/cli/bin/jsui.js build site/app/index.jsx -o navp/app/bundle.js --minify --format source`
(the same command as the 45b7f07 runs). 3 rounds × 8 routes × 200 frames = 4,800 frames, 1280x800 points.
Mac Studio M1 Ultra, macOS 26.5.2. Runs were taken under `~/Development/.bench-lock` at load 5.1 to 7.7, 2026-10-25,
in order: graphite+wait pr2, graphite pr2, cpu pr2, graphite+wait pr1, cpu pr2 (m124), then the whole set again.
Raw outputs are in `nav-phases/`.

## Summary (ms per frame)

| Config | run | p50 | p95 | p99 | max | > 16.7 ms | js | paint | submit | gpu wait | peak RSS (MiB) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| graphite, --gpu-wait, pr 2 (2560x1600) | 1 | 0.96 | 3.96 | 13.66 | 139.22 | 38 | 0.135 | 0.336 | 0.759 | 0.669 | 373 |
| graphite, --gpu-wait, pr 2 (2560x1600) | 2 | 0.96 | 3.47 | 9.03 | 25.60 | 3 | 0.134 | 0.334 | 0.080 | 0.670 | 371 |
| graphite, no wait, pr 2 | 1 | 0.21 | 1.74 | 7.75 | 23.57 | 2 | 0.130 | 0.286 | 0.065 | 0.000 | 395 |
| graphite, no wait, pr 2 | 2 | 0.21 | 1.83 | 8.04 | 23.11 | 1 | 0.131 | 0.290 | 0.066 | 0.000 | 398 |
| cpu, pr 2 | 1 | 25.53 | 54.68 | 70.83 | 86.79 | 2916 | 0.146 | 23.237 | 0.000 | 0.000 | 435 |
| cpu, pr 2 | 2 | 25.53 | 54.42 | 70.94 | 87.56 | 2918 | 0.148 | 23.243 | 0.000 | 0.000 | 376 |
| graphite, --gpu-wait, pr 1 (1280x800) | 1 | 0.79 | 2.53 | 7.97 | 17.16 | 1 | 0.133 | 0.250 | 0.074 | 0.505 | 299 |
| graphite, --gpu-wait, pr 1 (1280x800) | 2 | 0.79 | 2.56 | 7.82 | 16.33 | 0 | 0.134 | 0.248 | 0.073 | 0.491 | 302 |
| cpu, pr 2, Skia m124 prebuilt | 1 | 24.09 | 62.22 | 80.65 | 96.90 | 2920 | 0.145 | 23.908 | 0.000 | 0.000 | 355 |
| cpu, pr 2, Skia m124 prebuilt | 2 | 24.01 | 61.98 | 80.56 | 95.91 | 2917 | 0.144 | 23.859 | 0.000 | 0.000 | 355 |

- **Graphite turns the paint-bound showcase into a sub-millisecond one.** At pixel ratio 2, the CPU raster paint
  averages 23.2 ms per frame (p50 frame 25.5 ms, 61% of frames over 16.7 ms). Graphite records in 0.29 to 0.34 ms
  plus 0.07 to 0.08 ms of submit, and the GPU takes about 0.67 ms more (`--gpu-wait`). The p50 frame is 0.96 ms with the
  wait and 0.21 ms without. JS is 0.13 to 0.15 ms per frame on average in every config.
- **The first graphite run had 38 frames of 80 to 138 ms, all in `submit`, each with one pipeline compile.** Each run
  compiles the same 38 pipelines (36 at pr 1). After the first run on this machine they come from the OS's Metal
  shader cache, so run 2 is 0.08 ms of submit and has 3 frames over 16.7 ms. A cold start (first launch after install,
  or a new driver) pays about 100 ms per new pipeline, mostly on the first `/screens` and `/interactions` visits.
  Precompiling or warming the pipelines (Graphite has a precompile API) would avoid it.
- The remaining frames over 16.7 ms on Graphite are route transitions: the first `/screens` frame (5.7 ms JS, 2.1 ms
  layout, 13.5 ms paint: 66 paragraph builds, 121 text measures), `/playground` and `/input` frames with about 12 to 15 ms of
  paint, and the occasional GC frame (`/interactions+2/+3`, 7 to 9 ms of gc). GC totals 26 to 31 ms per run, at most 9.4 ms in one frame.
- Skia m157 vs m124 on the CPU backend: the same p50 (25.5 vs 24.0 ms, paint 23.2 vs 23.9 ms mean), but m157 has
  a shorter tail (p95 54.5 vs 62.1, max 87 vs 96 ms).
- Peak RSS: graphite 371 to 398 MiB at pr 2 and 299 to 302 at pr 1, cpu 376 to 435 (m157) and 355 (m124).

## Profile: graphite, --gpu-wait, pr 2

`sample` at 1 ms, 120 s, on `jsui-nav navp/app/bundle.js 2 --backend graphite --gpu-wait --rounds 12 --worst 5` (19,200
frames, p50 0.99 / p95 3.95 / p99 9.16 / max 25.9 ms, 28 frames over 16.7 ms). Its load was 12.6 when it started,
so the timings are less clean than the table above. The sample counts are what matter. Main thread: 19,147 samples,
9,483 (49.5%) of them waiting for the GPU (`__psynch_cvwait` in the `--gpu-wait` wait), 9,664 busy.
Full output: `nav-phases/sample-12r.txt.gz`. Summary from `nav-phases/profile-summary.txt`:

Inclusive buckets (outermost frame matching, % of all main-thread samples; they overlap, e.g. text inside paint):

| Bucket | samples | % of all | % of busy |
|---|---|---|---|
| QuickJS (JS_CallInternal) | 2435 | 12.7% | 25.2% |
| jsui tick (Runtime::tick) | 3385 | 17.7% | 35.0% |
| jsui paint (jsui_render_gpu_target / painter) | 6229 | 32.5% | 64.5% |
| Graphite submit/flush (Recorder snap, insertRecording, submit) | 357 | 1.9% | 3.7% |
| Skia/Graphite (any skgpu::graphite) | 4112 | 21.5% | 42.5% |
| Text: SkParagraph / shaping | 2240 | 11.7% | 23.2% |
| Text: glyph rasterization (SkScalerContext / strikes) | 1693 | 8.8% | 17.5% |
| Yoga | 279 | 1.5% | 2.9% |
| GC (gc_collect / JS_RunGC) | 122 | 0.6% | 1.3% |
| GPU wait (waitUntilCompleted / checkForFinishedWork) | 9554 | 49.9% | 98.9% |
| Metal driver (AGX / IOGPU) | 891 | 4.7% | 9.2% |
| mask blur (SkMaskBlurFilter) | 352 | 1.8% | 3.6% |

Top symbols by self time (main thread):

```
  9483  49.5%  __psynch_cvwait  (in libsystem_kernel.dylib)
  1326   6.9%  JS_CallInternal  (in jsui-nav)
   328   1.7%  _platform_memmove  (in libsystem_platform.dylib)
   293   1.5%  SkMaskBlurFilter::blur(SkMask const&, SkMaskBuilder*) const  (in jsui-nav)
   222   1.2%  <deduplicated_symbol>  (in jsui-nav)
   204   1.1%  neon::blit_row_color32(unsigned int*, int, unsigned int)  (in jsui-nav)
   184   1.0%  neon::blit_mask_d32_a8(unsigned int*, unsigned long, unsigned char const*, unsigned long, unsigned int, int, int)  (in jsui-nav)
   131   0.7%  sktext::gpu::TextBlob::Key::operator==(sktext::gpu::TextBlob::Key const&) const  (in jsui-nav)
   125   0.7%  _xzm_free  (in libsystem_malloc.dylib)
    93   0.5%  jsui::(anonymous namespace)::paintContent(jsui::Node*, SkCanvas*, jsui::(anonymous namespace)::Ctx&, SkPoint, float, float)  (in jsui-nav)
    93   0.5%  _platform_memcmp  (in libsystem_platform.dylib)
    92   0.5%  SkScalerContext_Mac::generateImage(SkGlyph const&, void*)  (in jsui-nav)
    88   0.5%  _platform_memset  (in libsystem_platform.dylib)
    75   0.4%  (anonymous namespace)::can_use_direct(SkMatrix const&, SkMatrix const&)  (in jsui-nav)
    74   0.4%  js_strict_eq2  (in jsui-nav)
    71   0.4%  jsui::(anonymous namespace)::updateRecords(jsui::Node*, SkMatrix const&, SkMatrix const&, SkIRect const&, unsigned int, SkRegion*, std::vect
    70   0.4%  mach_continuous_time  (in libsystem_kernel.dylib)
    65   0.3%  mach_msg2_trap  (in libsystem_kernel.dylib)
    65   0.3%  __bzero  (in libsystem_platform.dylib)
    64   0.3%  gc_collect  (in jsui-nav)
    61   0.3%  skgpu::RectanizerSkyline::addRect(int, int, SkIPoint16*)  (in jsui-nav)
    61   0.3%  AGX::RenderContext<AGX::G13::Encoders, AGX::G13::Classes, AGX::G13::ObjClasses, AGX::G13::CommandEncoding>::encodeAndEmitRenderState(AGX::G1
    60   0.3%  jsui::NativeAnimated::flush()  (in jsui-nav)
    57   0.3%  _xzm_xzone_malloc_tiny  (in libsystem_malloc.dylib)
    55   0.3%  neon::rect_memset32(unsigned int*, unsigned int, int, unsigned long, int)  (in jsui-nav)
    53   0.3%  CGSColorMaskCopyARGB8888  (in CoreGraphics)
```

Reading it: with the GPU wait taken out, paint (jsui painter + Skia recording) is about 64% of the busy main thread
and the tick about 35%. Inside the tick, QuickJS is 25% of busy. Text dominates paint: SkParagraph layout and shaping is
23% of busy, and glyph rasterization (`SkScalerContext_Mac::generateImage`, strikes, atlas packing via `RectanizerSkyline`)
is 18%. Glyphs are still rasterized on the CPU for the Graphite atlas. `SkMaskBlurFilter::blur` plus the
`neon::blit_*` CPU blitters (about 8% of busy: 3.6% blur inclusive plus 4.6% blitter self time) show that some blurs and masks are still drawn on the CPU under Graphite.
Yoga is 3% and GC 1.3%. The Metal driver (AGX/IOGPU encoding) is about 9%.

## Reproduce

```
git worktree add ab-nav c50e923
cmake -S ab-nav -B ab-nav/build-m157 -G Ninja -DCMAKE_BUILD_TYPE=Release -DJSUI_BUILD_GODOT=OFF -DJSUI_BUILD_TOOLS=ON \
  -DJSUI_SKIA_DIR=<skia-desktop m157>   # omit for the m124 prebuilt (cpu only)
cmake --build ab-nav/build-m157 --target jsui-nav
jsui-nav navp/app/bundle.js 2 --backend graphite --gpu-wait --worst 20   # and the other configs above
```
