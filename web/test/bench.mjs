// Frame-cost benchmark of the WASM build in Node (no browser, no blit):
// 1080p HUD fixture, steady state with one bar animating every frame.
//
//   node web/test/bench.mjs [bundle.js] [frames]
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const WEB = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const bundle = process.argv[2] || path.join(WEB, 'test/fixtures/hud.js');
const N = Number(process.argv[3] || 300);
const { default: createJsuiModule } = await import(pathToFileURL(path.join(WEB, 'dist/jsui.js')).href);
const M = await createJsuiModule();

const W = 1920, H = 1080;
const rt = M._jsui_web_create(0, W, H, 1, 0, 0, 0);
const src = fs.readFileSync(bundle, 'utf8');
const n = M.lengthBytesUTF8(src);
const p = M._malloc(n + 1);
M.stringToUTF8(src, p, n + 1);
M._jsui_load_bundle_source(rt, p, n, 0);
M._free(p);
const buf = M._malloc(W * H * 4);

let t = 0;
const step = () => {
  t += 16.67;
  const a = performance.now();
  M._jsui_tick(rt, t);
  const b = performance.now();
  if (M._jsui_needs_redraw(rt)) M._jsui_render_rgba(rt, buf, W, H, W * 4);
  return [b - a, performance.now() - b];
};
for (let i = 0; i < 30; i++) step();  // warm up (JIT, caches)
const ticks = [], paints = [];
for (let i = 0; i < N; i++) {
  const [tk, pt] = step();
  ticks.push(tk);
  paints.push(pt);
}
const stat = (a) => {
  const s = [...a].sort((x, y) => x - y);
  return `mean ${(a.reduce((x, y) => x + y, 0) / a.length).toFixed(3)} ms, p50 ${s[a.length >> 1].toFixed(3)}, p95 ${s[Math.floor(a.length * 0.95)].toFixed(3)}`;
};
console.log(`${path.basename(bundle)} ${W}x${H}, ${N} frames`);
console.log(`  tick  ${stat(ticks)}`);
console.log(`  paint ${stat(paints)}`);
M._free(buf);
M._jsui_destroy(rt);
