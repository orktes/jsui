// Chrome side ("expected"): react-native-web + react-dom in headless
// Chromium via Playwright.
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import * as esbuild from 'esbuild';
import { chromium } from 'playwright';
import { CACHE_DIR, FONTS_DIR, INTER_FACES, PKG_DIR } from './paths.js';

const require = createRequire(import.meta.url);

// Chromium flags that remove platform/GPU variance from text and colour.
export const CHROME_ARGS = [
  '--font-render-hinting=none',
  '--disable-lcd-text',
  '--disable-font-subpixel-positioning',
  '--force-color-profile=srgb',
  '--disable-gpu',
  '--hide-scrollbars',
  '--disable-skia-runtime-opts',
  '--disable-checker-imaging',
  '--num-raster-threads=1',
];

// Bump to invalidate cached screenshots when the page harness changes in a
// way the content hash cannot see.
const HARNESS_VERSION = 6;

// RN code may rely on the platform default font. JSUI's default is Inter, so
// every family react-native-web's "System" stack names is aliased to Inter.
const SYSTEM_ALIASES = ['Inter', 'System', '-apple-system', 'BlinkMacSystemFont', 'Segoe UI', 'Roboto', 'Helvetica', 'Arial', 'sans-serif'];

function fontFaceCss() {
  const rules = [];
  for (const family of SYSTEM_ALIASES) {
    for (const f of INTER_FACES) {
      rules.push(
        `@font-face{font-family:${JSON.stringify(family)};src:url(/__fonts/${f.file}) format("truetype");` +
          `font-weight:${f.weight};font-style:${f.style};font-display:block}`,
      );
    }
  }
  return rules.join('\n');
}

// -webkit-font-smoothing:antialiased only changes anything on macOS, where
// Chrome's default text uses hinted (kNormal) CoreText glyphs; with
// --disable-lcd-text those come from CoreGraphics' dilated "font smoothing"
// outlines, bolder than JSUI's unhinted, undilated ones and dependent on the
// machine's AppleFontSmoothing setting. "antialiased" makes Chrome use
// SkFontHinting::kNone + kAntiAlias edging, i.e. exactly JSUI's text settings.
// Linux and Windows Chrome ignore the property.
function pageHtml(meta) {
  return `<!doctype html>
<html><head><meta charset="utf-8">
<style>
${fontFaceCss()}
html,body{margin:0;padding:0;overflow:hidden;background:transparent;-webkit-font-smoothing:antialiased}
#root{position:relative;display:flex;flex-direction:column;width:${meta.width}px;height:${meta.height}px;overflow:hidden;font-family:Inter}
</style></head>
<body><div id="root"></div><script src="/bundle.js"></script></body></html>`;
}

// react-native-web (0.21) passes unknown style keys straight to CSS, so the RN
// 0.76+ `experimental_backgroundImage` would be dropped by the browser. Map it
// to the standard CSS property inside RNW's style preprocessor; app code
// stays standard RN.
const rnwShimPlugin = {
  name: 'rnw-background-image',
  setup(build) {
    build.onLoad({ filter: /react-native-web[\\/]dist[\\/]exports[\\/]StyleSheet[\\/]preprocess\.js$/ }, async (args) => {
      let src = await fs.promises.readFile(args.path, 'utf8');
      const anchor = 'var PROPERTIES_STANDARD = {';
      if (!src.includes(anchor)) throw new Error('react-native-web preprocess.js changed; update rnwShimPlugin');
      src = src.replace(anchor, `${anchor}\n  experimental_backgroundImage: 'backgroundImage',`);
      return { contents: src, loader: 'js' };
    });
  },
};

export async function buildWebBundle(entryFile, outDir) {
  await fs.promises.rm(outDir, { recursive: true, force: true });
  const entry = `
import { AppRegistry } from 'react-native';
import App from ${JSON.stringify(entryFile)};
AppRegistry.registerComponent('App', () => App);
AppRegistry.runApplication('App', { rootTag: document.getElementById('root') });
`;
  await esbuild.build({
    stdin: { contents: entry, resolveDir: path.dirname(entryFile), loader: 'jsx', sourcefile: 'web-entry.jsx' },
    bundle: true,
    outdir: outDir,
    entryNames: 'bundle',
    assetNames: 'assets/[name]-[hash]',
    format: 'iife',
    jsx: 'automatic',
    minify: false,
    alias: { 'react-native': 'react-native-web' },
    // Resolve shared deps from this package so examples need no node_modules.
    nodePaths: [path.join(PKG_DIR, 'node_modules'), path.resolve(PKG_DIR, '../../node_modules')],
    define: { 'process.env.NODE_ENV': '"production"', __DEV__: 'false' },
    loader: { '.png': 'file', '.jpg': 'file', '.jpeg': 'file', '.js': 'jsx' },
    plugins: [rnwShimPlugin],
    logLevel: 'silent',
  });
  return outDir;
}

