// Frame-time benchmark of the web host in headless Chromium, per renderer:
// bundles a JSX app (default examples/showcase/interactions.jsx, which
// animates every frame), mounts it at 1280x720 and steps frames by hand,
// timing tick + paint + upload + the GPU finishing the frame (WebGPU:
// queue.onSubmittedWorkDone(); WebGL / CPU-with-WebGL-blit: gl.finish()).
//
//   node web/test/frame-bench.mjs [--dist dist-gpu] [--frames 120] [--renderers webgpu,webgl,cpu]
//                                 [--width 1280 --height 720 --dpr 1] [app.jsx]
//
// Headless Chromium has no GPU here: WebGL runs on SwiftShader and WebGPU on
// its SwiftShader fallback adapter (--enable-unsafe-webgpu), so the numbers
// compare the paths on a CPU-emulated GPU, not real hardware.
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
const N = Number(opt('--frames', 120));
const W = Number(opt('--width', 1280)), H = Number(opt('--height', 720)), DPR = Number(opt('--dpr', 1));
const RENDERERS = opt('--renderers', 'webgpu,webgl,cpu').split(',');
const entry = args.find((a, i) => !a.startsWith('--') && !args[i - 1]?.startsWith('--')) ??
  path.join(ROOT, 'examples/showcase/interactions.jsx');

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

const browser = await chromium.launch({
  args: ['--enable-unsafe-swiftshader', '--use-angle=swiftshader', '--ignore-gpu-blocklist', '--force-color-profile=srgb',
    '--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-vulkan=swiftshader'],
});
console.log(`${path.relative(ROOT, entry)} ${W}x${H} @${DPR}x, web/${DIST}, ${N} frames, Chromium ${browser.version()}`);
for (const renderer of RENDERERS) {
  const context = await browser.newContext({ viewport: { width: W, height: H }, deviceScaleFactor: DPR });
  const page = await context.newPage();
  await page.goto(`${ORIGIN}/test/page.html`);
  await page.waitForFunction(() => window.testReady);
  const r = await page.evaluate(async ({ renderer, dist, w, h, n }) => {
    const c = document.createElement('canvas');
    c.style.width = `${w}px`;
    c.style.height = `${h}px`;
    document.body.appendChild(c);
    const j = await window.createJsui({ canvas: c, bundleUrl: 'output/bench/bundle.js', renderer, autoFocus: false,
      moduleUrl: new URL(`../${dist}/jsui.js`, location.href) });
    j.on('log', () => {});
    await Promise.race([j.whenIdle(), new Promise((r) => setTimeout(r, 8000))]);
    j.stop();
    const sync = async () => {
      if (j._wgpu) await j._wgpu.device.queue.onSubmittedWorkDone();
      else j._gl?.finish();
    };
    let t = performance.now();
    const times = [];
    const r0 = j.stats.renders;
    for (let i = 0; i < n + 20; i++) {
      t += 16.67;
      const a = performance.now();
      j.frame(t);
      await sync();
      if (i >= 20) times.push(performance.now() - a);  // after warm-up
    }
    const renders = j.stats.renders - r0;
    const name = j.renderer;
    j.destroy();
    return { times, renders, name };
  }, { renderer, dist: DIST, w: W, h: H, n: N });
  const s = [...r.times].sort((a, b) => a - b);
  const mean = r.times.reduce((a, b) => a + b, 0) / r.times.length;
  console.log(`  ${renderer.padEnd(7)} (${r.name.padEnd(10)}) mean ${mean.toFixed(2)} ms, p50 ${s[s.length >> 1].toFixed(2)}, ` +
    `p95 ${s[Math.floor(s.length * 0.95)].toFixed(2)}, ${r.renders} repaints / ${N + 20} frames`);
  await context.close();
}
await browser.close();
server.close();
