// Frame-time benchmark of the web host, per renderer: bundles a JSX app
// (default examples/showcase/interactions.jsx, which repaints every frame),
// mounts it at 1280x720, lets its entrance animations finish (--settle) and
// measures, per renderer:
//
//   interval  what the user feels: the host runs its own requestAnimationFrame
//             loop and we record the rAF-to-rAF interval in the steady state
//             (mean / p50 / p95 / p99, and frames that took more than 1.5x the
//             display's vsync, "late"), plus the main-thread time of each frame
//             (jsui.frame(): tick + paint + submitting the GPU work). WebGPU
//             also reports when the GPU finished each frame
//             (queue.onSubmittedWorkDone(), observed without waiting on it).
//   threads   (Chrome tracing, --no-trace to skip) busy time per frame of the
//             threads that make a frame: the page's main thread, the
//             compositor and raster workers, the GPU process' main thread
//             (which validates and runs every WebGL / WebGPU call) and viz.
//             Comparable across renderers, including 'chrome' below.
//   finish    the old metric (secondary): frames stepped by hand, each timed
//             until the GPU finished it (WebGPU: await onSubmittedWorkDone();
//             WebGL / CPU-with-WebGL-blit: gl.finish()). It includes the
//             round trip to the GPU process, which the real loop never waits for.
//   gpu calls WebGPU only: WebGPU API calls per frame (passes with their
//             load / store ops, draws, buffer writes, bind groups, ...), counted
//             in a separate page (web/test/gpu-count.js) so the timings above
//             carry no instrumentation.
//   gpu time  WebGPU only (--no-gpu-timing to skip): GPU timestamps around
//             every render pass and copy (web/test/gpu-timing.js, in a page of
//             its own), summed per frame and listed by pass target and kind.
//             Needs the adapter's 'timestamp-query' feature (real GPUs; the
//             SwiftShader adapter may not have it, then it says so).
//
// Renderers: webgpu, webgl, cpu (JSUI's web host), and chrome: the same app
// bundled with react-native-web (as the reference tests do) and rendered by
// Chrome itself, the bar JSUI is held to. For chrome, "main thread" and the
// GPU process come from the trace.
//
// Scenarios (--scenario):
//   steady    (default) the app's looping animations after the entrance
//   entrance  from mount on (--settle 0): staggered springs, fading panels
//   scroll    steady, plus a wheel scroll over the right-hand list every frame
//
//   node web/test/frame-bench.mjs [--dist dist-gpu] [--frames 120] [--renderers webgpu,webgl,cpu,chrome]
//                                 [--width 1280 --height 720 --dpr 1] [--real-gpu] [--scenario steady]
//                                 [--settle 3000] [--no-counts] [--no-gpu-timing] [--no-trace]
//                                 [--webgl-sync readpixels|fence|finish]
//                                 [--json out.json] [app.jsx]
//
// Default: headless Chromium from Playwright, where WebGL runs on SwiftShader
// and WebGPU on its SwiftShader fallback adapter (--enable-unsafe-webgpu), so
// the numbers compare the paths on a CPU-emulated GPU, not real hardware.
// --real-gpu: the installed Google Chrome (channel 'chrome'), headed, with no
// SwiftShader flags, i.e. the machine's GPU (Metal on a Mac). Keep the window
// visible and in front while it runs (hidden windows get throttled rAF).
// Both run with --enable-webgpu-developer-features so GPU timestamps are not
// quantized to 100 us.
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromium } from 'playwright';
import { build } from '@orktes/jsui-cli';

