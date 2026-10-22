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
    '--force-color-profile=srgb'],
});

class Skip extends Error {}
const skip = (why, soft = false) => { const e = new Skip(why); e.soft = soft; throw e; };
const tests = [];
const test = (name, fn, ctxOpts = {}) => tests.push({ name, fn, ctxOpts });
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
async function mount(page, opts = {}) {
  return page.evaluate(async ({ id, w, h, opts, dist }) => {
    if (dist !== 'dist' && !opts.moduleUrl) opts = { ...opts, moduleUrl: new URL(`../${dist}/jsui.js`, location.href) };
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
    return { renderer: jsui.renderer, pw: jsui.pixelWidth, ph: jsui.pixelHeight };
  }, { id: opts.id ?? 'ui', w: opts.width ?? 320, h: opts.height ?? 200, opts: opts.jsui ?? {}, dist: DIST });
}

// Mounts with renderer 'gpu'; SKIPs when the module has no GPU backend (the
// host then fell back to the CPU path, which the other tests cover).
async function mountGpu(page, opts = {}) {
  const info = await mount(page, { ...opts, jsui: { ...opts.jsui, renderer: 'gpu' } });
  // Expected to be missing from the default (CPU) module: not a STRICT failure there.
  if (!info.renderer.startsWith('gpu')) skip(`module in web/${DIST} has no GPU backend (renderer ${info.renderer})`, DIST === 'dist');
  return info;
}

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

// Wraps module exports to record calls (decoding string args where named).
async function spy(page, id = 'ui') {
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
const takeCalls = (page) => page.evaluate(() => window.calls.splice(0));

const BANDS = [[10, 25], [10, 75], [10, 125], [10, 175]];
const RED = [255, 0, 0, 255], GREEN = [0, 255, 0, 255], BLUE = [0, 0, 255, 255], HALF_RED = [255, 127, 127, 255];

// ------------------------------------------------------------------- tests
for (const renderer of ['webgl', '2d']) {
  test(`renders bands with correct premultiplied blending (${renderer})`, async ({ page }) => {
    const info = await mount(page, { jsui: { bundleUrl: 'fixtures/bands.js', renderer } });
    assert(renderer === '2d' ? info.renderer === '2d' : info.renderer.startsWith('webgl'), `renderer ${info.renderer}`);
    assert(info.pw === 320 && info.ph === 200, `backing store ${info.pw}x${info.ph}`);
    const px = await screenPixels(page, '#ui', BANDS);
    [RED, GREEN, BLUE, HALF_RED].forEach((want, i) => assert(near(px[i], want), `band ${i}: ${fmt(px[i])} want ${fmt(want)}`));
    const raw = await page.evaluate(() => { const p = window.hosts.ui.readPixels(); const o = (175 * p.width + 10) * 4; return Array.from(p.data.slice(o, o + 4)); });
    assert(near(raw, [128, 0, 0, 128], 1), `premultiplied frame ${fmt(raw)}`);
    const below = await page.evaluate(() => getComputedStyle(document.getElementById('ui')).cursor);
    assert(below === 'default', `cursor ${below}`);
  });
}

test('devicePixelRatio 2: backing store and 1:1 blit', async ({ page }) => {
  const info = await mount(page, { jsui: { bundleUrl: 'fixtures/bands.js' } });
  assert(info.pw === 640 && info.ph === 400, `backing store ${info.pw}x${info.ph}`);
  const px = await screenPixels(page, '#ui', [[20, 99], [20, 100], [20, 199], [20, 200], [600, 50]]);
  assert(near(px[0], RED) && near(px[1], GREEN), `edge at y=100: ${fmt(px[0])} ${fmt(px[1])}`);
  assert(near(px[2], GREEN) && near(px[3], BLUE), `edge at y=200: ${fmt(px[2])} ${fmt(px[3])}`);
  assert(near(px[4], RED), `x=600: ${fmt(px[4])}`);
}, { deviceScaleFactor: 2 });

test('canvas resize updates the viewport without stretching', async ({ page }) => {
  await mount(page, { jsui: { bundleUrl: 'fixtures/bands.js' } });
  await page.evaluate(() => { document.getElementById('ui').style.height = '300px'; document.getElementById('ui').style.width = '400px'; });
  await frames(page, 4);
  const size = await page.evaluate(() => [window.hosts.ui.pixelWidth, window.hosts.ui.pixelHeight, window.hosts.ui.width]);
  assert(size[0] === 400 && size[1] === 300 && size[2] === 400, `after resize ${size}`);
  const px = await screenPixels(page, '#ui', [[10, 25], [10, 75], [10, 250], [380, 25]]);
  assert(near(px[0], RED) && near(px[1], GREEN), `bands moved/stretched: ${fmt(px[0])} ${fmt(px[1])}`);
  assert(near(px[2], [255, 255, 255, 255]) && near(px[3], [255, 255, 255, 255]), `outside bands: ${fmt(px[2])} ${fmt(px[3])}`);
});

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
});

