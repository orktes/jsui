// Browser tests for the WASM build + web host (web/jsui-web.js) in headless
// Chromium via Playwright. Serves web/ over HTTP, loads hand-written bridge
// bundles from web/test/fixtures and checks real screen pixels (Playwright
// screenshots, i.e. after the WebGL / 2D blit and page compositing).
//
//   node web/test/browser.test.mjs            (needs web/dist from the wasm build)
//   JSUI_WEB_STRICT=1 node web/test/browser.test.mjs
//   JSUI_WEB_DIST=dist-webgl node web/test/browser.test.mjs   (the GPU module,
//       WEBGL=1 scripts/build-wasm.sh: every test runs against it, and the
//       'gpu' tests draw through Skia on WebGL instead of SKIPping)
//   JSUI_WEB_DIST=dist-gpu node web/test/browser.test.mjs     (every backend,
//       GPU=1 scripts/build-wasm.sh: the 'webgpu' tests draw through Skia
//       Graphite on WebGPU too; headless Chromium exposes a SwiftShader
//       fallback adapter with --enable-unsafe-webgpu)
//
// Tests that depend on other parts of the runtime (engine host messages,
// input hit testing) report SKIP when that part is still a skeleton stub;
// JSUI_WEB_STRICT=1 turns those skips into failures.
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromium } from 'playwright';

const WEB = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const STRICT = !!process.env.JSUI_WEB_STRICT;
const ONLY = process.env.JSUI_WEB_ONLY;  // substring filter on test names
const DIST = process.env.JSUI_WEB_DIST || 'dist';  // module directory under web/
const FONTS = path.join(WEB, '../assets/fonts');

if (!fs.existsSync(path.join(WEB, DIST, 'jsui.wasm'))) {
  console.error(`web/${DIST}/jsui.wasm missing: build the wasm target first (see cmake/Emscripten.cmake)`);
  process.exit(1);
}

// ------------------------------------------------------------------ server
const MIME = { '.js': 'text/javascript', '.mjs': 'text/javascript', '.html': 'text/html', '.wasm': 'application/wasm',
  '.json': 'application/json', '.png': 'image/png', '.map': 'application/json' };
const dev = { version: 'v1', bundle: '' };
const setDevColor = (color) => {
  dev.bundle = `(function(){const N=__jsui_native;const id=N.createNode('View');` +
    `N.setProps(id,{style:{width:2000,height:100,backgroundColor:'${color}'}});N.appendChild(N.rootId,id);})();`;
};
setDevColor('#ff8800');

