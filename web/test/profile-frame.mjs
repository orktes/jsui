// Main-thread profile of the web host's frames: runs the app in headless
// Chromium (as frame-bench.mjs does), samples the page's main thread with the
// V8 CPU profiler while the host's rAF loop runs, and prints the time per
// frame spent under each phase of jsui.frame():
//
//   tick          jsui_tick: timers, rAF callbacks, promise jobs (React) ...
//     js          ... of which QuickJS (JS_* / js_* functions)
//     animated    native-driven Animated (NativeAnimated::tick)
//     layout      Yoga (Document::layout)
//   render        jsui_render_webgpu / jsui_render_gl: everything below
//     damage      updateDamage (paint records)
//     paint       paintDocument (the painter's canvas calls)
//     snap        Graphite Recorder::snap (draw lists -> a recording)
//     commands    insertRecording's TaskList::addCommands (WebGPU encoding)
//     submit      QueueManager::submitToGpu (queue.submit, work-done callbacks)
//     surface     the canvas texture: getCurrentTexture, wrapping, release
// Under LTO Recorder::snap and Context::insertRecording are inlined into
// the render function, so these match the functions they call.
//
// Function names need a module linked with --profiling-funcs (build one
// with -DCMAKE_EXE_LINKER_FLAGS=--profiling-funcs into its own dist dir).
//
//   node web/test/profile-frame.mjs --dist dist-gpu-prof [--renderer webgpu] [--scenario steady]
//                                   [--width 1280 --height 720 --dpr 1] [--frames 120] [--top 40] [app.jsx]
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromium } from 'playwright';
import { build } from '@jsui/cli';

const WEB = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const ROOT = path.resolve(WEB, '..');
const args = process.argv.slice(2);
const opt = (name, def) => { const i = args.indexOf(name); return i >= 0 ? args[i + 1] : def; };
const DIST = opt('--dist', 'dist-gpu');
const RENDERER = opt('--renderer', 'webgpu');
const SCENARIO = opt('--scenario', 'steady');
const N = Number(opt('--frames', 120));
const TOP = Number(opt('--top', 40));
const W = Number(opt('--width', 1280)), H = Number(opt('--height', 720)), DPR = Number(opt('--dpr', 1));
const entry = args.find((a, i) => !a.startsWith('--') && !args[i - 1]?.startsWith('--')) ??
  path.join(ROOT, 'examples/showcase/interactions.jsx');

const OUT = path.join(WEB, 'test/output/profile');
fs.mkdirSync(OUT, { recursive: true });
await build({ entry, outfile: path.join(OUT, 'bundle.js'), minify: true });
const MIME = { '.js': 'text/javascript', '.html': 'text/html', '.wasm': 'application/wasm', '.png': 'image/png',
  '.jpg': 'image/jpeg', '.ttf': 'font/ttf' };
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
const browser = await chromium.launch({
  args: ['--force-color-profile=srgb', '--enable-unsafe-swiftshader', '--use-angle=swiftshader', '--ignore-gpu-blocklist',
    '--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-vulkan=swiftshader'],
});
const context = await browser.newContext({ viewport: { width: W, height: H }, deviceScaleFactor: DPR });
const page = await context.newPage();
await page.goto(`${ORIGIN}/test/page.html`);
await page.waitForFunction(() => window.testReady);
const stepped = SCENARIO === 'entrance';
await page.evaluate(async ({ renderer, dist, w, h, stepped }) => {
  const c = document.createElement('canvas');
  c.style.width = `${w}px`;
  c.style.height = `${h}px`;
  document.body.appendChild(c);
  const j = window.j = await window.createJsui({ canvas: c, bundleUrl: 'output/profile/bundle.js', renderer,
    autoFocus: false, autoStart: !stepped, moduleUrl: new URL(`../${dist}/jsui.js`, location.href) });
  j.on('log', () => {});
  if (!stepped) await Promise.race([j.whenIdle(), new Promise((r) => setTimeout(r, 8000))]);
}, { renderer: RENDERER, dist: DIST, w: W, h: H, stepped });
if (!stepped) await page.waitForTimeout(3000);

const cdp = await context.newCDPSession(page);
await cdp.send('Profiler.enable');
await cdp.send('Profiler.setSamplingInterval', { interval: 100 });
await cdp.send('Profiler.start');
// Frames stepped by hand, each awaited until the GPU finished it: the
// profile then holds only frame work (and the waits, which are idle).
const frameMs = await page.evaluate(async ({ n, scenario }) => {
  const j = window.j;
  j.stop();
  const gl = j._gl, px = new Uint8Array(4);
  const sync = async () => {
    if (j._wgpu) await j._wgpu.device.queue.onSubmittedWorkDone();
    else if (gl) gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
  };
  let t = performance.now(), total = 0;
  for (let i = 0; i < n; i++) {
    t += 16.67;
    if (scenario === 'scroll') {
      const c = j.canvas, r = c.getBoundingClientRect();
      c.dispatchEvent(new WheelEvent('wheel', { clientX: r.right - 200, clientY: r.top + r.height * 0.55,
        deltaY: Math.floor(i / 60) % 2 ? -12 : 12, bubbles: true, cancelable: true }));
    }
    const a = performance.now();
    j.frame(t);
    total += performance.now() - a;
    await sync();
  }
  return total / n;
}, { n: N, scenario: SCENARIO });
const { profile } = await cdp.send('Profiler.stop');
await browser.close();
server.close();

