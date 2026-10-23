# Reference tests

Proves that JSUI renders React Native code the way a real browser does. Every
example in [`examples/`](../../examples) is an ordinary React Native component
(`import { View, Text, Image, StyleSheet } from 'react-native'`). It is rendered
twice at the same viewport size and the PNGs are compared pixel by pixel:

| Side | How |
| --- | --- |
| **expected** | esbuild aliases `react-native` → `react-native-web`; react-dom renders it into a fixed-size root in headless Chromium (Playwright); screenshot. |
| **actual** | the same file is bundled for JSUI (`@orktes/jsui-cli` `build()`, which aliases `react-native` → `@orktes/jsui-react`) and rendered by `build/jsui-render`. |

Chrome is the oracle, so no golden images are committed. Chrome screenshots
are cached by content hash in `output/.cache/`, which makes repeat runs fast.

## Running

```sh
npm install                       # repo root, once (npm workspaces)
cmake --build build -j2           # builds build/jsui-render

npm run test:reference            # all cases, exits non-zero on any failure
npm run test:reference -- hud radii         # only cases whose name contains a filter
npm run test:reference -- --chrome-only     # expected side only (design review)
npm run test:reference -- --scale 2         # 2x device pixel ratio variant
npm run test:reference -- --no-cache        # re-screenshot Chrome
```

Or from this directory: `node run.js [filters...] [--chrome-only] [--scale N] [--no-cache] [--jobs N]`.

Environment:

| Variable | Default | Meaning |
| --- | --- | --- |
| `JSUI_RENDER` | `build/jsui-render` | renderer binary |
| `JSUI_REF_BUILDER` | auto | force `cli`, `package` (esbuild + `@orktes/jsui-react`) or `fallback` |
| `JSUI_REF_OUTPUT` | `tests/reference/output` | where results go |
| `PLAYWRIGHT_BROWSERS_PATH` | (Playwright default) | Chromium location; never run `playwright install` here |

## Output

`output/` (git-ignored):

* `report.html` (`report@2x.html` for `--scale 2`): self-contained page with
  expected / actual / diff side by side, mismatch % per case, a swipe
  comparison, logs, and pass/fail filters.
* `results.json`: machine-readable summary.
* `cases/<group>__<name>/`: `expected.png`, `actual.png`, `diff.png`, plus the
  `web/` and `jsui/` bundles that produced them.

In `diff.png`, red marks pixels where the actual is brighter and blue where it
is darker; anti-aliased edge pixels are detected and ignored.

## Pass criteria

pixelmatch with `threshold: 0.1` and anti-aliasing detection. A case passes
when the mismatched-pixel percentage is at most its `meta.maxMismatch`
(default 1.5 % for text-heavy screens, 0.5 % for shape-only feature cases) and
both images have the same size. A JSUI render error, a missing image or a size
mismatch fails the case.

A whole-image percentage can hide a local defect (a missing corner arc is a
few dozen pixels out of 230,000), so cases can also set local checks:

| `meta` key | Meaning |
| --- | --- |
| `maxTileMismatch` | worst mismatched share (%) of any `tileSize`×`tileSize` tile (CSS px, default 16, scanned at half-tile steps) |
| `regions` | `[{ name, x, y, w, h, maxMismatch }]` in CSS px, each with its own budget |
| `scale` | per-case device pixel ratio multiplier, e.g. `4` for a zoomed-in case |

Shape-only feature cases use `maxTileMismatch: 3`. `features/corners` renders
at 4× with 8 px tiles and named corner regions, so a wrong pixel on a rounded
border shows up.

## Animated cases

`examples/animated/*.jsx` set `meta.frames`. The App reads its frame index
from `globalThis.__FRAME` (JSUI: prepended to the bundle, one `jsui-render`
run per frame) and exposes `globalThis.__setFrame` (Chrome: one page, set the
frame, wait two animation frames, screenshot), and derives every animated
value from the frame (e.g. an analytic spring), so both sides show exactly
the same transforms. Every frame is pixel-compared (the worst sets the
mismatch %), and `meta.motion` adds a motion check (`lib/motion.js`): the
cases draw `#0000ff` boxes with white text on black, so the blue channel is
the coverage of box + text and the red channel that of the text; per region
and frame the centroids give subpixel positions, and

