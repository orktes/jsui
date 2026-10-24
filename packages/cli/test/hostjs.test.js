// The host JS engine (docs/ARCHITECTURE.md "Host JS engine"): the same
// bundles on QuickJS in WASM and on Node's own V8 render the same pixels and
// view trees, see the same events and calls, and hot-update the same way.
// Needs the CPU WebAssembly module (web/dist, or JSUI_WASM_DIR) built with
// JSUI_HOST_JS; skipped without it unless JSUI_HEADLESS_STRICT=1.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, readFile, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { attach, dev, launch, findWasmDir } from '../src/index.js';
import { loadModule } from '../src/headless.js';

const ROOT = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', '..', '..');
const BIN = path.join(ROOT, 'packages', 'cli', 'bin', 'jsui.js');
const exec = promisify(execFile);
const WASM = findWasmDir();
const M = WASM ? await loadModule() : null;
const HOST = !!M?._jsui_web_host_js?.();
if (!HOST && process.env.JSUI_HEADLESS_STRICT) throw new Error('JSUI_HEADLESS_STRICT: no jsui.wasm with the host JS engine');
const skip = HOST ? false : 'no CPU jsui.wasm built with JSUI_HOST_JS (scripts/build-wasm.sh -> web/dist)';

// Runs `drive(app)` on both engines; returns {quickjs, host} of its result
// plus the final PNG and view tree.
async function both(o, drive = async (app) => app.wait(500)) {
  const out = {};
  for (const js of ['quickjs', 'host']) {
    const app = await launch({ width: 800, height: 600, ...o, js });
    try {
      await app.ready();
      assert.equal(app.hostJs, js === 'host');
      const extra = await drive(app);
      out[js] = { png: await app.screenshot(), tree: await app.tree().catch(() => null), extra, errors: app.errors };
    } finally {
      app.close();
    }
  }
  return out;
}

function assertSame(r, what) {
  assert.deepEqual(r.quickjs.errors, [], `${what}: QuickJS errors`);
  assert.deepEqual(r.host.errors, [], `${what}: host errors`);
  assert.deepEqual(r.host.tree, r.quickjs.tree, `${what}: view trees differ`);
  assert.ok(r.host.png.equals(r.quickjs.png), `${what}: pixels differ`);
  assert.deepEqual(r.host.extra, r.quickjs.extra, `${what}: results differ`);
}

const frames = async (app, n, each) => {
  for (let i = 0; i < n; i++) {
    if (each) each(i);
    await app.frame();
  }
};

test('pixel parity: examples/showcase', { skip }, async () => {
  for (const f of ['interactions.jsx', 'spring-wiggle.jsx']) {
    const entry = path.join(ROOT, 'examples', 'showcase', f);
    assertSame(await both({ entry }, async (app) => {
      await app.wait(1000);
      // Pointer input over the middle (Pressables, hover springs).
      await app.click({ x: 400, y: 300 });
      await app.wait(300);
    }), f);
  }
});

test('pixel parity: jsui-bench scenarios', { skip }, async () => {
  const scenario = (n) => path.join(ROOT, 'tools', 'jsui-bench', 'scenarios', n);
  // markers: 100 markers moving every frame, all three feeds.
  const feed = (app, i) => {
    const b = new Float32Array(300);
    for (let k = 0; k < 100; k++) b.set([(k * 37 + i * 3) % 760, (k * 53 + i * 2) % 560, (k * 7 + i) % 100], k * 3);
    app.setBuffer('enemies', b);
    app.setState('enemies', Array.from({ length: 100 }, (_, k) => [b[k * 3], b[k * 3 + 1], b[k * 3 + 2]]));
  };
  for (const component of ['JsonMarkers', 'BufferMarkers', 'NativeMarkers'])
    assertSame(await both({ entry: scenario('markers.jsx'), component }, (app) => frames(app, 20, (i) => feed(app, i))), component);
  // inventory: one slot patched per frame.
  assertSame(await both({ entry: scenario('inventory.jsx') }, (app) =>
    frames(app, 20, (i) => app.patchState('inventory', { slots: { [i * 7]: { count: i + 1 } } }))), 'inventory');
  assertSame(await both({ entry: scenario('list.jsx') }, (app) => frames(app, 20)), 'list');
  assertSame(await both({ entry: scenario('menu.jsx') }, async (app) => {
    for (let y = 100; y < 500; y += 40) { await app.hover({ x: 200, y }); await frames(app, 3); }
  }), 'menu');
  // surfaces: 30 Plate root views, props changing every frame.
  assertSame(await both({ entry: scenario('surfaces.jsx') }, async (app) => {
    const props = (i, f) => JSON.stringify({ name: `Enemy ${i}`, hp: (i * 13 + f * 3) % 100, level: 1 + i });
    const s = [];
    for (let i = 0; i < 30; i++) s.push(app._withStrings(['Plate', props(i, 0)], (a, p) => app.M._jsui_surface_create(app.rt, a, p, 120, 40, 1)));
    await frames(app, 10, (f) => s.forEach((h, i) => app._withStrings([props(i, f)], (p) => app.M._jsui_surface_set_props(h, p))));
    // Every surface's pixels.
    const px = new Uint8Array(120 * 40 * 4);
    const p = app.M._malloc(px.length);
    const sums = s.map((h) => {
      app.M._jsui_render_rgba(h, p, 120, 40, 480);
      let sum = 0;
      for (const v of app.M.HEAPU8.subarray(p, p + px.length)) sum = (sum * 31 + v) >>> 0;
      return sum;
    });
    app.M._free(p);
    s.forEach((h) => app.M._jsui_surface_destroy(h));
    return sums;
  }), 'surfaces');
});

