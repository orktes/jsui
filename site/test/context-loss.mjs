#!/usr/bin/env node
// Which part of the /screens/cinematic trailer loses a browser's WebGL
// context (seen in Playwright WebKit on macOS, main and worker mode). Runs
// each variant in a fresh page for --seconds and reports every
// webglcontextlost / webglcontextrestored, with the trailer's state:
//
//   site-poster   the showcase on /screens/cinematic, ?autoplay=0 (poster only)
//   site-video    the same, playing the trailer (1080p VP9 on the GPU paths)
//   site-worker   the same in worker mode (?worker=1)
//   bare-video    no JSUI: a WebGL2 canvas, texImage2D(<video>) per presented frame
//   bare-frame    no JSUI: texImage2D(new VideoFrame(video)) per presented frame
//   bare-blur     bare-video, also drawing it through a 37-tap blur, full canvas
//
//   node site/test/context-loss.mjs [--browser webkit|chromium] [--dir site/dist] [--seconds 10]
//                                   [--local-media <dir with 1080p.webm, poster.jpg>] [variants...]
//
// Build the site first (node site/scripts/build.mjs). --local-media answers
// the Wikimedia URLs from local files (offline machines).
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as pw from 'playwright';

const SITE = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2);
const opt = (name, def) => (args.includes(name) ? args[args.indexOf(name) + 1] : def);
const DIR = path.resolve(opt('--dir', path.join(SITE, 'dist')));
const BROWSER = opt('--browser', 'webkit');
const SECONDS = Number(opt('--seconds', 10));
const MEDIA = opt('--local-media', null);
const named = args.filter((a, i) => !a.startsWith('--') && !args[i - 1]?.startsWith('--'));
const VARIANTS = ['site-poster', 'site-video', 'site-worker', 'bare-video', 'bare-frame', 'bare-blur']
  .filter((v) => !named.length || named.includes(v));
const VIDEO = 'https://upload.wikimedia.org/wikipedia/commons/transcoded/c/c0/Big_Buck_Bunny_4K.webm/Big_Buck_Bunny_4K.webm.1080p.vp9.webm';

const MIME = { '.js': 'text/javascript', '.html': 'text/html', '.wasm': 'application/wasm', '.json': 'application/json',
  '.png': 'image/png', '.jpg': 'image/jpeg', '.ttf': 'font/ttf' };
const server = http.createServer((req, res) => {
  const p = decodeURIComponent(new URL(req.url, 'http://x').pathname);
  if (p === '/bare.html') {
    res.writeHead(200, { 'content-type': 'text/html' });
    return res.end('<!doctype html><body style="margin:0;background:#000"><canvas width=1280 height=720></canvas></body>');
  }
  const file = path.normalize(path.join(DIR, p.endsWith('/') ? p + 'index.html' : p));
  if (!file.startsWith(DIR) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) { res.writeHead(404); return res.end(); }
  res.writeHead(200, { 'content-type': MIME[path.extname(file)] || 'application/octet-stream' });
  fs.createReadStream(file).pipe(res);
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const ORIGIN = `http://127.0.0.1:${server.address().port}`;

// Every WebGL context of the page reports loss / restore. A worker's
// OffscreenCanvas is watched through jsui's log lines ('WebGL context lost',
// '... restored', renderer changes).
const watch = () => {
  const t0 = performance.now();
  window.__glEvents = [];
  const orig = HTMLCanvasElement.prototype.getContext;
  HTMLCanvasElement.prototype.getContext = function (type, ...a) {
    const c = orig.call(this, type, ...a);
    if (c && /webgl/.test(type) && !this.__watched) {
      this.__watched = true;
      const v = () => document.querySelector('video');
      const state = () => (v() ? `video ${v().videoWidth}x${v().videoHeight} t=${v().currentTime.toFixed(2)} rs=${v().readyState}` : 'no video');
      for (const ev of ['webglcontextlost', 'webglcontextrestored']) {
        this.addEventListener(ev, () => window.__glEvents.push(`${((performance.now() - t0) / 1000).toFixed(2)}s ${ev} ` +
          `(${this.isConnected ? `${this.width}x${this.height}` : 'a detached canvas: the host\'s WebGL probe'}, ${state()})`));
      }
    }
    return c;
  };
};

// No JSUI: a <video> uploaded into a WebGL2 texture per presented frame and drawn.
const bare = async ({ url, mode, seconds }) => {
  const c = document.querySelector('canvas');
  const gl = c.getContext('webgl2');
  if (!gl) return 'no WebGL2';
  const v = Object.assign(document.createElement('video'), { crossOrigin: 'anonymous', muted: true, loop: true, playsInline: true, src: url });
  v.style.cssText = 'position:fixed;left:0;top:0;width:2px;height:2px;opacity:0.01';
  document.body.appendChild(v);
  const vs = `#version 300 es
    out vec2 uv; void main() { vec2 p = vec2(gl_VertexID & 1, gl_VertexID >> 1) * 2.0; uv = vec2(p.x, 1.0 - p.y) * 0.5 + 0.0;
    gl_Position = vec4(p * 2.0 - 1.0, 0, 1); }`;
  const taps = mode === 'blur' ? 18 : 0;
  const fs = `#version 300 es
    precision mediump float; uniform sampler2D t; in vec2 uv; out vec4 o;
    void main() { vec4 s = vec4(0); float n = 0.0;
      for (int i = -${taps}; i <= ${taps}; i++) { s += texture(t, uv + vec2(float(i) * 0.002, 0)); n += 1.0; } o = s / n; }`;
  const sh = (type, src) => { const s = gl.createShader(type); gl.shaderSource(s, src); gl.compileShader(s); return s; };
  const prog = gl.createProgram();
  gl.attachShader(prog, sh(gl.VERTEX_SHADER, vs));
  gl.attachShader(prog, sh(gl.FRAGMENT_SHADER, fs));
  gl.linkProgram(prog);
  const tex = gl.createTexture();
  let uploads = 0, failures = 0;
  const draw = () => {
    if (gl.isContextLost() || v.readyState < 2) return;
    try {
      gl.bindTexture(gl.TEXTURE_2D, tex);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
      if (mode === 'frame') {
        const f = new VideoFrame(v);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, gl.RGBA, gl.UNSIGNED_BYTE, f);
        f.close();
      } else {
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, gl.RGBA, gl.UNSIGNED_BYTE, v);
      }
      gl.useProgram(prog);
      gl.drawArrays(gl.TRIANGLES, 0, 3);
      uploads++;
    } catch (e) {
      failures++;
    }
  };
  const onFrame = () => { draw(); v.requestVideoFrameCallback(onFrame); };
  v.requestVideoFrameCallback(onFrame);
  await v.play().catch((e) => window.__glEvents.push(`play: ${e}`));
  await new Promise((r) => setTimeout(r, seconds * 1000));
  return `${uploads} uploads, ${failures} failed, video ${v.videoWidth}x${v.videoHeight} t=${v.currentTime.toFixed(2)}`;
};