test('host -> UI messages (jsui.postMessage)', async ({ page }) => {
  await mount(page, { jsui: { bundleUrl: 'fixtures/bands.js' } });
  await page.evaluate(() => window.hosts.ui.postMessage('paint', { color: '#ff00ff' }));
  await frames(page, 3);
  const [px] = await screenPixels(page, '#ui', [BANDS[0]]);
  if (near(px, RED)) skip('engine hostMessage() is a skeleton stub');
  assert(near(px, [255, 0, 255, 255]), `band 0 after message: ${fmt(px)}`);
});

test('UI -> host messages with UTF-8 payload', async ({ page }) => {
  await mount(page, { jsui: { bundleUrl: 'fixtures/bands.js' } });
  await frames(page, 2);
  const msgs = await page.evaluate(() => window.hosts.ui.messages);
  if (!msgs.length) skip('engine __jsui_native.postMessage not implemented yet');
  const ready = msgs.find(([n]) => n === 'ready');
  assert(ready && ready[1].text === 'héllo ✓ 日本' && ready[1].bands === 4, `messages ${JSON.stringify(msgs)}`);
});

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
});

test('touch input is forwarded', async ({ page }) => {
  await mount(page, { jsui: { bundleUrl: 'fixtures/bands.js' } });
  await spy(page);
  await page.touchscreen.tap(30, 130);
  await page.waitForTimeout(50);
  const calls = await takeCalls(page);
  const touches = calls.filter(([n]) => n === 'send_touch');
  assert(touches.length >= 2 && touches[0][2] === 0 && touches.at(-1)[2] === 2, `touch phases ${JSON.stringify(touches)}`);
  assert(touches[0][3] === 30 && touches[0][4] === 130, `touch pos ${JSON.stringify(touches[0])}`);
}, { hasTouch: true });

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
});

test('click reaches the UI and UI messages the host', async ({ page }) => {
  await mount(page, { jsui: { bundleUrl: 'fixtures/bands.js' } });
  await page.mouse.click(100, 75);
  await frames(page, 3);
  const [px] = await screenPixels(page, '#ui', [BANDS[1]]);
  if (near(px, GREEN)) skip('InputManager hit testing is a skeleton stub');
  assert(near(px, [255, 255, 255, 255]), `band 1 after click: ${fmt(px)}`);
});

test('bundle errors are reported (404 and throwing bundle)', async ({ page }) => {
  await mount(page, { id: 'a', jsui: { bundleUrl: 'fixtures/missing.js', expectError: true } });
  await mount(page, { id: 'b', jsui: { bundleUrl: 'fixtures/throws.js', expectError: true } });
  await frames(page, 5);
  const [ea, eb] = await page.evaluate(() => [window.hosts.a.errors, window.hosts.b.errors]);
  assert(ea.some((e) => /failed to load bundle/.test(e) && /404/.test(e) && /fixtures\/missing\.js/.test(e)), `404 error: ${JSON.stringify(ea)}`);
  assert(eb.some((e) => /fixture exploded/.test(e)), `throw error: ${JSON.stringify(eb)}`);
  return { allowErrors: true };
});

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
});

test('bundleSource option and loadSource()', async ({ page }) => {
  const src = fs.readFileSync(path.join(WEB, 'test/fixtures/solid.js'), 'utf8');
  await mount(page, { jsui: { bundleSource: src } });
  await frames(page, 3);
  const [px] = await screenPixels(page, '#ui', [[10, 50]]);
  assert(near(px, [0, 128, 128, 255]), `bundleSource: ${fmt(px)}`);
});

for (const renderer of ['auto', 'gpu']) test(`create/destroy cycles: no heap growth, destroy during load is safe (${renderer})`, async ({ page }) => {
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
  if (renderer === 'gpu' && !result.used.startsWith('gpu')) skip(`module in web/${DIST} has no GPU backend`, DIST === 'dist');
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
  await mount(page, { id: 'cpu', width: 480, height: 320, jsui: { bundleUrl: `fixtures/${fixture}.js`, renderer: 'webgl' } });
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

for (const renderer of ['webgl', '2d']) test(`perf: 1080p HUD (~540 nodes), steady state (${renderer})`, async ({ page }) => {
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

// -------------------------------------------------------------------- run
let failed = 0, skipped = 0, passed = 0;
for (const t of tests) {
  if (ONLY && !t.name.includes(ONLY)) continue;
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