| `meta.motion` key | Meaning |
| --- | --- |
| `regions` | `[{ name, x, y, w, h }]` in CSS px, one box with its text each |
| `maxWiggle` | RMS of the frame-to-frame second difference of text − box (device px, default 0.3): ~0 for rigid motion, 0.35–0.7 when box or baseline snap to whole pixels |
| `maxBoxError` | worst distance of a box centroid from Chrome's (device px, default 0.1) |

To look at motion by eye, `node frames.js <fixture.jsx> --out <dir>` writes
`<dir>/chrome/NNNN.png` and `<dir>/jsui/NNNN.png` for any such fixture
(`examples/showcase/spring-wiggle.jsx` is a 864-frame menu hover + panel
spring at 4x slow motion).

## Adding an example

Create `examples/features/<name>.jsx` (one property family, ideally no text) or
`examples/screens/<name>.jsx` (a full game screen):

```jsx
import { View, Text, StyleSheet } from 'react-native';
export const meta = { width: 640, height: 360, maxMismatch: 0.5, title: 'My case' };
export default function App() { /* ... */ }
```

`meta` must be a plain object literal (it is read without running the file).
Optional keys: `waitMs` (simulated time before capture, default 300).
Use `fontFamily: 'Inter'` (the default font is Inter on both sides anyway) and
only the shipped weights 400/500/600/700 plus 400 italic. Images are imported
with `require('../assets/x.png')`; the art in `examples/assets/` is generated
by `npm --workspace tests/reference run assets`
(`scripts/gen-assets.py`, PIL + numpy, deterministic, no third-party art).

## How determinism is achieved

* Chromium runs with `--font-render-hinting=none --disable-lcd-text
  --disable-font-subpixel-positioning --force-color-profile=srgb --disable-gpu`
  and one raster thread; each case gets a fresh browser context.
* Text rasterisation settings match JSUI's (unhinted outlines, grayscale
  anti-aliasing, subpixel glyph positions) on every platform. Most flags above
  only affect Linux / Windows Chrome; on macOS the page sets
  `-webkit-font-smoothing: antialiased`, without which Chrome draws hinted
  CoreText glyphs, i.e. CoreGraphics' dilated "font smoothing" outlines
  (bolder than JSUI's, and dependent on the machine's AppleFontSmoothing
  setting). Linux and Windows Chrome ignore the property.
* Line boxes: Chrome on Linux borrows a pixel from the ascent when the
  descent rounds down; on macOS / Windows it does not. JSUI follows the host
  platform (`chromeLineMetrics` in `src/text/text_layout.cpp`), so each
  platform is compared against its own Chrome. CI runs the suite on Linux
  (required) and macOS (`reference-macos`, advisory).
* Fonts: the same `assets/fonts/Inter-*.ttf` files are served to Chrome via
  `@font-face` (also aliased under the `System` / platform family names that
  react-native-web uses by default) and passed to `jsui-render --font`.
* The harness waits for `document.fonts`, image decode and two animation
  frames, then screenshots until two consecutive frames are identical.
* Images that are drawn downscaled are PNG, not JPEG: Chrome sometimes
  decodes JPEGs at a reduced DCT scale, which made portraits differ by a few
  LSBs between runs.

## react-native-web caveats

* RNW 0.21 passes unknown style keys straight to CSS, so the RN 0.76+
  `experimental_backgroundImage` would be dropped. The web bundle maps it to
  CSS `background-image` inside RNW's style preprocessor (esbuild `onLoad`
  patch in `lib/web.js`). App code stays standard RN.
* `numberOfLines` uses `-webkit-line-clamp`, which lets the next line peek
  into a Text's own padding. Examples put padding on a wrapping `View`.
* Legacy `shadow*` props become a CSS `box-shadow` with blur = `shadowRadius`
  and alpha multiplied by `shadowOpacity` (iOS semantics).

## JSUI bundling

`lib/jsui.js` `buildJsuiBundle(entry, outFile)` prefers `@orktes/jsui-cli`'s
programmatic `build({ entry, outfile })`. Without it, it uses esbuild with
`react-native` aliased to `@orktes/jsui-react`; if that package is missing too, it
uses `jsui-fallback/react-native.js`, a minimal static reconciler over the
`__jsui_native` bridge that exists only so the pipeline can run before
`@orktes/jsui-react` lands. The report header says which builder was used.

Both sides boot the app the React Native way:
`AppRegistry.registerComponent('App', () => App)` then `runApplication`.
