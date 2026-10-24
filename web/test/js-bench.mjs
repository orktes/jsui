// JS cost per frame on QuickJS (in jsui.wasm) and on the host's own JS engine
// (createJsui({ js: 'host' }), docs/ARCHITECTURE.md "Host JS engine"), in
// Node (the CLI's headless runtime, no paint) and in headless Chromium (the
// web host, CPU renderer, painting every frame). Scenarios (the jsui-bench
// ones, tools/jsui-bench/scenarios, at 1280x720):
//
//   markers    100 markers moving every frame, fed with jsui_set_buffer
//              (BufferMarkers: a React render of every marker per frame)
//   inventory  500 slots, one jsui_patch_state per frame
//   surfaces   30 root views (jsui_surface_create), every one's props
//              changing every frame
//   mount      examples/showcase/interactions.jsx loaded into a running
//              runtime (jsui_load_bundle_source): evaluation plus the first
//              10 frames, in one total (a cold compile: every run's source
//              differs)
//
// Per frame: js = the profiler's js phase (jsui_get_perf js_us: the engine's
// timers, rAF, events and microtasks, without the native work they call into),
// core = js + commit + animated + style + layout, and wall = the frame's wall
// time until its microtasks ran (Node: jsui_tick; Chromium: jsui.frame(),
// which also paints). Mean and p50 over --frames frames after --warm frames.
//
//   node web/test/js-bench.mjs [--frames 300] [--warm 60] [--runs 3] [--only node|chromium]
//                              [--scenarios markers,inventory,surfaces,mount] [--dist dist] [--json out.json]
//                              [--real-gpu] [--renderer cpu|webgpu|webgl]
import fs from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromium } from 'playwright';
import { build, launch } from '@orktes/jsui-cli';

const WEB = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const ROOT = path.resolve(WEB, '..');
const args = process.argv.slice(2);
const opt = (name, def) => { const i = args.indexOf(name); return i >= 0 ? args[i + 1] : def; };
const N = +opt('--frames', 300);
const WARM = +opt('--warm', 60);
const RUNS = +opt('--runs', 3);
const ONLY = opt('--only', '');
const DIST = opt('--dist', 'dist');
// --real-gpu: the installed Chrome, headed, on the machine's GPU; --renderer
// cpu (default) | webgpu | webgl (with a module that has it, e.g. --dist dist-gpu).
const REAL_GPU = args.includes('--real-gpu');
const RENDERER = opt('--renderer', 'cpu');
const SCENARIOS = opt('--scenarios', 'markers,inventory,surfaces,mount').split(',');
const W = 1280, H = 720;

const OUT = fs.mkdtempSync(path.join(os.tmpdir(), 'jsui-js-bench-'));
const SCEN = path.join(ROOT, 'tools/jsui-bench/scenarios');
const ENTRIES = { markers: 'markers.jsx', inventory: 'inventory.jsx', surfaces: 'surfaces.jsx' };
const bundles = {};
for (const [k, f] of Object.entries(ENTRIES)) {
  await build({ entry: path.join(SCEN, f), outfile: path.join(OUT, k, 'bundle.js'), minify: true });
  bundles[k] = fs.readFileSync(path.join(OUT, k, 'bundle.js'), 'utf8');
}
await build({ entry: path.join(ROOT, 'examples/showcase/interactions.jsx'), outfile: path.join(OUT, 'mount', 'bundle.js'), minify: true });
bundles.mount = fs.readFileSync(path.join(OUT, 'mount', 'bundle.js'), 'utf8');
const EMPTY = fs.readFileSync(path.join(WEB, 'test/fixtures/solid.js'), 'utf8');

