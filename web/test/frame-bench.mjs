// Frame-time benchmark of the web host, per renderer: bundles a JSX app
// (default examples/showcase/interactions.jsx, which repaints every frame),
// mounts it at 1280x720 and measures, per renderer:
//
//   interval  what the user feels: the host runs its own requestAnimationFrame
//             loop and we record the rAF-to-rAF interval in the steady state
//             (mean / p50 / p95 / p99, and frames that took more than 1.5x the
//             display's vsync, "late"), plus the main-thread time of each frame
//             (jsui.frame(): tick + paint + submitting the GPU work). WebGPU
//             also reports when the GPU finished each frame
//             (queue.onSubmittedWorkDone(), observed without waiting on it).
//   finish    the old metric (secondary): frames stepped by hand, each timed
//             until the GPU finished it (WebGPU: await onSubmittedWorkDone();
//             WebGL / CPU-with-WebGL-blit: gl.finish()). It includes the
//             round trip to the GPU process, which the real loop never waits for.
//   gpu calls WebGPU only: WebGPU API calls per frame (passes with their
//             load / store ops, draws, buffer writes, bind groups, ...), counted
//             in a separate page (web/test/gpu-count.js) so the timings above
//             carry no instrumentation.
//
//   node web/test/frame-bench.mjs [--dist dist-gpu] [--frames 120] [--renderers webgpu,webgl,cpu]
//                                 [--width 1280 --height 720 --dpr 1] [--real-gpu] [--no-counts]
//                                 [--json out.json] [app.jsx]
//
// Default: headless Chromium from Playwright, where WebGL runs on SwiftShader
// and WebGPU on its SwiftShader fallback adapter (--enable-unsafe-webgpu), so
// the numbers compare the paths on a CPU-emulated GPU, not real hardware.
// --real-gpu: the installed Google Chrome (channel 'chrome'), headed, with no
// SwiftShader flags, i.e. the machine's GPU (Metal on a Mac). Keep the window
// visible and in front while it runs (hidden windows get throttled rAF).
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromium } from 'playwright';
import { build } from '@jsui/cli';

const WEB = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const ROOT = path.resolve(WEB, '..');
const args = process.argv.slice(2);
const FLAGS = new Set(['--real-gpu', '--no-counts']);
const opt = (name, def) => { const i = args.indexOf(name); return i >= 0 ? args[i + 1] : def; };
const DIST = opt('--dist', 'dist-gpu');
const N = Number(opt('--frames', 120));
const W = Number(opt('--width', 1280)), H = Number(opt('--height', 720)), DPR = Number(opt('--dpr', 1));
const RENDERERS = opt('--renderers', 'webgpu,webgl,cpu').split(',');
const REAL_GPU = args.includes('--real-gpu');
const COUNTS = !args.includes('--no-counts');
const JSON_OUT = opt('--json', null);
const entry = args.find((a, i) => !a.startsWith('--') && !(args[i - 1]?.startsWith('--') && !FLAGS.has(args[i - 1]))) ??
  path.join(ROOT, 'examples/showcase/interactions.jsx');
const WARMUP = 30;

const OUT = path.join(WEB, 'test/output/bench');
fs.mkdirSync(OUT, { recursive: true });
await build({ entry, outfile: path.join(OUT, 'bundle.js'), minify: true });

const MIME = { '.js': 'text/javascript', '.html': 'text/html', '.wasm': 'application/wasm', '.json': 'application/json',
  '.png': 'image/png', '.jpg': 'image/jpeg', '.ttf': 'font/ttf' };
