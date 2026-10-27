// The DevTools frontend `jsui dev` serves (src/devtools-frontend.js): fetched
// once per file from the CDN and cached, entry pages patched.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import http from 'node:http';
import { mkdtemp, readFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { createDevtoolsFrontend, editEntryPage, DEVTOOLS_REVISION } from '../src/devtools-frontend.js';

const PAGE = `<meta http-equiv="Content-Security-Policy" content="default-src 'self'; connect-src data: 'self' devtools: ws://127.0.0.1:*;">
<script type="module" src="./entrypoints/js_app/js_app.js"></script>`;

test('entry pages hide the screencast and may connect to the server', () => {
  const out = editEntryPage(PAGE, 'ws://10.0.0.5:5173');
  assert.match(out, /connect-src data: 'self' devtools: ws:\/\/127\.0\.0\.1:\* ws:\/\/10\.0\.0\.5:5173;/);
  assert.match(out, /<script src="\.\/jsui-devtools-early\.js"><\/script>\n<script type="module" src="\.\/entrypoints\/js_app\/js_app\.js"><\/script>/);
});

test('files come from the pinned revision once, then from the cache', async () => {
  const cacheDir = await mkdtemp(path.join(tmpdir(), 'jsui-devtools-'));
  const fetched = [];
  const fake = async (url) => {
    fetched.push(url);
    if (url.endsWith('missing.js')) return new Response('', { status: 404 });
    return new Response(url.endsWith('.html') ? PAGE : 'export {};');
  };
  const fe = createDevtoolsFrontend({ cacheDir, fetch: fake });
  const server = http.createServer((req, res) => {
    if (!fe.handle(req, res, new URL(req.url, 'http://x'))) res.end('other');
  });
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  const base = `http://127.0.0.1:${server.address().port}/__jsui/devtools/`;
  try {
    for (let i = 0; i < 2; i++) {
      const r = await fetch(base + 'core/sdk/sdk.js');
      assert.equal(r.status, 200);
      assert.match(r.headers.get('content-type'), /javascript/);
      assert.equal(await r.text(), 'export {};');
    }
    assert.deepEqual(fetched, [`https://chrome-devtools-frontend.appspot.com/serve_rev/@${DEVTOOLS_REVISION}/core/sdk/sdk.js`]);
    assert.equal(await readFile(path.join(cacheDir, 'core/sdk/sdk.js'), 'utf8'), 'export {};');

    const page = await (await fetch(base + 'js_app.html')).text();
    assert.match(page, /jsui-devtools-early\.js/);
    assert.match(page, new RegExp(`ws://127\\.0\\.0\\.1:${server.address().port};`));
    assert.match(await (await fetch(base + 'jsui-devtools-early.js')).text(), /screencast-enabled/);
    assert.equal((await fetch(base + 'missing.js')).status, 404);
    const raw = (p) => new Promise((r) => http.get({ host: '127.0.0.1', port: server.address().port, path: p }, (res) => { res.resume(); r(res.statusCode); }));
    await raw('/__jsui/devtools/../../etc/passwd');  // URL parsing drops the dots: not ours
    assert.equal(await raw('/__jsui/devtools/a//b.js'), 404);
    assert.equal(fetched.filter((u) => u.includes('passwd')).length, 0);
    assert.equal(fe.url('127.0.0.1:5173', 'abc'), 'http://127.0.0.1:5173/__jsui/devtools/inspector.html?ws=127.0.0.1:5173/__jsui/inspect/page/abc');
  } finally {
    server.close();
  }
});