// One scenario on a runtime, the same code in Node and in the page. env:
// {M, rt, frame(t), next() (a new task: the host's microtasks settle), load(src)}.
async function core(env, scenario, n, warm, src) {
  const { M, rt } = env;
  const cstr = (s) => { const k = M.lengthBytesUTF8(s) + 1; const p = M._malloc(k); M.stringToUTF8(s, p, k); return p; };
  const pp = M._malloc(128);
  const perf = () => {
    M._jsui_get_perf(rt, pp, 1);
    const f = new Float64Array(M.HEAPU8.buffer, pp, 8);
    return { js: f[0], core: f[0] + f[1] + f[2] + f[3] + f[4] };
  };
  const step = 1000 / 60;
  let t = env.t0;
  const surfaces = [];
  let feed = () => {};
  if (scenario === 'markers') {
    const p = cstr('BufferMarkers'), q = cstr('{}');
    M._jsui_surface_run(rt, p, q);
    const key = cstr('enemies'), data = M._malloc(300 * 4);
    feed = (i) => {
      const f = new Float32Array(M.HEAPU8.buffer, data, 300);
      for (let k = 0; k < 100; k++) {
        f[k * 3] = (k * 37 + i * 3) % 1200;
        f[k * 3 + 1] = (k * 53 + i * 2) % 680;
        f[k * 3 + 2] = (k * 7 + i) % 100;
      }
      M._jsui_set_buffer(rt, key, 0, data, 300);
    };
  } else if (scenario === 'inventory') {
    const key = cstr('inventory');
    feed = (i) => {
      const j = cstr(JSON.stringify({ slots: { [(i * 7) % 500]: { count: i % 20 } } }));
      M._jsui_patch_state(rt, key, j);
      M._free(j);
    };
  } else if (scenario === 'surfaces') {
    const app = cstr('Plate');
    const props = (k, i) => cstr(JSON.stringify({ name: `Enemy ${k}`, hp: (k * 13 + i * 3) % 100, level: 1 + k }));
    for (let k = 0; k < 30; k++) {
      const p = props(k, 0);
      surfaces.push(M._jsui_surface_create(rt, app, p, 120, 40, 1));
      M._free(p);
    }
    feed = (i) => surfaces.forEach((s, k) => {
      const p = props(k, i);
      M._jsui_surface_set_props(s, p);
      M._free(p);
    });
  }
  M._jsui_set_profiling(rt, 1);
  const frameOnce = async (i) => {
    const t1 = performance.now();
    feed(i);
    env.frame((t += step));
    await null;  // the host engine's promise jobs of this frame
    return performance.now() - t1;
  };
  const rows = [];
  if (scenario === 'mount') {
    perf();
    // A source no engine has seen (V8 caches compiled eval code by its text).
    src += `\n// ${Date.now()} ${Math.random()}`;
    const t1 = performance.now();
    env.load(src);
    let wall = performance.now() - t1;
    for (let i = 0; i < 10; i++) {
      wall += await frameOnce(i);
      await env.next();
    }
    rows.push({ ...perf(), wall });
  } else {
    for (let i = 0; i < warm + n; i++) {
      const wall = await frameOnce(i);
      const p = perf();
      if (i >= warm) rows.push({ ...p, wall });
      await env.next();
    }
  }
  M._jsui_set_profiling(rt, 0);
  surfaces.forEach((s) => M._jsui_surface_destroy(s));
  M._free(pp);
  const stat = (k) => {
    const v = rows.map((r) => r[k]).sort((a, b) => a - b);
    return { mean: v.reduce((a, b) => a + b, 0) / v.length, p50: v[v.length >> 1] };
  };
  return { js: stat('js'), core: stat('core'), wall: stat('wall') };
}

const results = [];
const median = (xs) => xs.slice().sort((a, b) => a - b)[xs.length >> 1];
function record(where, scenario, js, runs) {
  const pick = (k, s) => median(runs.map((r) => r[k][s]));
  const r = {
    where, scenario, js,
    js_us: pick('js', 'mean'), js_p50_us: pick('js', 'p50'),
    core_us: pick('core', 'mean'), wall_ms: pick('wall', 'mean'),
  };
  results.push(r);
  console.log(`${where.padEnd(9)} ${scenario.padEnd(10)} ${js.padEnd(8)} js ${(r.js_us / 1000).toFixed(3).padStart(7)} ms ` +
    `(p50 ${(r.js_p50_us / 1000).toFixed(3)})  core ${(r.core_us / 1000).toFixed(3).padStart(7)} ms  wall ${r.wall_ms.toFixed(3).padStart(7)} ms`);
}