const WEB = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const ROOT = path.resolve(WEB, '..');
const args = process.argv.slice(2);
const FLAGS = new Set(['--real-gpu', '--no-counts', '--no-gpu-timing', '--no-trace']);
const opt = (name, def) => { const i = args.indexOf(name); return i >= 0 ? args[i + 1] : def; };
const DIST = opt('--dist', 'dist-gpu');
const N = Number(opt('--frames', 120));
const W = Number(opt('--width', 1280)), H = Number(opt('--height', 720)), DPR = Number(opt('--dpr', 1));
const RENDERERS = opt('--renderers', 'webgpu,webgl,cpu').split(',');
const REAL_GPU = args.includes('--real-gpu');
const COUNTS = !args.includes('--no-counts');
const GPU_TIMING = !args.includes('--no-gpu-timing');
const TRACE = !args.includes('--no-trace');
const SCENARIO = opt('--scenario', 'steady');
// How the finish metric waits for a WebGL frame: readpixels (default) a
// 1-pixel read-back; fence a fenceSync polled from short tasks (no read-back,
// but the polling competes for the CPU with SwiftShader: +35% there); finish
// gl.finish(), which does not wait for the GPU on ANGLE (2.4 ms p50 for
// frames whose timer queries say 45 ms on SwiftShader).
const WEBGL_SYNC = opt('--webgl-sync', 'readpixels');
if (!['steady', 'entrance', 'scroll'].includes(SCENARIO)) throw new Error(`unknown --scenario ${SCENARIO}`);
// The entrance animations of interactions.jsx take ~2.5 s (staggered springs,
// panels fading in, the progress bar). Before they finish a frame repaints
// most of the screen and fades whole panels through offscreen layers.
const SETTLE = Number(opt('--settle', SCENARIO === 'entrance' ? 0 : 3000));
const JSON_OUT = opt('--json', null);
const entry = args.find((a, i) => !a.startsWith('--') && !(args[i - 1]?.startsWith('--') && !FLAGS.has(args[i - 1]))) ??
  path.join(ROOT, 'examples/showcase/interactions.jsx');
const WARMUP = 30;
// Where the scroll scenario turns the wheel: the armory list on the right of
// interactions.jsx (CSS px).
const SCROLL_AT = { x: W - 200, y: Math.round(H * 0.55) };

const OUT = path.join(WEB, 'test/output/bench');
fs.mkdirSync(OUT, { recursive: true });
await build({ entry, outfile: path.join(OUT, 'bundle.js'), minify: true });

// The same app for Chrome itself: react-native-web + react-dom (the reference
// tests' bundler and page, with Inter for every system font name).
let rnwPage = null;
if (RENDERERS.includes('chrome')) {
  const { buildWebBundle, pageHtml } = await import('../../tests/reference/lib/web.js');
  await buildWebBundle(entry, path.join(OUT, 'rnw'));
  rnwPage = pageHtml({ width: W, height: H }).replace('/bundle.js', '/test/output/bench/rnw/bundle.js');
}
const FONTS = path.join(ROOT, 'assets/fonts');

const MIME = { '.js': 'text/javascript', '.html': 'text/html', '.wasm': 'application/wasm', '.json': 'application/json',
  '.png': 'image/png', '.jpg': 'image/jpeg', '.ttf': 'font/ttf' };
const server = http.createServer((req, res) => {
  const rel = decodeURIComponent(new URL(req.url, 'http://x').pathname);
  if (rel === '/rnw.html' && rnwPage) {
    res.writeHead(200, { 'content-type': 'text/html' });
    return res.end(rnwPage);
  }
  // The RNW bundle's assets are relative to the page (/assets/...).
  const file = rel.startsWith('/__fonts/') ? path.join(FONTS, path.basename(rel))
    : rel.startsWith('/assets/') ? path.normalize(path.join(OUT, 'rnw', rel))
      : path.normalize(path.join(WEB, rel));
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
    args: [...COMMON_ARGS, '--enable-unsafe-swiftshader', '--use-angle=swiftshader', '--ignore-gpu-blocklist',
      '--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-vulkan=swiftshader'],
  });
