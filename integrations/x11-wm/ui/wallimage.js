// Home's wallpaper (shell.jsx Wallpaper) as a small PNG data URI, with a dim
// colour over it: the launcher's and the switcher's background (WallScrim in
// panels.jsx). Drawn stretched, a picture of smooth gradients looks the same
// at 1/8 of the screen's size, and the glows are placed relative to the
// width and height, so one picture serves both orientations.
// Why a picture: the panels fade their background in. Faded as a group (the
// gradient, three glows and the dim in an opacity layer of the whole
// screen), a frame cost the Tab A7 about twice the rest of the launcher's
// (~30 fps); one image fading costs about what the grid does.

const W = 192;
const H = 116;

function parse(c) {
  if (c[0] === '#') {
    const n = parseInt(c.slice(1), 16);
    return [(n >> 16) & 255, (n >> 8) & 255, n & 255, 1];
  }
  const m = c.match(/rgba?\(([^)]*)\)/);
  const p = m[1].split(',').map((x) => parseFloat(x));
  return [p[0], p[1], p[2], p.length > 3 ? p[3] : 1];
}

let crcTable = null;
function crc32(bytes, start, end) {
  if (!crcTable) {
    crcTable = new Int32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
      crcTable[n] = c;
    }
  }
  let c = -1;
  for (let i = start; i < end; i++) c = crcTable[(c ^ bytes[i]) & 255] ^ (c >>> 8);
  return (c ^ -1) >>> 0;
}

// RGB rows -> PNG: zlib with stored (uncompressed) deflate blocks.
function png(rgb) {
  const row = W * 3 + 1;
  const raw = new Uint8Array(row * H);
  for (let y = 0; y < H; y++) raw.set(rgb.subarray(y * W * 3, (y + 1) * W * 3), y * row + 1);
  const blocks = Math.ceil(raw.length / 65535);
  const zlen = 2 + raw.length + blocks * 5 + 4;
  const out = new Uint8Array(8 + 25 + 12 + zlen + 12);
  let o = 0;
  const u32 = (v) => {
    out[o++] = (v >>> 24) & 255;
    out[o++] = (v >>> 16) & 255;
    out[o++] = (v >>> 8) & 255;
    out[o++] = v & 255;
  };
  const chunk = (type, len, fill) => {
    u32(len);
    const s = o;
    for (let i = 0; i < 4; i++) out[o++] = type.charCodeAt(i);
    fill();
    u32(crc32(out, s, o));
  };
  [137, 80, 78, 71, 13, 10, 26, 10].forEach((b) => (out[o++] = b));
  chunk('IHDR', 13, () => {
    u32(W);
    u32(H);
    out[o++] = 8; // bit depth
    out[o++] = 2; // RGB
    out[o++] = 0;
    out[o++] = 0;
    out[o++] = 0;
  });
  chunk('IDAT', zlen, () => {
    out[o++] = 0x78;
    out[o++] = 0x01;
    let a = 1;
    let b = 0;
    for (let i = 0; i < raw.length; i += 65535) {
      const n = Math.min(65535, raw.length - i);
      out[o++] = i + n >= raw.length ? 1 : 0;
      out[o++] = n & 255;
      out[o++] = n >> 8;
      out[o++] = ~n & 255;
      out[o++] = (~n >> 8) & 255;
      for (let k = 0; k < n; k++) {
        const v = raw[i + k];
        out[o++] = v;
        a = (a + v) % 65521;
        b = (b + a) % 65521;
      }
    }
    u32(((b << 16) | a) >>> 0);
  });
  chunk('IEND', 0, () => {});
  return out;
}

const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
function base64(bytes) {
  let s = '';
  for (let i = 0; i < bytes.length; i += 3) {
    const n = (bytes[i] << 16) | ((bytes[i + 1] || 0) << 8) | (bytes[i + 2] || 0);
    s += B64[(n >> 18) & 63] + B64[(n >> 12) & 63];
    s += i + 1 < bytes.length ? B64[(n >> 6) & 63] : '=';
    s += i + 2 < bytes.length ? B64[n & 63] : '=';
  }
  return s;
}

const cache = new Map();

// The wallpaper of theme `t` for a `width` x `height` screen (the gradient's
// angle depends on the aspect), with `dim` (a CSS colour) over it.
export function wallImage(t, width, height, dim) {
  const key = [t.wallTop, t.wallMid, t.wallBottom, t.glowA, t.glowB, t.glowC, dim, width > height].join();
  if (cache.has(key)) return cache.get(key);
  const top = parse(t.wallTop);
  const mid = parse(t.wallMid);
  const bot = parse(t.wallBottom);
  // As Wallpaper's ellipses, in fractions of the width / height.
  const glows = [
    [parse(t.glowA), 0.18, 0.22, 0.42, 0.5],
    [parse(t.glowB), 0.86, 0.78, 0.46, 0.55],
    [parse(t.glowC), 0.62, 0.12, 0.3, 0.3],
  ];
  const d = parse(dim);
  // linear-gradient(160deg): CSS angles run clockwise from "to top"; the
  // gradient line spans the box's corners projected onto it.
  const ang = (160 * Math.PI) / 180;
  const dx = Math.sin(ang);
  const dy = -Math.cos(ang);
  const len = Math.abs(width * dx) + Math.abs(height * dy);
  const rgb = new Uint8Array(W * H * 3);
  for (let y = 0; y < H; y++) {
    const fy = (y + 0.5) / H;
    for (let x = 0; x < W; x++) {
      const fx = (x + 0.5) / W;
      const tt = Math.min(1, Math.max(0, ((fx - 0.5) * width * dx + (fy - 0.5) * height * dy) / len + 0.5));
      let a;
      let b;
      let f;
      if (tt < 0.55) {
        a = top;
        b = mid;
        f = tt / 0.55;
      } else {
        a = mid;
        b = bot;
        f = (tt - 0.55) / 0.45;
      }
      let r = a[0] + (b[0] - a[0]) * f;
      let g = a[1] + (b[1] - a[1]) * f;
      let bl = a[2] + (b[2] - a[2]) * f;
      for (const [c, cx, cy, rx, ry] of glows) {
        const ex = (fx - cx) / rx;
        const ey = (fy - cy) / ry;
        const dist = Math.sqrt(ex * ex + ey * ey);
        if (dist >= 1) continue;
        const al = c[3] * (1 - dist);
        r += (c[0] - r) * al;
        g += (c[1] - g) * al;
        bl += (c[2] - bl) * al;
      }
      r += (d[0] - r) * d[3];
      g += (d[1] - g) * d[3];
      bl += (d[2] - bl) * d[3];
      const i = (y * W + x) * 3;
      rgb[i] = Math.round(r);
      rgb[i + 1] = Math.round(g);
      rgb[i + 2] = Math.round(bl);
    }
  }
  const uri = 'data:image/png;base64,' + base64(png(rgb));
  cache.set(key, uri);
  return uri;
}
