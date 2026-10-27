// JS debugging in `jsui dev`: the relay between DevTools frontends
// (WebSocket) and a runtime's CDP agent (HTTP long-polls), as the native
// runtime (src/js/inspector.cpp) speaks it, and the source maps of the
// bundles the server serves.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, writeFile, readFile } from 'node:fs/promises';
import { realpathSync } from 'node:fs';
import { tmpdir } from 'node:os';
import http from 'node:http';
import net from 'node:net';
import crypto from 'node:crypto';
import path from 'node:path';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { fileURLToPath } from 'node:url';
import { dev } from '../src/index.js';

const exec = promisify(execFile);
const BIN = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', 'bin', 'jsui.js');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function until(pred, what, ms = 8000) {
  const t0 = Date.now();
  for (;;) {
    const v = await pred();
    if (v) return v;
    if (Date.now() - t0 > ms) throw new Error('timed out waiting for ' + what);
    await sleep(20);
  }
}

const APP = `import React, { useState } from 'react';
import { View, Text } from 'react-native';

// Line 4: a comment, so the marker is not on the line the compiled code has.
export default function App({ size = 1 }) {
  const [n, setN] = useState(0);
  const marker = 'MARKER_7';
  return (
    <View>
      <Text nativeID="title">{marker + n + size}</Text>
    </View>
  );
}
`;

async function server(o = {}) {
  const dir = await mkdtemp(path.join(tmpdir(), 'jsui-inspect-test-'));
  await writeFile(path.join(dir, 'App.jsx'), APP);
  await writeFile(path.join(dir, 'index.jsx'), "import { AppRegistry } from 'react-native';\nimport App from './App';\nAppRegistry.registerComponent('main', () => App);\n");
  const logs = [];
  const s = await dev({ entry: path.join(dir, 'index.jsx'), port: 0, log: (m) => logs.push(m), ...o });
  return { ...s, origin: `http://127.0.0.1:${s.port}`, dir, logs };
}

const post = (origin, p, body) => fetch(origin + p, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });

/** One poll of a fake runtime: resolves with its events. */
const poll = async (origin, runtime, extra = {}) => (await (await post(origin, '/__jsui/inspect/poll', { runtime, name: 'Fake runtime', title: origin + '/bundle.js', ...extra })).json()).events;

/** A frontend: a WebSocket that collects what it receives. */
async function frontend(url) {
  const ws = new WebSocket(url);
  const got = [];
  const closed = new Promise((r) => ws.addEventListener('close', (e) => r(e)));
  ws.addEventListener('message', (e) => got.push(e.data));
  await new Promise((resolve, reject) => {
    ws.addEventListener('open', resolve);
    ws.addEventListener('error', () => reject(new Error('WebSocket failed: ' + url)));
  });
  return { ws, got, closed };
}

