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
// JSUI_WEB_DIST=dist-webgl: the GPU module (its CPU path must work the same).
const DIST = process.env.JSUI_WEB_DIST || 'dist';
const { default: createJsuiModule } = await import(pathToFileURL(path.join(WEB, DIST, 'jsui.js')).href);
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
  const rt = M._jsui_web_create(0, 320, 200, 1, 0, 0, 0);
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
    if (req.url === '/echo') {
      let body = '';
      req.on('data', (c) => { body += c; });
      req.on('end', () => {
        res.writeHead(201, { 'content-type': 'text/plain', 'x-reply': 'yes' });
        res.end(`${req.method} ${req.headers['x-test']} ${body}`);
      });
      return;
    }
    const f = path.join(WEB, 'test/fixtures', path.basename(req.url));
    if (!fs.existsSync(f)) { res.writeHead(404); return res.end(); }
    res.writeHead(200, { 'content-type': 'text/javascript' });
    res.end(fs.readFileSync(f));
  });
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  const base = `http://127.0.0.1:${server.address().port}/app/index.html`;
  const baseP = str(base);
  const rt = M._jsui_web_create(0, 320, 200, 1, 0, baseP, 0);
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

  const rt2 = M._jsui_web_create(0, 100, 100, 1, 0, baseP, 0);
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
  const rt3 = M._jsui_web_create(0, 100, 100, 1, 0, baseP, 0);
  const u3 = str('../fixtures/solid.js');
  M._jsui_load_bundle_url(rt3, u3);
  M._free(u3);
  M._jsui_destroy(rt3);
  await new Promise((r) => setTimeout(r, 200));
  M._jsui_tick(rt, t + 16);
  check(true, 'destroy during fetch did not crash');

  // fetch() from JS through the browser loader: method, request/response headers, body, status.
  const got = [];
  (M.__jsuiHosts ??= new Map()).set(7, { onMessage: (name, json) => got.push([name, JSON.parse(json)]) });
  const rt4 = M._jsui_web_create(7, 100, 100, 1, 0, baseP, 0);
  const fsrc = `fetch('/echo', { method: 'POST', headers: { 'X-Test': 'abc' }, body: 'héllo' })
    .then((r) => r.text().then((t) => __jsui_native.postMessage('echo', JSON.stringify(
      { status: r.status, ok: r.ok, reply: r.headers.get('x-reply'), text: t }))))
    .catch((e) => __jsui_native.postMessage('echo', JSON.stringify({ error: String(e) })));`;
  const fp = str(fsrc), fname = str(base);
  M._jsui_load_bundle_source(rt4, fp, M.lengthBytesUTF8(fsrc), fname);
  M._free(fp); M._free(fname);
  for (const deadline = Date.now() + 3000; Date.now() < deadline && !got.length; t += 16) {
    M._jsui_tick(rt4, t);
    await new Promise((r) => setTimeout(r, 10));
  }
  const echo = got[0]?.[1];
  check(echo && echo.status === 201 && echo.ok && echo.reply === 'yes' && echo.text === 'POST abc héllo',
    `fetch POST round trip ${JSON.stringify(got)}`);
  M._jsui_destroy(rt4);

  M._jsui_destroy(rt);
  M._jsui_destroy(rt2);
  M._free(baseP);
  server.close();
}

// Surfaces through the C API: a second root view of one runtime, its own
// pixels and an auto-sized one (web/test/fixtures/surfaces.js).
{
  const rt = M._jsui_web_create(0, 64, 64, 1, 0, 0, 0);
  const key = str('Plate'), props = str('{"color":"#ff0000"}');
  const s = M._jsui_surface_create(rt, key, props, 40, 20, 1);
  check(s !== 0 && M._jsui_surface_tag(s) > 1 && M._jsui_surface_runtime(s) === rt, 'surface_create');
  const src = fs.readFileSync(path.join(WEB, 'test/fixtures/surfaces.js'), 'utf8');
  const p = str(src), name = str('surfaces.js');
  check(M._jsui_load_bundle_source(rt, p, M.lengthBytesUTF8(src), name) === 0, 'surfaces bundle');
  M._free(p); M._free(name);
  const bkey = str('Badge'), bprops = str('{"color":"#0000ff","size":[30,12]}');
  const b = M._jsui_surface_create(rt, bkey, bprops, 0, 0, 1);
  for (let t = 0; t <= 64; t += 16) M._jsui_tick(rt, t);
  check(JSON.stringify(frame(s, 40, 20)(20, 10)) === '[255,0,0,255]', 'surface pixels');
  check(JSON.stringify(frame(rt, 64, 64)(10, 50)) === '[0,128,128,255]', 'main surface pixels');
  const size = M._malloc(8);
  M._jsui_surface_get_size(b, size, size + 4);
  const f = new Float32Array(M.HEAPU8.buffer, size, 2);
  check(f[0] === 30 && f[1] === 12, `auto size ${f[0]}x${f[1]}`);
  M._free(size);
  M._jsui_surface_destroy(s);
  [key, props, bkey, bprops].forEach((x) => M._free(x));
  M._jsui_destroy(rt);  // frees the badge handle too
}

// Frame data: jsui_set_buffer / jsui_map_buffer into the WASM heap, read by
// JS as a typed array over the same memory.
{
  const got = [];
  (M.__jsuiHosts ??= new Map()).set(9, { onMessage: (name, json) => got.push([name, JSON.parse(json)]) });
  const rt = M._jsui_web_create(9, 64, 64, 1, 0, 0, 0);
  const src = `globalThis.__jsui_onHostBuffer = (keys) => {
    const i = __jsui_native.buffer('pos');
    __jsui_native.postMessage('buf', JSON.stringify([keys, i[1], Array.from(new Float32Array(i[0], 0, i[2]))]));
  };`;
  const p = str(src), name = str('buffers.js'), key = str('pos');
  M._jsui_load_bundle_source(rt, p, M.lengthBytesUTF8(src), name);
  const data = M._malloc(12);
  new Float32Array(M.HEAPU8.buffer, data, 3).set([1.5, -2, 3]);
  check(M._jsui_set_buffer(rt, key, 0, data, 3) === 1 && M._jsui_set_buffer(rt, key, 0, data, 3) === 0, 'set_buffer');
  M._jsui_tick(rt, 16);
  check(JSON.stringify(got) === '[["buf",[["pos"],0,[1.5,-2,3]]]]', `buffer reached JS ${JSON.stringify(got)}`);
  got.length = 0;
  const m = M._jsui_map_buffer(rt, key, 0, 2);
  new Float32Array(M.HEAPU8.buffer, m, 2).set([7, 8]);
  M._jsui_commit_buffer(rt, key);
  M._jsui_tick(rt, 32);
  check(JSON.stringify(got) === '[["buf",[["pos"],0,[7,8]]]]', `mapped buffer ${JSON.stringify(got)}`);
  [p, name, key, data].forEach((x) => M._free(x));
  M._jsui_destroy(rt);
}

console.log(failures ? `${failures} failure(s)` : 'node smoke: all ok');
process.exit(failures ? 1 : 0);