const server = http.createServer((req, res) => {
  const url = new URL(req.url, 'http://x');
  if (url.pathname === '/__jsui/version') {
    res.writeHead(200, { 'content-type': 'text/plain', 'cache-control': 'no-store' });
    return res.end(dev.version);
  }
  if (url.pathname === '/dev/bundle.js') {
    res.writeHead(200, { 'content-type': 'text/javascript', 'cache-control': 'no-store' });
    return res.end(dev.bundle);
  }
  if (url.pathname === '/slow/solid.js') {  // delayed response for destroy-during-load tests
    return setTimeout(() => {
      res.writeHead(200, { 'content-type': 'text/javascript' });
      res.end(fs.readFileSync(path.join(WEB, 'test/fixtures/solid.js')));
    }, 150);
  }
  if (url.pathname.startsWith('/__fonts/')) {  // the repo's fonts (assets/fonts), for the font tests
    const font = path.join(FONTS, path.basename(decodeURIComponent(url.pathname)));
    if (!fs.existsSync(font)) {
      res.writeHead(404);
      return res.end('not found');
    }
    res.writeHead(200, { 'content-type': 'font/ttf' });
    return fs.createReadStream(font).pipe(res);
  }
  const file = path.normalize(path.join(WEB, decodeURIComponent(url.pathname)));
  if (!file.startsWith(WEB) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) {
    res.writeHead(404);
    return res.end('not found');
  }
  res.writeHead(200, { 'content-type': MIME[path.extname(file)] || 'application/octet-stream' });
  fs.createReadStream(file).pipe(res);
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const ORIGIN = `http://127.0.0.1:${server.address().port}`;
const PAGE = `${ORIGIN}/test/page.html`;

// ------------------------------------------------------------------ harness
const browser = await chromium.launch({
  args: ['--enable-unsafe-swiftshader', '--use-angle=swiftshader', '--ignore-gpu-blocklist',
    '--force-color-profile=srgb',
    // WebGPU on SwiftShader; Vulkan-backed so canvas presentation works headless
    // (without it the device breaks once a canvas texture is used).
    '--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-vulkan=swiftshader'],
});

class Skip extends Error {}
const skip = (why, soft = false) => { const e = new Skip(why); e.soft = soft; throw e; };
const tests = [];
// Tests registered with { worker: true } run twice: as written, and as
// 'worker: <name>' with createJsui({ worker: true }) (web/jsui-worker.js:
// the runtime and its rendering in a Worker). mount(), spy() and the other
// helpers follow the running test's mode (WORKER).
let WORKER = false;
const test = (name, fn, ctxOpts = {}, { worker = false } = {}) => {
  tests.push({ name, fn, ctxOpts, worker: false });
  if (worker) tests.push({ name: `worker: ${name}`, fn, ctxOpts, worker: true });
};
const assert = (cond, msg) => { if (!cond) throw new Error(msg); };
const near = (px, want, tol = 3) => want.every((v, i) => Math.abs(px[i] - v) <= tol);
const fmt = (px) => `(${Array.from(px).join(',')})`;

async function newPage(ctxOpts) {
  const context = await browser.newContext({ viewport: { width: 800, height: 600 }, deviceScaleFactor: 1, ...ctxOpts });
  const page = await context.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(String(e)));
  page.on('console', (m) => {
    if (m.type() === 'error') errors.push(m.text());
    if (process.env.JSUI_WEB_DEBUG) console.log(`        console.${m.type()}: ${m.text()}`);
  });
  await page.goto(PAGE);
  await page.waitForFunction(() => window.testReady);
  return { page, context, errors };
}

// Creates a canvas + runtime in the page; resolves once the first frame was painted.
// Tests pin the module (web/<DIST>) and default to the CPU renderer, so they
// check the same path whichever module is under test; GPU tests pass
// renderer 'webgl' / 'webgpu' / 'auto' explicitly.
async function mount(page, opts = {}) {
  const info = await page.evaluate(async ({ id, w, h, opts, dist, worker }) => {
    opts = { renderer: 'cpu', moduleUrl: new URL(`../${dist}/jsui.js`, location.href), worker, ...opts };
    const c = document.createElement('canvas');
    c.id = id;
    c.style.width = `${w}px`;
    c.style.height = `${h}px`;
    document.body.appendChild(c);
    const jsui = await window.createJsui({ canvas: c, ...opts });
    (window.hosts ??= {})[id] = jsui;
    jsui.messages = [];
    jsui.errors = [];
    jsui.on('message', (name, data) => jsui.messages.push([name, data]));
    jsui.on('error', (msg) => jsui.errors.push(msg));
    if (!opts.expectError) {
      await Promise.race([jsui.whenIdle(), new Promise((r) => setTimeout(r, 5000))]);
    }
    return { renderer: jsui.renderer, pw: jsui.pixelWidth, ph: jsui.pixelHeight, worker: !!jsui.worker };
  }, { id: opts.id ?? 'ui', w: opts.width ?? 320, h: opts.height ?? 200, opts: opts.jsui ?? {}, dist: DIST, worker: WORKER });
  // Worker mode must not have fallen back to the main thread.
  if ((opts.jsui?.worker ?? WORKER) && !info.worker) throw new Error('worker mode fell back to the main thread');
  return info;
}

// Mounts with renderer 'webgl' (Skia Ganesh on WebGL); SKIPs when the module
// has no GPU backend (the host then fell back to the CPU path, which the
// other tests cover).
async function mountGpu(page, opts = {}) {
  const info = await mount(page, { ...opts, jsui: { ...opts.jsui, renderer: 'webgl' } });
  // Expected to be missing from the default (CPU) module: not a STRICT failure there.
  if (!info.renderer.startsWith('gpu')) skip(`module in web/${DIST} has no GPU backend (renderer ${info.renderer})`, DIST === 'dist');
  return info;
}

// Mounts with renderer 'webgpu' (Skia Graphite on WebGPU); SKIPs when the
// module has no WebGPU backend (only web/dist-gpu has) or the browser no
// WebGPU adapter.
async function mountWebGPU(page, opts = {}) {
  const info = await mount(page, { ...opts, jsui: { ...opts.jsui, renderer: 'webgpu' } });
  if (info.renderer !== 'webgpu') skip(`no WebGPU renderer with web/${DIST} (renderer ${info.renderer})`, DIST !== 'dist-gpu');
  return info;
}
const skipNoBackend = (mode, name) => {
  if (mode === 'webgpu' && name !== 'webgpu') skip(`no WebGPU renderer with web/${DIST} (renderer ${name})`, DIST !== 'dist-gpu');
  if (mode === 'webgl' && !name.startsWith('gpu')) skip(`module in web/${DIST} has no GPU backend`, DIST === 'dist');
};

// Difference stats of two runtimes' premultiplied RGBA frames.
const frameDiff = (page, ids) => page.evaluate(async (ids) => {
  const [a, b] = await Promise.all(ids.map((id) => window.hosts[id].readPixelsAsync()));
  if (!a || !b) return { missing: true };
  if (a.data.length !== b.data.length) return { size: [a.width, a.height, b.width, b.height] };
  let sum = 0, max = 0, over = 0;
  for (let i = 0; i < a.data.length; i += 4) {
    let m = 0;
    for (let c = 0; c < 4; c++) { const v = Math.abs(a.data[i + c] - b.data[i + c]); sum += v; m = Math.max(m, v); }
    max = Math.max(max, m);
    if (m > 24) over++;
  }
  return { mean: sum / a.data.length, max, over: over / (a.data.length / 4) };
}, ids);

const frames = (page, n = 3) => page.evaluate(async (n) => { for (let i = 0; i < n; i++) await new Promise(requestAnimationFrame); }, n);

// Reads screen pixels (device px) of element `sel` at the given element-relative device coordinates.
async function screenPixels(page, sel, points) {
  const png = await page.locator(sel).screenshot();
  return page.evaluate(async ({ b64, points }) => {
    const blob = await (await fetch(`data:image/png;base64,${b64}`)).blob();
    const bmp = await createImageBitmap(blob);
    const c = new OffscreenCanvas(bmp.width, bmp.height);
    const g = c.getContext('2d');
    g.drawImage(bmp, 0, 0);
    return points.map(([x, y]) => Array.from(g.getImageData(x, y, 1, 1).data));
  }, { b64: png.toString('base64'), points });
}

// Wraps module exports to record calls (decoding string args where named);
// in worker mode the worker's (ProxyView._debug).
async function spy(page, id = 'ui') {
  if (WORKER) return page.evaluate((id) => window.hosts[id]._debug('spy'), id);
  await page.evaluate((id) => {
    const jsui = window.hosts[id], M = jsui.module;
    const calls = window.calls = [];
    const strArgs = { _jsui_send_key: [1, 2], _jsui_send_text: [1], _jsui_set_gamepad_connected: [3] };
    for (const name of ['_jsui_send_mouse_move', '_jsui_send_mouse_button', '_jsui_send_mouse_wheel',
      '_jsui_send_mouse_leave', '_jsui_send_touch', '_jsui_send_key', '_jsui_send_text', '_jsui_send_gamepad_button',
      '_jsui_send_gamepad_axis', '_jsui_set_gamepad_connected']) {
      const f = M[name];
      const wrapper = (...a) => {
        const rec = a.slice(1).map((v, i) => (strArgs[name]?.includes(i + 1) ? (v ? M.UTF8ToString(v) : null) : v));
        calls.push([name.slice(6), ...rec]);
        const r = f(...a);
        M[name] = wrapper;  // Emscripten's lazy export stubs overwrite Module[name] on first call
        return r;
      };
      M[name] = wrapper;
    }
  }, id);
}
const takeCalls = (page, id = 'ui') => page.evaluate(({ id, worker }) => (worker ? window.hosts[id]._debug('calls')
  : window.calls.splice(0)), { id, worker: WORKER });
// Resolves once runtime `id` had a frame with nothing left to load or paint
// (in worker mode: input posted so far was applied, its events came back).
const idle = (page, id = 'ui') => page.evaluate((id) => window.hosts[id].whenIdle(), id);
// Waits for `fn(arg)` in the page to be truthy.
const until = (page, fn, arg, timeout = 3000) => page.waitForFunction(fn, arg, { timeout, polling: 10 });

const BANDS = [[10, 25], [10, 75], [10, 125], [10, 175]];
const RED = [255, 0, 0, 255], GREEN = [0, 255, 0, 255], BLUE = [0, 0, 255, 255], HALF_RED = [255, 127, 127, 255];

// ------------------------------------------------------------------- tests
for (const renderer of ['cpu', '2d']) {
  test(`renders bands with correct premultiplied blending (${renderer})`, async ({ page }) => {
    const info = await mount(page, { jsui: { bundleUrl: 'fixtures/bands.js', renderer } });
    assert(renderer === '2d' ? info.renderer === '2d' : info.renderer.startsWith('webgl'), `renderer ${info.renderer}`);
    assert(info.pw === 320 && info.ph === 200, `backing store ${info.pw}x${info.ph}`);
    const px = await screenPixels(page, '#ui', BANDS);
    [RED, GREEN, BLUE, HALF_RED].forEach((want, i) => assert(near(px[i], want), `band ${i}: ${fmt(px[i])} want ${fmt(want)}`));
    const raw = await page.evaluate(async () => { const p = await window.hosts.ui.readPixelsAsync(); const o = (175 * p.width + 10) * 4; return Array.from(p.data.slice(o, o + 4)); });
    assert(near(raw, [128, 0, 0, 128], 1), `premultiplied frame ${fmt(raw)}`);
    const below = await page.evaluate(() => getComputedStyle(document.getElementById('ui')).cursor);
    assert(below === 'default', `cursor ${below}`);
  }, {}, { worker: true });
}

test('devicePixelRatio 2: backing store and 1:1 blit', async ({ page }) => {
  const info = await mount(page, { jsui: { bundleUrl: 'fixtures/bands.js' } });
  assert(info.pw === 640 && info.ph === 400, `backing store ${info.pw}x${info.ph}`);
  const px = await screenPixels(page, '#ui', [[20, 99], [20, 100], [20, 199], [20, 200], [600, 50]]);
  assert(near(px[0], RED) && near(px[1], GREEN), `edge at y=100: ${fmt(px[0])} ${fmt(px[1])}`);
  assert(near(px[2], GREEN) && near(px[3], BLUE), `edge at y=200: ${fmt(px[2])} ${fmt(px[3])}`);
  assert(near(px[4], RED), `x=600: ${fmt(px[4])}`);
}, { deviceScaleFactor: 2 }, { worker: true });

test('canvas resize updates the viewport without stretching', async ({ page }) => {
  await mount(page, { jsui: { bundleUrl: 'fixtures/bands.js' } });
  await page.evaluate(() => { document.getElementById('ui').style.height = '300px'; document.getElementById('ui').style.width = '400px'; });
  await frames(page, 4);
  const size = await page.evaluate(() => [window.hosts.ui.pixelWidth, window.hosts.ui.pixelHeight, window.hosts.ui.width]);
  assert(size[0] === 400 && size[1] === 300 && size[2] === 400, `after resize ${size}`);
  const px = await screenPixels(page, '#ui', [[10, 25], [10, 75], [10, 250], [380, 25]]);
  assert(near(px[0], RED) && near(px[1], GREEN), `bands moved/stretched: ${fmt(px[0])} ${fmt(px[1])}`);
  assert(near(px[2], [255, 255, 255, 255]) && near(px[3], [255, 255, 255, 255]), `outside bands: ${fmt(px[2])} ${fmt(px[3])}`);
}, {}, { worker: true });

test('WebGL context loss and restore repaints', async ({ page }) => {
  await mount(page, { jsui: { bundleUrl: 'fixtures/bands.js' } });
  const ok = await page.evaluate(async () => {
    const ext = window.hosts.ui._gl?.getExtension('WEBGL_lose_context');
    if (!ext) return false;
    ext.loseContext();
    for (let i = 0; i < 3; i++) await new Promise(requestAnimationFrame);
    ext.restoreContext();
    for (let i = 0; i < 6; i++) await new Promise(requestAnimationFrame);
    return true;
  });
  if (!ok) skip('WEBGL_lose_context unavailable');
  const px = await screenPixels(page, '#ui', BANDS);
  [RED, GREEN].forEach((want, i) => assert(near(px[i], want), `band ${i} after restore: ${fmt(px[i])}`));
});

test('timers drive repaints', async ({ page }) => {
  await mount(page, { jsui: { bundleUrl: 'fixtures/bands.js' } });
  await page.waitForTimeout(400);
  await frames(page, 2);
  const [px] = await screenPixels(page, '#ui', [BANDS[2]]);
  assert(near(px, [255, 255, 0, 255]), `band 2 after timer: ${fmt(px)}`);
}, {}, { worker: true });

test('profiling: getPerf() reports phase times and counters', async ({ page }) => {
  await mount(page, { jsui: { bundleUrl: 'fixtures/bands.js' } });
  await frames(page, 2);
  const perf = await page.evaluate(async () => {
    const h = window.hosts.ui;
    h.setProfiling(true);
    await h.getPerf();  // reset (a Promise in worker mode)
    h.postMessage('paint', { color: '#ff00ff' });
    for (let i = 0; i < 3; i++) await new Promise((r) => requestAnimationFrame(r));
    await h.whenIdle();
    const p = await h.getPerf();
    h.setProfiling(false);
    return p;
  });
  assert(perf && typeof perf.paint_us === 'number' && typeof perf.bridge_calls === 'number', `getPerf: ${JSON.stringify(perf)}`);
  const total = perf.js_us + perf.commit_us + perf.style_us + perf.layout_us + perf.paint_us + perf.gpu_submit_us +
    perf.other_us;
  assert(total > 0, `no time measured: ${JSON.stringify(perf)}`);
}, {}, { worker: true });

test('host -> UI messages (jsui.postMessage)', async ({ page }) => {
  await mount(page, { jsui: { bundleUrl: 'fixtures/bands.js' } });
  await page.evaluate(() => window.hosts.ui.postMessage('paint', { color: '#ff00ff' }));
  await frames(page, 3);
  const [px] = await screenPixels(page, '#ui', [BANDS[0]]);
  if (near(px, RED)) skip('engine hostMessage() is a skeleton stub');
  assert(near(px, [255, 0, 255, 255]), `band 0 after message: ${fmt(px)}`);
}, {}, { worker: true });

test('UI -> host messages with UTF-8 payload', async ({ page }) => {
  await mount(page, { jsui: { bundleUrl: 'fixtures/bands.js' } });
  await frames(page, 2);
  const msgs = await page.evaluate(() => window.hosts.ui.messages);
  if (!msgs.length) skip('engine __jsui_native.postMessage not implemented yet');
  const ready = msgs.find(([n]) => n === 'ready');
  assert(ready && ready[1].text === 'héllo ✓ 日本' && ready[1].bands === 4, `messages ${JSON.stringify(msgs)}`);
}, {}, { worker: true });

test('pointer, wheel, keyboard and text input are forwarded', async ({ page }) => {
  await mount(page, { jsui: { bundleUrl: 'fixtures/bands.js' } });
  await spy(page);
  await page.mouse.move(100, 75);
  await page.mouse.down();
  await page.mouse.up();
  await page.mouse.click(50, 20, { button: 'right' });
  await page.mouse.wheel(0, 120);
  await page.waitForTimeout(50);
  let calls = await takeCalls(page);
  const has = (pred, what) => assert(calls.some(pred), `${what} missing in ${JSON.stringify(calls)}`);
  has(([n, x, y]) => n === 'send_mouse_move' && x === 100 && y === 75, 'mouse_move(100,75)');
  has(([n, x, y, b, d]) => n === 'send_mouse_button' && x === 100 && y === 75 && b === 0 && d === 1, 'left down');
  has(([n, x, y, b, d]) => n === 'send_mouse_button' && b === 0 && d === 0, 'left up');
  has(([n, x, y, b, d]) => n === 'send_mouse_button' && x === 50 && y === 20 && b === 2 && d === 1, 'right down');
  has(([n, , , dx, dy]) => n === 'send_mouse_wheel' && dx === 0 && dy === 120, 'wheel');

  await page.keyboard.press('Tab');
  await page.keyboard.down('Shift');
  await page.keyboard.press('KeyA');
  await page.keyboard.up('Shift');
  await page.keyboard.type('é');
  await page.keyboard.insertText('日本語');  // IME-style commit without key events
  await page.waitForTimeout(50);
  calls = await takeCalls(page);
  has(([n, k, c, d]) => n === 'send_key' && k === 'Tab' && c === 'Tab' && d === 1, 'Tab down');
  has(([n, k, c, d, r, m]) => n === 'send_key' && k === 'A' && c === 'KeyA' && d === 1 && m === 1, 'Shift+A with mods');
  has(([n, t]) => n === 'send_text' && t === 'A', 'text A');
  has(([n, t]) => n === 'send_text' && t === 'é', 'text é');
  has(([n, t]) => n === 'send_text' && t === '日本語', 'text 日本語');
  const texts = calls.filter(([n]) => n === 'send_text').map(([, t]) => t).join('');
  assert(texts === 'Aé日本語', `text sequence ${JSON.stringify(texts)}`);

  // A key held while the window loses focus is released.
  await page.keyboard.down('KeyW');
  await page.evaluate(() => window.dispatchEvent(new Event('blur')));
  calls = await takeCalls(page);
  has(([n, k, c, d]) => n === 'send_key' && c === 'KeyW' && d === 0, 'KeyW released on blur');
  await page.keyboard.up('KeyW');
}, {}, { worker: true });

test('touch input is forwarded', async ({ page }) => {
  await mount(page, { jsui: { bundleUrl: 'fixtures/bands.js' } });
  await spy(page);
  await page.touchscreen.tap(30, 130);
  await page.waitForTimeout(50);
  const calls = await takeCalls(page);
  const touches = calls.filter(([n]) => n === 'send_touch');
  assert(touches.length >= 2 && touches[0][2] === 0 && touches.at(-1)[2] === 2, `touch phases ${JSON.stringify(touches)}`);
  assert(touches[0][3] === 30 && touches[0][4] === 130, `touch pos ${JSON.stringify(touches[0])}`);
}, { hasTouch: true }, { worker: true });

test('gamepad buttons / axes / connection are forwarded', async ({ page }) => {
  await page.evaluate(() => {
    const mk = (pressed, axis) => ({ id: 'Test Pad (STANDARD GAMEPAD)', index: 0, connected: true, mapping: 'standard',
      buttons: Array.from({ length: 17 }, (_, i) => ({ pressed: i === 0 && pressed, value: i === 0 && pressed ? 1 : 0 })),
      axes: [axis, 0, 0, 0] });
    window.__pad = null;
    window.__mkPad = mk;
    navigator.getGamepads = () => [window.__pad, null, null, null];
  });
  await mount(page, { jsui: { bundleUrl: 'fixtures/bands.js' } });
  await spy(page);
  await page.evaluate(() => { window.__pad = window.__mkPad(true, 0.5); window.dispatchEvent(new Event('gamepadconnected')); });
  await frames(page, 3);
  await page.evaluate(() => { window.__pad = window.__mkPad(false, 0); });
  await frames(page, 3);
  await page.evaluate(() => { window.__pad = null; });
  await frames(page, 3);
  const calls = (await takeCalls(page)).filter(([n]) => n.includes('gamepad'));
  const want = [
    ['set_gamepad_connected', 0, 1, 'Test Pad (STANDARD GAMEPAD)'],
    ['send_gamepad_button', 0, 0, 1, 1],
    ['send_gamepad_axis', 0, 0, 0.5],
    ['send_gamepad_button', 0, 0, 0, 0],
    ['send_gamepad_axis', 0, 0, 0],
    ['set_gamepad_connected', 0, 0, null],
  ];
  assert(JSON.stringify(calls) === JSON.stringify(want), `gamepad calls ${JSON.stringify(calls)}`);
}, {}, { worker: true });

test('click reaches the UI and UI messages the host', async ({ page }) => {
  await mount(page, { jsui: { bundleUrl: 'fixtures/bands.js' } });
  await page.mouse.click(100, 75);
  await frames(page, 3);
  await idle(page);
  const [px] = await screenPixels(page, '#ui', [BANDS[1]]);
  if (near(px, GREEN)) skip('InputManager hit testing is a skeleton stub');
  assert(near(px, [255, 255, 255, 255]), `band 1 after click: ${fmt(px)}`);
}, {}, { worker: true });

test('bundle errors are reported (404 and throwing bundle)', async ({ page }) => {
  await mount(page, { id: 'a', jsui: { bundleUrl: 'fixtures/missing.js', expectError: true } });
  await mount(page, { id: 'b', jsui: { bundleUrl: 'fixtures/throws.js', expectError: true } });
  await frames(page, 5);
  const [ea, eb] = await page.evaluate(() => [window.hosts.a.errors, window.hosts.b.errors]);
  assert(ea.some((e) => /failed to load bundle/.test(e) && /404/.test(e) && /fixtures\/missing\.js/.test(e)), `404 error: ${JSON.stringify(ea)}`);
  assert(eb.some((e) => /fixture exploded/.test(e)), `throw error: ${JSON.stringify(eb)}`);
  return { allowErrors: true };
}, {}, { worker: true });

test('several runtimes on one page are independent', async ({ page }) => {
  await mount(page, { id: 'a', jsui: { bundleUrl: 'fixtures/bands.js' } });
  await mount(page, { id: 'b', width: 200, height: 150, jsui: { bundleUrl: 'fixtures/solid.js', bundleSource: undefined } });
  const pa = await screenPixels(page, '#a', [BANDS[0]]);
  const pb = await screenPixels(page, '#b', [[10, 50], [10, 120]]);
  assert(near(pa[0], RED), `a: ${fmt(pa[0])}`);
  assert(near(pb[0], [0, 128, 128, 255]) && near(pb[1], [255, 255, 255, 255]), `b: ${fmt(pb[0])} ${fmt(pb[1])}`);
  await page.evaluate(() => window.hosts.b.destroy());
  await page.waitForTimeout(350);
  await frames(page, 2);
  const [p2] = await screenPixels(page, '#a', [BANDS[2]]);
  assert(near(p2, [255, 255, 0, 255]), `a keeps running after b destroyed: ${fmt(p2)}`);
}, {}, { worker: true });

test('moduleVersions: only deployed modules are tried, jsui.js and jsui.wasm carry the version', async ({ page }) => {
  const requests = [];
  page.on('request', (r) => requests.push(new URL(r.url())));
  const info = await mount(page, { jsui: { bundleUrl: 'fixtures/bands.js', moduleUrl: null,
    moduleBase: `${ORIGIN}/`, moduleVersions: { [DIST]: 'abc123' } } });
  assert(info.renderer, 'no renderer');
  const mods = requests.filter((u) => /\/jsui\.(js|wasm)$/.test(u.pathname)).map((u) => u.pathname + u.search);
  const want = [`/${DIST}/jsui.js?v=abc123`, `/${DIST}/jsui.wasm?v=abc123`];
  assert(JSON.stringify(mods) === JSON.stringify(want), `module requests ${mods.join(' ')}, want ${want.join(' ')}`);
});

test('bundleSource option and loadSource()', async ({ page }) => {
  const src = fs.readFileSync(path.join(WEB, 'test/fixtures/solid.js'), 'utf8');
  await mount(page, { jsui: { bundleSource: src } });
  await frames(page, 3);
  const [px] = await screenPixels(page, '#ui', [[10, 50]]);
  assert(near(px, [0, 128, 128, 255]), `bundleSource: ${fmt(px)}`);
}, {}, { worker: true });

test('fonts: registerFont, loadFont, unregisterFont, fallbacks, default family', async ({ page }) => {
  await mount(page, { jsui: { bundleUrl: 'fixtures/solid.js' } });
  const r = await page.evaluate(async () => {
    const h = window.hosts.ui;
    const italic = await (await fetch('/__fonts/Inter-Italic.ttf')).arrayBuffer();
    const out = {
      load: await h.loadFont('Test Display', '/__fonts/Inter-Bold.ttf'),
      loadOpts: await h.loadFont('Test Display', '/__fonts/Inter-Regular.ttf', { weight: 400, style: 'normal' }),
      missing: await h.loadFont('Missing', '/__fonts/missing.ttf'),
      bytes: await h.registerFont('Test Italic', new Uint8Array(italic), 400, true),
      fromFile: await h.registerFont(null, italic),  // family, weight and style from the file
      bad: await h.registerFont('Bad', new Uint8Array([1, 2, 3, 4])),
    };
    h.setFontFallbacks(['Test Italic', 'Test Display']);
    h.setFontFallbacks(null);
    h.setDefaultFontFamily('Test Display');
    h.setDefaultFontFamily('Inter');
    out.unregister = await h.unregisterFont('Test Display');
    out.unregisterAgain = await h.unregisterFont('Test Display');
    await h.whenIdle();
    return out;
  });
  const want = { load: true, loadOpts: true, missing: false, bytes: true, fromFile: true, bad: false, unregister: true,
    unregisterAgain: false };
  assert(JSON.stringify(r) === JSON.stringify(want), `fonts: ${JSON.stringify(r)}, want ${JSON.stringify(want)}`);
  return { allowErrors: true };  // the missing font's 404 and the refused bytes are logged
}, {}, { worker: true });

for (const renderer of ['auto', 'webgl', 'webgpu']) test(`create/destroy cycles: no heap growth, destroy during load is safe (${renderer})`, async ({ page }) => {
  const result = await page.evaluate(async ({ dist, renderer }) => {
    const moduleUrl = new URL(`../${dist}/jsui.js`, location.href);
    const c = document.createElement('canvas');
    c.style.width = '320px'; c.style.height = '200px';
    document.body.appendChild(c);
    let used = null;
    const cycle = async (n, url, wait) => {
      for (let i = 0; i < n; i++) {
        const j = await window.createJsui({ canvas: c, bundleUrl: url, autoFocus: false, moduleUrl, renderer });
        used ??= j.renderer;
        if (wait) { for (let f = 0; f < 3; f++) { j.frame(); await new Promise(requestAnimationFrame); } }
        j.destroy();
      }
    };
    // destroy while the bundle fetch is in flight, then let the responses arrive
    await cycle(10, '/slow/solid.js', false);
    await new Promise((r) => setTimeout(r, 300));
    await cycle(5, 'fixtures/bands.js', true);  // warm up allocator
    const M = (await window.createJsui({ canvas: document.createElement('canvas'), autoStart: false, moduleUrl })).module;
    const before = M.HEAPU8.length;
    await cycle(40, 'fixtures/bands.js', true);
    return { before, after: M.HEAPU8.length, used };
  }, { dist: DIST, renderer });
  if (renderer !== 'auto') skipNoBackend(renderer, result.used);
  assert(result.after <= result.before, `heap grew from ${result.before} to ${result.after}`);
});

test('dev mode reloads when /__jsui/version changes', async ({ page }) => {
  dev.version = 'v1';
  setDevColor('#ff8800');
  await mount(page, { width: 200, height: 120, jsui: { bundleUrl: `${ORIGIN}/dev/bundle.js`, devMode: true } });
  let [px] = await screenPixels(page, '#ui', [[10, 50]]);
  assert(near(px, [255, 136, 0, 255]), `before reload: ${fmt(px)}`);
  setDevColor('#0088ff');
  dev.version = 'v2';
  await page.waitForTimeout(1200);
  await frames(page, 3);
  [px] = await screenPixels(page, '#ui', [[10, 50]]);
  assert(near(px, [0, 136, 255, 255]), `after reload: ${fmt(px)}`);
});

// --------------------------------------------- renderer selection / fallback
test("default renderer 'auto' uses the GPU when the module has it", async ({ page }) => {
  const info = await mount(page, { jsui: { bundleUrl: 'fixtures/bands.js', renderer: undefined } });
  // (The test browser runs SwiftShader with --ignore-gpu-blocklist, which
  // Chrome reports as WebGL without a major performance caveat.)
  const hasGpu = await page.evaluate(async () => {
    const { webglAvailable } = await import('../jsui-web.js');
    const M = window.hosts.ui.module;
    return !!M._jsui_gpu_create_gl && !!M.GL && webglAvailable(true);
  });
  if (hasGpu) assert(info.renderer.startsWith('gpu-webgl'), `auto with the GPU module: ${info.renderer}`);
  else assert(info.renderer.startsWith('webgl'), `auto with the CPU module: ${info.renderer}`);
  const px = await screenPixels(page, '#ui', BANDS);
  [RED, GREEN].forEach((want, i) => assert(near(px[i], want), `band ${i}: ${fmt(px[i])}`));
});

for (const renderer of ['webgl', 'cpu']) test(`WebGL context lost for good: CPU fallback on a fresh canvas (${renderer})`, async ({ page }) => {
  const info = await mount(page, { jsui: { bundleUrl: 'fixtures/bands.js', renderer, contextRestoreTimeout: 100 } });
  if (renderer === 'webgl') skipNoBackend(renderer, info.renderer);
  const r = await page.evaluate(async () => {
    const j = window.hosts.ui, old = j.canvas;
    const ext = j._gl?.getExtension('WEBGL_lose_context');
    if (!ext) return null;
    let switched = null;
    j.on('renderer', (name, reason) => { switched = [name, reason]; });
    j.on('log', () => {});  // the fallback warning is expected
    ext.loseContext();  // never restored
    await new Promise((r) => setTimeout(r, 300));
    for (let i = 0; i < 4; i++) await new Promise(requestAnimationFrame);
    return { switched, renderer: j.renderer, replaced: j.canvas !== old && !old.isConnected && j.canvas.isConnected,
      id: j.canvas.id };
  });
  if (!r) skip('WEBGL_lose_context unavailable');
  assert(r.switched, 'no renderer event');
  assert(r.replaced && r.id === 'ui', `canvas not replaced: ${JSON.stringify(r)}`);
  assert(renderer === 'webgl' ? r.renderer.startsWith('webgl') : r.renderer === '2d', `fallback renderer ${r.renderer}`);
  const px = await screenPixels(page, '#ui', BANDS);
  [RED, GREEN, null, HALF_RED].forEach((want, i) => want && assert(near(px[i], want), `band ${i} after fallback: ${fmt(px[i])}`));
});

// ------------------------------------------------ perf regression guards
// Cheap, deterministic proxies for frame cost: an unchanged UI must not
// repaint or upload anything, and a small change must upload only the
// damaged rows / rect, not the whole frame.
for (const renderer of ['cpu', '2d', 'webgl', 'webgpu']) test(`perf guard: idle frames do not repaint, a change uploads only its damage (${renderer})`, async ({ page }) => {
  const info = await mount(page, { jsui: { bundleUrl: 'fixtures/bands.js', renderer } });
  skipNoBackend(renderer, info.renderer);
  await page.waitForTimeout(300);  // band 2's 200 ms timer
  const r = await page.evaluate(async () => {
    const j = window.hosts.ui, st = j.stats;
    j.stop();
    let t = performance.now();
    const step = (n) => { for (let i = 0; i < n; i++) j.frame(t += 16.67); };
    step(3);  // settle (timer repaint)
    const r0 = st.renders, b0 = st.blits, p0 = st.blitPixels;
    step(30);
    const idle = { renders: st.renders - r0, blits: st.blits - b0 };
    j.postMessage('paint', { color: '#ff00ff' });  // band 0: 320x50 of 320x200
    step(1);
    const change = { renders: st.renders - r0, blits: st.blits - b0, pixels: st.blitPixels - p0, last: st.lastBlit };
    step(30);
    const after = { renders: st.renders - r0 };
    return { idle, change, after, w: j.pixelWidth, h: j.pixelHeight };
  });
  assert(r.idle.renders === 0 && r.idle.blits === 0, `idle frames repainted: ${JSON.stringify(r.idle)}`);
  assert(r.change.renders === 1, `change: ${JSON.stringify(r.change)}`);
  assert(r.after.renders === 1, `frames after the change repainted: ${JSON.stringify(r.after)}`);
  if (renderer === 'cpu' || renderer === '2d') {
    const l = r.change.last;
    // Band 0 is rows 0-49 (the damage may include a row of anti-aliasing outset).
    assert(r.change.blits === 1 && l && l.x === 0 && l.y === 0 && l.width === 320 && l.height >= 50 && l.height <= 52 &&
      r.change.pixels === l.width * l.height, `upload was not just band 0: ${JSON.stringify(r.change)} of ${r.w}x${r.h}`);
  }
  const [px] = await screenPixels(page, '#ui', [BANDS[0]]);
  assert(near(px, [255, 0, 255, 255]), `band 0 after the partial upload: ${fmt(px)}`);
});

// ---------------------------------------------------- GPU (Skia on WebGL)
test('gpu: renders bands with correct premultiplied blending', async ({ page }) => {
  const info = await mountGpu(page, { jsui: { bundleUrl: 'fixtures/bands.js' } });
  assert(info.pw === 320 && info.ph === 200, `backing store ${info.pw}x${info.ph}`);
  await page.waitForTimeout(300);  // band 2 turns yellow on a 200 ms timer: repaints follow
  await frames(page, 3);
  const px = await screenPixels(page, '#ui', BANDS);
  [RED, GREEN, [255, 255, 0, 255], HALF_RED].forEach((want, i) => assert(near(px[i], want), `band ${i}: ${fmt(px[i])} want ${fmt(want)}`));
  const raw = await page.evaluate(() => { const p = window.hosts.ui.readPixels(); const o = (175 * p.width + 10) * 4; return Array.from(p.data.slice(o, o + 4)); });
  assert(near(raw, [128, 0, 0, 128], 1), `premultiplied frame ${fmt(raw)}`);
});

for (const fixture of ['hud', 'effects']) test(`gpu: matches the CPU raster (${fixture} fixture)`, async ({ page }) => {
  await mountGpu(page, { id: 'gpu', width: 480, height: 320, jsui: { bundleUrl: `fixtures/${fixture}.js` } });
  await mount(page, { id: 'cpu', width: 480, height: 320, jsui: { bundleUrl: `fixtures/${fixture}.js`, renderer: 'cpu' } });
  const d = await page.evaluate(() => {
    const a = window.hosts.gpu.readPixels(), b = window.hosts.cpu.readPixels();
    if (a.data.length !== b.data.length) return { size: [a.width, a.height, b.width, b.height] };
    let sum = 0, max = 0, over = 0;
    for (let i = 0; i < a.data.length; i += 4) {
      let m = 0;
      for (let c = 0; c < 4; c++) { const v = Math.abs(a.data[i + c] - b.data[i + c]); sum += v; m = Math.max(m, v); }
      max = Math.max(max, m);
      if (m > 24) over++;
    }
    return { mean: sum / a.data.length, max, over: over / (a.data.length / 4) };
  });
  assert(!d.size, `size mismatch ${d.size}`);
  console.log(`      gpu vs cpu: max ${d.max}, mean ${d.mean.toFixed(3)}, >24 ${(d.over * 100).toFixed(3)}%`);
  assert(d.mean < 1 && d.over < 0.01, `GPU frame differs: mean ${d.mean}, >24 ${d.over}`);
});

test('gpu: devicePixelRatio 2 and resize', async ({ page }) => {
  const info = await mountGpu(page, { jsui: { bundleUrl: 'fixtures/bands.js' } });
  assert(info.pw === 640 && info.ph === 400, `backing store ${info.pw}x${info.ph}`);
  let px = await screenPixels(page, '#ui', [[20, 99], [20, 100], [20, 199], [20, 200], [600, 50]]);
  assert(near(px[0], RED) && near(px[1], GREEN), `edge at y=100: ${fmt(px[0])} ${fmt(px[1])}`);
  // band 2 is blue, or yellow once its 200 ms timer fired
  assert(near(px[2], GREEN) && (near(px[3], BLUE) || near(px[3], [255, 255, 0, 255])), `edge at y=200: ${fmt(px[2])} ${fmt(px[3])}`);
  await page.evaluate(() => { document.getElementById('ui').style.height = '300px'; document.getElementById('ui').style.width = '400px'; });
  await frames(page, 4);
  px = await screenPixels(page, '#ui', [[20, 50], [20, 150], [700, 50], [20, 500]]);
  assert(near(px[0], RED) && near(px[1], GREEN), `bands after resize: ${px.map(fmt)}`);
  assert(near(px[2], [255, 255, 255, 255]) && near(px[3], [255, 255, 255, 255]), `outside bands: ${px.map(fmt)}`);
}, { deviceScaleFactor: 2 });

test('gpu: WebGL context loss and restore repaints', async ({ page }) => {
  await mountGpu(page, { jsui: { bundleUrl: 'fixtures/bands.js' } });
  const ok = await page.evaluate(async () => {
    const ext = window.hosts.ui._gl?.getExtension('WEBGL_lose_context');
    if (!ext) return false;
    ext.loseContext();
    for (let i = 0; i < 3; i++) await new Promise(requestAnimationFrame);
    ext.restoreContext();
    for (let i = 0; i < 6; i++) await new Promise(requestAnimationFrame);
    return true;
  });
  if (!ok) skip('WEBGL_lose_context unavailable');
  const px = await screenPixels(page, '#ui', BANDS);
  // (band 2 turns yellow on a timer)
  [RED, GREEN, null, HALF_RED].forEach((want, i) => want && assert(near(px[i], want), `band ${i} after restore: ${fmt(px[i])}`));
});

test('gpu: several runtimes on one page and destroy', async ({ page }) => {
  await mountGpu(page, { id: 'a', jsui: { bundleUrl: 'fixtures/bands.js' } });
  await mountGpu(page, { id: 'b', jsui: { bundleUrl: 'fixtures/solid.js' } });
  const px = await screenPixels(page, '#a', BANDS);
  [RED, GREEN, null, HALF_RED].forEach((want, i) => want && assert(near(px[i], want), `a band ${i}: ${fmt(px[i])}`));
  await page.evaluate(() => window.hosts.b.destroy());
  await frames(page, 3);
  const after = await screenPixels(page, '#a', BANDS);
  assert(near(after[0], RED), `a after destroying b: ${fmt(after[0])}`);
});

test('perf: 1080p HUD (~540 nodes), steady state (gpu)', async ({ page }) => {
  await mountGpu(page, { width: 1920, height: 1080, jsui: { bundleUrl: 'fixtures/hud.js' } });
  const r = await page.evaluate(async () => {
    const j = window.hosts.ui;
    j.stop();
    const M = j.module, rt = j.rt;
    const N = 60;
    let tick = 0, paint = 0, t = performance.now();
    for (let i = 0; i < N; i++) {
      t += 16.67;
      const a = performance.now();
      M._jsui_tick(rt, t);
      const b = performance.now();
      j._renderGpu();
      j._gl.finish();
      const c = performance.now();
      tick += b - a; paint += c - b;
    }
    return { tick: tick / N, paint: paint / N, renderer: j.renderer };
  });
  console.log(`      perf (${r.renderer}, 1920x1080, headless/swiftshader): tick ${r.tick.toFixed(2)} ms, Skia GL draw+finish ${r.paint.toFixed(2)} ms`);
});

for (const renderer of ['cpu', '2d']) test(`perf: 1080p HUD (~540 nodes), steady state (${renderer})`, async ({ page }) => {
  await mount(page, { width: 1920, height: 1080, jsui: { bundleUrl: 'fixtures/hud.js', renderer } });
  const r = await page.evaluate(async () => {
    const j = window.hosts.ui;
    j.stop();
    const M = j.module, rt = j.rt;
    const N = 120;
    let tick = 0, paint = 0, blit = 0, t = performance.now();
    for (let i = 0; i < N; i++) {
      t += 16.67;
      const a = performance.now();
      M._jsui_tick(rt, t);
      const b = performance.now();
      j._ensureBuffer();
      M._jsui_render_rgba(rt, j._buf, j.pixelWidth, j.pixelHeight, j.pixelWidth * 4);
      const c = performance.now();
      j._blit();
      j._gl?.finish();
      const d = performance.now();
      tick += b - a; paint += c - b; blit += d - c;
    }
    return { tick: tick / N, paint: paint / N, blit: blit / N, renderer: j.renderer };
  });
  console.log(`      perf (${r.renderer}, 1920x1080, headless/swiftshader): tick ${r.tick.toFixed(2)} ms, paint ${r.paint.toFixed(2)} ms, upload+draw ${r.blit.toFixed(2)} ms`);
});

// ------------------------------------------------- WebGPU (Skia Graphite)
test('webgpu: renders bands with correct premultiplied blending', async ({ page }) => {
  const info = await mountWebGPU(page, { jsui: { bundleUrl: 'fixtures/bands.js' } });
  assert(info.pw === 320 && info.ph === 200, `backing store ${info.pw}x${info.ph}`);
  await page.waitForTimeout(300);  // band 2 turns yellow on a 200 ms timer
  await frames(page, 3);
  const px = await screenPixels(page, '#ui', BANDS);
  [RED, GREEN, [255, 255, 0, 255], HALF_RED].forEach((want, i) => assert(near(px[i], want), `band ${i}: ${fmt(px[i])} want ${fmt(want)}`));
  const raw = await page.evaluate(async () => { const p = await window.hosts.ui.readPixelsAsync(); const o = (175 * p.width + 10) * 4; return Array.from(p.data.slice(o, o + 4)); });
  assert(near(raw, [128, 0, 0, 128], 1), `premultiplied frame ${fmt(raw)}`);
});

for (const fixture of ['hud', 'effects']) test(`webgpu: matches the CPU raster (${fixture} fixture)`, async ({ page }) => {
  await mountWebGPU(page, { id: 'gpu', width: 480, height: 320, jsui: { bundleUrl: `fixtures/${fixture}.js` } });
  await mount(page, { id: 'cpu', width: 480, height: 320, jsui: { bundleUrl: `fixtures/${fixture}.js`, renderer: 'cpu' } });
  const d = await frameDiff(page, ['gpu', 'cpu']);
  assert(!d.missing && !d.size, `readback ${JSON.stringify(d)}`);
  console.log(`      webgpu vs cpu: max ${d.max}, mean ${d.mean.toFixed(3)}, >24 ${(d.over * 100).toFixed(3)}%`);
  assert(d.mean < 1 && d.over < 0.01, `WebGPU frame differs: mean ${d.mean}, >24 ${d.over}`);
});

test('webgpu: incremental repaints match a full repaint', async ({ page }) => {
  // Only damage is repainted into the retained frame: after changes it must
  // still equal the CPU raster's frame.
  await mountWebGPU(page, { id: 'a', jsui: { bundleUrl: 'fixtures/bands.js' } });
  await mount(page, { id: 'b', jsui: { bundleUrl: 'fixtures/bands.js', renderer: 'cpu' } });
  await page.waitForTimeout(300);
  await page.evaluate(() => { for (const id of ['a', 'b']) window.hosts[id].postMessage('paint', { color: '#ff00ff' }); });
  await frames(page, 3);
  const d = await frameDiff(page, ['a', 'b']);
  assert(!d.missing && !d.size, `readback ${JSON.stringify(d)}`);
  assert(d.max <= 2, `incremental WebGPU frame differs from the CPU frame: ${JSON.stringify(d)}`);
  const [px] = await screenPixels(page, '#a', [BANDS[0]]);
  if (near(px, RED)) skip('engine hostMessage() is a skeleton stub');
  assert(near(px, [255, 0, 255, 255]), `band 0 after message: ${fmt(px)}`);
});

test('webgpu: full-frame changes paint straight into the canvas, small ones through the retained frame', async ({ page }) => {
  // web/test/gpu-count.js records the render passes (size, load op).
  await page.addInitScript({ path: path.join(WEB, 'test/gpu-count.js') });
  await page.reload();
  await page.waitForFunction(() => window.testReady);
  const style = (s) => `{position:'absolute',${s}}`;
  const bundleSource = `(function(){const N=__jsui_native;` +
    `const bg=N.createNode('View');const box=N.createNode('View');` +
    `const set=(id,s,c)=>N.setProps(id,{style:Object.assign(s,{backgroundColor:c})});` +
    `const BG=()=>(${style('left:0,top:0,width:320,height:200')}),BOX=()=>(${style('left:10,top:10,width:20,height:20')});` +
    `set(bg,BG(),'#203040');set(box,BOX(),'#ff0000');N.appendChild(N.rootId,bg);N.appendChild(N.rootId,box);` +
    `globalThis.__jsui_onHostMessage=function(name,json){const c=JSON.parse(json).color;` +
    `if(name==='bg')set(bg,BG(),c);if(name==='box')set(box,BOX(),c);};})();`;
  await mountWebGPU(page, { id: 'a', jsui: { bundleSource } });
  await mount(page, { id: 'b', jsui: { bundleSource, renderer: 'cpu' } });
  const step = (name, color) => page.evaluate(async ({ name, color }) => {
    const { a, b } = window.hosts;
    a.stop();
    b.stop();
    window.__gpuCounts.clear();
    window.t = (window.t ?? performance.now()) + 16.67;
    for (const j of [a, b]) {
      j.postMessage(name, { color });
      j.frame(window.t);
    }
    await a._wgpu.device.queue.onSubmittedWorkDone();
    const passes = [...window.__gpuCounts].filter(([k]) => k.startsWith('pass color ')).map(([k, n]) => `${k} x${n}`);
    const canvas = passes.filter((p) => p.startsWith(`pass color ${a.pixelWidth}x${a.pixelHeight} `));
    return { canvas, passes: passes.length, loads: canvas.filter((p) => p.includes('load=load')).length,
      clears: canvas.filter((p) => p.includes('load=clear')).reduce((n, p) => n + Number(p.split(' x').pop()), 0),
      copies: window.__gpuCounts.get('GPUCommandEncoder.copyTextureToTexture') ?? 0,
      submits: window.__gpuCounts.get('GPUQueue.submit') ?? 0,
      encoders: window.__gpuCounts.get('GPUDevice.createCommandEncoder') ?? 0 };
  }, { name, color });
  const same = async (what) => {
    const d = await frameDiff(page, ['a', 'b']);
    assert(!d.missing && !d.size && d.max <= 2, `${what}: WebGPU frame differs from the CPU frame: ${JSON.stringify(d)}`);
  };
  // Most of the frame changed: one pass on the canvas texture, nothing loaded
  // or copied.
  let r = await step('bg', '#405060');
  assert(r.loads === 0 && r.clears === 1 && r.copies === 0, `full-frame change: ${JSON.stringify(r)}`);
  await same('after a full-frame change');
  // Small changes: after a few frames the canvas pass draws the last frame
  // (a copy of the canvas) and only the damage over it, and the damage is
  // copied back for the next frame. Still one pass: no retained render
  // target to load and store (two extra full-frame passes on a 4K canvas).
  // The copy is recorded with the pass: one command encoder and one submit.
  for (let i = 0; i < 5; i++) r = await step('box', i % 2 ? '#00ff00' : '#0000ff');
  assert(r.passes === 1 && r.clears === 1 && r.copies === 1 && r.submits === 1 && r.encoders === 1,
    `small change: not one pass plus a damage copy-back: ${JSON.stringify(r)}`);
  await same('after small changes');
  r = await step('bg', '#102030');
  assert(r.loads === 0 && r.clears === 1 && r.copies === 0, `full-frame change after small ones: ${JSON.stringify(r)}`);
  await same('after the next full-frame change');
  r = await step('box', '#ffffff');
  await same('after a small change following a full-frame one');
});

test('webgpu: no multisampled targets, no per-frame blur passes for inset shadows', async ({ page }) => {
  // Browser WebGPU cannot load / resolve MSAA cheaply: Graphite must not use
  // MSAA for paths (the dashed rounded border), clips or the damage clip.
  await page.addInitScript({ path: path.join(WEB, 'test/gpu-count.js') });
  await page.reload();
  await page.waitForFunction(() => window.testReady);
  await mountWebGPU(page, { width: 480, height: 320, jsui: { bundleUrl: 'fixtures/effects.js' } });
  const r = await page.evaluate(async () => {
    const j = window.hosts.ui;
    await j._wgpu.device.queue.onSubmittedWorkDone();
    const keys = (re) => [...window.__gpuCounts.keys()].filter((k) => re.test(k));
    const msaa = keys(/^(pass|new texture) .*\d+x\d+x\d/);
    // Repaint the whole frame twice more (a resize of 1 px and back): the inset
    // shadow's blurred mask is cached, so no new single-channel offscreen passes.
    const passes = () => keys(/^pass color .* r8unorm/).reduce((n, k) => n + window.__gpuCounts.get(k), 0);
    const c = j.canvas, before = passes(), renders = j.stats.renders;
    for (const w of ['481px', '480px']) {
      c.style.width = w;
      for (let i = 0; i < 3; i++) await new Promise(requestAnimationFrame);
    }
    await j._wgpu.device.queue.onSubmittedWorkDone();
    return { msaa, r8: passes() - before, renders: j.stats.renders - renders };
  });
  assert(r.msaa.length === 0, `multisampled targets: ${r.msaa.join(' | ')}`);
  assert(r.renders >= 2, `${r.renders} repaints after the resizes`);
  assert(r.r8 === 0, `${r.r8} single-channel (blur mask) passes in full repaints after the first frame`);
});

test('webgpu: steady animation frames (interactions showcase): retained, few passes and bind groups, exact', async ({ page }) => {
  // examples/showcase/interactions.jsx keeps animating a glow, a pulsing ring
  // and a live dot. Steady frames repaint only that damage over the last
  // frame in the canvas pass (scissored rects; curved outlines need not grow
  // to whole nodes on Graphite) and copy it back, fold the opacity of
  // single-box layers into their paint (no offscreen passes), and reuse
  // uniform bind groups. Before: 2-3 canvas-size passes, ~172 draws and ~175
  // new bind groups per frame; then 2 passes (a retained render target).
  let build;
  try {
    ({ build } = await import('@orktes/jsui-cli'));
  } catch (e) {
    skip(`@orktes/jsui-cli unavailable: ${e.message}`, true);
  }
  const outfile = path.join(WEB, 'test/output/interactions.js');
  fs.mkdirSync(path.dirname(outfile), { recursive: true });
  await build({ entry: path.join(WEB, '../examples/showcase/interactions.jsx'), outfile, minify: true });
  await page.addInitScript({ path: path.join(WEB, 'test/gpu-count.js') });
  await page.reload();
  await page.waitForFunction(() => window.testReady);
  // Created together and driven by the same frame times from the first frame
  // on.
  const renderers = await page.evaluate(async (dist) => {
    // The bundle inline (bundleUrl only for its assets): both evaluate it on
    // creation, where a fetched one can start one runtime's animations later.
    const bundleSource = await (await fetch('output/interactions.js')).text();
    const make = () => {
      const c = document.createElement('canvas');
      c.style.width = '1280px';
      c.style.height = '720px';
      document.body.appendChild(c);
      return window.createJsui({ canvas: c, bundleSource, bundleUrl: 'output/interactions.js', renderer: 'webgpu', autoStart: false,
        moduleUrl: new URL(`../${dist}/jsui.js`, location.href) });
    };
    const [a, b] = await Promise.all([make(), make()]);
    window.hosts = { a, b };
    return [a.renderer, b.renderer];
  }, DIST);
  if (renderers.some((n) => n !== 'webgpu')) skip(`no WebGPU renderer with web/${DIST} (${renderers})`, DIST !== 'dist-gpu');
  const r = await page.evaluate(async () => {
    const { a, b } = window.hosts;
    if (b._wgpu) b._wgpu.flags = 4;  // JSUI_WEBGPU_NO_RETAIN: 'b' repaints every frame fully
    let t = performance.now();
    const sum = (re) => [...window.__gpuCounts].filter(([k]) => re.test(k)).reduce((n, [, v]) => n + v, 0);
    const rows = [];
    for (let i = 0; i < 240; i++) {
      t += 16.67;
      window.__gpuCounts.clear();
      // Back to back: some of the app's state follows the wall clock. The
      // WebGPU calls (counted) are all made inside frame().
      a.frame(t);
      if (i >= 180) {  // after the entrance animations
        rows.push({ passes: sum(/^pass color /), retained: sum(/\.copyTextureToTexture$/) > 0 ? 1 : 0,
          bindGroups: sum(/\.createBindGroup$/), draws: sum(/\.draw(Indexed)?(Indirect)?$/) });
      }
      b.frame(t);
      await a._wgpu.device.queue.onSubmittedWorkDone();
    }
    const max = (k) => Math.max(...rows.map((row) => row[k]));
    const mean = (k) => rows.reduce((n, row) => n + row[k], 0) / rows.length;
    return { maxPasses: max('passes'), retained: mean('retained'), maxBindGroups: max('bindGroups'),
      bindGroups: mean('bindGroups'), draws: mean('draws') };
  });
  console.log(`      steady frame: passes <= ${r.maxPasses}, bind groups ${r.bindGroups.toFixed(1)} (max ${r.maxBindGroups}), ` +
    `draws ${r.draws.toFixed(1)}, retained ${(r.retained * 100).toFixed(0)}%`);
  assert(r.retained >= 0.9, `steady frames do not go through the retained frame: ${JSON.stringify(r)}`);
  assert(r.maxPasses <= 1, `more than the canvas pass in steady frames: ${JSON.stringify(r)}`);
  assert(r.maxBindGroups <= 24, `uniform bind groups are not reused: ${JSON.stringify(r)}`);
  assert(r.draws <= 100, `steady frames repaint more than their damage: ${JSON.stringify(r)}`);
  const d = await frameDiff(page, ['a', 'b']);
  assert(!d.missing && !d.size, `readback ${JSON.stringify(d)}`);
  assert(d.max <= 1, `retained frame differs from a full repaint: ${JSON.stringify(d)}`);
}, { viewport: { width: 1300, height: 1500 } });

test('webgpu: raster cache: subtrees repainted unchanged are drawn from textures', async ({ page }) => {
  // In the showcase's steady frames a glow drifts behind the top bar, the
  // menu, the carousel and the armory panel, which nothing else changes:
  // each is drawn from a texture of it ('a') instead of node by node ('b',
  // JSUI_WEBGPU_NO_RASTER_CACHE). The texture is the same draws composited
  // as a group: equal up to rounding where the subtree is translucent (a
  // level), and a few levels on some anti-aliased edges (the scaled carousel
  // card's border is a little closer to the CPU raster from the texture). A
  // stale texture would differ by far more (text and box edges).
  let build;
  try {
    ({ build } = await import('@orktes/jsui-cli'));
  } catch (e) {
    skip(`@orktes/jsui-cli unavailable: ${e.message}`, true);
  }
  const outfile = path.join(WEB, 'test/output/interactions.js');
  fs.mkdirSync(path.dirname(outfile), { recursive: true });
  await build({ entry: path.join(WEB, '../examples/showcase/interactions.jsx'), outfile, minify: true });
  await page.addInitScript({ path: path.join(WEB, 'test/gpu-count.js') });
  await page.reload();
  await page.waitForFunction(() => window.testReady);
  const renderers = await page.evaluate(async (dist) => {
    const bundleSource = await (await fetch('output/interactions.js')).text();
    const make = () => {
      const c = document.createElement('canvas');
      c.style.width = '1280px';
      c.style.height = '720px';
      document.body.appendChild(c);
      return window.createJsui({ canvas: c, bundleSource, bundleUrl: 'output/interactions.js', renderer: 'webgpu', autoStart: false,
        moduleUrl: new URL(`../${dist}/jsui.js`, location.href) });
    };
    const [a, b] = await Promise.all([make(), make()]);
    window.hosts = { a, b };
    return [a.renderer, b.renderer];
  }, DIST);
  if (renderers.some((n) => n !== 'webgpu')) skip(`no WebGPU renderer with web/${DIST} (${renderers})`, DIST !== 'dist-gpu');
  const r = await page.evaluate(async () => {
    const { a, b } = window.hosts;
    if (b._wgpu) b._wgpu.flags = 16;  // JSUI_WEBGPU_NO_RASTER_CACHE
    let t = performance.now();
    const sum = (re) => [...window.__gpuCounts].filter(([k]) => re.test(k)).reduce((n, [, v]) => n + v, 0);
    const draws = { a: 0, b: 0 }, offscreen = { a: 0, b: 0 };
    for (let i = 0; i < 240; i++) {
      t += 16.67;
      for (const k of ['a', 'b']) {
        window.__gpuCounts.clear();
        window.hosts[k].frame(t);
        if (i >= 180) {  // after the entrance animations
          draws[k] += sum(/\.draw(Indexed)?(Indirect)?$/) / 60;
          offscreen[k] += sum(/^pass color /) - sum(/^pass color 1280x720 /);
        }
      }
      await a._wgpu.device.queue.onSubmittedWorkDone();
    }
    return { draws, offscreen };
  });
  const d = await frameDiff(page, ['a', 'b']);
  console.log(`      steady draws per frame: raster cache ${r.draws.a.toFixed(1)}, without ${r.draws.b.toFixed(1)}; ` +
    `diff max ${d.max}, mean ${d.mean.toFixed(4)}`);
  assert(r.draws.a * 1.5 < r.draws.b, `raster cache does not cut the draws: ${JSON.stringify(r)}`);
  assert(r.offscreen.a === 0, `textures rendered again in steady frames: ${JSON.stringify(r)}`);
  assert(!d.missing && !d.size, `readback ${JSON.stringify(d)}`);
  assert(d.over === 0 && d.max <= 12 && d.mean < 0.1, `raster-cached subtrees differ from direct painting: ${JSON.stringify(d)}`);
}, { viewport: { width: 1300, height: 1500 } });

test('webgpu: raster cache: a list scrolled at 2x draws its rows from textures', async ({ page }) => {
  // The showcase's armory list, scrolled by the wheel every frame at 2x: its
  // rows are drawn from textures of them ('a') instead of node by node ('b',
  // JSUI_WEBGPU_NO_RASTER_CACHE), each rendered once as it enters. Its
  // content (48 rows of 60 px) is taller than a texture can be at 2x: a
  // texture of its part on the screen would be rendered again in every
  // frame it moves, with all its rows in it.
  let build;
  try {
    ({ build } = await import('@orktes/jsui-cli'));
  } catch (e) {
    skip(`@orktes/jsui-cli unavailable: ${e.message}`, true);
  }
  const outfile = path.join(WEB, 'test/output/interactions.js');
  fs.mkdirSync(path.dirname(outfile), { recursive: true });
  await build({ entry: path.join(WEB, '../examples/showcase/interactions.jsx'), outfile, minify: true });
  await page.addInitScript({ path: path.join(WEB, 'test/gpu-count.js') });
  await page.reload();
  await page.waitForFunction(() => window.testReady);
  const renderers = await page.evaluate(async (dist) => {
    const bundleSource = await (await fetch('output/interactions.js')).text();
    const make = () => {
      const c = document.createElement('canvas');
      c.style.width = '1280px';
      c.style.height = '720px';
      c.style.display = 'block';
      document.body.appendChild(c);
      return window.createJsui({ canvas: c, bundleSource, bundleUrl: 'output/interactions.js', renderer: 'webgpu', autoStart: false,
        autoFocus: false, moduleUrl: new URL(`../${dist}/jsui.js`, location.href) });
    };
    const [a, b] = await Promise.all([make(), make()]);
    window.hosts = { a, b };
    return [a.renderer, b.renderer];
  }, DIST);
  if (renderers.some((n) => n !== 'webgpu')) skip(`no WebGPU renderer with web/${DIST} (${renderers})`, DIST !== 'dist-gpu');
  const r = await page.evaluate(async () => {
    const { a, b } = window.hosts;
    if (b._wgpu) b._wgpu.flags = 16;  // JSUI_WEBGPU_NO_RASTER_CACHE
    let t = performance.now();
    const sum = (re) => [...window.__gpuCounts].filter(([k]) => re.test(k)).reduce((n, [, v]) => n + v, 0);
    // Texels of the offscreen passes (textures rendered), not the canvas'.
    const offscreenPx = (w) => [...window.__gpuCounts].reduce((n, [k, v]) => {
      const m = /^pass color (\d+)x(\d+) /.exec(k);
      return m && !(Number(m[1]) === w.width && Number(m[2]) === w.height) ? n + v * m[1] * m[2] : n;
    }, 0);
    const wheel = (host, dy) => {
      const c = host.canvas, rc = c.getBoundingClientRect();
      c.dispatchEvent(new WheelEvent('wheel', { clientX: rc.left + 1080, clientY: rc.top + 396, deltaY: dy, bubbles: true,
        cancelable: true }));
    };
    const draws = { a: 0, b: 0 }, offscreen = { a: 0, b: 0 };
    const N = 30;
    for (let i = 0; i < 120 + N; i++) {
      t += i < 120 ? 33.3 : 16.67;  // the entrance animations, then 30 frames of scrolling
      for (const k of ['a', 'b']) {
        const host = window.hosts[k];
        if (i >= 120 - 10) wheel(host, 12);
        window.__gpuCounts.clear();
        host.frame(t);
        if (i >= 120) {
          draws[k] += sum(/\.draw(Indexed)?(Indirect)?$/) / N;
          offscreen[k] += offscreenPx(host.canvas) / N;
        }
      }
      await a._wgpu.device.queue.onSubmittedWorkDone();
    }
    return { draws, offscreen, canvas: a.canvas.width * a.canvas.height };
  });
  const d = await frameDiff(page, ['a', 'b']);
  console.log(`      scroll draws per frame at 2x: raster cache ${r.draws.a.toFixed(1)}, without ${r.draws.b.toFixed(1)}; ` +
    `textures rendered ${(r.offscreen.a / 1000).toFixed(0)}k px per frame; diff max ${d.max}, mean ${d.mean.toFixed(4)}`);
  assert(r.draws.a * 1.5 < r.draws.b, `raster cache does not cut the draws: ${JSON.stringify(r)}`);
  assert(r.offscreen.a < r.canvas * 0.05, `textures rendered again while the list scrolls: ${JSON.stringify(r)}`);
  assert(!d.missing && !d.size, `readback ${JSON.stringify(d)}`);
  assert(d.over === 0 && d.max <= 12 && d.mean < 0.1, `raster-cached rows differ from direct painting: ${JSON.stringify(d)}`);
}, { viewport: { width: 1300, height: 1500 }, deviceScaleFactor: 2 });

test('webgpu: entrance of the interactions showcase: composited layers equal plain layers', async ({ page }) => {
  // The entrance fades and slides panels and menu buttons (layers over
  // children) while some of their content animates too: every frame of 'a'
  // (layer cache) must match 'b' (JSUI_WEBGPU_NO_LAYER_CACHE), so a cached
  // layer is never drawn with stale content.
  let build;
  try {
    ({ build } = await import('@orktes/jsui-cli'));
  } catch (e) {
    skip(`@orktes/jsui-cli unavailable: ${e.message}`, true);
  }
  const outfile = path.join(WEB, 'test/output/interactions.js');
  fs.mkdirSync(path.dirname(outfile), { recursive: true });
  await build({ entry: path.join(WEB, '../examples/showcase/interactions.jsx'), outfile, minify: true });
  const renderers = await page.evaluate(async (dist) => {
    const bundleSource = await (await fetch('output/interactions.js')).text();
    const make = () => {
      const c = document.createElement('canvas');
      c.style.width = '1280px';
      c.style.height = '720px';
      document.body.appendChild(c);
      return window.createJsui({ canvas: c, bundleSource, bundleUrl: 'output/interactions.js', renderer: 'webgpu', autoStart: false,
        moduleUrl: new URL(`../${dist}/jsui.js`, location.href) });
    };
    window.t0 = performance.now();
    const [a, b] = await Promise.all([make(), make()]);
    window.hosts = { a, b };
    if (b._wgpu) b._wgpu.flags = 8;  // JSUI_WEBGPU_NO_LAYER_CACHE
    return [a.renderer, b.renderer];
  }, DIST);
  if (renderers.some((n) => n !== 'webgpu')) skip(`no WebGPU renderer with web/${DIST} (${renderers})`, DIST !== 'dist-gpu');
  let worst = { max: 0, over: 0, mean: 0 };  // the worst frame (all of them may be exact)
  for (let k = 0; k < 6; k++) {
    await page.evaluate(async (k) => {
      const { a, b } = window.hosts;
      for (let i = 0; i < 12; i++) {
        const t = window.t0 + (k * 12 + i + 1) * 16.67;
        a.frame(t);
        b.frame(t);
        await a._wgpu.device.queue.onSubmittedWorkDone();
      }
    }, k);
    const d = await frameDiff(page, ['a', 'b']);
    assert(!d.missing && !d.size, `readback ${JSON.stringify(d)}`);
    if (d.max > worst.max || d.over > (worst.over ?? 0)) worst = { ...d, frame: (k + 1) * 12 };
  }
  console.log(`      entrance: composited vs plain layers max diff ${worst.max}`);
  // Some of the app's state follows the wall clock (the JS-driven progress
  // bar), so the two runtimes can differ by a few levels at an edge; a stale
  // or misplaced layer differs by far more (text and box edges).
  assert(worst.over === 0 && worst.max <= 12 && worst.mean < 0.01,
    `a composited layer differs from a plain one during the entrance: ${JSON.stringify(worst)}`);
}, { viewport: { width: 1300, height: 1500 } });

test('webgpu: fading and moving layers are composited from cached textures, exact', async ({ page }) => {
  // A panel with children needs a layer for its opacity: an offscreen render
  // pass every frame of a fade (one per damage rect). With the layer cache
  // its texture is drawn again while only its opacity / position change:
  // no offscreen passes, the same pixels as a layer (whole-pixel moves; a
  // sub-pixel move is composited at the nearest pixel while it moves and
  // repainted exactly when it stops).
  await page.addInitScript({ path: path.join(WEB, 'test/gpu-count.js') });
  await page.reload();
  await page.waitForFunction(() => window.testReady);
  const renderers = await page.evaluate(async (dist) => {
    const make = () => {
      const c = document.createElement('canvas');
      c.style.width = '400px';
      c.style.height = '300px';
      document.body.appendChild(c);
      return window.createJsui({ canvas: c, bundleUrl: 'fixtures/layers.js', renderer: 'webgpu', autoStart: false,
        moduleUrl: new URL(`../${dist}/jsui.js`, location.href) });
    };
    const [a, b] = await Promise.all([make(), make()]);
    window.hosts = { a, b };
    return [a.renderer, b.renderer];
  }, DIST);
  if (renderers.some((n) => n !== 'webgpu')) skip(`no WebGPU renderer with web/${DIST} (${renderers})`, DIST !== 'dist-gpu');
  await page.evaluate(() => {
    if (window.hosts.b._wgpu) window.hosts.b._wgpu.flags = 8;  // JSUI_WEBGPU_NO_LAYER_CACHE: a plain layer every frame
    window.t = performance.now();
  });
  // Steps both runtimes through `moves`; returns the offscreen passes of 'a'
  // per frame after the first `warm` frames.
  const run = (moves, warm = 0) => page.evaluate(async ({ moves, warm }) => {
    const { a, b } = window.hosts;
    const offscreen = [];
    for (let i = 0; i < moves.length; i++) {
      for (const h of [a, b]) if (moves[i]) h.postMessage('move', moves[i]);
      window.t += 16.67;
      window.__gpuCounts.clear();
      a.frame(window.t);
      const n = [...window.__gpuCounts].filter(([k]) => k.startsWith('pass color ') && !k.includes(' 400x300 '))
        .reduce((s, [, v]) => s + v, 0);
      if (i >= warm) offscreen.push(n);
      b.frame(window.t);
      await a._wgpu.device.queue.onSubmittedWorkDone();
    }
    return offscreen;
  }, { moves, warm });
  const steps = (n, f) => Array.from({ length: n }, (_, i) => f(i));
  await run(steps(3, () => null));
  const fade = await run(steps(12, (i) => ({ opacity: 0.3 + i * 0.05, x: 0, y: 0 })), 2);
  assert(fade.every((n) => n === 0), `offscreen passes while fading: ${fade}`);
  let d = await frameDiff(page, ['a', 'b']);
  assert(!d.missing && !d.size && d.max <= 1, `faded: composited layer differs from a layer: ${JSON.stringify(d)}`);
  const slide = await run(steps(8, (i) => ({ opacity: 0.8 - i * 0.04, x: 3 * (i + 1), y: -(i + 1) })));
  assert(slide.every((n) => n === 0), `offscreen passes while moving by whole pixels: ${slide}`);
  d = await frameDiff(page, ['a', 'b']);
  assert(d.max <= 1, `moved: composited layer differs from a layer: ${JSON.stringify(d)}`);
  const glide = await run(steps(8, (i) => ({ opacity: 0.6, x: 24 + 0.37 * (i + 1), y: -8 + 0.21 * (i + 1) })));
  assert(glide.every((n) => n === 0), `offscreen passes while moving by sub-pixels: ${glide}`);
  await run(steps(3, () => null));  // at rest
  d = await frameDiff(page, ['a', 'b']);
  assert(d.max <= 1, `at rest after a sub-pixel move: not repainted exactly: ${JSON.stringify(d)}`);
  // Content under the layer changes (a child's own opacity, transform or
  // colour) while the panel fades: the cached texture must follow.
  const childMoves = [{ opacity: 0.5 }, { transform: [{ rotate: '20deg' }] }, { backgroundColor: '#00ff88' }];
  for (const [k, cm] of childMoves.entries()) {
    await page.evaluate((cm) => { for (const h of Object.values(window.hosts)) h.postMessage('child', cm); }, cm);
    await run(steps(3, (i) => ({ opacity: 0.5 + 0.1 * i, x: 24, y: -8 + k })));
    d = await frameDiff(page, ['a', 'b']);
    assert(d.max <= 1, `child change ${JSON.stringify(cm)}: stale composited layer: ${JSON.stringify(d)}`);
  }
}, { viewport: { width: 900, height: 400 } });

test('webgpu: static backdrop: damage repainted over a texture of it, exact', async ({ page }) => {
  // The layers fixture's background and gradient strip are what it paints
  // first and they do not change: each damage rect of the fading panel is
  // repainted over a texture of them (one draw instead of a clear and their
  // draws), the same pixels as painting them ('b', JSUI_WEBGPU_NO_BACKDROP),
  // and a change to the strip renders the texture again.
  await page.addInitScript({ path: path.join(WEB, 'test/gpu-count.js') });
  await page.reload();
  await page.waitForFunction(() => window.testReady);
  const renderers = await page.evaluate(async (dist) => {
    const make = () => {
      const c = document.createElement('canvas');
      c.style.width = '400px';
      c.style.height = '300px';
      document.body.appendChild(c);
      return window.createJsui({ canvas: c, bundleUrl: 'fixtures/layers.js', renderer: 'webgpu', autoStart: false,
        moduleUrl: new URL(`../${dist}/jsui.js`, location.href) });
    };
    const [a, b] = await Promise.all([make(), make()]);
    window.hosts = { a, b };
    return [a.renderer, b.renderer];
  }, DIST);
  if (renderers.some((n) => n !== 'webgpu')) skip(`no WebGPU renderer with web/${DIST} (${renderers})`, DIST !== 'dist-gpu');
  // Fades the panel (over the strip) in both for `n` frames; returns the
  // draws of each.
  const fade = (n, msg) => page.evaluate(async ({ n, msg }) => {
    const { a, b } = window.hosts;
    if (b._wgpu) b._wgpu.flags = 32;  // JSUI_WEBGPU_NO_BACKDROP
    if (msg) for (const h of [a, b]) h.postMessage('backdrop', msg);
    window.t ??= performance.now();
    const draws = { a: 0, b: 0 };
    for (let i = 0; i < n; i++) {
      window.t += 16.67;
      for (const k of ['a', 'b']) {
        window.hosts[k].postMessage('move', { opacity: 0.4 + 0.03 * (i % 10), x: 0, y: 40 });
        window.__gpuCounts.clear();
        window.hosts[k].frame(window.t);
        draws[k] += [...window.__gpuCounts].filter(([key]) => /\.draw(Indexed)?$/.test(key)).reduce((s, [, v]) => s + v, 0);
      }
      await a._wgpu.device.queue.onSubmittedWorkDone();
    }
    return draws;
  }, { n, msg });
  await fade(4);
  let draws = await fade(8);
  let d = await frameDiff(page, ['a', 'b']);
  assert(!d.missing && !d.size && d.max === 0, `over the backdrop texture: differs from painting it: ${JSON.stringify(d)}`);
  assert(draws.a < draws.b, `damage not repainted over the backdrop texture: ${JSON.stringify(draws)}`);
  // The strip changes: painted directly until it is still again, then over
  // a new texture of it.
  await fade(1, { backgroundImage: 'linear-gradient(90deg, #00c8ff, #ffe600)', left: 24 });
  d = await frameDiff(page, ['a', 'b']);
  assert(d.max === 0, `backdrop changing: ${JSON.stringify(d)}`);
  draws = await fade(8);
  d = await frameDiff(page, ['a', 'b']);
  assert(d.max === 0, `stale backdrop texture: ${JSON.stringify(d)}`);
  assert(draws.a < draws.b, `no new backdrop texture: ${JSON.stringify(draws)}`);
}, { viewport: { width: 900, height: 400 } });

test('webgpu: devicePixelRatio 2 and resize', async ({ page }) => {
  const info = await mountWebGPU(page, { jsui: { bundleUrl: 'fixtures/bands.js' } });
  assert(info.pw === 640 && info.ph === 400, `backing store ${info.pw}x${info.ph}`);
  let px = await screenPixels(page, '#ui', [[20, 99], [20, 100], [20, 199], [20, 200], [600, 50]]);
  assert(near(px[0], RED) && near(px[1], GREEN), `edge at y=100: ${fmt(px[0])} ${fmt(px[1])}`);
  assert(near(px[2], GREEN) && (near(px[3], BLUE) || near(px[3], [255, 255, 0, 255])), `edge at y=200: ${fmt(px[2])} ${fmt(px[3])}`);
  await page.evaluate(() => { document.getElementById('ui').style.height = '300px'; document.getElementById('ui').style.width = '400px'; });
  await frames(page, 4);
  px = await screenPixels(page, '#ui', [[20, 50], [20, 150], [700, 50], [20, 500]]);
  assert(near(px[0], RED) && near(px[1], GREEN), `bands after resize: ${px.map(fmt)}`);
  assert(near(px[2], [255, 255, 255, 255]) && near(px[3], [255, 255, 255, 255]), `outside bands: ${px.map(fmt)}`);
}, { deviceScaleFactor: 2 });

test('webgpu: device loss recreates the renderer (or falls back) and repaints', async ({ page }) => {
  await mountWebGPU(page, { jsui: { bundleUrl: 'fixtures/bands.js' } });
  const r = await page.evaluate(async () => {
    const j = window.hosts.ui;
    const events = [];
    j.on('renderer', (name, reason) => events.push([name, reason]));
    j.on('log', () => {});  // the loss warning is expected
    j._wgpu.device.destroy();
    for (let i = 0; i < 80 && !events.length; i++) await new Promise((r) => setTimeout(r, 25));
    for (let i = 0; i < 4; i++) await new Promise(requestAnimationFrame);
    return { events, renderer: j.renderer };
  });
  assert(r.events.length, 'no renderer event after device loss');
  const px = await screenPixels(page, '#ui', BANDS);
  [RED, GREEN, null, HALF_RED].forEach((want, i) => want && assert(near(px[i], want), `band ${i} after loss (${r.renderer}): ${fmt(px[i])}`));
  console.log(`      after device loss: ${r.renderer} (${r.events.map((e) => e[1]).join(', ')})`);
  return { allowErrors: true };  // Skia logs the buffer maps the loss aborted
});

test('webgpu: several runtimes share the device; destroy one', async ({ page }) => {
  await mountWebGPU(page, { id: 'a', jsui: { bundleUrl: 'fixtures/bands.js' } });
  await mountWebGPU(page, { id: 'b', width: 200, height: 150, jsui: { bundleUrl: 'fixtures/solid.js' } });
  const pa = await screenPixels(page, '#a', BANDS);
  const pb = await screenPixels(page, '#b', [[10, 50], [10, 120]]);
  [RED, GREEN, null, HALF_RED].forEach((want, i) => want && assert(near(pa[i], want), `a band ${i}: ${fmt(pa[i])}`));
  assert(near(pb[0], [0, 128, 128, 255]) && near(pb[1], [255, 255, 255, 255]), `b: ${fmt(pb[0])} ${fmt(pb[1])}`);
  await page.evaluate(() => window.hosts.b.destroy());
  await page.waitForTimeout(300);
  await frames(page, 3);
  const [p2] = await screenPixels(page, '#a', [BANDS[2]]);
  assert(near(p2, [255, 255, 0, 255]), `a keeps running after b destroyed: ${fmt(p2)}`);
});

test("webgpu: 'auto' skips a software WebGPU adapter", async ({ page }) => {
  const r = await page.evaluate(async () => {
    const { requestWebGPU } = await import('../jsui-web.js');
    const w = await requestWebGPU({ allowSoftware: true });
    return w && { software: w.software, description: w.description };
  });
  if (!r) skip('no WebGPU adapter in this browser', true);
  const info = await mount(page, { jsui: { bundleUrl: 'fixtures/bands.js', renderer: 'auto' } });
  if (r.software) assert(info.renderer !== 'webgpu', `auto used the software adapter ${r.description}`);
  console.log(`      adapter: ${r.description} (software ${r.software}); auto -> ${info.renderer}`);
});

test('perf: 1080p HUD (~540 nodes), steady state (webgpu)', async ({ page }) => {
  await mountWebGPU(page, { width: 1920, height: 1080, jsui: { bundleUrl: 'fixtures/hud.js' } });
  const r = await page.evaluate(async () => {
    const j = window.hosts.ui;
    j.stop();
    const M = j.module, rt = j.rt, dev = j._wgpu.device;
    const N = 60;
    let tick = 0, paint = 0, t = performance.now();
    for (let i = 0; i < N; i++) {
      t += 16.67;
      const a = performance.now();
      M._jsui_tick(rt, t);
      const b = performance.now();
      j._wgpu.render();
      await dev.queue.onSubmittedWorkDone();
      const c = performance.now();
      tick += b - a; paint += c - b;
    }
    return { tick: tick / N, paint: paint / N, renderer: j.renderer };
  });
  console.log(`      perf (${r.renderer}, 1920x1080, headless/swiftshader): tick ${r.tick.toFixed(2)} ms, Skia Graphite draw+GPU done ${r.paint.toFixed(2)} ms`);
});

// passThrough: the canvas is a layer over a "game" element; what the UI
// doesn't capture reaches the game, what it consumes doesn't.
test('passThrough: unhandled input reaches the page under the canvas', async ({ page }) => {
  await page.evaluate(() => {
    const g = document.createElement('div');
    g.id = 'game';
    Object.assign(g.style, { position: 'absolute', left: '0px', top: '0px', width: '320px', height: '200px' });
    document.body.appendChild(g);
    window.gameEvents = [];
    for (const t of ['pointerdown', 'pointerup', 'click', 'wheel', 'contextmenu']) {
      g.addEventListener(t, (e) => window.gameEvents.push(t), { passive: true });
    }
  });
  await mount(page, { jsui: { bundleUrl: 'fixtures/passthrough.js', passThrough: true } });
  await page.evaluate(() => Object.assign(document.getElementById('ui').style,
    { position: 'absolute', left: '0px', top: '0px', zIndex: '1' }));
  const take = () => page.evaluate(() => {
    const r = { game: window.gameEvents.join(), press: window.hosts.ui.messages.filter(([n]) => n === 'press').length };
    window.gameEvents = [];
    window.hosts.ui.messages = [];
    return r;
  });
  const hits = await page.evaluate(() => [window.hosts.ui.hitTest(50, 25), window.hosts.ui.hitTest(200, 150)]);
  assert(hits.join() === '1,0', `hitTest ${hits}`);

  await page.mouse.click(50, 25);  // the button
  await frames(page);
  await idle(page);
  let r = await take();
  assert(r.press === 1 && r.game === '', `button click ${JSON.stringify(r)}`);
  const hot = await page.evaluate(() => document.getElementById('ui').style.pointerEvents);
  assert(hot === 'auto', `pointer-events over the button: ${hot}`);

  await page.mouse.click(200, 150);  // painted panel, no handlers
  await page.mouse.wheel(0, 100);
  await page.mouse.click(200, 150, { button: 'right' });
  await frames(page);
  await idle(page);
  r = await take();
  const sorted = r.game.split(',').sort().join();
  assert(r.press === 0 && sorted === 'click,contextmenu,pointerdown,pointerdown,pointerup,pointerup,wheel',
    `panel click ${JSON.stringify(r)}`);
  const cold = await page.evaluate(() => document.getElementById('ui').style.pointerEvents);
  assert(cold === 'none', `pointer-events over the panel: ${cold}`);

  // A drag the game started stays the game's over the button.
  await page.mouse.move(200, 150);
  await page.mouse.down();
  await page.mouse.move(50, 25, { steps: 4 });
  await page.mouse.up();
  await frames(page);
  await idle(page);
  r = await take();
  assert(r.press === 0 && r.game.startsWith('pointerdown') && r.game.includes('pointerup'), `game drag ${JSON.stringify(r)}`);

  // Unconsumed keys reach the page; the keyboard isn't taken by unhandled clicks.
  const focused = await page.evaluate(() => document.activeElement?.tagName);
  assert(focused !== 'TEXTAREA', `focus after a pass-through click: ${focused}`);
}, {}, { worker: true });

// ---------------------------------------------------------------- surfaces
// One runtime, several canvases (createSurface): own props, pixels, input,
// sizes; props updates, auto-size and destroy.
test('surfaces: one runtime on several canvases', async ({ page }) => {
  await mount(page, { jsui: { bundleUrl: 'fixtures/surfaces.js' } });
  const made = await page.evaluate(async () => {
    const j = window.hosts.ui;
    const holder = document.createElement('div');
    holder.id = 'plate-holder';
    Object.assign(holder.style, { width: '120px', height: '40px' });
    document.body.appendChild(holder);
    const plate = j.createSurface(holder, { component: 'Plate', initialProps: { color: '#ff0000' }, renderer: 'cpu' });
    plate.canvas.id = 'plate';
    const c = document.createElement('canvas');
    c.id = 'badge';
    document.body.appendChild(c);
    const badge = j.createSurface(c, { component: 'Badge', initialProps: { color: '#0000ff', size: [30, 12] },
      autoSize: true, renderer: '2d' });
    window.surfaces = { plate, badge };
    await Promise.race([j.whenIdle(), new Promise((r) => setTimeout(r, 3000))]);
    return { plate: plate.rootTag, badge: badge.rootTag, size: [c.style.width, c.style.height],
      runs: j.messages.filter(([n]) => n === 'run').map(([, d]) => d.appKey), count: j.surfaces.length };
  });
  assert(made.plate > 1 && made.badge > 1 && made.plate !== made.badge, `root tags ${made.plate} ${made.badge}`);
  assert(made.runs.join() === 'Plate,Badge', `runs ${made.runs}`);
  assert(made.count === 2, `surfaces ${made.count}`);
  assert(made.size.join() === '30px,12px', `auto size ${made.size}`);
  const [plate] = await screenPixels(page, '#plate', [[60, 20]]);
  assert(near(plate, RED), `plate ${fmt(plate)}`);
  const [badge] = await screenPixels(page, '#badge', [[15, 6]]);
  assert(near(badge, BLUE), `badge ${fmt(badge)}`);
  const [main] = await screenPixels(page, '#ui', [[10, 50]]);
  assert(near(main, [0, 128, 128, 255]), `main ${fmt(main)}`);

  // Input reaches the surface under the pointer.
  const box = await page.locator('#plate').boundingBox();
  await page.mouse.move(box.x + 60, box.y + 20);
  await page.mouse.down();
  await page.mouse.up();
  await frames(page);
  await idle(page);
  const press = await page.evaluate(() => window.hosts.ui.messages.filter(([n]) => n === 'press').map(([, d]) => d));
  assert(press.join() === String(made.plate), `press ${press}`);

  // New props repaint just that surface; destroy unmounts and drops the canvas createSurface made.
  await page.evaluate(() => window.surfaces.plate.setProps({ color: '#00ff00' }));
  await frames(page);
  await idle(page);
  const [green] = await screenPixels(page, '#plate', [[60, 20]]);
  assert(near(green, GREEN), `plate after setProps ${fmt(green)}`);
  const after = await page.evaluate(async () => {
    window.surfaces.plate.destroy();
    await new Promise(requestAnimationFrame);
    const j = window.hosts.ui;
    await j.whenIdle();
    return { stops: j.messages.filter(([n]) => n === 'stop').map(([, d]) => d), canvas: !!document.getElementById('plate'),
      count: j.surfaces.length };
  });
  assert(after.stops.join() === String(made.plate) && !after.canvas && after.count === 1, `destroy ${JSON.stringify(after)}`);
}, {}, { worker: true });

test('surfaces: createJsui without a canvas, component picks the main app', async ({ page }) => {
  const r = await page.evaluate(async ({ dist, worker }) => {
    const j = await window.createJsui({ renderer: 'cpu', moduleUrl: new URL(`../${dist}/jsui.js`, location.href),
      bundleUrl: 'fixtures/surfaces.js', component: 'Main', initialProps: { color: '#ff0000' }, worker });
    const msgs = [];
    j.on('message', (n, d) => msgs.push([n, d]));
    const c = document.createElement('canvas');
    c.id = 'w';
    Object.assign(c.style, { width: '50px', height: '50px' });
    document.body.appendChild(c);
    j.createSurface(c, { component: 'W', initialProps: { color: '#0000ff' }, renderer: 'cpu' });
    await Promise.race([j.whenIdle(), new Promise((res) => setTimeout(res, 3000))]);
    return { msgs, renderer: j.renderer ?? null, worker: !!j.worker };
  }, { dist: DIST, worker: WORKER });
  assert(r.worker === WORKER, `worker mode ${r.worker}`);
  const runs = r.msgs.filter(([n]) => n === 'run').map(([, d]) => `${d.tag}:${d.appKey}`);
  assert(runs.includes('1:Main') && runs.some((s) => s.endsWith(':W')), `runs ${runs}`);
  const [px] = await screenPixels(page, '#w', [[25, 25]]);
  assert(near(px, BLUE), `widget ${fmt(px)}`);
}, {}, { worker: true });

// Many GPU surfaces: more canvases than the browser's ~16 live WebGL contexts,
// all drawn through the runtime's shared GPU context (one offscreen WebGL
// context, or one Skia Graphite context on the WebGPU device). Every surface
// shows its own pixels, keeps showing them after new props, and no context
// is lost.
for (const mode of ['webgl', 'webgpu']) {
  test(`surfaces: 24 GPU surfaces share one ${mode} context`, async ({ page }) => {
    const N = 24, COLS = 6, W = 50, H = 40;
    if (mode === 'webgpu') await mountWebGPU(page, { jsui: { bundleUrl: 'fixtures/surfaces.js' } });
    else await mount(page, { jsui: { bundleUrl: 'fixtures/surfaces.js' } });  // main view: CPU
    const COLORS = ['#ff0000', '#00ff00', '#0000ff'];
    const r = await page.evaluate(async ({ N, COLS, W, H, COLORS, mode }) => {
      // Count the WebGL contexts made from here on and watch them for loss.
      const stats = { webgl: 0, lost: 0 };
      for (const C of [HTMLCanvasElement, globalThis.OffscreenCanvas].filter(Boolean)) {
        const get = C.prototype.getContext;
        C.prototype.getContext = function (type, ...rest) {
          const had = this.__ctx;
          const ctx = get.call(this, type, ...rest);
          if (ctx && !had && (type === 'webgl' || type === 'webgl2')) {
            stats.webgl++;
            this.addEventListener('webglcontextlost', () => { stats.lost++; });
          }
          if (ctx) this.__ctx = true;
          return ctx;
        };
      }
      window.manyStats = stats;
      const j = window.hosts.ui;
      const holder = document.createElement('div');
      holder.id = 'many';
      Object.assign(holder.style, { position: 'relative', width: `${COLS * W}px`, height: `${Math.ceil(N / COLS) * H}px` });
      document.body.appendChild(holder);
      const list = [];
      for (let i = 0; i < N; i++) {
        const c = document.createElement('canvas');
        Object.assign(c.style, { position: 'absolute', left: `${(i % COLS) * W}px`, top: `${Math.floor(i / COLS) * H}px` });
        holder.appendChild(c);
        // 'gpu' for the CPU main view (its mode would make the surfaces CPU);
        // the WebGPU runtime's surfaces take its renderer.
        list.push(j.createSurface(c, { component: 'Box', width: W, height: H,
          initialProps: { color: COLORS[i % 3] }, ...(mode === 'webgl' ? { renderer: 'gpu' } : {}) }));
      }
      window.many = list;
      await Promise.race([j.whenIdle(), new Promise((res) => setTimeout(res, 5000))]);
      return { renderers: [...new Set(list.map((s) => s.renderer))],
        skias: new Set(list.map((s) => s._wgpu?.skia ?? s._sgl)).size };
    }, { N, COLS, W, H, COLORS, mode });
    if (mode === 'webgl' && !r.renderers.every((n) => n.endsWith('-shared'))) {
      skip(`module in web/${DIST} has no GPU backend (renderers ${r.renderers})`, DIST === 'dist');
    }
    if (mode === 'webgpu') assert(r.renderers.join() === 'webgpu', `renderers ${r.renderers}`);
    assert(r.skias === 1, `${r.skias} GPU contexts for ${N} surfaces`);

    const RGB = { '#ff0000': RED, '#00ff00': GREEN, '#0000ff': BLUE };
    const points = Array.from({ length: N }, (_, i) => [(i % COLS) * W + W / 2, Math.floor(i / COLS) * H + H / 2]);
    const check = async (shift, what) => {
      const px = await screenPixels(page, '#many', points);
      px.forEach((p, i) => {
        const want = RGB[COLORS[(i + shift) % 3]];
        assert(near(p, want), `${what}: surface ${i} ${fmt(p)}, want ${fmt(want)}`);
      });
    };
    await check(0, 'first frame');
    // New props for every surface: each repaints, none lost its context.
    await page.evaluate((COLORS) => window.many.forEach((s, i) => s.setProps({ color: COLORS[(i + 1) % 3] })), COLORS);
    await frames(page, 4);
    await check(1, 'after setProps');
    const after = await page.evaluate(() => ({ ...window.manyStats,
      renderers: [...new Set(window.many.map((s) => s.renderer))] }));
    assert(after.lost === 0, `${after.lost} WebGL contexts lost`);
    assert(after.webgl <= 1, `${after.webgl} WebGL contexts for the surfaces`);
    assert(after.renderers.join() === r.renderers.join(), `renderers changed to ${after.renderers}`);
    // After destroying them all a new surface still gets a shared GPU renderer
    // (WebGPU: a new Skia context once the last reference went).
    const again = await page.evaluate(async () => {
      window.many.forEach((s) => s.destroy());
      const j = window.hosts.ui;
      const c = document.createElement('canvas');
      c.id = 'again';
      document.body.appendChild(c);
      const s = j.createSurface(c, { component: 'Box', width: 20, height: 20, initialProps: { color: '#0000ff' },
        ...(j.renderer === 'webgpu' ? {} : { renderer: 'gpu' }) });
      await Promise.race([j.whenIdle(), new Promise((res) => setTimeout(res, 3000))]);
      return { renderer: s.renderer, count: j.surfaces.length };
    });
    assert(again.count === 1 && again.renderer === r.renderers[0], `after destroy ${JSON.stringify(again)}`);
    const [blue] = await screenPixels(page, '#again', [[10, 10]]);
    assert(near(blue, BLUE), `surface after destroy ${fmt(blue)}`);
  });
}

// ------------------------------------------------- game data, textures, scroll
// Host API round trips that return Promises in worker mode (awaited here in
// both modes).
test('game data and calls: state, frame data, callUi, Host.call', async ({ page }) => {
  await mount(page, { width: 340, height: 200, jsui: { bundleUrl: 'fixtures/data.js' } });
  const r = await page.evaluate(async () => {
    const j = window.hosts.ui, out = {};
    const message = async (name) => {
      for (let i = 0; i < 200; i++) {
        const m = j.messages.find(([n]) => n === name);
        if (m) return m[1];
        await new Promise((res) => setTimeout(res, 10));
      }
      return 'timeout';
    };
    out.set1 = await j.setState('k', { a: 1 });
    out.set2 = await j.setState('k', { a: 1 });
    out.patch = await j.patchState('k', { b: 2 });
    out.get = await j.getState('k');
    out.missing = await j.getState('nope');
    out.buf = await j.setBuffer('b', Float32Array.of(1, 2, 3));
    const m = j.mapBuffer('b', Float32Array, 3);
    out.mapped = Array.from(m);
    m[1] = 5;
    out.commit = await j.commitBuffer('b');
    j.postMessage('buf');
    out.seen = await message('buf');
    j.handle('host.greet', (a) => `hi ${a.who}`);
    j.postMessage('greet', { who: 'ui' });
    out.sum = await j.callUi('ui.add', { a: 2, b: 3 });
    try { await j.callUi('ui.nope', {}); } catch (e) { out.err = [e.name, e.message]; }
    out.greeted = await message('greeted');
    out.removed = await j.removeBuffer('b');
    out.removedAgain = await j.removeBuffer('b');
    return out;
  });
  const want = { set1: true, set2: false, patch: true, get: { a: 1, b: 2 }, missing: undefined, buf: true, mapped: [1, 2, 3],
    commit: true, seen: [1, 5, 3], sum: 5, removed: true, removedAgain: false };
  for (const [k, v] of Object.entries(want)) assert(JSON.stringify(r[k]) === JSON.stringify(v), `${k}: ${JSON.stringify(r[k])}, want ${JSON.stringify(v)}`);
  assert(r.err?.[0] === 'UiCallError' && /ui\.nope/.test(r.err[1]), `callUi rejection ${JSON.stringify(r.err)}`);
  assert(r.greeted?.ok && JSON.parse(r.greeted.payload) === 'hi ui', `Host.call ${JSON.stringify(r.greeted)}`);
}, {}, { worker: true });

// Host textures from the page's sources (worker mode: converted here, sent
// to the worker; bindVideo captures VideoFrames here).
for (const renderer of ['cpu', 'webgpu']) test(`host textures: ImageData, canvas, raw pixels, a bound video (${renderer})`, async ({ page }) => {
  const info = await mount(page, { width: 340, height: 200, jsui: { bundleUrl: 'fixtures/data.js', renderer } });
  if (renderer === 'webgpu') skipNoBackend('webgpu', info.renderer);
  const at = async () => {
    await frames(page, 2);
    await idle(page);
    return (await screenPixels(page, '#ui', [[50, 50]]))[0];
  };
  const set = (what) => page.evaluate(async (what) => {
    const j = window.hosts.ui;
    if (what === 'imagedata') {
      const d = new ImageData(4, 4);
      for (let i = 0; i < d.data.length; i += 4) d.data.set([255, 0, 0, 255], i);
      return j.setTexture('tex', d);
    }
    if (what === 'canvas') {
      const c = document.createElement('canvas');
      c.width = c.height = 8;
      const g = c.getContext('2d');
      g.fillStyle = '#00ff00';
      g.fillRect(0, 0, 8, 8);
      return j.setTexture('tex', c);
    }
    if (what === 'raw') {
      const data = new Uint8Array(2 * 2 * 4);
      for (let i = 0; i < data.length; i += 4) data.set([0, 0, 255, 255], i);
      return j.setTexture('tex', { data, width: 2, height: 2 });
    }
    return null;
  }, what);
  let ok = await set('imagedata');
  assert(ok === true, `setTexture(ImageData): ${ok}`);
  let px = await at();
  assert(near(px, RED), `ImageData texture ${fmt(px)}`);
  const backend = await page.evaluate(() => window.hosts.ui.textureBackend);
  assert(backend === (renderer === 'webgpu' ? 'webgpu' : 'pixels'), `textureBackend ${backend}`);
  ok = await set('canvas');
  assert(ok === true, `setTexture(canvas): ${ok}`);
  px = await at();
  assert(near(px, GREEN), `canvas texture ${fmt(px)}`);
  ok = await set('raw');
  assert(ok === true, `setTexture(raw pixels): ${ok}`);
  px = await at();
  assert(near(px, BLUE), `raw pixel texture ${fmt(px)}`);
  // A <video> playing a canvas' stream, bound with bindVideo.
  const state = await page.evaluate(async () => {
    const c = document.createElement('canvas');
    c.width = c.height = 32;
    const g = c.getContext('2d');
    let n = 0;
    const paint = () => { g.fillStyle = n++ % 2 ? '#ffff00' : '#ffff01'; g.fillRect(0, 0, 32, 32); };
    paint();
    window.__paint = setInterval(paint, 20);
    const v = document.createElement('video');
    Object.assign(v, { muted: true, playsInline: true, srcObject: c.captureStream(30) });
    Object.assign(v.style, { position: 'fixed', left: '0px', top: '300px', width: '4px', height: '4px' });
    document.body.appendChild(v);
    try { await v.play(); } catch (e) { return `play: ${e}`; }
    for (let i = 0; i < 100 && v.readyState < 2; i++) await new Promise((r) => setTimeout(r, 20));
    window.__unbind = window.hosts.ui.bindVideo('tex', v);
    await new Promise((r) => setTimeout(r, 300));
    return v.readyState;
  });
  if (typeof state === 'string' || state < 2) skip(`no video playback in this browser (${state})`, true);
  px = await at();
  assert(near(px, [255, 255, 0, 255], 24), `video texture ${fmt(px)}`);
  ok = await page.evaluate(() => { clearInterval(window.__paint); return window.hosts.ui.removeTexture('tex'); });
  assert(ok === true, `removeTexture: ${ok}`);
  px = await at();
  assert(near(px, [255, 255, 255, 255]), `after removeTexture ${fmt(px)}`);
}, {}, { worker: true });

// The wheel scrolls a ScrollView and a click lands on the row under it.
// Worker mode: also how long after an input the page's hit map shows its
// effect (the pass-through / preventDefault answers come from it).
test('scroll: the wheel scrolls a ScrollView, clicks land on the scrolled rows', async ({ page }) => {
  await mount(page, { width: 340, height: 200, jsui: { bundleUrl: 'fixtures/data.js' } });
  const hits = await page.evaluate(() => [130, 50, 335].map((x) => window.hosts.ui.hitTest(x, 10)));
  assert(hits.join() === '3,0,0', `hitTest over a row, the texture, nothing: ${hits}`);
  await page.mouse.move(200, 100);
  await page.mouse.wheel(0, 120);
  await frames(page, 3);
  await idle(page);
  let [px] = await screenPixels(page, '#ui', [[130, 10]]);
  assert(near(px, BLUE), `row 3 after scrolling 120px: ${fmt(px)}`);
  await page.mouse.click(200, 10);
  await frames(page, 2);
  await idle(page);
  let rows = await page.evaluate(() => window.hosts.ui.messages.filter(([n]) => n === 'row').map(([, i]) => i));
  assert(rows.join() === '3', `clicked rows ${rows}`);
  // 30 more wheel steps of 10px, one per ~frame, each timed until the page's
  // hit map moved (worker mode).
  const r = await page.evaluate(async () => {
    const j = window.hosts.ui, c = j.canvas, box = c.getBoundingClientRect();
    const waits = [];
    // A task yield that lets the hit map's message in (setTimeout(0) is
    // clamped to 4 ms after a few nested calls); one channel, reused.
    const ch = new MessageChannel();
    let wake = null;
    ch.port1.onmessage = () => wake?.();
    const yieldTask = () => new Promise((res) => {
      wake = res;
      ch.port2.postMessage(0);
    });
    if (j.hitMapStats) {  // count from here
      for (const k of ['latency', 'transit']) j.hitMapStats[k] = { n: 0, mean: 0, max: 0, samples: [] };
    }
    for (let i = 0; i < 30; i++) {
      const before = j.hitMapStats?.updates ?? 0, t0 = performance.now();
      c.dispatchEvent(new WheelEvent('wheel', { clientX: box.left + 200, clientY: box.top + 100, deltaY: 10,
        bubbles: true, cancelable: true }));
      if (j.hitMapStats) {
        while (j.hitMapStats.updates === before && performance.now() - t0 < 1000) await yieldTask();
        waits.push(performance.now() - t0);
      }
      await new Promise(requestAnimationFrame);
    }
    ch.port1.close();
    await j.whenIdle();
    const s = j.hitMapStats;
    const pct = (a, p) => { const v = [...a].sort((x, y) => x - y); return v[Math.min(v.length - 1, Math.floor(p * v.length))]; };
    return s && { waits, p50: pct(waits, 0.5), p95: pct(waits, 0.95), max: Math.max(...waits),
      updates: s.updates, regions: s.regions,
      latency: { p50: pct(s.latency.samples, 0.5), p95: pct(s.latency.samples, 0.95), max: s.latency.max },
      transit: { mean: s.transit.mean, max: s.transit.max }, misses: s.misses, lookups: s.lookups };
  });
  await page.mouse.click(200, 10);
  await frames(page, 2);
  await idle(page);
  rows = await page.evaluate(() => window.hosts.ui.messages.filter(([n]) => n === 'row').map(([, i]) => i));
  assert(rows.join() === '3,10', `clicked rows after 420px ${rows}`);
  [px] = await screenPixels(page, '#ui', [[130, 10]]);
  assert(near(px, GREEN), `row 10 after scrolling 420px: ${fmt(px)}`);
  if (r) {
    const f = (x) => x.toFixed(1);
    console.log(`      hit map: input -> page has the new map: median ${f(r.p50)} ms, p95 ${f(r.p95)}, ` +
      `max ${f(r.max)} (step ${r.waits.indexOf(r.max)}); ` +
      `frame start -> page: median ${f(r.latency.p50)}, p95 ${f(r.latency.p95)}, max ${f(r.latency.max)}; ` +
      `postMessage: mean ${f(r.transit.mean)}, max ${f(r.transit.max)}; ${r.updates} maps of ~${r.regions} rects, ` +
      `${r.misses} mispredictions in ${r.lookups} lookups`);
    assert(r.misses === 0, `${r.misses} mispredictions`);
  }
}, {}, { worker: true });

// A lost WebGL context can be noticed before its webglcontextlost event (a
// later task): WebKit, switching the showcase to its video, lost the context
// and a frame and a texture upload ran on it first (in worker mode: a failed
// shader compile, then emscripten's glCreateProgram threw on the null program,
// then the upload's createTexture). Same here with WEBGL_lose_context: GL
// work in the task that lost it, then the restore. Nothing may throw or log,
// and the restored frame equals one that was never lost.
test('WebGL context lost: no GL work before the event, everything back after the restore', async ({ page }) => {
  const info = await mount(page, { width: 340, height: 200, jsui: { bundleUrl: 'fixtures/data.js', renderer: 'webgl' } });
  skipNoBackend('webgl', info.renderer);
  await mount(page, { id: 'ref', width: 340, height: 200, jsui: { bundleUrl: 'fixtures/data.js', renderer: 'webgl' } });
  const r = await page.evaluate(async (worker) => {
    const j = window.hosts.ui, ref = window.hosts.ref;
    const logs = [], renderers = [];
    j.on('log', (level, text) => { if (level >= 2) logs.push(text); });
    j.on('renderer', (name, reason) => renderers.push([name, reason]));
    let res;
    if (worker) {
      res = await j._debug('loseGL', { work: true });
    } else {
      const ext = j._gl?.getExtension('WEBGL_lose_context');
      if (!ext) return null;
      ext.loseContext();
      res = { error: null };
      try {
        j.postMessage('fx', null);
        const img = new ImageData(8, 8);
        img.data.fill(255);
        j.setTexture('tex', img);
        j.frame();
      } catch (e) {
        res.error = String(e?.stack ?? e);
      }
      setTimeout(() => ext.restoreContext(), 50);
    }
    if (!res) return null;
    // The reference gets the same, without a loss.
    ref.postMessage('fx', null);
    const img = new ImageData(8, 8);
    img.data.fill(255);
    await ref.setTexture('tex', img);
    for (let i = 0; i < 40; i++) await new Promise((r) => setTimeout(r, 20));
    await Promise.all([j.whenIdle(), ref.whenIdle()]);
    return { ...res, logs, renderers, renderer: j.renderer };
  }, WORKER);
  if (!r) skip('WEBGL_lose_context unavailable');
  assert(!r.error, `GL work on the lost context threw: ${r.error}`);
  assert(!r.logs.length, `logged: ${JSON.stringify(r.logs)}`);
  assert(!r.renderers.length && r.renderer === info.renderer, `renderer changed: ${JSON.stringify(r)}`);
  const d = await frameDiff(page, ['ui', 'ref']);
  assert(!d.missing && !d.size && d.max === 0, `restored frame vs never lost: ${JSON.stringify(d)}`);
  const [tex, fx] = await screenPixels(page, '#ui', [[50, 50], [85, 145]]);
  assert(near(tex, [255, 255, 255, 255]), `texture after the restore ${fmt(tex)}`);
  assert(!near(fx, [255, 255, 255, 255]) && fx[3] > 200, `'fx' after the restore ${fmt(fx)}`);
}, {}, { worker: true });

// -------------------------------------------------------------- worker mode
// The same frame as main mode, pixel for pixel (text included: measured and
// shaped in the worker).
for (const renderer of ['cpu', 'webgl', 'webgpu']) test(`worker: pixels equal main mode (effects fixture, ${renderer})`, async ({ page }) => {
  const a = await mount(page, { id: 'a', width: 480, height: 320, jsui: { bundleUrl: 'fixtures/effects.js', renderer, worker: false } });
  if (renderer !== 'cpu') skipNoBackend(renderer, a.renderer);
  const b = await mount(page, { id: 'b', width: 480, height: 320, jsui: { bundleUrl: 'fixtures/effects.js', renderer, worker: true } });
  assert(a.renderer === b.renderer, `renderers: main ${a.renderer}, worker ${b.renderer}`);
  const d = await frameDiff(page, ['a', 'b']);
  assert(!d.missing && !d.size, `readback ${JSON.stringify(d)}`);
  assert(d.max === 0, `worker frame differs from the main-thread one: ${JSON.stringify(d)}`);
  const pts = [[20, 20], [100, 60], [240, 160], [400, 280], [60, 250]];
  const [pa, pb] = [await screenPixels(page, '#a', pts), await screenPixels(page, '#b', pts)];
  pts.forEach((_, i) => assert(near(pa[i], pb[i], 1), `screen at ${pts[i]}: main ${fmt(pa[i])}, worker ${fmt(pb[i])}`));
});

test('worker: WebGL context loss: restored, then lost for good (CPU on a fresh canvas)', async ({ page }) => {
  const info = await mount(page, { jsui: { bundleUrl: 'fixtures/bands.js', renderer: 'webgl', contextRestoreTimeout: 100, worker: true } });
  skipNoBackend('webgl', info.renderer);
  const r = await page.evaluate(async () => {
    const j = window.hosts.ui, old = j.canvas;
    const events = [];
    j.on('renderer', (name, reason) => events.push([name, reason]));
    j.on('log', () => {});  // the fallback warning is expected
    if (!(await j._debug('loseGL'))) return null;  // restored after 50 ms
    for (let i = 0; i < 10; i++) await new Promise(requestAnimationFrame);
    await j.whenIdle();
    const restored = { events: events.length, renderer: j.renderer };
    await j._debug('loseGLForGood');
    for (let i = 0; i < 100 && !events.length; i++) await new Promise((res) => setTimeout(res, 20));
    await j.whenIdle();
    return { restored, events, renderer: j.renderer, replaced: j.canvas !== old && !old.isConnected && j.canvas.isConnected,
      id: j.canvas.id };
  });
  if (!r) skip('WEBGL_lose_context unavailable');
  assert(r.restored.events === 0 && r.restored.renderer.startsWith('gpu-'), `after a restore: ${JSON.stringify(r.restored)}`);
  assert(r.events.length === 1 && r.replaced && r.id === 'ui', `fallback: ${JSON.stringify(r)}`);
  assert(r.renderer.startsWith('webgl'), `fallback renderer ${r.renderer}`);
  const px = await screenPixels(page, '#ui', BANDS);
  [RED, GREEN, null, HALF_RED].forEach((want, i) => want && assert(near(px[i], want), `band ${i} after fallback: ${fmt(px[i])}`));
});

test('worker: WebGPU device loss recreates the renderer and repaints', async ({ page }) => {
  const info = await mount(page, { jsui: { bundleUrl: 'fixtures/bands.js', renderer: 'webgpu', worker: true } });
  skipNoBackend('webgpu', info.renderer);
  const r = await page.evaluate(async () => {
    const j = window.hosts.ui;
    const events = [];
    j.on('renderer', (name, reason) => events.push([name, reason]));
    j.on('log', () => {});
    await j._debug('destroyDevice');
    for (let i = 0; i < 80 && !events.length; i++) await new Promise((res) => setTimeout(res, 25));
    for (let i = 0; i < 4; i++) await new Promise(requestAnimationFrame);
    await j.whenIdle();
    return { events, renderer: j.renderer };
  });
  assert(r.events.length, 'no renderer event after device loss');
  const px = await screenPixels(page, '#ui', BANDS);
  [RED, GREEN, null, HALF_RED].forEach((want, i) => want && assert(near(px[i], want), `band ${i} after loss (${r.renderer}): ${fmt(px[i])}`));
  console.log(`      after device loss: ${r.renderer} (${r.events.map((e) => e[1]).join(', ')})`);
  return { allowErrors: true };  // Skia logs the buffer maps the loss aborted
});

test("worker: frames are timed by the worker's requestAnimationFrame, or the page's (workerLoop 'main')", async ({ page }) => {
  await mount(page, { id: 'a', jsui: { bundleUrl: 'fixtures/bands.js', worker: true } });
  await mount(page, { id: 'b', jsui: { bundleUrl: 'fixtures/bands.js', worker: true, workerLoop: 'main' } });
  await page.waitForTimeout(300);  // band 2's 200 ms timer
  await frames(page, 3);
  const info = await page.evaluate(async () => Promise.all(['a', 'b'].map((id) => window.hosts[id]._debug('info'))));
  assert(info[0].rafs && !info[0].ticks, `worker rAF: ${JSON.stringify(info[0])}`);
  assert(info[1].ticks, `page-timed: ${JSON.stringify(info[1])}`);
  for (const id of ['a', 'b']) {
    const [px] = await screenPixels(page, `#${id}`, [BANDS[2]]);
    assert(near(px, [255, 255, 0, 255]), `${id}: band 2 after its timer ${fmt(px)}`);
  }
});

test('worker: without Worker, createJsui({ worker: true }) runs on the main thread with one warning', async ({ page }) => {
  const warnings = [];
  page.on('console', (m) => { if (m.type() === 'warning' && /worker mode/.test(m.text())) warnings.push(m.text()); });
  const r = await page.evaluate(async (dist) => {
    window.Worker = undefined;
    const c = document.createElement('canvas');
    c.id = 'ui';
    Object.assign(c.style, { width: '320px', height: '200px' });
    document.body.appendChild(c);
    const j = await window.createJsui({ canvas: c, worker: true, renderer: 'cpu', bundleUrl: 'fixtures/bands.js',
      moduleUrl: new URL(`../${dist}/jsui.js`, location.href) });
    await j.whenIdle();
    return { worker: !!j.worker, renderer: j.renderer, sameCanvas: j.canvas === c };
  }, DIST);
  assert(!r.worker && r.sameCanvas && r.renderer.startsWith('webgl'), `fallback ${JSON.stringify(r)}`);
  assert(warnings.length === 1 && /no Worker/.test(warnings[0]), `warnings ${JSON.stringify(warnings)}`);
  const px = await screenPixels(page, '#ui', BANDS);
  [RED, GREEN].forEach((want, i) => assert(near(px[i], want), `band ${i}: ${fmt(px[i])}`));
});

// -------------------------------------------------------------------- run
let failed = 0, skipped = 0, passed = 0;
for (const t of tests) {
  if (ONLY && !t.name.includes(ONLY)) continue;
  WORKER = t.worker;
  const { page, context, errors } = await newPage(t.ctxOpts);
  try {
    const r = await t.fn({ page });
    const unexpected = r?.allowErrors ? [] : errors;
    assert(unexpected.length === 0, `page errors: ${unexpected.join(' | ')}`);
    console.log(`  ok    ${t.name}`);
    passed++;
  } catch (e) {
    if (e instanceof Skip && (!STRICT || e.soft)) {
      console.log(`  SKIP  ${t.name}: ${e.message}`);
      skipped++;
    } else {
      console.log(`  FAIL  ${t.name}: ${e.message}`);
      if (process.env.JSUI_WEB_DEBUG) console.log(e.stack);
      if (errors.length) console.log(`        page errors: ${errors.join(' | ')}`);
      failed++;
    }
  } finally {
    await context.close();
  }
}
await browser.close();
server.close();
console.log(`\n${passed} passed, ${skipped} skipped, ${failed} failed`);
process.exit(failed ? 1 : 0);
