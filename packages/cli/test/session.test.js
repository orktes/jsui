// The dev server's shared session from the runtimes' side (src/dev_session.h
// speaks this protocol from C++): a fake runtime polls for commands, answers
// them and pushes events, and agents attached to the server drive it. The
// real runtimes joining it (WebAssembly in Node, the native viewer, Godot)
// are tested in headless.test.js and on the Mac.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { fileURLToPath } from 'node:url';
import { attach, dev, sessionClients } from '../src/index.js';

const exec = promisify(execFile);
const BIN = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', 'bin', 'jsui.js');

async function server(o = {}) {
  const dir = await mkdtemp(path.join(tmpdir(), 'jsui-session-test-'));
  await writeFile(path.join(dir, 'app.jsx'), "import { Text } from 'react-native';\nexport default function App() { return <Text>hi</Text>; }\n");
  const s = await dev({ entry: path.join(dir, 'app.jsx'), port: 0, log: () => {}, ...o });
  return { ...s, origin: `http://127.0.0.1:${s.port}`, dir };
}

const post = (origin, p, body, headers = {}) =>
  fetch(origin + p, { method: 'POST', headers: { 'Content-Type': 'application/json', ...headers }, body: JSON.stringify(body) });

/** A runtime that speaks the session protocol; `answer(cmd)` returns a command's value. */
function fakeRuntime(origin, { id = 'fake0001', name = 'Fake game', answer = () => null, size = [800, 600] } = {}) {
  const rt = { commands: [], events: [], stopped: false, polls: 0 };
  rt.loop = (async () => {
    while (!rt.stopped) {
      rt.polls++;
      const res = await post(origin, '/__jsui/session/poll', { runtime: id, info: { name, platform: 'macOS', width: size[0], height: size[1], scale: 2 } }).catch(() => null);
      if (!res) break;
      const { commands = [], leave } = await res.json();
      if (leave) break;
      const results = [];
      for (const c of commands) {
        rt.commands.push(c);
        try {
          results.push({ id: c.id, ok: true, value: await answer(c) });
        } catch (e) {
          results.push({ id: c.id, ok: false, error: e.message });
        }
      }
      const events = rt.events.splice(0);
      if (results.length || events.length) await post(origin, '/__jsui/session/push', { runtime: id, results, events });
    }
  })();
  rt.stop = async () => {
    rt.stopped = true;
    await rt.loop;
  };
  return rt;
}

const STATUS = { name: 'Fake game', platform: 'macOS', width: 800, height: 600, scale: 2, wantsFrame: false, pendingLoads: 0, time: 1234 };

test('session: agents drive the runtime that joined; its events reach the feed', async () => {
  const s = await server();
  const state = {};
  const rt = fakeRuntime(s.origin, {
    answer: (c) => {
      if (c.op === 'status') return STATUS;
      if (c.op === 'setState') return void (state[c.key] = c.value);
      if (c.op === 'getState') return state[c.key] ?? null;
      if (c.op === 'screenshot') return Buffer.from('\x89PNG fake', 'latin1').toString('base64');
      if (c.op === 'callUi' && c.name === '__jsui.inspect') {
        return { roots: [{ rootTag: 1, viewport: { width: 800, height: 600 }, children: [{ type: 'View', id: 2, testID: 'play', rect: { x: 100, y: 50, width: 200, height: 40 }, children: [] }] }] };
      }
      if (c.op === 'mouseButton') return true;
      if (c.op === 'unknown') throw new Error('unknown op unknown');
      return true;
    },
  });
  try {
    const app = await attach(s.origin);
    // A command before the runtime's first poll waits for it.
    await app.setState('gold', 42);
    assert.deepEqual(await app.getState('gold'), 42);
    assert.equal(app.width, 800);
    assert.equal(app.runtime.label, 'Fake game (macOS)');

    // Selectors resolve through the runtime's inspector; clicks are real-time commands.
    const consumed = await app.click('#play');
    assert.equal(consumed, true);
    const ops = rt.commands.map((c) => c.op);
    assert.ok(ops.includes('mouseMove') && ops.includes('mouseButton'), ops.join());
    const down = rt.commands.find((c) => c.op === 'mouseButton' && c.down);
    assert.deepEqual([down.x, down.y], [200, 70]);

    const png = await app.screenshot();
    assert.equal(png.toString('latin1', 1, 4), 'PNG');

    // What the person does in the runtime and what the UI does show in the feed.
    rt.events.push({ who: 'user', kind: 'click', x: 5, y: 6, consumed: true, time: 2000 }, { who: 'ui', kind: 'message', name: 'play', data: { level: 1 }, time: 2001 });
    await app.frame();  // one more round trip carries the push
    await app.frame();
    assert.deepEqual(app.messages.map((m) => m.name), ['play']);
    const feed = await app.activity(0);
    assert.ok(feed.some((e) => e.kind === 'join' && e.client === 'Fake game (macOS)'));
    assert.ok(feed.some((e) => e.who === 'user' && e.kind === 'click' && e.client === 'Fake game (macOS)'));
    assert.ok(feed.some((e) => e.who === 'agent' && /setState/.test(e.text)));

    // Host.call answers and resizing belong to the host.
    await assert.rejects(app.handle('shop.buy', { value: 1 }), /answers Host.call itself/);
    await assert.rejects(app.resize(100, 100), /owns its viewport/);

    // The CLI: inline steps, clients.
    const r = await exec(process.execPath, [BIN, 'run', '--attach', s.origin, 'state', 'gold', 'value=7', 'expectState', 'gold', 'value=7']).catch((e) => e);
    assert.ok(JSON.parse(r.stdout.trim().split('\n').at(-1)).ok, r.stdout + r.stderr);
    const list = await exec(process.execPath, [BIN, 'clients', '--attach', s.origin]);
    assert.match(list.stdout, /^\* fake0001 {2}Fake game \(macOS\) {2}800x600 @2x/m);
  } finally {
    await s.close();  // answers the held poll with leave
    await rt.stop().catch(() => {});
  }
});

