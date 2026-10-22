#!/usr/bin/env node
// Site smoke test: serves a built site (site/dist; run `node site/scripts/build.mjs`
// first), opens every route in headless Chromium at desktop and phone sizes,
// waits for the runtime to go idle and checks that it reported no error and
// painted something. Screenshots land in site/test/output/.
//
//   node site/test/smoke.mjs [--dir site/dist] [route filters...]
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

const ROUTES = ['/', '/screens', '/screens/main-menu', '/screens/hud', '/screens/inventory', '/screens/battle-pass',
  '/screens/scoreboard', '/screens/dialogue', '/screens/settings', '/screens/loading', '/screens/notifications',
  '/interactions', '/input', '/motion', '/playground', '/embed'].filter((r) => !filters.length || filters.some((f) => r.includes(f)));
const SIZES = [{ name: 'desktop', width: 1440, height: 900, scale: 1 }, { name: 'phone', width: 390, height: 844, scale: 2 }];

const MIME = { '.js': 'text/javascript', '.html': 'text/html', '.wasm': 'application/wasm', '.json': 'application/json',
  '.png': 'image/png', '.jpg': 'image/jpeg', '.ttf': 'font/ttf' };
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

const browser = await chromium.launch({
  args: ['--enable-unsafe-swiftshader', '--use-angle=swiftshader', '--ignore-gpu-blocklist', '--force-color-profile=srgb'],
});
let failed = 0;
for (const size of SIZES) {
  const ctx = await browser.newContext({ viewport: { width: size.width, height: size.height }, deviceScaleFactor: size.scale });
  const page = await ctx.newPage();
  const logs = [];
  page.on('console', (m) => logs.push(m.text()));
  page.on('pageerror', (e) => logs.push('pageerror: ' + e.message));
  await page.goto(`${ORIGIN}/index.html`);
  await page.waitForFunction(() => window.jsui, null, { timeout: 60000 });
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
  await ctx.close();
}
await browser.close();
server.close();
console.log(failed ? `\n${failed} failed` : '\nall routes ok');
process.exit(failed ? 1 : 0);
