#!/usr/bin/env node
// Site smoke test: serves a built site (site/dist; run `node site/scripts/build.mjs`
// first), opens every route in headless Chromium at desktop and phone sizes,
// waits for the runtime to go idle and checks that it reported no error and
// painted something. Screenshots land in site/test/output/. At desktop size
// it then unlocks audio with a click and checks that the main menu's music
// plays through Web Audio without errors.
//
//   node site/test/smoke.mjs [--dir site/dist] [route filters...]
//   SITE_JS=quickjs node site/test/smoke.mjs     (the app on QuickJS in the WASM
//       module, ?js=quickjs; default: the browser's own JS engine)
//   SITE_REAL_GPU=1 node site/test/smoke.mjs     (the installed Google Chrome,
//       headed, on the machine's GPU)
//   SITE_WORKER=0|1 node site/test/smoke.mjs     (?worker=0|1: force main-thread or
//       worker mode; default: the page's own choice, worker mode where the worker
//       has WebGPU)
//   SITE_EXPECT_WORKER=0|1                       (fail unless the runtime runs on the
//       page's thread / in a worker)
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromium } from 'playwright';

const SITE = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2);
const di = args.indexOf('--dir');
const DIR = path.resolve(di >= 0 ? args[di + 1] : path.join(SITE, 'dist'));
const filters = args.filter((a, i) => !a.startsWith('--') && args[i - 1] !== '--dir');
const OUT = path.join(SITE, 'test/output');
fs.mkdirSync(OUT, { recursive: true });
if (!fs.existsSync(path.join(DIR, 'index.html'))) {
  console.error(`${DIR}/index.html missing: run node site/scripts/build.mjs first`);
  process.exit(1);
}
// Crawlers and agents: the static files and the page's text content (the UI itself is a canvas).
for (const f of ['llms.txt', 'og.png']) {
  if (!fs.existsSync(path.join(DIR, f))) {
    console.error(`${DIR}/${f} missing (site/public)`);
    process.exit(1);
  }
}
if (!/<main id="about"[\s\S]*github\.com\/orktes\/jsui/.test(fs.readFileSync(path.join(DIR, 'index.html'), 'utf8'))) {
  console.error('index.html: the crawlable #about content is missing');
  process.exit(1);
}

const ROUTES = ['/', '/examples', '/examples/main-menu', '/examples/hud', '/examples/inventory', '/examples/battle-pass',
  '/examples/scoreboard', '/examples/dialogue', '/examples/settings', '/examples/loading', '/examples/notifications',
  '/examples/cinematic', '/examples/victory', '/examples/rift', '/examples/hud/full', '/screens/dialogue', '/vector',
  '/use-cases', '/features', '/features/interactions', '/features/input', '/features/motion', '/features/vector', '/playground', '/embed'].filter((r) => !filters.length || filters.some((f) => r.includes(f)));
// Links from before the site had sections, and the path the app replaces them with.
const REDIRECTS = { '/screens/dialogue': '/examples/dialogue', '/vector': '/features/vector' };
const SIZES = [{ name: 'desktop', width: 1440, height: 900, scale: 1 }, { name: 'phone', width: 390, height: 844, scale: 2 }];

const MIME = { '.js': 'text/javascript', '.html': 'text/html', '.wasm': 'application/wasm', '.json': 'application/json',
  '.png': 'image/png', '.jpg': 'image/jpeg', '.ttf': 'font/ttf', '.wav': 'audio/wav' };
