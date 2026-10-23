#!/usr/bin/env node
// First-visit hitch: serves a built site (site/dist), lets the home page
// settle (and preload what it preloads), then opens a route and records the
// browser's frame times while it first shows. Prints the longest frames.
// Meant for a machine with a real GPU and Chrome:
//
//   node site/test/first-visit.mjs [--dir site/dist] [--route /screens/rift] [--settle 6000]
//     [--webgl] [--headless] [--runs 3]
//
// Each run is a fresh browser (nothing compiled or cached from the last).
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromium } from 'playwright';

const SITE = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2);
const opt = (name, def) => {
  const i = args.indexOf(name);
  return i >= 0 ? args[i + 1] : def;
};
const DIR = path.resolve(opt('--dir', path.join(SITE, 'dist')));
const ROUTE = opt('--route', '/screens/rift');
const SETTLE = Number(opt('--settle', 6000));
const RUNS = Number(opt('--runs', 3));
const WEBGL = args.includes('--webgl');

const MIME = { '.js': 'text/javascript', '.html': 'text/html', '.wasm': 'application/wasm', '.json': 'application/json',
  '.png': 'image/png', '.jpg': 'image/jpeg', '.ttf': 'font/ttf', '.mp4': 'video/mp4' };
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

const results = [];
for (let run = 0; run < RUNS; run++) {
  const browser = await chromium.launch({
    channel: 'chrome',
    headless: args.includes('--headless'),
    args: ['--enable-unsafe-webgpu', '--ignore-gpu-blocklist', ...(WEBGL ? ['--disable-features=WebGPU'] : [])],
  });
  const page = await browser.newPage({ viewport: { width: 1440, height: 900 } });
  await page.goto(`${ORIGIN}/index.html?autoplay=0`);
  await page.waitForFunction(() => window.jsui, null, { timeout: 60000 });
  const renderer = await page.evaluate(() => window.jsui.renderer);
  await page.evaluate(() => window.jsui.whenIdle());
  await page.waitForTimeout(SETTLE);
  await page.evaluate(() => window.jsui.whenIdle());
  const frames = await page.evaluate(
    (route) =>
      new Promise((resolve) => {
        const out = [];
        let last = performance.now();
        const t0 = last;
        const tick = (t) => {
          out.push(t - last);
          last = t;
          if (t - t0 < 2500) requestAnimationFrame(tick);
          else resolve(out);
        };
        requestAnimationFrame((t) => {
          last = t;
          location.hash = route;
          requestAnimationFrame(tick);
        });
      }),
    ROUTE,
  );
  const sorted = [...frames].sort((a, b) => b - a);
  results.push(sorted[0]);
  console.log(
    `run ${run + 1} (${renderer}): ${frames.length} frames, longest ${sorted
      .slice(0, 4)
      .map((f) => f.toFixed(1))
      .join(' / ')} ms, ${frames.filter((f) => f > 50).length} over 50 ms`,
  );
  await browser.close();
}
results.sort((a, b) => a - b);
console.log(`median longest frame: ${results[Math.floor(results.length / 2)].toFixed(1)} ms`);
server.close();
