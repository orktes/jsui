// Motion checks for animated cases (examples/animated): does text move
// rigidly with its box from frame to frame, and does the box land where
// Chrome puts it?
//
// Animated fixtures draw pure blue (#0000ff) boxes with white text on black,
// so a pixel's blue channel is the coverage of "box or text" and its red
// channel the coverage of the text alone (sRGB blending is linear in these
// channels). Coverage-weighted centroids then give subpixel positions of a
// box and of its text without any edge fitting.
import { PNG } from 'pngjs';

/** Centroids (device px) of the box (blue) and the text (red) inside `rect`. */
export function sampleRegion(png, rect) {
  const x0 = Math.max(0, Math.floor(rect.x)), y0 = Math.max(0, Math.floor(rect.y));
  const x1 = Math.min(png.width, Math.ceil(rect.x + rect.w)), y1 = Math.min(png.height, Math.ceil(rect.y + rect.h));
  let bs = 0, bx = 0, by = 0, ts = 0, tx = 0, ty = 0;
  for (let y = y0; y < y1; y++) {
    for (let x = x0; x < x1; x++) {
      const i = (y * png.width + x) * 4;
      const a = png.data[i + 3] / 255;
      const t = (png.data[i] / 255) * a, b = (png.data[i + 2] / 255) * a;
      bs += b; bx += b * (x + 0.5); by += b * (y + 0.5);
      ts += t; tx += t * (x + 0.5); ty += t * (y + 0.5);
    }
  }
  return { box: [bx / bs, by / bs], text: [tx / ts, ty / ts] };
}

const rms = (v) => Math.sqrt(v.reduce((s, x) => s + x * x, 0) / Math.max(1, v.length));
const secondDiff = (v) => v.slice(2).map((x, i) => x - 2 * v[i + 1] + v[i]);

/**
 * Per region: `wiggle` = RMS over frames of the second difference of the
 * text-minus-box offset (worst axis, device px). It is ~0 for any smooth
 * motion of a rigid box + text (constant offset under translation, linear
 * under a scale ramp) and grows when text and box snap to pixels
 * differently. `boxError` (with `reference` samples) = worst per-frame
 * distance between this side's box centroid and the reference's.
 */
export function motionStats(samples, reference) {
  const rel = (s, k) => s.text[k] - s.box[k];
  const wig = [0, 1].map((k) => rms(secondDiff(samples.map((s) => rel(s, k)))));
  const out = { wiggle: Math.max(...wig), wiggleX: wig[0], wiggleY: wig[1] };
  if (reference) {
    out.boxError = Math.max(...samples.map((s, i) => Math.hypot(s.box[0] - reference[i].box[0], s.box[1] - reference[i].box[1])));
  }
  return out;
}

/** Samples every region (CSS px rects, scaled to device px) of PNG buffers. */
export function sampleFrames(pngBufs, regions, scale) {
  const pngs = pngBufs.map((b) => PNG.sync.read(b));
  return regions.map((r) =>
    pngs.map((png) => sampleRegion(png, { x: r.x * scale, y: r.y * scale, w: r.w * scale, h: r.h * scale })),
  );
}

/**
 * Checks of meta.motion = { regions: [{ name, x, y, w, h }], maxWiggle,
 * maxBoxError } (device px at the case's scale; defaults 0.3 and 0.1).
 * Skia places glyphs on a 1/4 px subpixel grid, which alone gives up to
 * ~0.25 px wiggle on a 1/8 px ramp; snapping boxes or baselines to whole
 * device pixels gives 0.35 - 0.7 px, and boxes snapped after the transform
 * land up to 0.5 px away from Chrome's.
 * Returns [{ ok, text, stats }] like run.js localChecks; Chrome's own wiggle
 * is reported for comparison but not enforced (with subpixel positioning
 * off, Chrome's text steps by whole pixels under transforms).
 */
export function motionChecks(expectedBufs, actualBufs, meta, scale) {
  const m = meta.motion;
  const maxWiggle = m.maxWiggle ?? 0.3;
  const maxBoxError = m.maxBoxError ?? 0.1;
  const exp = sampleFrames(expectedBufs, m.regions, scale);
  const act = sampleFrames(actualBufs, m.regions, scale);
  const out = [];
  m.regions.forEach((r, i) => {
    const a = motionStats(act[i], exp[i]);
    const e = motionStats(exp[i]);
    out.push({
      ok: a.wiggle <= maxWiggle,
      text: `motion "${r.name}": text-vs-box wiggle ${a.wiggle.toFixed(3)}px (max ${maxWiggle}; chrome ${e.wiggle.toFixed(3)})`,
      stats: { jsui: a, chrome: e },
    });
    out.push({
      ok: a.boxError <= maxBoxError,
      text: `motion "${r.name}": box position vs chrome ${a.boxError.toFixed(3)}px (max ${maxBoxError})`,
      stats: { jsui: a },
    });
  });
  return out;
}
