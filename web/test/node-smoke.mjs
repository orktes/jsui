// Fast smoke test of web/dist/jsui.{js,wasm} in Node (no browser): renders a
// bundle from source and one fetched over HTTP through the WASM fetch()
// loader backend, and checks pixels of the RGBA frame.
//
//   node web/test/node-smoke.mjs
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const WEB = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const { default: createJsuiModule } = await import(pathToFileURL(path.join(WEB, 'dist/jsui.js')).href);
const M = await createJsuiModule();

const str = (s) => { const n = M.lengthBytesUTF8(s) + 1; const p = M._malloc(n); M.stringToUTF8(s, p, n); return p; };
let failures = 0;
const check = (cond, msg) => { console.log(`  ${cond ? 'ok  ' : 'FAIL'}  ${msg}`); if (!cond) failures++; };

function frame(rt, w, h) {
  const buf = M._malloc(w * h * 4);
  M._jsui_render_rgba(rt, buf, w, h, w * 4);
  const px = M.HEAPU8.slice(buf, buf + w * h * 4);
  M._free(buf);
  return (x, y) => Array.from(px.slice((y * w + x) * 4, (y * w + x) * 4 + 4));
}

// 1. bundle from source
{
  const rt = M._jsui_web_create(0, 320, 200, 1, 0, 0);
  const src = fs.readFileSync(path.join(WEB, 'test/fixtures/bands.js'), 'utf8');
  const n = M.lengthBytesUTF8(src);
  const p = str(src), name = str('bands.js');
  check(M._jsui_load_bundle_source(rt, p, n, name) === 0, 'load_bundle_source');
  M._free(p); M._free(name);
  for (let t = 0; t <= 300; t += 16) M._jsui_tick(rt, t);
  check(M._jsui_last_error(rt) === 0, 'no JS error');
  const at = frame(rt, 320, 200);
  check(JSON.stringify(at(10, 25)) === '[255,0,0,255]', `red band ${at(10, 25)}`);
  check(JSON.stringify(at(10, 125)) === '[255,255,0,255]', `timer turned band 2 yellow ${at(10, 125)}`);
  check(JSON.stringify(at(10, 175)) === '[128,0,0,128]', `premultiplied 50% red ${at(10, 175)}`);
  M._jsui_destroy(rt);
}

// 2. bundle over HTTP via the fetch() backend (+ 404 handling)
{
  const server = http.createServer((req, res) => {
    const f = path.join(WEB, 'test/fixtures', path.basename(req.url));
    if (!fs.existsSync(f)) { res.writeHead(404); return res.end(); }
    res.writeHead(200, { 'content-type': 'text/javascript' });
    res.end(fs.readFileSync(f));
  });
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  const base = `http://127.0.0.1:${server.address().port}/app/index.html`;
  const baseP = str(base);
  const rt = M._jsui_web_create(0, 320, 200, 1, 0, baseP);
  const url = str('../fixtures/solid.js');  // relative to the base URL
  M._jsui_load_bundle_url(rt, url);
  M._free(url);
  let t = 0;
  for (const deadline = Date.now() + 3000; Date.now() < deadline; t += 16) {
    M._jsui_tick(rt, t);
    const at = frame(rt, 320, 200);
    if (at(10, 50)[3]) break;
    await new Promise((r) => setTimeout(r, 10));
  }
  const at = frame(rt, 320, 200);
  check(JSON.stringify(at(10, 50)) === '[0,128,128,255]', `fetched bundle rendered ${at(10, 50)}`);

  const rt2 = M._jsui_web_create(0, 100, 100, 1, 0, baseP);
  const missing = str('nope.js');
  M._jsui_load_bundle_url(rt2, missing);
  M._free(missing);
  let err = null;
  for (const deadline = Date.now() + 3000; Date.now() < deadline && !err; t += 16) {
    M._jsui_tick(rt2, t);
    const e = M._jsui_last_error(rt2);
    if (e) err = M.UTF8ToString(e);
    await new Promise((r) => setTimeout(r, 10));
  }
  check(err && err.includes('404') && err.includes('/app/nope.js'), `404 reported: ${err}`);

  // Destroy with a request in flight: the late response must be dropped safely.
  const rt3 = M._jsui_web_create(0, 100, 100, 1, 0, baseP);
  const u3 = str('../fixtures/solid.js');
  M._jsui_load_bundle_url(rt3, u3);
  M._free(u3);
  M._jsui_destroy(rt3);
  await new Promise((r) => setTimeout(r, 200));
  M._jsui_tick(rt, t + 16);
  check(true, 'destroy during fetch did not crash');

  M._jsui_destroy(rt);
  M._jsui_destroy(rt2);
  M._free(baseP);
  server.close();
}

console.log(failures ? `${failures} failure(s)` : 'node smoke: all ok');
process.exit(failures ? 1 : 0);