const browser = await pw[BROWSER].launch(BROWSER === 'chromium'
  ? { args: ['--autoplay-policy=no-user-gesture-required', '--enable-unsafe-webgpu'] } : {});
for (const variant of VARIANTS) {
  const ctx = await browser.newContext({ viewport: { width: 1440, height: 900 } });
  const page = await ctx.newPage();
  const logs = [];
  page.on('console', (m) => { if (/jsui|WebGL|context/i.test(m.text())) logs.push(m.text().slice(0, 200)); });
  page.on('pageerror', (e) => logs.push('pageerror: ' + e.message.slice(0, 200)));
  if (MEDIA) {
    await page.route('https://upload.wikimedia.org/**', (route) => {
      const webm = route.request().url().endsWith('.webm');
      route.fulfill({ status: 200, body: fs.readFileSync(path.join(MEDIA, webm ? '1080p.webm' : 'poster.jpg')),
        headers: { 'access-control-allow-origin': '*', 'content-type': webm ? 'video/webm' : 'image/jpeg' } });
    });
  }
  await page.addInitScript(watch);
  let result = '';
  if (variant.startsWith('site')) {
    const q = { 'site-poster': 'autoplay=0', 'site-video': '', 'site-worker': 'worker=1' }[variant];
    await page.goto(`${ORIGIN}/index.html?${q}#/screens/cinematic`);
    await page.waitForFunction(() => window.jsui, null, { timeout: 60000 });
    await page.waitForTimeout(SECONDS * 1000);
    result = await page.evaluate(() => {
      const v = document.querySelector('video');
      return `renderer ${window.jsui.renderer}, textures ${window.jsui.textureBackend}, worker ${!!window.jsui.worker}, ` +
        (v ? `video ${v.videoWidth}x${v.videoHeight} t=${v.currentTime.toFixed(2)}` : 'no video');
    });
  } else {
    await page.goto(`${ORIGIN}/bare.html`);
    result = await page.evaluate(bare, { url: VIDEO, mode: variant.slice(5), seconds: SECONDS });
  }
  const events = await page.evaluate(() => window.__glEvents);
  console.log(`${variant}: ${result}`);
  for (const e of events) console.log(`    ${e}`);
  for (const l of logs.slice(0, 12)) console.log(`    log: ${l}`);
  await ctx.close();
}
await browser.close();
server.close();
