// Hover benchmark: what a hover costs JSUI's web host against Chrome's own
// CSS :hover, on the same card grid (web/test/bench-apps/hover-grid.jsx with
// native state styles, and hover-grid.html with CSS transitions and no
// script). Real mouse input moves from card to card, one every --dwell ms;
// each move starts a 120 ms transition out of one card and into the next.
//
// Per renderer, over the hover loop (traced):
//   busy        CPU ms per hover of the threads that make frames: the page's
//               main thread, compositor, raster workers, the GPU process'
//               main thread and viz (Chrome's own work for CSS lands on the
//               compositor / raster / GPU threads, JSUI's on the main thread).
//   frame       JSUI only: jsui.frame() main-thread time per painted frame
//               (mean / p95 / max), and painted frames per hover.
//
//   node web/test/hover-bench.mjs [--real-gpu] [--renderers webgpu,webgl,css] [--hovers 24] [--dwell 300]
//                                 [--dpr 2] [--dist dist-gpu] [--json out.json]
//
// Default: headless Chromium on SwiftShader (as frame-bench.mjs); --real-gpu
// the installed Chrome, headed, on the machine's GPU. Keep its window in front.
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromium } from 'playwright';
import { build } from '@orktes/jsui-cli';

const WEB = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const ROOT = path.resolve(WEB, '..');
const args = process.argv.slice(2);
const opt = (name, def) => { const i = args.indexOf(name); return i >= 0 ? args[i + 1] : def; };
const REAL_GPU = args.includes('--real-gpu');
const RENDERERS = opt('--renderers', 'webgpu,css').split(',');
const HOVERS = Number(opt('--hovers', 24));
const DWELL = Number(opt('--dwell', 300));
const DPR = Number(opt('--dpr', 2));
const DIST = opt('--dist', 'dist-gpu');
const JSON_OUT = opt('--json', null);
const W = 1280, H = 720;

const OUT = path.join(WEB, 'test/output/hover-bench');
fs.mkdirSync(OUT, { recursive: true });
await build({ entry: path.join(WEB, 'test/bench-apps/hover-grid.jsx'), outfile: path.join(OUT, 'bundle.js'), minify: true });
const FONTS = path.join(ROOT, 'assets/fonts');

const MIME = { '.js': 'text/javascript', '.html': 'text/html', '.wasm': 'application/wasm', '.json': 'application/json',
  '.png': 'image/png', '.ttf': 'font/ttf' };