function hashDir(h, dir) {
  for (const f of fs.readdirSync(dir, { recursive: true }).map(String).sort()) {
    const p = path.join(dir, f);
    if (fs.statSync(p).isFile()) {
      h.update(f);
      h.update(fs.readFileSync(p));
    }
  }
}

const CONTENT_TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.png': 'image/png', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg', '.ttf': 'font/ttf' };

export class ChromeRenderer {
  async start() {
    this.browser = await chromium.launch({ args: CHROME_ARGS });
    this.version = this.browser.version();
  }

  async stop() {
    await this.browser?.close();
  }

  // A fresh context per case gives each page its own renderer process, so
  // image decode caches warmed by earlier cases cannot change how scaled
  // images are filtered (observed as 1-2 LSB differences on portraits).
  newContext(scale) {
    return this.browser.newContext({ deviceScaleFactor: scale, colorScheme: 'dark', reducedMotion: 'reduce' });
  }

  /**
   * Screenshot one built case. Returns { png: Buffer, cached: boolean, logs: string[] }.
   */
  async render(webDir, meta, scale, { useCache = true } = {}) {
    const html = pageHtml(meta);
    const h = crypto.createHash('sha256');
    h.update(JSON.stringify({ HARNESS_VERSION, platform: process.platform, v: this.version, CHROME_ARGS, scale, w: meta.width, h: meta.height, waitMs: meta.waitMs }));
    h.update(html);
    hashDir(h, webDir);
    const key = h.digest('hex').slice(0, 24);
    const cacheFile = path.join(CACHE_DIR, `${key}.png`);
    if (useCache && fs.existsSync(cacheFile)) return { png: fs.readFileSync(cacheFile), cached: true, logs: [] };

    const ctx = await this.newContext(scale);
    const page = await ctx.newPage();
    const logs = [];
    page.on('console', (m) => {
      if (m.type() === 'error' || m.type() === 'warning') logs.push(`[${m.type()}] ${m.text()}`);
    });
    page.on('pageerror', (e) => logs.push(`[pageerror] ${e.message}`));
    // Serve everything from a fake origin so fonts and images load without
    // file:// CORS quirks and without opening a port.
    await page.route('http://reference.test/**', async (route) => {
      const url = new URL(route.request().url());
      const rel = decodeURIComponent(url.pathname);
      if (rel === '/') return route.fulfill({ body: html, contentType: 'text/html' });
      const file = rel.startsWith('/__fonts/') ? path.join(FONTS_DIR, path.basename(rel)) : path.join(webDir, rel);
      if (!file.startsWith(webDir) && !file.startsWith(FONTS_DIR)) return route.fulfill({ status: 403 });
      if (!fs.existsSync(file)) return route.fulfill({ status: 404, body: 'not found' });
      return route.fulfill({ body: fs.readFileSync(file), contentType: CONTENT_TYPES[path.extname(file)] || 'application/octet-stream' });
    });
    try {
      await page.setViewportSize({ width: meta.width, height: meta.height });
      await page.goto('http://reference.test/', { waitUntil: 'load' });
      await page.evaluate(async (faces) => {
        await Promise.all(faces.map((f) => document.fonts.load(`${f.style} ${f.weight} 16px Inter`)));
        await document.fonts.ready;
        const imgs = [...document.images];
        await Promise.all(imgs.map((img) => (img.complete ? img.decode().catch(() => {}) : new Promise((r) => { img.onload = img.onerror = r; }))));
        await new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)));
      }, INTER_FACES);
      if (meta.waitMs) await page.waitForTimeout(Math.min(meta.waitMs, 2000));
      // Chrome may first paint scaled images with a cheaper filter and
      // upgrade later, so wait until two consecutive frames are identical.
      const shot = () => page.screenshot({ omitBackground: true, animations: 'disabled', caret: 'hide', clip: { x: 0, y: 0, width: meta.width, height: meta.height } });
      let png = await shot();
      for (let i = 0; i < 10; i++) {
        await page.waitForTimeout(150);
        const next = await shot();
        if (next.equals(png)) break;
        png = next;
        if (i === 9) logs.push('[harness] screenshot did not stabilise after 10 attempts');
      }
      await fs.promises.mkdir(CACHE_DIR, { recursive: true });
      await fs.promises.writeFile(cacheFile, png);
      return { png, cached: false, logs };
    } finally {
      await ctx.close();
    }
  }
}

export function rnwVersion() {
  return require('react-native-web/package.json').version;
}