// Inclusive time per function (each sample counts once per function on its
// stack) and self time.
const byId = new Map(profile.nodes.map((nd) => [nd.id, nd]));
const parent = new Map();
for (const nd of profile.nodes) for (const c of nd.children ?? []) parent.set(c, nd.id);
const incl = new Map(), self = new Map();
const dt = profile.timeDeltas;
const stackOf = new Map();
const stack = (id) => {
  let s = stackOf.get(id);
  if (s) return s;
  s = [];
  for (let k = id; k != null; k = parent.get(k)) s.push(byId.get(k).callFrame.functionName || '(anon)');
  stackOf.set(id, s);
  return s;
};
for (let i = 0; i < profile.samples.length; i++) {
  const d = (dt[i + 1] ?? dt[i]) / 1000;  // ms until the next sample
  const s = stack(profile.samples[i]);
  if (s[0] === '(idle)' || s[0] === '(program)' || s[0] === '(garbage collector)') {
    self.set(s[0], (self.get(s[0]) ?? 0) + d);
    continue;
  }
  self.set(s[0], (self.get(s[0]) ?? 0) + d);
  for (const f of new Set(s)) incl.set(f, (incl.get(f) ?? 0) + d);
}
const per = (v) => (v / N).toFixed(3);
const sumWhere = (re) => {
  let t = 0;
  for (let i = 0; i < profile.samples.length; i++) {
    const s = stack(profile.samples[i]);
    if (s.some((f) => re.test(f))) t += (dt[i + 1] ?? dt[i]) / 1000;
  }
  return t;
};
const sumUnder = (outer, inner) => {
  let t = 0;
  for (let i = 0; i < profile.samples.length; i++) {
    const s = stack(profile.samples[i]);
    const o = s.findIndex((f) => outer.test(f));
    if (o >= 0 && s.slice(0, o).some((f) => inner.test(f))) t += (dt[i + 1] ?? dt[i]) / 1000;
  }
  return t;
};
console.log(`${path.relative(ROOT, entry)} ${W}x${H}@${DPR} ${RENDERER} ${SCENARIO}: ${N} frames, jsui.frame() ${frameMs.toFixed(2)} ms mean (profiled)`);
const phases = [
  ['frame (jsui.frame)', sumWhere(/^frame$/)],
  ['tick', sumWhere(/^_?jsui_tick$/)],
  ['  js (QuickJS)', sumUnder(/^_?jsui_tick$/, /^(JS_|js_|__JS_|quickjs|free_function_bytecode|gc_)/)],
  ['  animated', sumUnder(/^_?jsui_tick$/, /NativeAnimated.*tick|Animated.*::tick/)],
  ['  layout', sumUnder(/^_?jsui_tick$/, /Document::layout/)],
  ['render', sumWhere(/^_?jsui_render_(webgpu|gl)/)],
  ['  damage', sumUnder(/^_?jsui_render_(webgpu|gl)/, /updateDamage/)],
  ['  paint', sumUnder(/^_?jsui_render_(webgpu|gl)/, /paintDocument/)],
  ['  snap', sumUnder(/^_?jsui_render_(webgpu|gl)/, /Recorder::snap|snapDrawPass|TaskList::prepareResources/)],
  ['  commands', sumUnder(/^_?jsui_render_(webgpu|gl)/, /TaskList::addCommands/)],
  ['  submit', sumUnder(/^_?jsui_render_(webgpu|gl)/, /QueueManager::submitToGpu|Context::submit|flushAndSubmit/)],
  ['  surface', sumUnder(/^_?jsui_render_(webgpu|gl)/, /^getCurrentTexture$|onCreateWrappedTexture|Surface::~Surface/)],
];
for (const [k, v] of phases) console.log(`  ${k.padEnd(22)} ${per(v)} ms/frame`);
const top = (m) => [...m].filter(([k]) => !k.startsWith('(')).sort((a, b) => b[1] - a[1]).slice(0, TOP);
console.log('top inclusive (ms/frame):');
for (const [k, v] of top(incl)) console.log(`  ${per(v).padStart(7)}  ${k.slice(0, 150)}`);
console.log('top self (ms/frame):');
for (const [k, v] of top(self)) console.log(`  ${per(v).padStart(7)}  ${k.slice(0, 150)}`);
fs.writeFileSync(path.join(OUT, 'profile.cpuprofile'), JSON.stringify(profile));