const APP = `import React, { useEffect, useRef, useState } from 'react';
import { Animated, AppRegistry, Pressable, Text, View } from 'react-native';
import { Host, HostState, useHostBufferSelector, useHostHandler, useHostMessage, useHostState } from '@orktes/jsui-react';

function App() {
  const gold = useHostState('gold', 0);
  const n = useHostBufferSelector('enemies', (b) => b.length, 0);
  const [log, setLog] = useState([]);
  const add = (s) => setLog((l) => [...l, s]);
  const x = useRef(new Animated.Value(0)).current;
  useHostMessage('ping', (d) => add('ping ' + d.n));
  useHostHandler('ui.sum', ({ a, b }) => a + b);
  useHostHandler('ui.later', async ({ a }) => { await new Promise((r) => setTimeout(r, 50)); return a * 2; });
  useEffect(() => {
    const t = setTimeout(() => add('timer'), 100);
    requestAnimationFrame(() => add('raf'));
    queueMicrotask(() => add('micro'));
    Promise.resolve().then(() => add('promise'));
    return () => clearTimeout(t);
  }, []);
  return (
    <View style={{ flex: 1, padding: 20, backgroundColor: '#123' }}>
      <Text testID="engine" style={{ color: '#fff' }}>{typeof Intl === 'object' ? 'v8' : 'quickjs'}</Text>
      <Text testID="gold" style={{ color: '#fc0' }}>{gold} gold, {n} values</Text>
      <Pressable testID="go" style={{ width: 120, height: 40, backgroundColor: '#468' }}
        onPress={() => {
          Host.postMessage('pressed', { at: 1 });
          Host.call('shop.buy', { item: 'potion' }).then((r) => add('bought ' + r.gold), (e) => add('err ' + e.message));
          HostState.set('settings', { volume: 0.5 });
          Animated.timing(x, { toValue: 200, duration: 300, useNativeDriver: true }).start(({ finished }) => add('anim ' + finished));
        }}>
        <Text style={{ color: '#fff' }}>Go</Text>
      </Pressable>
      <Animated.View testID="box" style={{ width: 40, height: 40, backgroundColor: '#e64', transform: [{ translateX: x }] }} />
      <Text testID="log" style={{ color: '#fff' }}>{log.join(', ')}</Text>
    </View>
  );
}
AppRegistry.registerComponent('main', () => App);
`;

