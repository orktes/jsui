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
  return {
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
