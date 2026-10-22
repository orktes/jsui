// Per-pixel comparison with tolerance (pixelmatch, anti-aliasing aware).
import { PNG } from 'pngjs';
import pixelmatch from 'pixelmatch';

export const PIXEL_THRESHOLD = 0.1;

/**
 * Compares two PNG buffers. Images of different sizes are compared on the
 * union canvas (missing area counts as mismatched), so a size bug still
 * yields a useful diff instead of an exception.
 * @returns {{ mismatchPct:number, mismatched:number, total:number, width:number, height:number, diffPng:Buffer, sizeMismatch:boolean }}
 */
export function comparePngs(expectedBuf, actualBuf) {
  const a = PNG.sync.read(expectedBuf);
  const b = PNG.sync.read(actualBuf);
  const width = Math.max(a.width, b.width);
  const height = Math.max(a.height, b.height);
  const pa = pad(a, width, height);
  const pb = pad(b, width, height);
  const diff = new PNG({ width, height });
  const mismatched = pixelmatch(pa, pb, diff.data, width, height, {
    threshold: PIXEL_THRESHOLD,
    includeAA: false,
    alpha: 0.25,
    diffColor: [255, 32, 80],
    diffColorAlt: [40, 160, 255],
  });
  const total = width * height;
  // Mismatch mask (pixels pixelmatch painted with a diff color), used for
  // region and tile checks that whole-image percentages would hide.
  const mask = new Uint8Array(total);
  for (let i = 0; i < total; i++) {
    const r = diff.data[i * 4], g = diff.data[i * 4 + 1], b = diff.data[i * 4 + 2];
    mask[i] = (r === 255 && g === 32 && b === 80) || (r === 40 && g === 160 && b === 255) ? 1 : 0;
  }
  return {
    mask,
    mismatchPct: (mismatched / total) * 100,
    mismatched,
    total,
    width,
    height,
    diffPng: PNG.sync.write(diff),
    sizeMismatch: a.width !== b.width || a.height !== b.height,
  };
}

function pad(png, width, height) {
  if (png.width === width && png.height === height) return png.data;
  const out = Buffer.alloc(width * height * 4);
  for (let y = 0; y < png.height; y++) {
    png.data.copy(out, y * width * 4, y * png.width * 4, (y + 1) * png.width * 4);
  }
  return out;
}

/** Mismatched share (%) of the device-pixel rect [x, y, w, h] in `cmp`. */
export function regionMismatchPct(cmp, x, y, w, h) {
  const x0 = Math.max(0, Math.floor(x)), y0 = Math.max(0, Math.floor(y));
  const x1 = Math.min(cmp.width, Math.ceil(x + w)), y1 = Math.min(cmp.height, Math.ceil(y + h));
  let n = 0;
  for (let yy = y0; yy < y1; yy++) for (let xx = x0; xx < x1; xx++) n += cmp.mask[yy * cmp.width + xx];
  const area = Math.max(1, (x1 - x0) * (y1 - y0));
  return (n / area) * 100;
}

/**
 * The worst `size`×`size` tile (device px, stepping by half a tile): a local
 * defect such as a missing corner arc fails it even when the whole image is
 * within budget. Returns { pct, x, y } with x/y in device pixels.
 */
export function worstTile(cmp, size) {
  const step = Math.max(1, size >> 1);
  let worst = { pct: 0, x: 0, y: 0 };
  for (let y = 0; y + size <= cmp.height; y += step) {
    for (let x = 0; x + size <= cmp.width; x += step) {
      const pct = regionMismatchPct(cmp, x, y, size, size);
      if (pct > worst.pct) worst = { pct, x, y };
    }
  }
  return worst;
}