test('events, host calls, host data, timers and native Animated match', { skip }, async () => {
  const dir = await mkdtemp(path.join(tmpdir(), 'jsui-hostjs-test-'));
  await writeFile(path.join(dir, 'app.jsx'), APP);
  const r = await both({ entry: path.join(dir, 'app.jsx'), calls: { 'shop.buy': () => ({ gold: 7 }) } }, async (app) => {
    await app.wait(300);
    const engine = (await app.find('#engine')).text;
    app.setState('gold', 42);
    app.setBuffer('enemies', [1, 2, 3, 4]);
    app.postMessage('ping', { n: 3 });
    await app.click('#go');
    await app.wait(500);
    const sum = await app.callUi('ui.sum', { a: 2, b: 3 });
    const later = await app.callUi('ui.later', { a: 21 });
    await app.wait(100);
    return {
      engine,
      log: (await app.find('#log')).text,
      gold: (await app.find('#gold')).text,
      box: (await app.find('#box')).rect,
      sum, later,
      messages: app.messages.map((m) => [m.name, m.data]),
      calls: app.calls.map((c) => [c.name, c.args]),
      writes: app.stateWrites.map((w) => [w.key, w.value]),
    };
  });
  // The engines differ only where the test looks for it.
  assert.equal(r.quickjs.extra.engine, 'quickjs');
  assert.equal(r.host.extra.engine, 'v8');
  r.host.extra.engine = r.quickjs.extra.engine;
  r.host.tree = JSON.parse(JSON.stringify(r.host.tree).replace('"v8"', '"quickjs"'));
  // Promise jobs run in the drain after each native -> JS call on QuickJS,
  // at the host's own microtask checkpoint (after the tick, so after this
  // tick's rAF) on the host engine; queueMicrotask is the runtime's on both.
  for (const k of ['quickjs', 'host']) {
    r[k].extra.log = r[k].extra.log.replace(/(micro), (?:promise, (raf)|(raf), promise)/, '$1, $2$3, (promise)');
    r[k].tree = JSON.parse(JSON.stringify(r[k].tree).replace(/(micro), (?:promise, (raf)|(raf), promise)/, '$1, $2$3, (promise)'));
  }
  const e = r.quickjs.extra;
  assert.equal(e.log, 'micro, raf, (promise), timer, ping 3, bought 7, anim true');
  assert.equal(e.gold, '42 gold, 4 values');
  assert.deepEqual(e.messages, [['pressed', { at: 1 }]]);
  assert.deepEqual(e.calls, [['shop.buy', { item: 'potion' }]]);
  assert.deepEqual(e.writes, [['settings', { volume: 0.5 }]]);
  assert.equal(e.sum, 5);
  assert.equal(e.later, 42);
  assert.deepEqual(r.host.tree, r.quickjs.tree);
  assert.deepEqual(r.host.extra, r.quickjs.extra);
  assert.deepEqual(r.host.errors, []);
});

test('Lottie and shader events, compileShader and precompileShaders match', { skip }, async () => {
  const dir = await mkdtemp(path.join(tmpdir(), 'jsui-hostjs-test-'));
  await writeFile(path.join(dir, 'coin.json'), await readFile(path.join(ROOT, 'examples', 'assets', 'lottie', 'coin.json')));
  await writeFile(path.join(dir, 'app.jsx'), `import React, { useEffect, useState } from 'react';
import { AppRegistry, LottieView, ShaderView, Text, View, compileShader, precompileShaders } from 'react-native';
import coin from './coin.json';
const GOOD = 'uniform float2 resolution; uniform float glow; half4 main(float2 p) { return half4(p.x / resolution.x, glow, 0.5, 1); }';
const BAD = 'half4 main(float2 p) { return nope; }';
function App() {
  const [log, setLog] = useState([]);
  const add = (s) => setLog((l) => [...l, s]);
  useEffect(() => {
    add('info ' + JSON.stringify(compileShader(GOOD).uniforms.map((u) => u.name)));
    add('bad ' + !!compileShader(BAD).error);
    precompileShaders([GOOD]).then((r) => add('pre ' + r.length));
  }, []);
  return (
    <View style={{ flex: 1, padding: 10, gap: 10, backgroundColor: '#123' }}>
      <LottieView source={coin} autoPlay loop={false} speed={4} style={{ width: 80, height: 80 }}
        onAnimationLoaded={() => add('loaded')} onAnimationFinish={(c) => add('finish ' + c)} />
      <ShaderView source={GOOD} uniforms={{ glow: 0.25 }} style={{ width: 100, height: 40 }} />
      <ShaderView source={BAD} style={{ width: 100, height: 40, backgroundColor: '#444' }} onError={(e) => add('error ' + !!e.nativeEvent.error)} />
      <Text testID="log" style={{ color: '#fff' }}>{log.join(', ')}</Text>
    </View>
  );
}
AppRegistry.registerComponent('main', () => App);
`);
  const r = await both({ entry: path.join(dir, 'app.jsx'), width: 400, height: 300 }, async (app) => {
    await app.wait(2000);
    return (await app.find('#log')).text.split(', ').sort();
  });
  assertSame(r, 'lottie / shader');
  // (precompileShaders waits for a GPU warm-up frame the CPU module never runs: same on both.)
  assert.deepEqual(r.quickjs.extra.filter((s) => !s.startsWith('pre ')), ['bad true', 'error true', 'finish false', 'info ["resolution","glow"]', 'loaded']);
});