const server = http.createServer((req, res) => {
  const rel = decodeURIComponent(new URL(req.url, 'http://x').pathname);
  const file = rel.startsWith('/__fonts/') ? path.join(FONTS, path.basename(rel)) : path.normalize(path.join(WEB, rel));
  if (!(file.startsWith(WEB) || file.startsWith(FONTS)) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) {
    res.writeHead(404);
    return res.end();
  }
  res.writeHead(200, { 'content-type': MIME[path.extname(file)] || 'application/octet-stream' });
  fs.createReadStream(file).pipe(res);
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const ORIGIN = `http://127.0.0.1:${server.address().port}`;

const COMMON_ARGS = ['--force-color-profile=srgb', '--enable-webgpu-developer-features'];
const browser = REAL_GPU
  ? await chromium.launch({ channel: 'chrome', headless: false, args: COMMON_ARGS })
  : await chromium.launch({
    executablePath: process.env.JSUI_CHROMIUM || undefined,  // a Chromium other than Playwright's own
    args: [...COMMON_ARGS, '--enable-unsafe-swiftshader', '--use-angle=swiftshader', '--ignore-gpu-blocklist',
      '--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-vulkan=swiftshader'],
  });
console.log(`hover-grid ${W}x${H} @${DPR}x, ${HOVERS} hovers every ${DWELL} ms, web/${DIST}, ` +
  `${REAL_GPU ? 'Chrome (real GPU)' : 'headless Chromium (SwiftShader)'} ${browser.version()}`);

// Busy time per thread from a Chrome trace (as frame-bench.mjs): the union of
// the slices on each thread, nested slices counted once.
const TRACE_CATEGORIES = ['toplevel', 'devtools.timeline', 'disabled-by-default-devtools.timeline', 'gpu', 'viz', 'cc',
  'blink', 'v8.execute'];
function threadBusy(json) {
  const events = (JSON.parse(json).traceEvents) ?? [];
  const names = new Map(), procs = new Map(), slices = new Map(), open = new Map();
  for (const e of events) {
    const k = `${e.pid}:${e.tid}`;
    if (e.ph === 'M' && e.name === 'thread_name') names.set(k, e.args?.name ?? '?');
    else if (e.ph === 'M' && e.name === 'process_name') procs.set(e.pid, e.args?.name ?? '?');
    else if (e.ph === 'X' && e.dur > 0) (slices.get(k) ?? slices.set(k, []).get(k)).push([e.ts, e.ts + e.dur]);
    else if (e.ph === 'B') (open.get(k) ?? open.set(k, []).get(k)).push(e.ts);
    else if (e.ph === 'E') {
      const t0 = open.get(k)?.pop();
      if (t0 != null) (slices.get(k) ?? slices.set(k, []).get(k)).push([t0, e.ts]);
    }
  }
  const out = {};
  for (const [k, s] of slices) {
    s.sort((a, b) => a[0] - b[0]);
    let busy = 0, end = -Infinity;
    for (const [a, b] of s) {
      if (b <= end) continue;
      busy += b - Math.max(a, end);
      end = b;
    }
    const pid = Number(k.split(':')[0]);
    const proc = (procs.get(pid) ?? '').replace(/ Process$/, '');
    const name = `${/gpu/i.test(proc) ? 'GPU ' : /renderer/i.test(proc) ? '' : `${proc} `}` +
      `${(names.get(k) ?? '?').replace(/\d+$/, '')}`;
    if (/renderer/i.test(proc)) out[name] = Math.max(out[name] ?? 0, busy / 1000);
    else out[name] = (out[name] ?? 0) + busy / 1000;
  }
  return out;
}
const THREADS = [['CrRendererMain', 'main'], ['Compositor', 'compositor'], ['CompositorTileWorker', 'raster'],
  ['GPU CrGpuMain', 'gpu-main'], ['GPU VizCompositorThread', 'viz']];

const sorted = (a) => [...a].sort((x, y) => x - y);
const avg = (a) => a.reduce((x, y) => x + y, 0) / Math.max(1, a.length);
const pct = (s, p) => s[Math.min(s.length - 1, Math.floor(s.length * p))];
const f2 = (v) => (v == null || Number.isNaN(v) ? '-' : v.toFixed(2));

// Card centres in CSS px (the grid's layout: 40 px padding, 150x170 cards,
// 18 px gaps, 6 per row), visited in a zigzag so each move is to a neighbour.
const centre = (i) => ({ x: 40 + (i % 6) * 168 + 75, y: 40 + Math.floor(i / 6) * 188 + 85 });
const order = [];
for (let r = 0; r < 4; r++) for (let c = 0; c < 6; c++) order.push(r * 6 + (r % 2 ? 5 - c : c));

async function open(renderer) {
  const context = await browser.newContext({ viewport: { width: W, height: H }, deviceScaleFactor: DPR });
  const page = await context.newPage();
  // A page that threw (a module that failed to mount) measures nothing.
  page.on('pageerror', (e) => { throw new Error(`${renderer}: uncaught error on the page: ${e.message}`); });
  if (renderer === 'css') {
    await page.goto(`${ORIGIN}/test/bench-apps/hover-grid.html`);
    await page.evaluate(() => document.fonts.ready);
  } else {
    await page.goto(`${ORIGIN}/test/page.html`);
    await page.waitForFunction(() => window.testReady);
    const name = await page.evaluate(async ({ renderer, dist, w, h }) => {
      document.body.style.background = '#0d1322';
      const c = document.createElement('canvas');
      c.style.width = `${w}px`;
      c.style.height = `${h}px`;
      document.body.appendChild(c);
      const j = window.j = await window.createJsui({ canvas: c, bundleUrl: 'output/hover-bench/bundle.js', renderer,
        autoFocus: false, moduleUrl: new URL(`../${dist}/jsui.js`, location.href) });
      j.on('log', () => {});
      await Promise.race([j.whenIdle(), new Promise((r) => setTimeout(r, 8000))]);
      return j.renderer;
    }, { renderer, dist: DIST, w: W, h: H });
    if (name !== renderer) console.log(`  (asked for ${renderer}, got ${name})`);
  }
  await page.bringToFront();
  await page.mouse.move(8, 8);  // outside every card
  await page.waitForTimeout(1500);
  return { context, page };
}

const results = [];
for (const renderer of RENDERERS) {
  const { context, page } = await open(renderer);
  const isCss = renderer === 'css';
  if (!isCss) {
    // jsui.frame() main-thread time of every painted frame.
    await page.evaluate(() => {
      const j = window.j, frame = j.frame.bind(j);
      window.__cpu = [];
      window.__on = false;
      j.frame = (now) => {
        const r0 = j.stats.renders, a = performance.now();
        frame(now);
        if (window.__on && j.stats.renders !== r0) window.__cpu.push(performance.now() - a);
      };
    });
  }
  // One untraced pass first: warms shader pipelines, glyphs and shadow masks
  // on both sides.
  for (let k = 0; k < 6; k++) {
    const p = centre(order[k]);
    await page.mouse.move(p.x, p.y);
    await page.waitForTimeout(DWELL);
  }
  await page.mouse.move(8, 8);
  await page.waitForTimeout(500);

  await browser.startTracing(page, { categories: TRACE_CATEGORIES });
  if (!isCss) await page.evaluate(() => { window.__on = true; });
  const t0 = Date.now();
  for (let k = 0; k < HOVERS; k++) {
    const p = centre(order[k % order.length]);
    await page.mouse.move(p.x, p.y);
    await page.waitForTimeout(DWELL);
  }
  const ms = Date.now() - t0;
  const frames = isCss ? null : await page.evaluate(() => { window.__on = false; return window.__cpu; });
  const busy = threadBusy((await browser.stopTracing()).toString());
  const threads = Object.fromEntries(THREADS.map(([k, label]) => [label, (busy[k] ?? 0) / HOVERS]));
  const total = Object.values(threads).reduce((a, b) => a + b, 0);
  const r = { renderer, hovers: HOVERS, windowMs: ms, perHover: threads, totalPerHover: total };
  if (frames) {
    const s = sorted(frames);
    r.frames = { perHover: frames.length / HOVERS, mean: avg(frames), p95: pct(s, 0.95), max: s[s.length - 1],
      perHoverMs: frames.reduce((a, b) => a + b, 0) / HOVERS };
  }
  results.push(r);
  await context.close();
}

console.log('\nCPU ms per hover (thread busy time from the trace):');
console.log(`  ${'renderer'.padEnd(9)} ${THREADS.map(([, l]) => l.padStart(10)).join(' ')} ${'total'.padStart(10)}`);
for (const r of results)
  console.log(`  ${r.renderer.padEnd(9)} ${THREADS.map(([, l]) => f2(r.perHover[l]).padStart(10)).join(' ')} ` +
    `${f2(r.totalPerHover).padStart(10)}`);
const jsui = results.filter((r) => r.frames);
if (jsui.length) {
  console.log('\nJSUI jsui.frame() per painted frame (ms):');
  for (const r of jsui)
    console.log(`  ${r.renderer.padEnd(9)} frames/hover ${f2(r.frames.perHover)}  mean ${f2(r.frames.mean)}  ` +
      `p95 ${f2(r.frames.p95)}  max ${f2(r.frames.max)}  sum/hover ${f2(r.frames.perHoverMs)}`);
}
if (JSON_OUT) fs.writeFileSync(JSON_OUT, JSON.stringify({ realGpu: REAL_GPU, dpr: DPR, results }, null, 2));
await browser.close();
server.close();
