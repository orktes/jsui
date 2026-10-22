// Asset handling: images and fonts imported from app code are emitted next to
// the bundle (assets/<name>-<hash>.<ext>) and the import evaluates to that
// URL, relative to the bundle. Images also register their intrinsic size so
// <Image source={require('./a.png')} /> sizes itself like React Native.

import { createHash } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import path from 'node:path';

export const IMAGE_EXTS = ['png', 'jpg', 'jpeg', 'gif', 'webp', 'bmp', 'svg'];
export const FONT_EXTS = ['ttf', 'otf', 'woff', 'woff2'];
export const OTHER_EXTS = ['mp3', 'wav', 'ogg', 'mp4', 'bin'];

/** Intrinsic size of an image buffer, or null. */
export function imageSize(buf, ext) {
  try {
    if (ext === 'png' && buf.length >= 24 && buf.readUInt32BE(12) === 0x49484452) {
      return { width: buf.readUInt32BE(16), height: buf.readUInt32BE(20) };
    }
    if (ext === 'gif' && buf.length >= 10) return { width: buf.readUInt16LE(6), height: buf.readUInt16LE(8) };
    if (ext === 'bmp' && buf.length >= 26) return { width: buf.readInt32LE(18), height: Math.abs(buf.readInt32LE(22)) };
    if (ext === 'jpg' || ext === 'jpeg') {
      let i = 2;
      while (i + 9 < buf.length) {
        if (buf[i] !== 0xff) {
          i++;
          continue;
        }
        const marker = buf[i + 1];
        const len = buf.readUInt16BE(i + 2);
        // SOF0..SOF15 except DHT (C4), JPG (C8), DAC (CC)
        if (marker >= 0xc0 && marker <= 0xcf && marker !== 0xc4 && marker !== 0xc8 && marker !== 0xcc) {
          return { width: buf.readUInt16BE(i + 7), height: buf.readUInt16BE(i + 5) };
        }
        i += 2 + len;
      }
      return null;
    }
    if (ext === 'webp' && buf.length >= 30 && buf.toString('ascii', 0, 4) === 'RIFF') {
      const fmt = buf.toString('ascii', 12, 16);
      if (fmt === 'VP8 ') return { width: buf.readUInt16LE(26) & 0x3fff, height: buf.readUInt16LE(28) & 0x3fff };
      if (fmt === 'VP8L') {
        const b = buf.readUInt32LE(21);
        return { width: (b & 0x3fff) + 1, height: ((b >> 14) & 0x3fff) + 1 };
      }
      if (fmt === 'VP8X') return { width: 1 + buf.readUIntLE(24, 3), height: 1 + buf.readUIntLE(27, 3) };
      return null;
    }
    if (ext === 'svg') {
      const head = buf.toString('utf8', 0, Math.min(buf.length, 4096));
      const tag = /<svg\b[^>]*>/i.exec(head);
      if (!tag) return null;
      const attr = (n) => {
        const m = new RegExp('\\b' + n + '\\s*=\\s*["\']\\s*([\\d.]+)(px)?\\s*["\']', 'i').exec(tag[0]);
        return m ? parseFloat(m[1]) : null;
      };
      let w = attr('width');
      let h = attr('height');
      const vb = /viewBox\s*=\s*["']\s*[-\d.]+[\s,]+[-\d.]+[\s,]+([\d.]+)[\s,]+([\d.]+)/i.exec(tag[0]);
      if ((w == null || h == null) && vb) {
        w = w == null ? parseFloat(vb[1]) : w;
        h = h == null ? parseFloat(vb[2]) : h;
      }
      return w != null && h != null ? { width: w, height: h } : null;
    }
  } catch {
    return null;
  }
  return null;
}

/**
 * esbuild plugin. `assets` is a Map(publishedPath -> Buffer) that the caller
 * writes to disk (build) or serves from memory (dev).
 */
export function assetPlugin(assets) {
  const exts = [...IMAGE_EXTS, ...FONT_EXTS, ...OTHER_EXTS];
  const filter = new RegExp('\\.(' + exts.join('|') + ')$', 'i');
  return {
    name: 'jsui-assets',
    setup(build) {
      build.onLoad({ filter }, async (args) => {
        const buf = await readFile(args.path);
        const ext = path.extname(args.path).slice(1).toLowerCase();
        const base = path.basename(args.path, path.extname(args.path));
        const hash = createHash('sha1').update(buf).digest('hex').slice(0, 8);
        const published = `assets/${base.replace(/[^\w@.-]/g, '_')}-${hash}.${ext}`;
        assets.set(published, buf);
        let code = `module.exports = ${JSON.stringify(published)};\n`;
        if (IMAGE_EXTS.includes(ext)) {
          const size = imageSize(buf, ext);
          if (size) {
            // name@2x.png: logical size is pixel size / scale (RN convention).
            const m = /@(\d(?:\.\d+)?)x$/.exec(base);
            const scale = m ? parseFloat(m[1]) : 1;
            const info = { width: size.width / scale, height: size.height / scale, scale };
            code =
              `var u = ${JSON.stringify(published)};\n` +
              `(globalThis.__jsui_assets || (globalThis.__jsui_assets = {}))[u] = ${JSON.stringify(info)};\n` +
              `module.exports = u;\n`;
          }
        }
        return { contents: code, loader: 'js', watchFiles: [args.path] };
      });
    },
  };
}
