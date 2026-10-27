#!/usr/bin/env node
// The Three.js engine demo (site/demo) in headless Chromium on WebGPU
// (SwiftShader): the engine examples' --selftest checks, through the page's
// own input handling with real mouse / touch input, at desktop and phone
// sizes:
//   1. a click on empty ground is not consumed by the HUD: the game counts it;
//   2. a click on the Spawn button is consumed, and game:spawn adds a unit;
//   3. a click on a unit selects it and the HUD reports it (ui:selected);
//      a click on empty ground clears it; another selects again.
// Screenshots land in site/test/output/demo-*.png.
//
//   node site/scripts/build.mjs --wasm-gpu web/dist-gpu ... && node site/test/demo.mjs [--dir site/dist]
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromium } from 'playwright';

const SITE = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2);
const di = args.indexOf('--dir');
const DIR = path.resolve(di >= 0 ? args[di + 1] : path.join(SITE, 'dist'));
const OUT = path.join(SITE, 'test/output');
fs.mkdirSync(OUT, { recursive: true });
if (!fs.existsSync(path.join(DIR, 'demo/index.html'))) {
  console.error(`${DIR}/demo/index.html missing: run node site/scripts/build.mjs first`);
  process.exit(1);
}

const MIME = { '.js': 'text/javascript', '.html': 'text/html', '.wasm': 'application/wasm', '.json': 'application/json',
  '.bin': 'application/octet-stream' };
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
const base = `http://127.0.0.1:${server.address().port}`;

const browser = await chromium.launch({
  ...(fs.existsSync('/opt/pw-browsers/chromium') ? { executablePath: '/opt/pw-browsers/chromium' } : {}),
  args: ['--enable-unsafe-swiftshader', '--use-angle=swiftshader', '--ignore-gpu-blocklist', '--force-color-profile=srgb',
    '--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-vulkan=swiftshader'],
});
const SIZES = [
  { name: 'desktop', viewport: { width: 1280, height: 720 }, deviceScaleFactor: 1 },
  { name: 'phone', viewport: { width: 390, height: 844 }, deviceScaleFactor: 2, isMobile: true, hasTouch: true },
  { name: 'phone-landscape', viewport: { width: 844, height: 390 }, deviceScaleFactor: 2, isMobile: true, hasTouch: true },
];
let failed = 0;
for (const size of SIZES) {
  const { name, ...options } = size;
  const ctx = await browser.newContext(options);
  const page = await ctx.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(e.message));
  page.on('console', (m) => m.type() === 'error' && !/Failed to load resource/.test(m.text()) && errors.push(m.text()));
  const touch = !!options.hasTouch;
  const tap = async (x, y) => (touch ? page.touchscreen.tap(x, y) : page.mouse.click(x, y));
  // Frames on SwiftShader are slow: wait for a few after each input.
  const frames = async (n = 3) => {
    const f0 = await page.evaluate(() => window.demoStats.frames);
    await page.waitForFunction((f) => window.demoStats.frames >= f, f0 + n, { timeout: 30000 });
  };
  const state = () => page.evaluate(() => JSON.parse(JSON.stringify(window.demo.state)));
  const check = (ok, what) => {
    if (!ok) throw new Error(what);
    console.log(`demo ${name}: ${what}`);
  };
  try {
    await page.goto(`${base}/demo/`);
    await page.waitForFunction(() => window.demo?.anchors && window.demoStats?.frames > 5, null, { timeout: 90000 });
    const err = await page.evaluate(() => document.getElementById('err').style.display === 'block'
      && document.getElementById('err').textContent);
    if (err) throw new Error(`the page reports: ${err}`);
    await page.evaluate(() => { window.demo.freeze = true; });
    await frames();

    // A point with neither HUD nor a unit under it.
    const empty = await page.evaluate(() => {
      const { hud, pick } = window.demo;
      for (let gy = 3; gy < 10; gy++) {
        for (let gx = 2; gx < 15; gx++) {
          const x = innerWidth * gx / 16, y = innerHeight * gy / 12;
          if (hud.hitTest(x, y) === 0 && pick(x, y) < 0) return { x, y };
        }
      }
      return null;
    });
    check(empty, 'found empty ground');
    let s = await state();
    await tap(empty.x, empty.y);
    await frames();
    check((await state()).passed.clicks === s.passed.clicks + 1, 'a click on empty ground reached the game');

    const spawn = await page.evaluate(() => window.demo.anchors.spawn);
    s = await state();
    await tap(spawn.x + spawn.width / 2, spawn.y + spawn.height / 2);
    await page.waitForFunction((n) => window.demo.state.entities === n, s.entities + 1, { timeout: 30000 })
      .catch(() => {});
    const after = await state();
    check(after.passed.clicks === s.passed.clicks, 'the Spawn button click was consumed by the HUD');
    check(after.entities === s.entities + 1, `game:spawn reached the game (${s.entities} -> ${after.entities} units)`);

    for (const round of [0, 1, 2]) {
      await frames();
      let want = -1, at = empty;
      if (round !== 1) {
        const unit = await page.evaluate(() => {
          const { hud, pick, unitOnScreen, state } = window.demo;
          for (let i = state.entities - 1; i >= 0; i--) {
            const p = unitOnScreen(i);
            if (!p || p.x < 40 || p.y < 40 || p.x > innerWidth - 40 || p.y > innerHeight - 40) continue;
            if (hud.hitTest(p.x, p.y) === 0 && pick(p.x, p.y) === i) return { i, ...p };
          }
          return null;
        });
        check(unit, 'found a unit to click');
        want = unit.i;
        at = unit;
      }
      await tap(at.x, at.y);
      await page.waitForFunction((w) => window.demo.state.selected === w && window.demo.reported === w, want,
        { timeout: 30000 }).catch(() => {});
      const now = await state();
      check(now.selected === want && (await page.evaluate(() => window.demo.reported)) === want,
        want >= 0 ? `a click on unit ${want} selected it; the HUD reports it` : 'a click on empty ground cleared the selection');
    }
    await frames(4);
    await page.screenshot({ path: path.join(OUT, `demo-${name}.png`) });
    if (errors.length) throw new Error(`errors: ${errors.join(' | ')}`);
    console.log(`demo ${name}: ok`);
  } catch (e) {
    failed++;
    console.error(`demo ${name}: FAILED: ${e.message}`);
    await page.screenshot({ path: path.join(OUT, `demo-${name}-failed.png`) }).catch(() => {});
  }
  await ctx.close();
}
await browser.close();
server.close();
process.exit(failed ? 1 : 0);