// ------------------------------------------------------------------ Node
if (ONLY !== 'chromium') {
  console.log(`Node ${process.version}: ${N} frames after ${WARM} (mount: one load + 10 frames), median of ${RUNS} runs`);
  for (const scenario of SCENARIOS) {
    for (const js of ['quickjs', 'host']) {
      const runs = [];
      for (let r = 0; r < RUNS; r++) {
        const file = path.join(OUT, scenario === 'mount' ? 'empty.js' : `${scenario}/bundle.js`);
        if (scenario === 'mount') fs.writeFileSync(file, EMPTY);
        const app = await launch({ bundle: file, width: W, height: H, js, wasmDir: path.join(WEB, DIST) });
        await app.ready();
        const M = app.M, rt = app.rt;
        runs.push(await core({
          M, rt, t0: app.time + 1000,
          frame: (t) => M._jsui_tick(rt, t),
          next: () => new Promise((res) => setImmediate(res)),
          load: (s) => {
            const k = M.lengthBytesUTF8(s) + 1, p = M._malloc(k);
            M.stringToUTF8(s, p, k);
            const n = M.lengthBytesUTF8('bundle.js') + 1, q = M._malloc(n);
            M.stringToUTF8('bundle.js', q, n);
            M._jsui_load_bundle_source(rt, p, k - 1, q);
            M._free(p);
            M._free(q);
          },
        }, scenario, N, WARM, bundles[scenario]));
        if (app.errors.length) throw new Error(`${scenario}/${js}: ${app.errors.join('\n')}`);
        app.close();
      }
      record('node', scenario, js, runs);
    }
  }
}

// -------------------------------------------------------------- Chromium
if (ONLY !== 'node') {
  const MIME = { '.js': 'text/javascript', '.html': 'text/html', '.wasm': 'application/wasm' };
  const server = http.createServer((req, res) => {
    const rel = decodeURIComponent(new URL(req.url, 'http://x').pathname);
    if (rel === '/bench.html') {
      res.writeHead(200, { 'content-type': 'text/html' });
      return res.end(`<!doctype html><meta charset="utf-8"><body style="margin:0"><script type="module">
        import { createJsui } from '/jsui-web.js'; window.createJsui = createJsui; window.ready = true;</script>`);
    }
    const file = path.normalize(path.join(WEB, rel));
    if (!file.startsWith(WEB) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) {
      res.writeHead(404);
      return res.end();
    }
    res.writeHead(200, { 'content-type': MIME[path.extname(file)] || 'application/octet-stream' });
    fs.createReadStream(file).pipe(res);
  });
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  const browser = REAL_GPU
    ? await chromium.launch({ channel: 'chrome', headless: false, args: ['--enable-unsafe-webgpu', '--ignore-gpu-blocklist'] })
    : await chromium.launch();
  console.log(`${REAL_GPU ? "Chrome (real GPU)" : "headless Chromium"} ${browser.version()}, ${RENDERER} renderer ${W}x${H}: ${N} frames after ${WARM}, median of ${RUNS} runs`);
  for (const scenario of SCENARIOS) {
    for (const js of ['quickjs', 'host']) {
      const runs = [];
      for (let r = 0; r < RUNS; r++) {
        const page = await browser.newPage({ viewport: { width: W, height: H } });
        const errors = [];
        page.on('pageerror', (e) => errors.push(e.message));
        await page.goto(`http://127.0.0.1:${server.address().port}/bench.html`);
        await page.waitForFunction(() => window.ready);
        runs.push(await page.evaluate(async ({ coreSrc, scenario, js, n, warm, src, boot, dist, w, h, renderer }) => {
          const core = (0, eval)('(' + coreSrc + ')');
          const c = document.createElement('canvas');
          c.style.width = w + 'px';
          c.style.height = h + 'px';
          document.body.appendChild(c);
          const j = await window.createJsui({ canvas: c, renderer, js, bundleSource: boot, moduleUrl: new URL(`/${dist}/jsui.js`, location.href) });
          await Promise.race([j.whenIdle(), new Promise((res) => setTimeout(res, 3000))]);
          if (j.jsEngine !== js) throw new Error('engine ' + j.jsEngine);
          j.stop();
          const ch = new MessageChannel();
          const next = () => new Promise((res) => { ch.port1.onmessage = () => res(); ch.port2.postMessage(0); });
          const out = await core({ M: j.module, rt: j.rt, t0: performance.now(), frame: (t) => j.frame(t), next,
            load: (s) => j.loadSource(s) }, scenario, n, warm, src);
          if (j.lastError) throw new Error(j.lastError);
          j.destroy();
          return out;
        }, { coreSrc: core.toString(), scenario, js, n: N, warm: WARM, src: bundles[scenario],
          boot: scenario === 'mount' ? EMPTY : bundles[scenario], dist: DIST, w: W, h: H, renderer: RENDERER }));
        if (errors.length) throw new Error(`${scenario}/${js}: ${errors.join('\n')}`);
        await page.close();
      }
      record('chromium', scenario, js, runs);
    }
  }
  await browser.close();
  server.close();
}

const json = opt('--json', '');
if (json) fs.writeFileSync(json, JSON.stringify(results, null, 2));
fs.rmSync(OUT, { recursive: true, force: true });