const server = http.createServer((req, res) => {
  const p = decodeURIComponent(new URL(req.url, 'http://x').pathname);
  const file = path.normalize(path.join(DIR, p.endsWith('/') ? p + 'index.html' : p));
  if (!file.startsWith(DIR) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) {
    res.writeHead(404);
    return res.end();
  }
  res.writeHead(200, { 'content-type': MIME[path.extname(file)] || 'application/octet-stream' });
  fs.createReadStream(file).pipe(res);
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const ORIGIN = `http://127.0.0.1:${server.address().port}`;
// SITE_WEBGPU=1: expose WebGPU to headless Chromium (a SwiftShader-backed
// fallback adapter on machines without a GPU, such as CI runners).
// Vulkan-backed so canvas presentation works headless.
const WEBGPU_FLAGS = ['--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-vulkan=swiftshader'];

const JS = process.env.SITE_JS || '';
const WORKER = process.env.SITE_WORKER || '';
const EXPECT_WORKER = process.env.SITE_EXPECT_WORKER || '';
const browser = process.env.SITE_REAL_GPU
  ? await chromium.launch({ channel: 'chrome', headless: false, args: ['--force-color-profile=srgb'] })
  : await chromium.launch({
    args: ['--enable-unsafe-swiftshader', '--use-angle=swiftshader', '--ignore-gpu-blocklist', '--force-color-profile=srgb',
      ...(process.env.SITE_WEBGPU ? WEBGPU_FLAGS : [])],
  });
let failed = 0;
for (const size of SIZES) {
  const ctx = await browser.newContext({ viewport: { width: size.width, height: size.height }, deviceScaleFactor: size.scale });
  // Playwright's evaluate() counts as a user activation: hide it, so the
  // routes run with audio locked, as on a first visit.
  await ctx.addInitScript(() => Object.defineProperty(navigator, 'userActivation', { value: { hasBeenActive: false } }));
  const page = await ctx.newPage();
  const logs = [];
  page.on('console', (m) => logs.push(m.text()));
  page.on('pageerror', (e) => logs.push('pageerror: ' + e.message));
  const requests = [];
  page.on('request', (r) => requests.push(r.url()));
  // autoplay=0: the cinematic screen streams its trailer only once played
  // (CI has no business fetching video, and a playing video never goes idle):
  // it must look finished with its own poster art.
  await page.goto(`${ORIGIN}/index.html?autoplay=0${JS ? '&js=' + encodeURIComponent(JS) : ''}` +
    (WORKER ? '&worker=' + encodeURIComponent(WORKER) : ''));
  await page.waitForFunction(() => window.jsui, null, { timeout: 60000 });
  // Every script, module, wasm and JSON the page loads is versioned (?v=<hash>,
  // site/scripts/build.mjs), so caches cannot mix files of two deploys.
  const unversioned = requests.filter((u) => u.startsWith(ORIGIN) && /\.(m?js|wasm|json)$/.test(new URL(u).pathname) &&
    !/^[0-9a-f]{16}$/.test(new URL(u).searchParams.get('v') ?? ''));
  if (unversioned.length) {
    failed++;
    console.log(`  FAIL  ${size.name}: unversioned URLs: ${unversioned.map((u) => u.slice(ORIGIN.length)).join(' ')}`);
  }
  // With a GPU module deployed the site must draw through Skia on the GPU:
  // the CPU raster is far too slow for full-page canvases. With the WebGPU
  // module and a browser that has WebGPU, on WebGPU (SITE_EXPECT_RENDERER
  // overrides the expectation, e.g. 'webgpu' in the wasm-webgpu CI job).
  const modules = JSON.parse(fs.readFileSync(path.join(DIR, 'modules.json'), 'utf8'));
  const renderer = await page.evaluate(() => window.jsui.renderer);
  const want = process.env.SITE_EXPECT_RENDERER;
  const ok = want ? renderer === want : !(modules.gpu || modules.webgpu) || renderer === 'webgpu' || renderer.startsWith('gpu');
  if (!ok) {
    failed++;
    console.log(`  FAIL  ${size.name}: renderer is ${renderer}, expected ${want || 'a GPU renderer'}`);
  } else console.log(`  ${size.name}: renderer ${renderer}`);
  // Worker mode (index.html's default where the worker has WebGPU) or the
  // page's main thread.
  const inWorker = await page.evaluate(() => !!window.jsui.worker);
  if (EXPECT_WORKER && inWorker !== (EXPECT_WORKER === '1')) {
    failed++;
    console.log(`  FAIL  ${size.name}: the runtime runs ${inWorker ? 'in a worker' : 'on the main thread'}, expected ` +
      (EXPECT_WORKER === '1' ? 'worker mode' : 'main-thread mode'));
  } else console.log(`  ${size.name}: ${inWorker ? 'worker mode' : 'main-thread mode'}`);
  // The browser's own JS engine unless SITE_JS asks for another.
  const engine = await page.evaluate(() => window.jsui.jsEngine);
  if (engine !== (JS || 'host')) {
    failed++;
    console.log(`  FAIL  ${size.name}: JS engine is ${engine}, expected ${JS || 'host'}`);
  } else console.log(`  ${size.name}: JS engine ${engine}`);
  // The splash stays up while the pipeline lists build and the intro warms up
  // (up to 10 s, then it hides anyway): screenshots wait until it is gone.
  try {
    await page.evaluate(() => window.jsui.whenPipelinesReady?.());
    await page.waitForFunction(() => !document.getElementById('splash'), null, { timeout: 15000 });
  } catch (e) {
    failed++;
    console.log(`  FAIL  ${size.name}: the loading splash never went away (${e.message.split('\n')[0]})`);
  }
  for (const route of ROUTES) {
    const name = `${size.name} ${route}`;
    logs.length = 0;
    try {
      const t0 = Date.now();
      await page.evaluate((r) => { location.hash = r; }, route);
      // Two idle waits: the route message lands, then its resources load.
      await page.evaluate(() => window.jsui.whenIdle());
      await page.waitForTimeout(400);
      await page.evaluate(() => window.jsui.whenIdle());
      const err = await page.evaluate(() => window.jsui.lastError);
      if (err) throw new Error('runtime error: ' + err);
      if (REDIRECTS[route]) {
        const at = await page.evaluate(() => decodeURIComponent(location.hash.slice(1)));
        if (at !== REDIRECTS[route]) throw new Error(`expected a redirect to ${REDIRECTS[route]}, at ${at}`);
      }
      const file = path.join(OUT, `${size.name}${route.replace(/\//g, '_') || '_home'}.png`);
      const png = await page.screenshot({ path: file });
      // A blank page compresses to almost nothing.
      if (png.length < 12000) throw new Error(`looks blank (${png.length} byte screenshot)`);
      const bad = logs.filter((l) => /error|exception/i.test(l));
      if (bad.length) throw new Error('console: ' + bad.join(' | '));
      console.log(`  ok    ${name} (${Date.now() - t0} ms)`);
    } catch (e) {
      failed++;
      console.log(`  FAIL  ${name}: ${e.message.split('\n').slice(0, 6).join('\n        ')}`);
    }
  }
  // Audio: locked until a gesture (UI sounds were dropped above without
  // errors); a click on an empty spot unlocks it and the menu music starts.
  if (size.name === 'desktop' && (!filters.length || filters.some((f) => '/examples/main-menu'.includes(f)))) {
    logs.length = 0;
    try {
      await page.evaluate(() => { location.hash = '/examples/main-menu'; });
      await page.evaluate(() => window.jsui.whenIdle());
      if (await page.evaluate(() => window.jsui.audio.unlocked)) throw new Error('audio unlocked before any gesture');
      await page.mouse.click(4, size.height - 4);
      await page.waitForFunction(() => window.jsui.audio.unlocked, null, { timeout: 5000 });
      await page.waitForFunction(() => window.jsui.audio.stats.playing >= 1, null, { timeout: 10000 });
      const stats = await page.evaluate(() => window.jsui.audio.stats);
      const err = await page.evaluate(() => window.jsui.lastError);
      if (err) throw new Error('runtime error: ' + err);
      const bad = logs.filter((l) => /error|exception|could not/i.test(l));
      if (bad.length) throw new Error('console: ' + bad.join(' | '));
      console.log(`  ok    ${size.name} audio (unlocked by a click; ${stats.playing} playing, ${stats.sounds} sounds decoded)`);
    } catch (e) {
      failed++;
      console.log(`  FAIL  ${size.name} audio: ${e.message.split('\n').slice(0, 6).join('\n        ')}`);
    }
  }
  await ctx.close();
}
await browser.close();
server.close();
console.log(failed ? `\n${failed} failed` : '\nall routes ok');
process.exit(failed ? 1 : 0);