test('session: the last runtime to join is live; --client picks another', async () => {
  const s = await server();
  const a = fakeRuntime(s.origin, { id: 'aaaa1111', name: 'jsui-viewer', answer: (c) => (c.op === 'status' ? { ...STATUS, name: 'jsui-viewer' } : 'a') });
  try {
    for (let i = 0; i < 100 && !(await sessionClients(s.origin)).length; i++) await new Promise((r) => setTimeout(r, 20));
    const b = fakeRuntime(s.origin, { id: 'bbbb2222', name: 'Godot: Menu', answer: (c) => (c.op === 'status' ? { ...STATUS, name: 'Godot: Menu' } : 'b') });
    try {
      for (let i = 0; i < 100 && (await sessionClients(s.origin)).length < 2; i++) await new Promise((r) => setTimeout(r, 20));
      const list = await sessionClients(s.origin);
      assert.deepEqual(list.map((c) => [c.id, c.live]), [['aaaa1111', false], ['bbbb2222', true]]);
      assert.equal(await (await attach(s.origin)).getState('x'), 'b');
      assert.equal(await (await attach(s.origin, { client: 'viewer' })).getState('x'), 'a');
      assert.equal(await (await attach(s.origin, { client: 'aaaa' })).getState('x'), 'a');
    } finally {
      await s.close();
      await b.stop().catch(() => {});
    }
  } finally {
    await s.close();
    await a.stop().catch(() => {});
  }
});

test('session: without a runtime, agent commands say how to open one', async () => {
  const s = await server({ waitMs: 200 });
  try {
    const app = await attach(s.origin);
    await assert.rejects(app.getState('x'), /no JSUI runtime is connected[\s\S]*\/shared[\s\S]*jsui-viewer --dev[\s\S]*Godot[\s\S]*--headless/);
    const r = await exec(process.execPath, [BIN, 'screenshot', '--attach', s.origin]).catch((e) => e);
    assert.equal(r.code, 1);
    assert.match(r.stderr, /no JSUI runtime is connected/);
    // The page still loads (it runs the app itself when it can).
    const page = await fetch(s.origin + '/shared');
    assert.match(await page.text(), /JSUI shared session/);
  } finally {
    await s.close();
  }
});

test('session: other web pages cannot join or drive it', async () => {
  const s = await server();
  try {
    const evil = { Origin: 'http://evil.example' };
    assert.equal((await post(s.origin, '/__jsui/session/poll', { runtime: 'evil0001', info: {} }, evil)).status, 403);
    assert.equal((await post(s.origin, '/__jsui/shared/call', { method: 'getState', args: ['x'] }, evil)).status, 403);
    // DNS rebinding: the page's origin matches the Host header, which is not an allowed name.
    const rebound = await post(s.origin, '/__jsui/shared/call', { method: 'getState', args: ['x'] }, { Origin: `http://evil.example:${s.port}`, Host: `evil.example:${s.port}` });
    assert.equal(rebound.status, 403);
    assert.deepEqual(await sessionClients(s.origin), []);
    assert.equal((await fetch(s.origin + '/__jsui/session/poll', { method: 'POST', body: '{}' })).status, 415);
  } finally {
    await s.close();
  }
});

test('session: a runtime that quits mid-poll leaves within seconds', async () => {
  const s = await server();
  try {
    const ctl = new AbortController();
    const held = fetch(s.origin + '/__jsui/session/poll', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ runtime: 'quit0001', info: { name: 'jsui-viewer', platform: 'macOS' } }), signal: ctl.signal }).catch(() => null);
    for (let i = 0; i < 100 && !(await sessionClients(s.origin)).length; i++) await new Promise((r) => setTimeout(r, 20));
    assert.equal((await sessionClients(s.origin)).length, 1);
    ctl.abort();  // the process exits: its socket closes
    await held;
    const t0 = Date.now();
    while ((await sessionClients(s.origin)).length && Date.now() - t0 < 5000) await new Promise((r) => setTimeout(r, 100));
    assert.deepEqual(await sessionClients(s.origin), []);
    assert.ok(Date.now() - t0 < 4000, `left after ${Date.now() - t0} ms`);
  } finally {
    await s.close();
  }
});