console.log(`${path.relative(ROOT, entry)} ${W}x${H} @${DPR}x, web/${DIST}, ${N} frames, scenario ${SCENARIO} ` +
  `(settle ${SETTLE} ms), ${REAL_GPU ? 'Chrome (real GPU)' : 'headless Chromium (SwiftShader)'} ${browser.version()}`);

// The scroll scenario's wheel input inside the page (stepped frames): a
// synthetic wheel event on JSUI's canvas, down for 60 frames, then up.
const DRIVE = ({ scenario, at }) => {
  window.__drive = (i) => {
    if (scenario !== 'scroll' || !window.j) return;
    const c = window.j.canvas, r = c.getBoundingClientRect();
    c.dispatchEvent(new WheelEvent('wheel', { clientX: r.left + at.x, clientY: r.top + at.y,
      deltaY: Math.floor(i / 60) % 2 ? -12 : 12, bubbles: true, cancelable: true }));
  };
};

// Mounts the app on a fresh page; the host's own rAF loop is running.
// stepped: the caller steps frames by hand. In the entrance scenario the
// host then does not start its loop, and window.__t0 (mount time) is where
// the caller's frame times start, so the entrance plays the same however long
// the page took to load (SwiftShader frames take ~250 ms of wall clock).
async function open(renderer, { init = [], stepped = false } = {}) {
  const context = await browser.newContext({ viewport: { width: W, height: H }, deviceScaleFactor: DPR });
  for (const f of init) await context.addInitScript({ path: path.join(WEB, 'test', f) });
  await context.addInitScript(DRIVE, { scenario: SCENARIO, at: SCROLL_AT });
  const page = await context.newPage();
  if (process.env.JSUI_BENCH_CONSOLE) {
    console.log(`[page] ${renderer} ${init.join(",") || "plain"}${stepped ? " stepped" : ""}`);
    page.on("console", (m) => console.log(`[console] ${m.text()}`));
  }
  if (renderer === 'chrome') {
    await page.goto(`${ORIGIN}/rnw.html`);
    await page.evaluate(async () => {
      await document.fonts.ready;
      await new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)));
    });
    await page.bringToFront();
    if (SETTLE) await page.waitForTimeout(SETTLE);
    return { context, page, info: { name: 'chrome', gl: 'react-native-web' } };
  }
  await page.goto(`${ORIGIN}/test/page.html`);
  await page.waitForFunction(() => window.testReady);
  await page.bringToFront();
  const fromMount = stepped && SCENARIO === 'entrance';
  const info = await page.evaluate(async ({ renderer, dist, w, h, fromMount }) => {
    const c = document.createElement('canvas');
    c.style.width = `${w}px`;
    c.style.height = `${h}px`;
    document.body.appendChild(c);
    window.__t0 = performance.now();
    const j = window.j = await window.createJsui({ canvas: c, bundleUrl: 'output/bench/bundle.js', renderer,
      autoFocus: false, autoStart: !fromMount, moduleUrl: new URL(`../${dist}/jsui.js`, location.href) });
    j.on('log', () => {});
    // Assets loaded and the first frames painted (whenIdle resolves right
    // after a frame, even while animations keep running).
    if (!fromMount) {
      window.__t0 = null;
      await Promise.race([j.whenIdle(), new Promise((r) => setTimeout(r, 8000))]);
    }
    return { name: j.renderer, adapter: j._webgpu?.description ?? null,
      gl: (() => {
        const gl = j._gl;
        const ext = gl?.getExtension('WEBGL_debug_renderer_info');
        return gl ? gl.getParameter(ext ? ext.UNMASKED_RENDERER_WEBGL : gl.RENDERER) : null;
      })() };
  }, { renderer, dist: DIST, w: W, h: H, fromMount });
  if (SETTLE && !fromMount) await page.waitForTimeout(SETTLE);  // the host's loop runs the entrance animations
  return { context, page, info };
}

