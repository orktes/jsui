// Frame data: HostBuffer / useHostBuffer* against the mock bridge (typed
// arrays over host storage), and Animated.hostBuffer with the JS driver.
// The native-driver serialization is in native-animated.test.js; the
// runtime side in tests/native/host_buffers_test.cpp.
import { mock } from './mock-native.js';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createElement as h } from 'react';
import {
  View,
  Text,
  Animated,
  render,
  HostBuffer,
  useHostBuffer,
  useHostBufferSelector,
  useHostBufferListener,
  useHostBufferValue,
} from '../src/index.js';

const wait = (ms = 0) => new Promise((r) => setTimeout(r, ms));
const byId = (id) => mock.find((x) => x.props.nativeID === id);
const textOf = (id) => {
  const n = byId(id);
  if (!n) return null;
  const out = [];
  const rec = (i) => {
    const x = mock.nodes.get(i);
    if (x.type === 'RawText') out.push(x.text);
    else x.children.forEach(rec);
  };
  rec(n.id);
  return out.join('');
};

test('HostBuffer.get: typed array over host storage, reused while it stays put, detached when it moves', () => {
  assert.equal(HostBuffer.get('pos'), undefined);
  assert.equal(HostBuffer.version('pos'), 0);
  mock.setHostBuffer('pos', new Float32Array([1, 2, 3, 4]));
  const a = HostBuffer.get('pos');
  assert.ok(a instanceof Float32Array);
  assert.deepEqual(Array.from(a), [1, 2, 3, 4]);
  assert.equal(HostBuffer.get('pos'), a, 'same object for the same version');
  mock.setHostBuffer('pos', new Float32Array([5, 6, 7, 8]));
  const b = HostBuffer.get('pos');
  assert.equal(b, a, 'same storage and length: same view, new contents');
  assert.deepEqual(Array.from(b), [5, 6, 7, 8]);
  mock.setHostBuffer('pos', new Float32Array([1, 2]));
  assert.deepEqual(Array.from(HostBuffer.get('pos')), [1, 2]);
  mock.setHostBuffer('pos', new Float32Array(1000));
  assert.equal(a.length, 0, 'storage moved: old view detached');
  assert.equal(HostBuffer.get('pos').length, 1000);
  mock.setHostBuffer('ids', new Uint16Array([7, 9]));
  assert.ok(HostBuffer.get('ids') instanceof Uint16Array);
  assert.deepEqual(HostBuffer.keys().sort(), ['ids', 'pos']);
  mock.setHostBuffer('pos', undefined);
  mock.setHostBuffer('ids', undefined);
  mock.tickBuffers();
  assert.equal(HostBuffer.get('pos'), undefined);
  assert.throws(() => HostBuffer.set('pos', new Float32Array(1)), /set by the host/);
});

test('hooks: useHostBuffer re-renders per change, the selector only when its result changes, listeners never', async () => {
  const renders = { all: 0, count: 0, listen: 0 };
  const heard = [];
  const views = [];
  function All() {
    renders.all++;
    const b = useHostBuffer('enemies');
    if (b && views[views.length - 1] !== b) views.push(b);
    return h(Text, { nativeID: 'all' }, b ? Array.from(b).join(',') : 'none');
  }
  function Count() {
    renders.count++;
    const n = useHostBufferSelector('enemies', (b) => b.length / 2, 0);
    return h(Text, { nativeID: 'count' }, String(n));
  }
  function Listen() {
    renders.listen++;
    useHostBufferListener('enemies', (b) => heard.push(Array.from(b)));
    return null;
  }
  render(h(View, null, h(All), h(Count), h(Listen)));
  await wait();
  assert.equal(textOf('all'), 'none');
  assert.equal(textOf('count'), '0');

  mock.setHostBuffer('enemies', new Float32Array([1, 2, 3, 4]));
  mock.tickBuffers();
  await wait();
  assert.equal(textOf('all'), '1,2,3,4');
  assert.equal(textOf('count'), '2');
  const r1 = { ...renders };

  mock.setHostBuffer('enemies', new Float32Array([5, 6, 7, 8]));
  mock.tickBuffers();
  await wait();
  assert.equal(textOf('all'), '5,6,7,8');
  assert.equal(renders.all, r1.all + 1);
  assert.equal(renders.count, r1.count, 'same count: no re-render');
  assert.equal(renders.listen, 1, 'listeners do not re-render');
  assert.deepEqual(heard, [[1, 2, 3, 4], [5, 6, 7, 8]]);
  // A new object per version (memoized code sees the change), over the same memory.
  assert.equal(views.length, 2);
  assert.equal(views[0].buffer, views[1].buffer);

  mock.setHostBuffer('enemies', new Float32Array([1, 1, 2, 2, 3, 3]));
  mock.tickBuffers();
  await wait();
  assert.equal(textOf('count'), '3');
  render(null);
  await wait();
  mock.setHostBuffer('enemies', undefined);
  mock.tickBuffers();
});

test('Animated.hostBuffer (JS driver): follows the buffer with style patches, no React render', async () => {
  mock.setHostBuffer('marks', new Float32Array([10, 20, 0.5]));
  let renders = 0;
  function Marker() {
    renders++;
    const x = useHostBufferValue('marks', 0);
    const y = useHostBufferValue('marks', 1);
    const o = useHostBufferValue('marks', 2);
    return h(Animated.View, {
      nativeID: 'mark',
      style: { position: 'absolute', opacity: o, transform: [{ translateX: x }, { translateY: y }] },
    });
  }
  render(h(Marker));
  await wait();
  const style = () => byId('mark').props.style;
  assert.equal(style().opacity, 0.5);
  assert.deepEqual(style().transform, [{ translateX: 10 }, { translateY: 20 }]);
  mock.setHostBuffer('marks', new Float32Array([30, 40, 1]));
  mock.tickBuffers();
  await wait();
  assert.equal(style().opacity, 1);
  assert.deepEqual(style().transform, [{ translateX: 30 }, { translateY: 40 }]);
  assert.equal(renders, 1);
  // Out of range / removed: 0.
  const v = Animated.hostBuffer('marks', 99);
  assert.equal(v.__getValue(), 0);
  const seen = [];
  const id = v.addListener(({ value }) => seen.push(value));
  mock.setHostBuffer('marks', new Float32Array(100).fill(3));
  mock.tickBuffers();
  assert.deepEqual(seen, [3]);
  v.removeListener(id);
  render(null);
  await wait();
});