test('inspect: runtime <-> frontend relay over poll/push and WebSocket', async () => {
  const s = await server();
  try {
    // Advertised on the bundle and the version endpoint.
    const b = await fetch(s.origin + '/bundle.js');
    assert.equal(b.headers.get('x-jsui-inspect'), '1');
    assert.match(b.headers.get('access-control-expose-headers'), /X-Jsui-Inspect/);
    assert.equal((await fetch(s.origin + '/__jsui/version')).headers.get('x-jsui-inspect'), '1');
    assert.ok(s.logs.some((l) => /chrome:\/\/inspect/.test(l) && /"type":"node","request":"attach"/.test(l) && l.includes(`"port":${s.port}`)), s.logs.join('\n'));

    const version = await (await fetch(s.origin + '/json/version')).json();
    assert.match(version.Browser, /^jsui\//);
    assert.equal(version['Protocol-Version'], '1.3');
    assert.deepEqual(await (await fetch(s.origin + '/json/list')).json(), []);

    // The first poll registers the runtime; it is held while nothing happens.
    const first = poll(s.origin, 'rt-1');
    await until(async () => (await (await fetch(s.origin + '/json/list')).json()).length === 1, 'target listed');
    const [t] = await (await fetch(s.origin + '/json')).json();
    assert.equal(t.id, 'rt-1');
    assert.equal(t.type, 'node');
    assert.equal(t.title, `Fake runtime — ${s.origin}/bundle.js`);
    assert.equal(t.url, `${s.origin}/bundle.js`);
    assert.equal(t.webSocketDebuggerUrl, `ws://127.0.0.1:${s.port}/__jsui/inspect/ws/rt-1`);
    assert.equal(t.devtoolsFrontendUrl, `devtools://devtools/bundled/js_app.html?experiments=true&v8only=true&ws=127.0.0.1:${s.port}/__jsui/inspect/ws/rt-1`);
    assert.match(t.devtoolsFrontendUrlCompat, /inspector\.html\?.*ws=127\.0\.0\.1:\d+\/__jsui\/inspect\/ws\/rt-1$/);

    // A frontend connects: the held poll answers with `open`.
    const fe = await frontend(t.webSocketDebuggerUrl);
    const opened = await first;
    assert.equal(opened.length, 1);
    assert.equal(opened[0].type, 'open');
    const sid = opened[0].session;
    assert.equal(typeof sid, 'string');
    assert.ok(s.logs.some((l) => /frontend .* attached to 'Fake runtime'/.test(l)));

    // Frontend -> runtime, in order (a big one too: 64-bit frame length).
    const big = JSON.stringify({ id: 3, method: 'Runtime.evaluate', params: { expression: 'x'.repeat(70000) } });
    fe.ws.send('{"id":1,"method":"Runtime.enable"}');
    fe.ws.send('{"id":2,"method":"Debugger.enable"}');
    fe.ws.send(big);
    const msgs = [];
    while (msgs.length < 3) msgs.push(...(await poll(s.origin, 'rt-1')));
    assert.deepEqual(msgs.map((e) => [e.type, e.session]), [['message', sid], ['message', sid], ['message', sid]]);
    assert.equal(msgs[0].message, '{"id":1,"method":"Runtime.enable"}');
    assert.equal(msgs[2].message, big);

    // Runtime -> frontend: to the session, and to everyone (session null).
    const longReply = JSON.stringify({ id: 3, result: { result: { type: 'string', value: 'é'.repeat(40000) } } });
    assert.equal((await post(s.origin, '/__jsui/inspect/push', {
      runtime: 'rt-1',
      messages: [
        { session: sid, message: '{"id":1,"result":{}}' },
        { session: null, message: '{"method":"Runtime.executionContextCreated","params":{}}' },
        { session: sid, message: longReply },
        { session: 'nope', message: '{"lost":true}' },
      ],
    })).status, 200);
    await until(() => fe.got.length === 3, 'pushed messages');
    assert.deepEqual(fe.got.slice(0, 2), ['{"id":1,"result":{}}', '{"method":"Runtime.executionContextCreated","params":{}}']);
    assert.equal(fe.got[2], longReply);

    // A second frontend is a second session; null reaches both.
    const pending = poll(s.origin, 'rt-1');
    const fe2 = await frontend(t.webSocketDebuggerUrl);
    const [open2] = await pending;
    assert.equal(open2.type, 'open');
    assert.notEqual(open2.session, sid);
    await post(s.origin, '/__jsui/inspect/push', { runtime: 'rt-1', messages: [{ session: null, message: '{"method":"Debugger.resumed"}' }] });
    await until(() => fe.got.length === 4 && fe2.got.length === 1, 'broadcast');

    // A second poll answers the held one with nothing.
    const held = poll(s.origin, 'rt-1');
    await sleep(50);
    const second = poll(s.origin, 'rt-1');
    assert.deepEqual(await held, []);

    // Closing a frontend tells the runtime.
    fe2.ws.close();
    assert.deepEqual(await second, [{ type: 'close', session: open2.session }]);

    // bye: the frontends are closed, the target is gone.
    const pollDuringBye = poll(s.origin, 'rt-1');
    await post(s.origin, '/__jsui/inspect/bye', { runtime: 'rt-1' });
    await fe.closed;
    assert.deepEqual(await pollDuringBye, []);
    // That poll re-registered nothing (it was held before bye).
    assert.deepEqual(await (await fetch(s.origin + '/json/list')).json(), []);
  } finally {
    await s.close();
  }
});

test('inspect: the relay serves source maps to DevTools and refuses domains the runtime lacks', async () => {
  const s = await server();
  try {
    const first = poll(s.origin, 'rt-1');
    await until(async () => (await (await fetch(s.origin + '/json/list')).json()).length === 1, 'target listed');
    const fe = await frontend(`ws://127.0.0.1:${s.port}/__jsui/inspect/ws/rt-1`);
    await first;
    const reply = async (id, msg) => {
      fe.ws.send(JSON.stringify({ id, ...msg }));
      return JSON.parse(await until(() => fe.got.find((m) => JSON.parse(m).id === id), `reply ${id}`));
    };

    // Network.loadNetworkResource + IO.read, as DevTools reads a source map.
    // DevTools drops the data of a read that says eof, so the last chunk
    // with data must not say it.
    const want = await (await fetch(s.origin + '/bundle.js.map')).text();
    assert.ok(want.length > 1000);
    const { result } = await reply(1, { method: 'Network.loadNetworkResource', params: { url: s.origin + '/bundle.js.map', options: {} } });
    assert.equal(result.resource.success, true);
    let text = '';
    for (let id = 2; ; id++) {
      const r = (await reply(id, { method: 'IO.read', params: { handle: result.resource.stream, size: 100000 } })).result;
      if (r.eof) {
        assert.equal(r.data, '');
        break;
      }
      text += r.data;
    }
    assert.equal(text, want);

    // Unknown methods of these domains are errors, not empty results:
    // DevTools reads fields of the result (Network.emulateNetworkConditionsByRule)
    // or waits for a profile (Profiler.stop).
    for (const [id, method] of [[100, 'Network.enable'], [101, 'Profiler.stop'], [102, 'HeapProfiler.enable']]) {
      const r = await reply(id, { method, params: {} });
      assert.equal(r.error.code, -32601, method);
    }

    // Everything else still reaches the runtime.
    fe.ws.send('{"id":200,"method":"Debugger.enable"}');
    const got = await poll(s.origin, 'rt-1');
    assert.deepEqual(got.map((e) => e.message), ['{"id":200,"method":"Debugger.enable"}']);

    // A runtime that records profiles gets the Profiler domain.
    const held = poll(s.origin, 'rt-1', { profiler: true });
    await sleep(100);
    fe.ws.send('{"id":201,"method":"Profiler.start"}');
    assert.deepEqual((await held).map((e) => e.message), ['{"id":201,"method":"Profiler.start"}']);
  } finally {
    await s.close();
  }
});

test('inspect: a frontend may connect before its runtime polls; a returning runtime is re-greeted', async () => {
  const s = await server();
  try {
    const fe = await frontend(`ws://127.0.0.1:${s.port}/__jsui/inspect/ws/late-runtime`);
    fe.ws.send('{"id":1,"method":"Runtime.enable"}');
    await sleep(50);
    assert.deepEqual(await (await fetch(s.origin + '/json/list')).json(), [], 'not a target before it polls');
    const events = await poll(s.origin, 'late-runtime');
    assert.deepEqual(events.map((e) => e.type), ['open', 'message']);
    assert.equal(events[1].message, '{"id":1,"method":"Runtime.enable"}');
    const sid = events[0].session;
    // The runtime goes quiet for a while, then polls again: `open` again for its sessions.
    const p = poll(s.origin, 'late-runtime');
    await post(s.origin, '/__jsui/inspect/push', { runtime: 'late-runtime', messages: [{ session: sid, message: '{"id":1,"result":{}}' }] });
    await until(() => fe.got.length === 1, 'reply');
    fe.ws.send('{"id":2}');
    assert.equal((await p)[0].message, '{"id":2}');
    await sleep(5200);
    const again = await poll(s.origin, 'late-runtime');
    assert.deepEqual(again, [{ type: 'open', session: sid }]);
  } finally {
    await s.close();
  }
});

/** A raw WebSocket client: sends hand-made (masked, fragmented) frames. */
async function rawSocket(port, pathname, headers = {}) {
  const sock = net.connect(port, '127.0.0.1');
  await new Promise((r) => sock.once('connect', r));
  const key = crypto.randomBytes(16).toString('base64');
  sock.write(`GET ${pathname} HTTP/1.1\r\nHost: 127.0.0.1:${port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n${Object.entries(headers).map(([k, v]) => `${k}: ${v}\r\n`).join('')}\r\n`);
  let buf = Buffer.alloc(0);
  sock.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
  });
  const head = await until(() => {
    const i = buf.indexOf('\r\n\r\n');
    if (i < 0) return null;
    const h = buf.subarray(0, i).toString();
    buf = buf.subarray(i + 4);
    return h;
  }, 'handshake');
  const frame = (opcode, payload, fin = true) => {
    const mask = crypto.randomBytes(4);
    const len = payload.length;
    const header = len < 126 ? Buffer.from([(fin ? 0x80 : 0) | opcode, 0x80 | len]) : Buffer.from([(fin ? 0x80 : 0) | opcode, 0x80 | 126, len >> 8, len & 255]);
    const body = Buffer.from(payload);
    for (let i = 0; i < body.length; i++) body[i] ^= mask[i & 3];
    sock.write(Buffer.concat([header, mask, body]));
  };
  /** Next server frame: {opcode, payload}. */
  const next = () => until(() => {
    if (buf.length < 2) return null;
    let len = buf[1] & 0x7f;
    let off = 2;
    if (len === 126) {
      len = buf.readUInt16BE(2);
      off = 4;
    }
    if (buf.length < off + len) return null;
    const f = { opcode: buf[0] & 0x0f, fin: !!(buf[0] & 0x80), masked: !!(buf[1] & 0x80), payload: buf.subarray(off, off + len) };
    buf = buf.subarray(off + len);
    return f;
  }, 'a frame');
  return { sock, head, key, frame, next };
}