const server = http.createServer((req, res) => {
  const file = path.normalize(path.join(WEB, decodeURIComponent(new URL(req.url, 'http://x').pathname)));
  if (!file.startsWith(WEB) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) {
    res.writeHead(404);
    return res.end();
  }
  res.writeHead(200, { 'content-type': MIME[path.extname(file)] || 'application/octet-stream' });
  fs.createReadStream(file).pipe(res);
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const ORIGIN = `http://127.0.0.1:${server.address().port}`;

const browser = REAL_GPU
  ? await chromium.launch({ channel: 'chrome', headless: false, args: ['--force-color-profile=srgb'] })
  : await chromium.launch({
    args: ['--enable-unsafe-swiftshader', '--use-angle=swiftshader', '--ignore-gpu-blocklist', '--force-color-profile=srgb',
      '--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-vulkan=swiftshader'],
  });
console.log(`${path.relative(ROOT, entry)} ${W}x${H} @${DPR}x, web/${DIST}, ${N} frames, ` +
  `${REAL_GPU ? 'Chrome (real GPU)' : 'headless Chromium (SwiftShader)'} ${browser.version()}`);

// Mounts the app on a fresh page; the host's own rAF loop is running.
async function open(renderer, { counts = false } = {}) {
  const context = await browser.newContext({ viewport: { width: W, height: H }, deviceScaleFactor: DPR });
  if (counts) await context.addInitScript({ path: path.join(WEB, 'test/gpu-count.js') });
  const page = await context.newPage();
  await page.goto(`${ORIGIN}/test/page.html`);
  await page.waitForFunction(() => window.testReady);
  await page.bringToFront();
  const info = await page.evaluate(async ({ renderer, dist, w, h }) => {
    const c = document.createElement('canvas');
    c.style.width = `${w}px`;
    c.style.height = `${h}px`;
    document.body.appendChild(c);
    const j = window.j = await window.createJsui({ canvas: c, bundleUrl: 'output/bench/bundle.js', renderer,
      autoFocus: false, moduleUrl: new URL(`../${dist}/jsui.js`, location.href) });
    j.on('log', () => {});
    await Promise.race([j.whenIdle(), new Promise((r) => setTimeout(r, 8000))]);
    return { name: j.renderer, adapter: j._webgpu?.description ?? null,
      gl: (() => {
        const gl = j._gl;
        const ext = gl?.getExtension('WEBGL_debug_renderer_info');
        return gl ? gl.getParameter(ext ? ext.UNMASKED_RENDERER_WEBGL : gl.RENDERER) : null;
      })() };
  }, { renderer, dist: DIST, w: W, h: H });
  return { context, page, info };
}

const sorted = (a) => [...a].sort((x, y) => x - y);
const avg = (a) => a.reduce((x, y) => x + y, 0) / Math.max(1, a.length);
const pct = (s, p) => s[Math.min(s.length - 1, Math.floor(s.length * p))];
const stats = (a) => { const s = sorted(a); return { mean: avg(a), p50: pct(s, 0.5), p95: pct(s, 0.95), p99: pct(s, 0.99), max: s[s.length - 1] }; };
const f2 = (v) => (v == null || Number.isNaN(v) ? '-' : v.toFixed(2));

const results = [];
for (const renderer of RENDERERS) {
  const { context, page, info } = await open(renderer);
  // 1. The real loop: rAF-to-rAF intervals and main-thread time per frame.
  const steady = await page.evaluate(async ({ n, warmup }) => {
    const j = window.j;
    const frame = j.frame.bind(j);
    const stamps = [], cpu = [], gpuDone = [];
    const dev = j._wgpu?.device;
    let i = 0;
    const r0 = j.stats.renders;
    await new Promise((resolve) => {
      j.frame = (now) => {
        const a = performance.now();
        frame(now);
        const b = performance.now();
        if (i >= warmup) {
          stamps.push(now);
          cpu.push(b - a);
          // When the GPU finished this frame, observed without waiting on it.
          if (dev) {
            const k = gpuDone.length;
            gpuDone.push(NaN);
            dev.queue.onSubmittedWorkDone().then(() => { gpuDone[k] = performance.now() - a; });
          }
        }
        if (++i >= warmup + n + 1) resolve();
      };
    });
    j.frame = frame;
    await new Promise((r) => setTimeout(r, 100));  // let the last work-done promises settle
    const intervals = stamps.slice(1).map((t, k) => t - stamps[k]);
    return { intervals, cpu: cpu.slice(1), gpuDone: gpuDone.slice(1).filter((v) => !Number.isNaN(v)),
      renders: j.stats.renders - r0, frames: i };
  }, { n: N, warmup: WARMUP });

  // 2. The old metric: frames stepped by hand, each awaited until the GPU finished it.
  const finish = await page.evaluate(async ({ n }) => {
    const j = window.j;
    j.stop();
    const sync = async () => {
      if (j._wgpu) await j._wgpu.device.queue.onSubmittedWorkDone();
      else j._gl?.finish();
    };
    let t = performance.now();
    const times = [], cpu = [];
    for (let i = 0; i < n + 20; i++) {
      t += 16.67;
      const a = performance.now();
      j.frame(t);
      const b = performance.now();
      await sync();
      if (i >= 20) {  // after warm-up
        times.push(performance.now() - a);
        cpu.push(b - a);
      }
    }
    j.destroy();
    return { times, cpu };
  }, { n: N });
  await context.close();

  // 3. WebGPU calls per frame (instrumented page).
  let counts = null;
  if (COUNTS && info.name === 'webgpu') {
    const c = await open(renderer, { counts: true });
    counts = await c.page.evaluate(async ({ n }) => {
      const j = window.j;
      j.stop();
      let t = performance.now();
      for (let i = 0; i < 10; i++) j.frame((t += 16.67));
      await j._wgpu.device.queue.onSubmittedWorkDone();
      window.__gpuCounts.clear();
      for (let i = 0; i < n; i++) {
        j.frame((t += 16.67));
        await j._wgpu.device.queue.onSubmittedWorkDone();
      }
      const out = {};
      for (const [k, v] of window.__gpuCounts) if (k !== 'GPUQueue.onSubmittedWorkDone') out[k] = v / n;
      j.destroy();
      return out;
    }, { n: Math.min(N, 30) });
    await c.context.close();
  }

  const iv = stats(steady.intervals), vsync = iv.p50;
  const late = steady.intervals.filter((v) => v > vsync * 1.5).length;
  const cpu = stats(steady.cpu), fin = stats(finish.times), finCpu = stats(finish.cpu);
  const gpu = steady.gpuDone.length ? stats(steady.gpuDone) : null;
  const where = info.name === 'webgpu' ? info.adapter : info.gl;
  console.log(`  ${renderer.padEnd(7)} (${info.name}${where ? `, ${where}` : ''})`);
  console.log(`    interval  mean ${f2(iv.mean)} ms, p50 ${f2(iv.p50)}, p95 ${f2(iv.p95)}, p99 ${f2(iv.p99)}, ` +
    `late (>1.5x p50) ${late}/${steady.intervals.length}; main thread mean ${f2(cpu.mean)}, p95 ${f2(cpu.p95)}` +
    (gpu ? `; GPU done after mean ${f2(gpu.mean)}, p95 ${f2(gpu.p95)}` : '') +
    `; ${steady.renders} repaints`);
  console.log(`    finish    mean ${f2(fin.mean)} ms, p50 ${f2(fin.p50)}, p95 ${f2(fin.p95)} (main thread ${f2(finCpu.mean)})`);
  if (counts) {
    const c = (k) => +(counts[k] ?? 0).toFixed(2);
    console.log(`    gpu per frame: ${c('GPUCommandEncoder.beginRenderPass')} render passes, ` +
      `${+(c('GPURenderPassEncoder.draw') + c('GPURenderPassEncoder.drawIndexed')).toFixed(2)} draws, ` +
      `${c('GPUDevice.createBindGroup')} bind groups created, ${c('GPURenderPassEncoder.setPipeline')} pipeline binds, ` +
      `${c('GPUBuffer.mapAsync')} buffer maps, ${c('GPUDevice.createTexture')} textures created, ` +
      `${((counts['bytes.colorLoadStore'] ?? 0) / 1048576).toFixed(1)} MB color attachment load+store`);
    const keys = Object.keys(counts).sort((a, b) => (a.startsWith('pass') - b.startsWith('pass')) || a.localeCompare(b));
    console.log(`    gpu calls per frame: ${keys.map((k) => `${k} ${+counts[k].toFixed(2)}`).join(', ')}`);
  }
  results.push({ renderer, name: info.name, adapter: where, interval: iv, late, mainThread: cpu, gpuDone: gpu,
    finish: fin, finishMainThread: finCpu, repaints: steady.renders, counts });
}
if (JSON_OUT) fs.writeFileSync(JSON_OUT, JSON.stringify({ entry, width: W, height: H, dpr: DPR, realGpu: REAL_GPU,
  browser: browser.version(), results }, null, 2));
await browser.close();
server.close();
