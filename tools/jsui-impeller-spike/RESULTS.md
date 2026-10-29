# Impeller spike: results (2026-10-29)

Machine: Mac Studio, Apple M1 Ultra (20 CPU / 48 GPU cores, 64 GB), macOS
26.5.2 (25F84), 3440x1440 display. Shared machine. Load average was about
2.5 before the run, and other work pushed it to about 20 during the
1440p runs at the end, so treat the 1440p rows as noisy.

Impeller: Flutter 3.47.9 stable, engine 89c39edfdfae2949f6806e4a315c0c08522238b7,
`impeller.h` 1.4.0 (darwin-arm64 `impeller_sdk.zip`). Skia: m157 desktop
build (Graphite Metal + Dawn + Ganesh GL). Release build.

`tools/jsui-impeller-spike/run.sh build --runs 3`, 240 frames per screen,
5 screens (main-menu, inventory, hud, settings, battle-pass), 1280x720 at 1x.
"first" setups start with the OS Metal shader cache
(`$DARWIN_USER_CACHE_DIR/com.apple.metal`) moved away, so they model a
user's first launch. Impeller always uses its offline-compiled shaders
(`impellerc`). Graphite gets nothing precompiled offline: `graphite-bare`
is plain Skia (no pipeline cache file, no startup precompile), and
`graphite-first` is JSUI as shipped on a first launch.

## Numbers (medians over 3 runs)

| setup (runs) | context ms | first present ms | max frame ms | p99 ms | p50 ms | frames >33 ms | stall ms | CPU record ms | CPU submit ms | peak MB | first 10 frames ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| graphite-bare (3) | 39.49 | 654 | 23.99 | 21.46 | 1.60 | 0.00 | 110 | 0.13 | 1.12 | 139 | 228 |
| graphite-first (3) | 39.19 | 595 | 30.71 | 21.09 | 1.70 | 0.00 | 77.40 | 0.14 | 0.78 | 141 | 176 |
| graphite-second (3) | 2.07 | 243 | 24.85 | 16.78 | 1.60 | 0.00 | 59.50 | 0.13 | 0.45 | 142 | 124 |
| impeller-first (3) | 54.98 | 334 | 22.89 | 17.05 | 1.86 | 0.00 | 59.40 | 0.12 | 0.93 | 131 | 158 |
| impeller-second (3) | 17.19 | 265 | 22.84 | 17.15 | 1.91 | 0.00 | 59.90 | 0.12 | 0.90 | 124 | 154 |
| graphite-1440p (1) | 37.39 | 569 | 24.88 | 22.09 | 1.74 | 0.00 | 279 | 0.14 | 0.58 | 253 | 221 |
| impeller-1440p (1) | 18.25 | 312 | 23.67 | 21.03 | 2.03 | 0.00 | 199 | 0.14 | 0.97 | 194 | 222 |

"first 10 frames ms" is the sum of the first 10 present-to-present
intervals of each of the 5 screens: the part of each screen where its
first-use pipeline work lands. Per screen (median of runs):

| setup | main-menu | inventory | hud | settings | battle-pass | sum |
|---|---:|---:|---:|---:|---:|---:|
| graphite-bare | 77.4 | 42.1 | 40.8 | 25.8 | 43.9 | 230 |
| graphite-first | 67.3 | 41.0 | 22.7 | 20.9 | 24.6 | 177 |
| graphite-second | 33.9 | 21.7 | 22.5 | 18.1 | 26.4 | 123 |
| impeller-first | 48.6 | 26.8 | 29.7 | 23.2 | 24.3 | 153 |
| impeller-second | 56.8 | 26.1 | 24.8 | 23.5 | 23.5 | 155 |

Worst frame per screen (ms, median over runs):

| setup | main-menu | inventory | hud | settings | battle-pass |
|---|---:|---:|---:|---:|---:|
| graphite-bare | 23.64 | 23.46 | 23.67 | 22.97 | 22.95 |
| graphite-first | 23.85 | 22.29 | 23.15 | 23.53 | 21.20 |
| graphite-second | 23.79 | 22.77 | 22.63 | 23.46 | 23.06 |
| impeller-first | 21.54 | 21.61 | 22.28 | 22.33 | 22.64 |
| impeller-second | 21.41 | 22.58 | 22.51 | 22.41 | 21.65 |
| graphite-1440p | 24.14 | 24.85 | 24.88 | 24.82 | 24.32 |
| impeller-1440p | 22.89 | 22.16 | 22.39 | 23.67 | 22.42 |

### Reading them

- **Max frame and p99 can't tell the setups apart.** Every setup,
  including the warm ones, has worst frames of 21–25 ms. The
  present-to-present intervals alternate between under 1 ms and 5–12 ms:
  `nextDrawable` blocks on the layer's three-drawable pool, which the
  compositor frees at display refresh. A single interval shows that pacing,
  not render cost. With the compile pool, Graphite never stalls one frame
  for a shader on Metal, so no frame goes over 33 ms in any setup.
