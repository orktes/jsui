// react-native-worklets (src/worklets.js) without the native module: the
// wire codec, remote function bookkeeping and the fallback that runs
// worklets on this runtime (the host JS engine on the web). The worklet
// runtimes themselves are tested in tests/native/react_worklets_e2e_test.cpp.
import { mock } from './mock-native.js';
import './mock-native-animated.js';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { Animated } from '../src/index.js';
import {
  RuntimeKind,
  getRuntimeKind,
  isWorkletFunction,
  runOnUI,
  runOnJS,
  scheduleOnUI,
  runOnUISync,
  runOnUIAsync,
  createWorkletRuntime,
  runOnRuntimeSync,
  createSynchronizable,
  createSerializable,
  createShareable,
  WorkletsModule,
  __internal,
} from '../src/worklets.js';

const { pack, unpack, onWorklets, remotes, sentCode } = __internal;
let hash = 1;
// What the CLI's transform makes of a 'worklet' function.
function worklet(fn, code, closure) {
  return Object.assign(fn, { __workletHash: hash++, __initData: { code, location: 'test.js', line: 1 }, __closure: closure });
}
const roundTrip = (v) => unpack(JSON.parse(JSON.stringify(pack(v, 0, 0))));
// The mock's frame clock (requestAnimationFrame runs on mock.frame).
const frame = async () => {
  await Promise.resolve();
  mock.frame(16);
};

test('the fallback is in use without the native module', () => {
  assert.equal(WorkletsModule, null);
  assert.equal(getRuntimeKind(), RuntimeKind.ReactNative);
  assert.equal(RuntimeKind[2], 'UI');
});

test('values survive the wire format', () => {
  const v = roundTrip({
    u: undefined,
    nan: NaN,
    inf: -Infinity,
    neg: -0,
    big: 12345678901234567890n,
    list: [1, 'x', null, true],
    map: new Map([['a', { b: 1 }]]),
    set: new Set([3]),
    re: /a+b/gi,
    err: new TypeError('bad'),
    f32: new Float32Array([1.5, 2.5]),
    dv: new DataView(new Uint8Array([7, 8]).buffer),
    ab: new Uint8Array([1, 2, 3]).buffer,
  });
  assert.ok('u' in v && v.u === undefined);
  assert.ok(Number.isNaN(v.nan));
  assert.equal(v.inf, -Infinity);
  assert.ok(Object.is(v.neg, -0));
  assert.equal(v.big, 12345678901234567890n);
  assert.deepEqual(v.list, [1, 'x', null, true]);
  assert.deepEqual(v.map.get('a'), { b: 1 });
  assert.ok(v.set.has(3));
  assert.equal(String(v.re), '/a+b/gi');
  assert.ok(v.err instanceof TypeError);
  assert.equal(v.err.message, 'bad');
  assert.deepEqual(Array.from(v.f32), [1.5, 2.5]);
  assert.equal(v.dv.getUint8(1), 8);
  assert.deepEqual(Array.from(new Uint8Array(v.ab)), [1, 2, 3]);
  const cyclic = {};
  cyclic.self = cyclic;
  assert.throws(() => pack(cyclic, 0, 0), /cyclic/);
});

test('worklets carry their code once per runtime and their closure always', () => {
  const k = 5;
  const add = worklet((x) => x + k, '(function add(x){const[k]=this.__closure;return x+k;})', [k]);
  assert.ok(isWorkletFunction(add));
  sentCode.clear();
  const first = pack(add, 2, 0);
  assert.equal(first.$, 'w');
  assert.equal(first.i.code, add.__initData.code);
  assert.deepEqual(first.c, [5]);
  const again = pack(add, 2, 0);
  assert.equal(again.i, undefined);  // the runtime has it
  assert.ok(pack(add, 3, 0).i);  // another runtime does not
  // Coming back, it is evaluated from its code with its own closure.
  const back = unpack({ ...first, c: [10] });
  assert.equal(back(1), 11);
  assert.ok(isWorkletFunction(back));
});

test('other functions cross as remote functions, called back and released', async () => {
  const seen = [];
  const report = (...a) => seen.push(a);
  const before = remotes.size;
  const w1 = pack(report, 2, 0);
  const w2 = pack({ cb: report }, 2, 0).v.cb;
  assert.equal(w1.$, 'f');
  assert.equal(w1.n, 'report');
  assert.equal(w2.id, w1.id);
  assert.equal(remotes.size, before + 1);
  assert.equal(remotes.get(w1.id).refs, 2);
  onWorklets(JSON.stringify([['c', w1, [1, { $: 'u' }]]]));
  assert.deepEqual(seen, [[1, undefined]]);
  // runOnJS itself crosses as a builtin, not a remote function.
  assert.deepEqual(pack(runOnJS, 2, 0), { $: 'b', n: 'runOnJS' });
  onWorklets(JSON.stringify([['a', w1.id]]));
  onWorklets(JSON.stringify([['r', w1.id, 2]]));
  assert.equal(remotes.size, before + 1);
  onWorklets(JSON.stringify([['r', w1.id, 1]]));
  assert.equal(remotes.size, before);
});

test('an Animated value reaching a worklet is made native', () => {
  const v = new Animated.Value(3);
  const w = pack(v, 2, 0);
  assert.equal(w.$, 'av');
  assert.equal(w.t, v.__getNativeTag());
  assert.equal(unpack(w), v);
  // The same getters a worklet sees.
  v.value = 7;
  assert.equal(v.getValue(), 7);
  assert.equal(v.value, 7);
});

test('without the native module worklets run on this runtime', async () => {
  const log = [];
  const k = 2;
  const w = worklet((x) => log.push(['ui', x * k]), '', [k]);
  runOnUI(w)(4);
  scheduleOnUI(w, 5);
  assert.deepEqual(log, []);  // the next frame, not now
  await frame();
  assert.deepEqual(log, [['ui', 8], ['ui', 10]]);
  assert.equal(runOnUISync(worklet((a, b) => a + b, ''), 2, 3), 5);
  const ok = runOnUIAsync(worklet((x) => x * 2, ''), 21);
  const bad = runOnUIAsync(worklet(() => {
    throw new Error('nope');
  }, ''));
  await frame();
  assert.equal(await ok, 42);
  await assert.rejects(bad, /nope/);
  assert.throws(() => runOnUI(() => {}), /not a worklet/);
  const rt = createWorkletRuntime({ name: 'bg' });
  assert.equal(rt.name, 'bg');
  assert.equal(runOnRuntimeSync(rt, worklet((x) => x + 1, ''), 1), 2);
  let called = 0;
  runOnJS(() => called++)();
  await Promise.resolve();
  assert.equal(called, 1);
});

test('synchronizables and serializables', () => {
  const s = createSynchronizable(1);
  s.setBlocking((x) => x + 1);
  assert.equal(s.getBlocking(), 2);
  s.lock();
  s.unlock();
  const f = createSynchronizable(0, { fixedType: true });
  f.setDirty(5);
  assert.equal(f.getDirty(), 5);
  assert.throws(() => createSynchronizable('x', { fixedType: true }), /fixedType/);
  const o = { a: 1 };
  assert.equal(createSerializable(o), o);
  assert.throws(() => createShareable(), /not supported/);
});