// Real wheel input from outside the page while a phase runs (scroll scenario).
function wheelLoop(page) {
  if (SCENARIO !== 'scroll') return { stop: async () => {} };
  let on = true, i = 0;
  const run = (async () => {
    await page.mouse.move(SCROLL_AT.x, SCROLL_AT.y);
    while (on) {
      await page.mouse.wheel(0, Math.floor(i++ / 60) % 2 ? -12 : 12).catch(() => {});
      await new Promise((r) => setTimeout(r, 16));
    }
  })();
  return { stop: async () => { on = false; await run; } };
}

// Busy time per thread from a Chrome trace: the union of the complete / begin-
// end slices on each thread (nested slices count once).
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
    // Several renderer processes (other pages): keep the busiest per name.
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
const stats = (a) => { const s = sorted(a); return { mean: avg(a), p50: pct(s, 0.5), p95: pct(s, 0.95), p99: pct(s, 0.99), max: s[s.length - 1] }; };
const f2 = (v) => (v == null || Number.isNaN(v) ? '-' : v.toFixed(2));

const results = [];
for (const renderer of RENDERERS) {
  const { context, page, info } = await open(renderer);
  const isChrome = renderer === 'chrome';
  // 1. The real loop: rAF-to-rAF intervals and main-thread time per frame
  //    (traced, unless --no-trace).
  if (TRACE) await browser.startTracing(page, { categories: TRACE_CATEGORIES });
  const wheel = wheelLoop(page);
  const steady = await page.evaluate(async ({ n, warmup, chrome }) => {
    const stamps = [], cpu = [], gpuDone = [];
    let i = 0, t0 = 0, t1 = 0;
    if (chrome) {
      // Chrome's own loop: only the rAF timestamps are visible from the page.
      await new Promise((resolve) => {
        const tick = (now) => {
          if (i === warmup) t0 = performance.now();
          if (i >= warmup) stamps.push(now);
          if (++i >= warmup + n + 1) { t1 = performance.now(); return resolve(); }
          requestAnimationFrame(tick);
        };
        requestAnimationFrame(tick);
      });
      return { intervals: stamps.slice(1).map((t, k) => t - stamps[k]), cpu: [], gpuDone: [], renders: null, frames: n, window: t1 - t0 };
    }
    const j = window.j;
    const frame = j.frame.bind(j);
    const dev = j._wgpu?.device;
    const r0 = j.stats.renders;
    await new Promise((resolve) => {
      j.frame = (now) => {
        if (i === warmup) t0 = performance.now();
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
        if (++i >= warmup + n + 1) { t1 = performance.now(); resolve(); }
      };
    });
    j.frame = frame;
    await new Promise((r) => setTimeout(r, 100));  // let the last work-done promises settle
    const intervals = stamps.slice(1).map((t, k) => t - stamps[k]);
    return { intervals, cpu: cpu.slice(1), gpuDone: gpuDone.slice(1).filter((v) => !Number.isNaN(v)),
      renders: j.stats.renders - r0, frames: i - warmup, window: t1 - t0 };
  }, { n: N, warmup: WARMUP, chrome: isChrome });
  await wheel.stop();
  let threads = null;
  if (TRACE) {
    const busy = threadBusy((await browser.stopTracing()).toString());
    // Per frame over the measured frames (the trace also holds the warm-up:
    // scale by the measured share of the traced window, approximately).
    const frames = steady.intervals.length + 1;
    const total = steady.window + WARMUP * (steady.window / frames);
    const share = steady.window / total;
    threads = Object.fromEntries(THREADS.map(([k, label]) => [label, ((busy[k] ?? 0) * share) / frames]));
  }

  // 2. The old metric: frames stepped by hand, each awaited until the GPU finished it.
  const finish = isChrome ? null : await page.evaluate(async ({ n, gpuTiming, webglSync }) => {
    const j = window.j;
    j.stop();
    const px = new Uint8Array(4), gl = j._gl;
    const isGl2 = typeof WebGL2RenderingContext !== 'undefined' && gl instanceof WebGL2RenderingContext;
    const mode = !isGl2 && webglSync === 'fence' ? 'readpixels' : webglSync;
    const syncKind = j._wgpu ? 'onSubmittedWorkDone' : gl ? mode : 'none';
    // A macrotask much shorter than setTimeout's clamp, to poll a fence.
    const ch = new MessageChannel();
    const yieldTask = () => new Promise((r) => { ch.port1.onmessage = r; ch.port2.postMessage(0); });
    // WebGL: see --webgl-sync (fence needs WebGL 2).
    const sync = async () => {
      if (j._wgpu) return j._wgpu.device.queue.onSubmittedWorkDone();
      if (!gl) return;
      if (mode === 'finish') return gl.finish();
      if (mode === 'fence') {
        const f = gl.fenceSync(gl.SYNC_GPU_COMMANDS_COMPLETE, 0);
        gl.flush();
        while (gl.clientWaitSync(f, 0, 0) === gl.TIMEOUT_EXPIRED) await yieldTask();
        gl.deleteSync(f);
        return;
      }
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
    };
    const r0 = j.stats.renders;
    let t = performance.now(), i = 0;
    const times = [], cpu = [];
    for (; i < n + 20; i++) {
      t += 16.67;
      window.__drive(i);
      const a = performance.now();
      j.frame(t);
      const b = performance.now();
      await sync();
      if (i >= 20) {  // after warm-up
        times.push(performance.now() - a);
        cpu.push(b - a);
      }
    }
    const renders = j.stats.renders - r0;
    // WebGL GPU time per frame, where the browser has timer queries
    // (EXT_disjoint_timer_query_webgl2): the WebGPU timestamps' counterpart.
    // A run of its own after the timed frames (none with --no-gpu-timing),
    // so the finish times above never include a TIME_ELAPSED query (which
    // ANGLE may implement with extra flushes / command buffers).
    const tq = gpuTiming ? gl?.getExtension('EXT_disjoint_timer_query_webgl2') : null;
    const queries = [], gpuMs = [];
    if (tq) {
      for (let k = 0; k < n; k++, i++) {
        t += 16.67;
        window.__drive(i);
        const q = gl.createQuery();
        gl.beginQuery(tq.TIME_ELAPSED_EXT, q);
        j.frame(t);
        gl.endQuery(tq.TIME_ELAPSED_EXT);
        queries.push(q);
        await sync();
      }
      for (let k = 0; k < 50 && queries.length; k++) {
        await new Promise((r) => setTimeout(r, 10));
        if (!gl.getQueryParameter(queries[queries.length - 1], gl.QUERY_RESULT_AVAILABLE)) continue;
        if (!gl.getParameter(tq.GPU_DISJOINT_EXT)) {
          for (const q of queries) gpuMs.push(gl.getQueryParameter(q, gl.QUERY_RESULT) / 1e6);
        }
        break;
      }
      queries.forEach((q) => gl.deleteQuery(q));
    }
    j.destroy();
    return { times, cpu, renders, gpuMs, timerQuery: !!tq, sync: syncKind };
  }, { n: N, gpuTiming: GPU_TIMING, webglSync: WEBGL_SYNC });
  await context.close();

  // 3. WebGPU calls per frame (instrumented page).
  let counts = null;
  if (COUNTS && info.name === 'webgpu') {
    const c = await open(renderer, { init: ['gpu-count.js'], stepped: true });
    counts = await c.page.evaluate(async ({ n }) => {
      const j = window.j;
      j.stop();
      let t = window.__t0 ?? performance.now();
      for (let i = 0; i < 10; i++) {
        window.__drive(i);
        j.frame((t += 16.67));
        await j._wgpu.device.queue.onSubmittedWorkDone();
      }
      await j._wgpu.device.queue.onSubmittedWorkDone();
      window.__gpuCounts.clear();
      for (let i = 0; i < n; i++) {
        window.__drive(i + 10);
        j.frame((t += 16.67));
        await j._wgpu.device.queue.onSubmittedWorkDone();
      }
      const out = {};
      for (const [k, v] of window.__gpuCounts) if (k !== 'GPUQueue.onSubmittedWorkDone') out[k] = v / n;
      j.destroy();
      return out;
    }, { n: Number(process.env.JSUI_BENCH_COUNT_FRAMES ?? Math.min(N, 30)) });
    await c.context.close();
  }

  // 4. GPU time per pass (timestamps, instrumented page).
  let timing = null;
  if (GPU_TIMING && info.name === 'webgpu') {
    const c = await open(renderer, { init: ['gpu-timing.js'], stepped: true });
    timing = await c.page.evaluate(async ({ n }) => {
      const j = window.j, T = window.__gpuTiming;
      j.stop();
      let t = window.__t0 ?? performance.now();
      for (let i = 0; i < 10; i++) {
        window.__drive(i);
        j.frame((t += 16.67));
        await j._wgpu.device.queue.onSubmittedWorkDone();
      }
      await j._wgpu.device.queue.onSubmittedWorkDone();
      await T.pending();
      T.clear();
      if (!T.supported) return { supported: false, adapter: [...(j._webgpu.adapter.features ?? [])].includes('timestamp-query') };
      for (let i = 0; i < n; i++) {
        T.frame = i;
        window.__drive(i + 10);
        j.frame((t += 16.67));
        await j._wgpu.device.queue.onSubmittedWorkDone();
      }
      await T.pending();
      j.destroy();
      return { supported: true, entries: T.entries, errors: T.errors, frames: n };
    }, { n: Math.min(N, 60) });
    await c.context.close();
  }

  const iv = stats(steady.intervals), vsync = iv.p50;
  const late = steady.intervals.filter((v) => v > vsync * 1.5).length;
  const cpu = steady.cpu.length ? stats(steady.cpu) : null;
  const fin = finish && stats(finish.times), finCpu = finish && stats(finish.cpu);
  const gpu = steady.gpuDone.length ? stats(steady.gpuDone) : null;
  const where = info.name === 'webgpu' ? info.adapter : info.gl;
  console.log(`  ${renderer.padEnd(7)} (${info.name}${where ? `, ${where}` : ''})`);
  console.log(`    interval  mean ${f2(iv.mean)} ms, p50 ${f2(iv.p50)}, p95 ${f2(iv.p95)}, p99 ${f2(iv.p99)}, ` +
    `late (>1.5x p50) ${late}/${steady.intervals.length}` +
    (cpu ? `; main thread mean ${f2(cpu.mean)}, p95 ${f2(cpu.p95)}` : '') +
    (gpu ? `; GPU done after mean ${f2(gpu.mean)}, p95 ${f2(gpu.p95)}` : '') +
    (steady.renders != null ? `; ${steady.renders} repaints` : ''));
  if (threads) {
    console.log(`    threads   busy ms per frame: ${THREADS.map(([, l]) => `${l} ${f2(threads[l])}`).join(', ')}`);
  }
  if (fin) {
    console.log(`    finish    mean ${f2(fin.mean)} ms, p50 ${f2(fin.p50)}, p95 ${f2(fin.p95)} (main thread ${f2(finCpu.mean)}; ` +
      `${finish.renders} repaints in ${finish.times.length + 20} frames; waited with ${finish.sync})`);
    if (info.name !== 'webgpu' && GPU_TIMING) {
      const g = finish.gpuMs.length ? stats(finish.gpuMs) : null;
      console.log(g ? `    gpu time  per frame (WebGL timer queries): mean ${f2(g.mean)} ms, p50 ${f2(g.p50)}, p95 ${f2(g.p95)}`
        : `    gpu time  unavailable: ${finish.timerQuery ? 'timer queries disjoint or not ready' : 'no EXT_disjoint_timer_query_webgl2'}`);
    }
  }
  if (counts) {
    const c = (k) => +(counts[k] ?? 0).toFixed(2);
    const sum = (re) => +Object.entries(counts).filter(([k]) => re.test(k)).reduce((n, [, v]) => n + v, 0).toFixed(2);
    console.log(`    gpu per frame: ${c('GPUCommandEncoder.beginRenderPass')} render passes, ` +
      `${+(c('GPURenderPassEncoder.draw') + c('GPURenderPassEncoder.drawIndexed')).toFixed(2)} draws, ` +
      `${c('GPUDevice.createBindGroup')} bind groups created, ${c('GPURenderPassEncoder.setPipeline')} pipeline binds, ` +
      `${c('GPUBuffer.mapAsync')} buffer maps, ${sum(/^GPUCommandEncoder\.copy/)} copies, ` +
      `${c('GPUDevice.createTexture')} textures created, ` +
      `${((counts['bytes.colorLoadStore'] ?? 0) / 1048576).toFixed(1)} MB color attachment load+store`);
    const keys = Object.keys(counts).sort((a, b) => (a.startsWith('pass') - b.startsWith('pass')) || a.localeCompare(b));
    console.log(`    gpu calls per frame: ${keys.map((k) => `${k} ${+counts[k].toFixed(2)}`).join(', ')}`);
  }
  let gpuTime = null;
  if (timing && !timing.supported) {
    console.log(`    gpu time  unavailable: no 'timestamp-query' on this adapter${timing.adapter ? ' (adapter has it, device did not get it)' : ''}`);
  } else if (timing) {
    const perFrame = new Array(timing.frames).fill(0);
    const by = new Map();
    for (const e of timing.entries) {
      perFrame[e.frame] += e.ms;
      // Copies by kind (their sizes vary frame to frame), with the area copied.
      const m = e.kind === 'copy' && /^(\S+) (\d+)x(\d+)$/.exec(e.label);
      const label = e.kind === 'copy' ? e.label.split(' ')[0] : e.label;
      const b = by.get(label) ?? by.set(label, { kind: e.kind, n: 0, ms: 0, px: 0 }).get(label);
      b.n++;
      b.ms += e.ms;
      if (m) b.px += m[2] * m[3];
    }
    const tot = stats(perFrame);
    const rows = [...by].map(([label, b]) => ({ label: b.px ? `${label} (${(b.px / b.n / 1000).toFixed(1)}k px each)` : label,
      kind: b.kind, perFrame: b.n / timing.frames, ms: b.ms / timing.frames }))
      .sort((a, b) => b.ms - a.ms);
    gpuTime = { total: tot, rows };
    console.log(`    gpu time  per frame (timestamps, passes + copies): mean ${f2(tot.mean)} ms, p50 ${f2(tot.p50)}, ` +
      `p95 ${f2(tot.p95)}${timing.errors ? `; ${timing.errors} read-backs failed` : ''}`);
    for (const r of rows) {
      console.log(`      ${r.ms.toFixed(3).padStart(7)} ms  ${r.perFrame.toFixed(2).padStart(6)}x  ${r.label}`);
    }
  }
  results.push({ renderer, name: info.name, adapter: where, interval: iv, late, mainThread: cpu, gpuDone: gpu, threads,
    finish: fin, finishMainThread: finCpu, repaints: steady.renders, counts,
    gpuTime: gpuTime ?? (finish?.gpuMs?.length ? { total: stats(finish.gpuMs) } : null) });
}
if (JSON_OUT) fs.writeFileSync(JSON_OUT, JSON.stringify({ entry, width: W, height: H, dpr: DPR, realGpu: REAL_GPU,
  scenario: SCENARIO, settle: SETTLE, browser: browser.version(), results }, null, 2));
await browser.close();
server.close();
