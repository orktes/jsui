// `jsui pipelines <dir>`: records the GPU pipelines a web build of the app
// uses, for createJsui({ pipelines: './pipelines-{format}.bin' }) (the
// index.html of `jsui build --web` passes that).
//
// Serves <dir> (a `jsui build --web` folder, or any page that sets
// window.jsui), opens it in Chrome through Playwright (not a dependency of
// the CLI: `npm install -D playwright`), once per canvas format
// (bgra8unorm: macOS / Windows, rgba8unorm: Android / Linux), visits each
// --visit (a path, ?query or #hash of the page) for --wait ms, runs --script,
// and writes <dir>/pipelines-<format>.bin from jsui.savePipelines().
import { createReadStream, existsSync, statSync } from 'node:fs';
import { writeFile } from 'node:fs/promises';
import http from 'node:http';
import { createRequire } from 'node:module';
import path from 'node:path';
import { parseArgs } from 'node:util';
import { pathToFileURL } from 'node:url';

export const PIPELINES_USAGE = `  jsui pipelines <web dir> [--visit <path|?query|#hash> ...] [--wait 3000]
     [--script flow.mjs] [--formats bgra8unorm,rgba8unorm] [--browser chrome|chromium]
     Records the GPU pipelines the app's WebGPU build uses (Skia Graphite
     compiles each kind of draw the first time it appears, tens of ms each
     with a cold browser cache) into <dir>/pipelines-<format>.bin, which
     createJsui({ pipelines: './pipelines-{format}.bin' }) builds while the
     page loads. Opens <dir>'s index.html in Chrome (Playwright, installed
     separately), visits each --visit for --wait ms (default: the page
     itself); --script exports default async (page, { visit }) => {} for
     anything else (clicks, keys). Use a machine with a GPU: real Chrome on
     the GPU by default, --browser chromium for Playwright's own.`;

const MIME = { '.js': 'text/javascript', '.mjs': 'text/javascript', '.html': 'text/html', '.wasm': 'application/wasm',
  '.json': 'application/json', '.png': 'image/png', '.jpg': 'image/jpeg', '.svg': 'image/svg+xml', '.ttf': 'font/ttf',
  '.woff2': 'font/woff2', '.wav': 'audio/wav', '.mp3': 'audio/mpeg', '.css': 'text/css' };

function serve(dir) {
  const server = http.createServer((req, res) => {
    const p = decodeURIComponent(new URL(req.url, 'http://x').pathname);
    let file = path.normalize(path.join(dir, p));
    if (existsSync(file) && statSync(file).isDirectory()) file = path.join(file, 'index.html');
    if (!file.startsWith(dir) || !existsSync(file)) {
      res.writeHead(404);
      return res.end();
    }
    res.writeHead(200, { 'content-type': MIME[path.extname(file)] || 'application/octet-stream', 'cache-control': 'no-store' });
    createReadStream(file).pipe(res);
  });
  return new Promise((resolve) => server.listen(0, '127.0.0.1', () => resolve(server)));
}

async function loadPlaywright() {
  // The app's own install first (the CLI does not depend on it).
  for (const from of [path.join(process.cwd(), 'noop.js'), import.meta.url]) {
    try {
      const req = createRequire(from);
      const m = await import(pathToFileURL(req.resolve('playwright')).href);
      // CommonJS: the named exports may only be on `default`.
      return m.chromium ? m : m.default;
    } catch {}
  }
  throw new Error('jsui pipelines needs Playwright: npm install -D playwright');
}

/**
 * Records the pipeline lists. Returns {[format]: {file, known, bytes}}.
 */