test('inspect: WebSocket framing (handshake, fragments, ping, close) and origin checks', async () => {
  const s = await server();
  try {
    const c = await rawSocket(s.port, '/__jsui/inspect/ws/rawrt');
    assert.match(c.head, /^HTTP\/1\.1 101/);
    const accept = crypto.createHash('sha1').update(c.key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
    assert.match(c.head, new RegExp(`Sec-WebSocket-Accept: ${accept.replace(/[+/]/g, '\\$&')}`, 'i'));
    // A fragmented text message with a ping between its fragments.
    const text = '{"id":7,"method":"Debugger.pause","params":{"pad":"' + 'p'.repeat(300) + '"}}';
    c.frame(1, Buffer.from(text.slice(0, 10)), false);
    c.frame(9, Buffer.from('hi'));
    c.frame(0, Buffer.from(text.slice(10, 200)), false);
    c.frame(0, Buffer.from(text.slice(200)), true);
    const pong = await c.next();
    assert.equal(pong.opcode, 10);
    assert.equal(pong.payload.toString(), 'hi');
    const events = await poll(s.origin, 'rawrt');
    assert.deepEqual(events.map((e) => e.type), ['open', 'message']);
    assert.equal(events[1].message, text);
    // Server frames are unmasked text frames.
    await post(s.origin, '/__jsui/inspect/push', { runtime: 'rawrt', messages: [{ session: events[0].session, message: '{"id":7,"result":{}}' }] });
    const f = await c.next();
    assert.deepEqual([f.opcode, f.fin, f.masked, f.payload.toString()], [1, true, false, '{"id":7,"result":{}}']);
    // Close handshake: the server answers with the code and the runtime hears `close`.
    const closing = poll(s.origin, 'rawrt');
    c.frame(8, Buffer.from([0x03, 0xe8]));
    const cf = await c.next();
    assert.equal(cf.opcode, 8);
    assert.equal(cf.payload.readUInt16BE(0), 1000);
    assert.deepEqual(await closing, [{ type: 'close', session: events[0].session }]);
    c.sock.destroy();

    // Unmasked client frames are a protocol error.
    const u = await rawSocket(s.port, '/__jsui/inspect/ws/rawrt');
    u.sock.write(Buffer.from([0x81, 0x02, 0x68, 0x69]));
    const err = await u.next();
    assert.equal(err.opcode, 8);
    assert.equal(err.payload.readUInt16BE(0), 1002);
    u.sock.destroy();

    // Web pages may not attach (they could run code in the app); DevTools may.
    const evil = await rawSocket(s.port, '/__jsui/inspect/ws/rawrt', { Origin: 'https://evil.example' });
    assert.match(evil.head, /^HTTP\/1\.1 403/);
    evil.sock.destroy();
    const devtools = await rawSocket(s.port, '/__jsui/inspect/ws/rawrt', { Origin: 'devtools://devtools' });
    assert.match(devtools.head, /^HTTP\/1\.1 101/);
    devtools.sock.destroy();
    const other = await rawSocket(s.port, '/somewhere');
    assert.match(other.head, /^HTTP\/1\.1 404/);
    other.sock.destroy();
    // DNS rebinding: another site's name resolving to this server.
    const rebind = await new Promise((resolve, reject) => {
      http.get({ host: '127.0.0.1', port: s.port, path: '/json/list', headers: { Host: 'evil.example' } }, (res) => {
        res.resume();
        resolve(res.statusCode);
      }).on('error', reject);
    });
    assert.equal(rebind, 403);
  } finally {
    await s.close();
  }
});

// chrome://inspect's network discovery asks for /json/version and /json/list
// in bare HTTP/1.1 requests without a Host header.
test('inspect: discovery without a Host header (chrome://inspect)', async () => {
  const s = await server({ hmr: false });
  const held = poll(s.origin, 'rt-nohost').catch(() => {});
  try {
    await until(async () => (await (await fetch(s.origin + '/json/list')).json()).length === 1, 'target listed');
    const get = (p) => new Promise((resolve, reject) => {
      const sock = net.connect(s.port, '127.0.0.1', () => sock.write(`GET ${p} HTTP/1.1\r\n\r\n`));
      let data = Buffer.alloc(0);
      const done = (body) => resolve({ status: Number(String(data).split(' ')[1]), body });
      sock.on('data', (d) => {
        data = Buffer.concat([data, d]);
        const head = data.indexOf('\r\n\r\n');
        const len = /content-length: (\d+)/i.exec(String(data));
        if (head >= 0 && len && data.length >= head + 4 + Number(len[1])) {
          sock.destroy();
          done(JSON.parse(data.subarray(head + 4).toString('utf8')));
        }
      });
      sock.on('end', () => done(null));
      sock.on('error', reject);
    });
    const v = await get('/json/version');
    assert.equal(v.status, 200);
    assert.match(v.body.Browser, /^jsui\//);
    const list = await get('/json/list');
    assert.equal(list.status, 200);
    assert.equal(list.body[0].id, 'rt-nohost');
    assert.equal(list.body[0].webSocketDebuggerUrl, `ws://127.0.0.1:${s.port}/__jsui/inspect/ws/rt-nohost`);
  } finally {
    await s.close();
    await held;
  }
});

test('--no-inspect: no header, no relay', async () => {
  const s = await server({ inspect: false, hmr: false });
  try {
    const b = await fetch(s.origin + '/bundle.js');
    assert.equal(b.headers.get('x-jsui-inspect'), null);
    assert.equal((await fetch(s.origin + '/__jsui/version')).headers.get('x-jsui-inspect'), null);
    assert.equal((await fetch(s.origin + '/json/list')).status, 404);
    assert.equal((await post(s.origin, '/__jsui/inspect/poll', { runtime: 'x1' })).status, 404);
    const ws = await rawSocket(s.port, '/__jsui/inspect/ws/x1');
    assert.match(ws.head, /^HTTP\/1\.1 404/);
    ws.sock.destroy();
    assert.ok(!s.logs.some((l) => /chrome:\/\/inspect/.test(l)));
  } finally {
    await s.close();
  }
  const help = await exec(process.execPath, [BIN, '--help']);
  assert.match(help.stdout, /--no-inspect/);
});

// ------------------------------------------------------------- source maps

// A small v3 decoder, independent of src/sourcemap.js.
function decode(mappings) {
  const map = {};
  [...'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/'].forEach((c, i) => (map[c] = i));
  const state = [0, 0, 0, 0, 0];
  return mappings.split(';').map((line) => {
    state[0] = 0;
    return line.split(',').filter(Boolean).map((seg) => {
      const vals = [];
      let v = 0;
      let shift = 0;
      for (const ch of seg) {
        const d = map[ch];
        v += (d & 31) << shift;
        if (d & 32) shift += 5;
        else {
          vals.push(v & 1 ? -(v >> 1) : v >> 1);
          v = 0;
          shift = 0;
        }
      }
      return vals.map((x, i) => (state[i] += x));
    });
  });
}

/** Original {source, line (1-based), column} of a generated position (line 0-based). */
function original(map, lines, line, column) {
  const seg = (lines[line] || []).filter((s) => s[0] <= column).at(-1);
  if (!seg || seg.length < 4) return null;
  return { source: map.sources[seg[1]], line: seg[2] + 1, column: seg[3], content: map.sourcesContent[seg[1]] };
}

function locate(code, needle) {
  const lines = code.split('\n');
  const line = lines.findIndex((l) => l.includes(needle));
  assert.ok(line >= 0, 'generated code has ' + needle);
  return [line, lines[line].indexOf(needle)];
}

function assertMapsBack(code, map, file, needle, want) {
  const lines = decode(map.mappings);
  const [l, c] = locate(code, needle);
  const o = original(map, lines, l, c);
  assert.ok(o, `${needle} is mapped`);
  assert.equal(realpathSync(fileURLToPath(o.source)), realpathSync(file));
  assert.deepEqual([o.line, o.column], want, `${needle} maps back to line ${want[0]}`);
  return o;
}

for (const reactCompiler of [false, true]) {
  test(`source maps: HMR bundle and hot updates map back to the JSX${reactCompiler ? ' (React Compiler)' : ''}`, async () => {
    const s = await server({ reactCompiler });
    const app = path.join(s.dir, 'App.jsx');
    try {
      const code = await (await fetch(s.origin + '/bundle.js')).text();
      assert.match(code, /__jsui_hmr\.start\(/);
      assert.match(code, /\n\/\/# sourceMappingURL=bundle\.js\.map\n$/);
      const res = await fetch(s.origin + '/bundle.js.map');
      assert.equal(res.status, 200);
      const map = await res.json();
      assert.equal(map.version, 3);
      const o = assertMapsBack(code, map, app, "'MARKER_7'".replace(/'/g, '"'), [7, 17]);
      assert.equal(o.content, APP);
      assertMapsBack(code, map, app, 'nativeID: "title"', [10, reactCompiler ? 21 : 12]);
      // Every source is a file on disk (vendor ones: node_modules), with its content.
      assert.ok(map.sources.some((x) => /\/node_modules\/react\//.test(x)));
      assert.ok(map.sources.every((x) => x.startsWith('file://')));
      assert.equal(map.sourcesContent.filter((x) => x == null).length, 0);

      // A hot update carries an inline map of its define()s.
      const from = s.state.version;
      await sleep(150);
      await writeFile(app, APP.replace('// Line 4', '// three\n// more\n// lines\n// Line 4'));
      // (A spurious rebuild of the unchanged files may come first.)
      let update;
      let text;
      await until(async () => {
        update = await fetch(s.origin + '/__jsui/hmr?from=' + from);
        text = await update.text();
        return text.includes('MARKER_7');
      }, 'rebuild with the edit');
      assert.equal(update.headers.get('x-jsui-version'), s.state.version);
      const m = /\n\/\/# sourceMappingURL=data:application\/json;charset=utf-8;base64,([A-Za-z0-9+/=]+)\n$/.exec(text);
      assert.ok(m, 'inline map at the end of the update:\n' + text.slice(0, 300) + '\n...\n' + text.slice(-300));
      const umap = JSON.parse(Buffer.from(m[1], 'base64').toString());
      assertMapsBack(text, umap, app, '"MARKER_7"', [10, 17]);
      assert.equal(umap.sourcesContent[umap.sources.findIndex((x) => x.endsWith('/App.jsx'))], await readFile(app, 'utf8'));
    } finally {
      await s.close();
    }
  });
}

test('source maps: --no-hmr bundle sources are files on disk', async () => {
  const s = await server({ hmr: false });
  try {
    const code = await (await fetch(s.origin + '/bundle.js')).text();
    assert.match(code, /\/\/# sourceMappingURL=bundle\.js\.map\s*$/);
    const map = await (await fetch(s.origin + '/bundle.js.map')).json();
    assertMapsBack(code, map, path.join(s.dir, 'App.jsx'), '"MARKER_7"', [7, 17]);
    assert.ok(map.sources.every((x) => x.startsWith('file://')));
  } finally {
    await s.close();
  }
});

test('profile trace: JS samples on Main, the phases on a render thread, worklets and GPU submits on tracks of their own', async () => {
  const { profileTrace } = await import('../src/profile-trace.js');
  const cf = (functionName) => ({ functionName, scriptId: '0', url: '', lineNumber: -1, columnNumber: -1 });
  const profile = {
    nodes: [
      { id: 1, callFrame: cf('(root)'), children: [2, 3, 4] },
      { id: 2, callFrame: cf('(idle)'), children: [] },
      { id: 3, callFrame: cf('App'), children: [] },
      { id: 4, callFrame: cf('(paint)'), children: [] },
    ],
    startTime: 1000, endTime: 2000,
    samples: [2, 3, 4, 2], timeDeltas: [0, 100, 200, 300],
    jsuiSpans: [['(paint)', 1300, 1400], ['(gpu)', 1400, 1450], ['(worklets)', 1500, 1520]],
  };
  const { traceEvents } = profileTrace(profile, { name: 'jsui-viewer' });
  const chunk = traceEvents.find((e) => e.name === 'ProfileChunk');
  assert.deepEqual(chunk.args.data.cpuProfile.nodes.map((n) => n.id), [1, 2, 3]);
  assert.deepEqual(chunk.args.data.cpuProfile.nodes[0].children, [2, 3]);
  assert.deepEqual(chunk.args.data.cpuProfile.samples, [2, 3, 2, 2]);
  const thread = (pid, tid) => traceEvents.find((e) => e.name === 'thread_name' && e.pid === pid && e.tid === tid).args.name;
  const paint = traceEvents.find((e) => e.name === 'Paint');
  assert.deepEqual([paint.ph, paint.ts, paint.dur, thread(paint.pid, paint.tid)], ['X', 1300, 100, 'JSUI render thread']);
  assert.equal(paint.args.data.layerId, 0);
  const gpu = traceEvents.find((e) => e.name === 'GPUTask');
  assert.deepEqual([gpu.ts, gpu.dur, thread(gpu.pid, gpu.tid)], [1400, 50, 'CrGpuMain']);
  assert.ok(traceEvents.some((e) => e.name === 'process_name' && e.pid === gpu.pid && e.args.name === 'GPU Process'));
  const worklet = traceEvents.find((e) => e.name === 'FunctionCall');
  assert.deepEqual([worklet.ts, worklet.dur, thread(worklet.pid, worklet.tid)], [1500, 20, 'JSUI worklets']);
  assert.ok(traceEvents.some((e) => e.name === 'thread_name' && e.args.name === 'CrRendererMain'));
});
