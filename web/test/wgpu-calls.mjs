// Counts WebGPU JS API calls per painted JSUI frame on the hover grid (WIP
// diagnostic; needs test/output/hover-bench/bundle.js from hover-bench.mjs).
// JSUI_CHROME=1: installed Chrome on the real GPU.
import path from 'node:path';
import fs from 'node:fs';
import http from 'node:http';
import { chromium } from 'playwright';
import { fileURLToPath } from 'node:url';
const WEB = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..'), FONTS = path.resolve(WEB, '../assets/fonts');
const MIME = { '.js': 'text/javascript', '.html': 'text/html', '.wasm': 'application/wasm', '.ttf': 'font/ttf' };
const server = http.createServer((req, res) => {
  const rel = decodeURIComponent(new URL(req.url, 'http://x').pathname);
  const file = rel.startsWith('/__fonts/') ? path.join(FONTS, path.basename(rel)) : path.join(WEB, rel);
  if (!fs.existsSync(file) || fs.statSync(file).isDirectory()) { res.writeHead(404); return res.end(); }
  res.writeHead(200, { 'content-type': MIME[path.extname(file)] || 'application/octet-stream' });
  fs.createReadStream(file).pipe(res);
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const ORIGIN = `http://127.0.0.1:${server.address().port}`;
const browser = process.env.JSUI_CHROME ? await chromium.launch({ channel: 'chrome', headless: false, args: ['--enable-unsafe-webgpu'] }) : await chromium.launch({ args: ['--enable-unsafe-swiftshader', '--use-angle=swiftshader', '--ignore-gpu-blocklist',
  '--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-vulkan=swiftshader'] });
const page = await browser.newPage({ viewport: { width: 1280, height: 720 }, deviceScaleFactor: 2 });
page.on('console', (m) => console.log('page:', m.text()));
await page.addInitScript(() => {
  window.__counts = new Map();
  window.__on = false;
  for (const C of ['GPUDevice', 'GPUQueue', 'GPUCommandEncoder', 'GPURenderPassEncoder', 'GPUComputePassEncoder', 'GPUTexture',
    'GPUCanvasContext', 'GPUBuffer', 'GPUCommandBuffer', 'GPURenderPipeline', 'GPUBindGroup', 'GPUTextureView', 'GPUSampler']) {
    const P = window[C]?.prototype;
    if (!P) continue;
    for (const k of Object.getOwnPropertyNames(P)) {
      const d = Object.getOwnPropertyDescriptor(P, k);
      if (typeof d.value !== 'function' || k === 'constructor') continue;
      const f = d.value;
      P[k] = function (...a) {
        if (window.__on) { const n = `${C}.${k}`; window.__counts.set(n, (window.__counts.get(n) || 0) + 1); }
        return f.apply(this, a);
      };
    }
  }
});
await page.goto(`${ORIGIN}/test/page.html`);
await page.waitForFunction(() => window.testReady);
await page.evaluate(async () => {
  const c = document.createElement('canvas'); c.style.width = '1280px'; c.style.height = '720px';
  document.body.appendChild(c);
  const j = window.j = await window.createJsui({ canvas: c, bundleUrl: 'output/hover-bench/bundle.js', renderer: 'webgpu',
    autoFocus: false, moduleUrl: new URL('../dist-gpu/jsui.js', location.href) });
  await Promise.race([j.whenIdle(), new Promise((r) => setTimeout(r, 8000))]);
});
const centre = (i) => ({ x: 40 + (i % 6) * 168 + 75, y: 40 + Math.floor(i / 6) * 188 + 85 });
for (let k = 0; k < 4; k++) { const p = centre(k); await page.mouse.move(p.x, p.y); await page.waitForTimeout(400); }
const r0 = await page.evaluate(() => { window.__on = true; return j.stats.renders; });
for (let k = 4; k < 10; k++) { const p = centre(k); await page.mouse.move(p.x, p.y); await page.waitForTimeout(400); }
const [r1, counts] = await page.evaluate(() => { window.__on = false; return [j.stats.renders, [...window.__counts]]; });
const per = await page.evaluate(async () => {
  const out = [], f = j.frame.bind(j);
  j.frame = (t) => { const r = j.stats.renders; window.__counts = new Map(); window.__on = true; f(t); window.__on = false;
    if (j.stats.renders !== r) out.push([...window.__counts].filter(([k]) => /draw$|drawIndexed|beginRenderPass|setBindGroup/.test(k)).map(([k, v]) => k.split('.')[1] + '=' + v).join(' ')); };
  window.__out = out;
});
for (let k = 10; k < 12; k++) { const p = centre(k); await page.mouse.move(p.x, p.y); await page.waitForTimeout(400); }
console.log((await page.evaluate(() => window.__out)).join('\n'));
await page.evaluate(() => { window.__out.length = 0; j.canvas.style.width = '1279px'; });
await page.waitForTimeout(600);
console.log('after resize (full repaint):\n' + (await page.evaluate(() => window.__out)).join('\n'));
const n = r1 - r0;
console.log(`painted frames ${n}`);
let tot = 0;
for (const [k, v] of counts.sort((a, b) => b[1] - a[1])) { tot += v; console.log(`${(v / n).toFixed(2).padStart(7)}  ${k}`); }
console.log(`${(tot / n).toFixed(2).padStart(7)}  total per frame`);
await browser.close(); server.close();