export async function recordPipelines({ dir, visits = [''], waitMs = 3000, script, formats = ['bgra8unorm', 'rgba8unorm'],
  browser: which = 'chrome', log = console.log }) {
  dir = path.resolve(dir);
  if (!existsSync(path.join(dir, 'index.html'))) throw new Error(`${dir}/index.html missing (jsui build <entry> --web ${dir})`);
  const { chromium } = await loadPlaywright();
  const flow = script ? (await import(pathToFileURL(path.resolve(script)).href)).default : null;
  const server = await serve(dir);
  const origin = `http://127.0.0.1:${server.address().port}`;
  const browser = await chromium.launch({
    ...(which === 'chrome' ? { channel: 'chrome' } : {}),
    headless: false,
    args: ['--enable-unsafe-webgpu', '--mute-audio', '--force-color-profile=srgb'],
  });
  const out = {};
  try {
    for (const format of formats) {
      // A fresh context: no list remembered from an earlier run.
      const ctx = await browser.newContext({ viewport: { width: 1440, height: 900 } });
      await ctx.addInitScript((f) => { globalThis.jsuiCanvasFormat = f; }, format);
      const page = await ctx.newPage();
      page.on('pageerror', (e) => log(`  page error: ${e.message}`));
      let loaded = false;
      const visit = async (where) => {
        if (loaded && where.startsWith('#')) {
          // A route of the running app: no reload.
          await page.evaluate((h) => { location.hash = h; }, where);
        } else {
          const url = where.startsWith('?') ? `${origin}/${where}` : new URL(where || '/', origin).href;
          loaded = true;
          await page.goto(url);
          await page.waitForFunction(() => window.jsui && window.jsui.renderer, null, { timeout: 30000 });
          const renderer = await page.evaluate(() => window.jsui.renderer);
          if (renderer !== 'webgpu') throw new Error(`the page renders with ${renderer}, not WebGPU (no GPU, or dist-gpu/ missing)`);
          await page.evaluate(() => window.jsui.whenIdle());
        }
        await page.waitForTimeout(waitMs);
      };
      for (const v of visits) {
        log(`  ${format}: ${v || '/'}`);
        await visit(v);
      }
      if (flow) await flow(page, { visit, origin, format });
      const bytes = await page.evaluate(async () => {
        const list = await window.jsui.savePipelines();
        return list ? Array.from(list) : null;
      });
      const stats = await page.evaluate(() => window.jsui.pipelineStats());
      if (!bytes) throw new Error('jsui.savePipelines() returned nothing (not WebGPU?)');
      const file = path.join(dir, `pipelines-${format}.bin`);
      await writeFile(file, Buffer.from(bytes));
      out[format] = { file, known: stats?.known ?? 0, bytes: bytes.length };
      log(`  ${format}: ${stats?.known ?? '?'} pipelines (${stats?.unserializable ?? 0} not recordable) -> ${file}`);
      await ctx.close();
    }
  } finally {
    await browser.close();
    server.close();
  }
  return out;
}

export async function pipelinesCommand(argv) {
  const { values, positionals } = parseArgs({
    args: argv,
    allowPositionals: true,
    options: {
      visit: { type: 'string', multiple: true },
      wait: { type: 'string' },
      script: { type: 'string' },
      formats: { type: 'string' },
      browser: { type: 'string' },
      help: { type: 'boolean', short: 'h' },
    },
  });
  if (values.help || positionals.length !== 1) {
    console.log(`usage:\n${PIPELINES_USAGE}`);
    return values.help ? 0 : 2;
  }
  const waitMs = values.wait != null ? Number(values.wait) : 3000;
  if (!Number.isFinite(waitMs) || waitMs < 0) {
    console.error(`jsui pipelines: invalid --wait ${values.wait}`);
    return 2;
  }
  try {
    await recordPipelines({
      dir: positionals[0],
      visits: values.visit?.length ? values.visit : [''],
      waitMs,
      script: values.script,
      formats: values.formats ? values.formats.split(',').map((f) => f.trim()) : undefined,
      browser: values.browser ?? 'chrome',
    });
    return 0;
  } catch (e) {
    console.error(`jsui pipelines: ${e?.message ?? e}`);
    return 1;
  }
}