- **The first-use cost shows up in the sums.**
  - First launch, "first 10 frames": plain Skia (`graphite-bare`) 228 ms,
    JSUI's warm-up (`graphite-first`) 176 ms, Impeller 158 ms.
  - Later launches: Graphite with its cache file 124 ms, Impeller 154 ms.
  - Total stall over 16.7 ms: graphite-bare 110 ms, graphite-first
    77 ms, Impeller about 60 ms (first and second), graphite-second 60 ms.
  - So on a first launch Impeller beats plain Skia by about 70 ms per
    5 screens and JSUI's shipped Graphite by about 20 ms. On later launches
    Graphite is ahead.
- **Time to first present is where Impeller wins most.**
  - First launch: Impeller 334 ms, Graphite 595 ms (shipped) and 654 ms
    (bare).
  - Later launches: Graphite 243 ms, Impeller 265 ms.
  - About 240 ms of that is the same for both: JS bundle evaluation, mount
    and asset loads.
  - So the first frame's GPU setup costs Graphite about 350–410 ms with no
    caches and Impeller about 70–95 ms. That is Graphite compiling its
    first pipelines (plus JSUI's common-pipeline precompile in
    `graphite-first`) against Impeller building pipelines from its
    shipped shaders.
- **Context creation:** Impeller 55 ms on first launch (17 ms later), and
  it builds its pipelines at startup. Graphite's context takes 39 ms on a
  first launch and 2 ms later.
- **Steady state:** the medians are within noise: p50 1.6–1.7 ms for
  Graphite against 1.9 ms for Impeller. Impeller's CPU submit is higher
  (0.9 against 0.45–0.8 ms per frame), so it builds and encodes more per
  frame. CPU record is the same (it is JSUI's painter).
- **Memory:** Impeller's peak is 124–131 MB, Graphite's 139–142 MB, at
  1280x720.

## Size

| | raw | `strip -x` | strip + gzip -9 |
|---|---:|---:|---:|
| `libimpeller.dylib` (SDK, arm64) | 11,549,248 | 8,488,040 | 3,555,446 |
| `libjsui.dylib` (this build, arm64, `JSUI_BUILD_SHARED`) | 27,479,840 | 24,577,280 | 11,150,849 |

The SDK's `lib/` also has `libimpeller.a` (928 MB, unlinked objects). The
dylib's install name is `./libimpeller.dylib`, so the spike rewrites it to
`@rpath/` after linking. libjsui here is the m157 desktop Skia with
Graphite Metal, Dawn and Ganesh GL, plus QuickJS, Yoga, ICU data and
embedded fonts. Impeller would be added next to Skia, never in place of
it (see README), so it means about +8.5 MB stripped (+3.6 MB gzipped) per
arch. That is far above the README's "under 200 KB core" figure. This
dylib carries text (Skia's SkParagraph is inside it) and all three
backends.

## Not translated

From the first impeller-first run, counted in draws over all 1200 frames:

- `mask-filter`: 33. Text shadows (main-menu title glow, hud text) are blur
  mask filters on glyph runs, and the paragraph path drops them. So the
  "AETHERFALL" glow is missing on Impeller.
- `text-unmapped-glyphs`: 4560. These are glyphs whose Unicode can't be
  recovered from the font's cmap, so they are dropped from the paragraph
  text. They are the shaped alternates Inter substitutes in context: the
  case-sensitive em dash in "SEASON 4 — EMBERFALL", "+" and "−" and
  the en dash with tabular figures in the inventory stats, the hyphen in
  "EU-West".

Nothing else in these screens went untranslated: gradients, images,
nine-patch shadows, clips, layers and colour filters all mapped.

## Fidelity

`screens/graphite-1.png` and `screens/impeller-1.png` are the last frame of
run 1 (battle-pass), downscaled to 960 px. All 5 screens were also checked
by eye. The geometry, images, gradients, clips, shadows and layers match.
The differences are all in text, as the README expects:

- Letter-spacing is lost (paragraphs re-shape the string), so tracked
  uppercase labels ("CONTINUE", "SEASON 4") are narrower.
- The glyphs listed above are missing.
- Text shadows are missing.

**Impeller bug found:** with Impeller 1.4 an axis-aligned linear gradient
drew nothing when its end points matched the rect's edges. This hit
main-menu's 4-stop scrim (0,360 → 1280,360 over a 1280x720 rect), so the
whole left-side darkening was missing. Shortening the gradient by 1 px
made it draw. The translator now nudges the end point of horizontal and
vertical linear gradients by 1/64 px (`impeller_canvas.cpp`), which is
invisible. This looks like Impeller's fast per-vertex gradient path; it is
not reduced further.

## Build notes (m157 Skia)

- `include/private/SkShadowFlags.h` is gone. `kTransparentOccluder_ShadowFlag`
  is in `include/utils/SkShadowUtils.h`.
- `SkPath::Iter` is gone. The translator uses `SkPath::iter()` /
  `SkPathIter`.
- The blur style enum needs `include/core/SkBlurTypes.h`.
- `impeller_canvas.cpp` subclasses `SkNoDrawCanvas` and Skia has no RTTI,
  so it builds with `-fno-rtti`.
- `SkShaderBase::asGradient`, `SkMaskFilterBase::asABlur`, `SkLatticeIter`
  and the `SkSpan` overload of `unicharsToGlyphs` work unchanged.
- `examples/screens/*.jsx` export a component and don't register it, so
  `run.sh` now builds each screen through a small entry that calls
  `AppRegistry.registerComponent`. Without it every frame was blank.