test('errors: a throwing handler and an unhandled rejection are reported', { skip }, async () => {
  const dir = await mkdtemp(path.join(tmpdir(), 'jsui-hostjs-test-'));
  await writeFile(path.join(dir, 'app.jsx'), `import { AppRegistry, Pressable, View } from 'react-native';
function App() {
  return <Pressable testID="b" style={{ width: 100, height: 100 }} onPress={() => { Promise.reject(new Error('nope')); throw new Error('boom'); }}><View /></Pressable>;
}
AppRegistry.registerComponent('main', () => App);
`);
  // In a child process: on the host engine the bundle's rejections are the
  // process's own, which node:test would take for this test's.
  for (const js of ['quickjs', 'host']) {
    const r = await exec(process.execPath, [BIN, 'run', path.join(dir, 'app.jsx'), '--js', js, '--width', '200', '--height', '200',
      'click', '#b', 'wait', '100']).catch((e) => e);
    assert.equal(r.code, 1, `${js}: exit code`);
    assert.match(r.stderr, /boom/, `${js}: thrown error`);
    assert.match(r.stderr, /Unhandled promise rejection: Error: nope/, `${js}: unhandled rejection`);
  }
});

test('hot updates on the host engine keep component state', { skip }, async () => {
  const dir = await mkdtemp(path.join(tmpdir(), 'jsui-hostjs-test-'));
  const TITLE = `import { Text } from 'react-native';
export default function Title({ n }) { return <Text testID="title">{'Count ' + n}</Text>; }
`;
  await writeFile(path.join(dir, 'title.jsx'), TITLE);
  await writeFile(path.join(dir, 'app.jsx'), `import React, { useState } from 'react';
import { AppRegistry, Pressable, Text, View } from 'react-native';
import Title from './title.jsx';
function App() {
  const [n, setN] = useState(0);
  return (
    <View style={{ flex: 1 }}>
      <Text testID="engine">{typeof Intl === 'object' ? 'v8' : 'quickjs'}</Text>
      <Title n={n} />
      <Pressable testID="inc" style={{ width: 100, height: 40 }} onPress={() => setN((x) => x + 1)}><View /></Pressable>
    </View>
  );
}
AppRegistry.registerComponent('main', () => App);
`);
  const server = await dev({ entry: path.join(dir, 'app.jsx'), port: 0, log: () => {}, headless: { width: 320, height: 240, js: 'host' } });
  try {
    const app = await attach(`http://127.0.0.1:${server.port}`);
    await app.idle(3000);
    assert.equal((await app.find('#engine')).text, 'v8');
    await app.click('#inc');
    await app.click('#inc');
    assert.equal((await app.find('#title')).text, 'Count 2');
    const since = (await app.activity(0)).at(-1).seq;
    await writeFile(path.join(dir, 'title.jsx'), TITLE.replace("'Count '", "'Total '"));
    for (let i = 0; i < 100 && (await app.find('#title')).text !== 'Total 2'; i++) await app.wait(100);
    assert.equal((await app.find('#title')).text, 'Total 2', 'state kept across the hot update');
    assert.ok(!(await app.activity(since)).some((e) => e.kind === 'log' && /reloading/.test(e.text)), 'hot update, not a reload');
    // An edit to the entry reloads: a fresh context, on the same engine.
    await writeFile(path.join(dir, 'app.jsx'), (await import('node:fs')).readFileSync(path.join(dir, 'app.jsx'), 'utf8').replace("'v8'", "'v8!'"));
    for (let i = 0; i < 100 && (await app.find('#engine').catch(() => ({}))).text !== 'v8!'; i++) await app.wait(100);
    assert.equal((await app.find('#engine')).text, 'v8!');
    assert.equal((await app.find('#title')).text, 'Total 0');
  } finally {
    await server.close();
  }
});
