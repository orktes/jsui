import { mock } from './mock-native.js';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createElement as h, useEffect } from 'react';
import { Animated, Easing, render, StyleSheet, ActivityIndicator } from '../src/index.js';
import { createInterpolation } from '../src/animated/nodes.js';
import { activeAnimationCount } from '../src/animated/animations.js';

const near = (a, b, eps = 1e-3) => assert.ok(Math.abs(a - b) < eps, `${a} != ${b}`);

test('interpolation: numbers, clamp, colors, strings', () => {
  const f = createInterpolation({ inputRange: [0, 1, 2], outputRange: [0, 10, 0] });
  near(f(0.5), 5);
  near(f(1.5), 5);
  near(f(3), -10);
  const c = createInterpolation({ inputRange: [0, 1], outputRange: [0, 10], extrapolate: 'clamp' });
  near(c(5), 10);
  const col = createInterpolation({ inputRange: [0, 1], outputRange: ['#000000', 'rgba(255, 0, 0, 0.5)'] });
  assert.equal(col(0.5), 'rgba(128, 0, 0, 0.75)');
  const deg = createInterpolation({ inputRange: [0, 1], outputRange: ['0deg', '360deg'] });
  assert.equal(deg(0.25), '90deg');
  near(Easing.bezier(0.25, 0.1, 0.25, 1)(0.5), 0.8024, 1e-3);
  near(Easing.inOut(Easing.quad)(0.5), 0.5);
});

test('timing drives patchStyle on the host node without React renders', () => {
  const v = new Animated.Value(0);
  let renders = 0;
  function App() {
    renders++;
    return h(Animated.View, {
      nativeID: 'a',
      style: [StyleSheet.absoluteFill, { opacity: v, transform: [{ translateX: v.interpolate({ inputRange: [0, 1], outputRange: [0, 100] }) }, { rotate: '45deg' }] }],
    });
  }
  render(h(App));
  const node = mock.find((n) => n.props.nativeID === 'a');
  assert.equal(node.props.style.opacity, 0);
  assert.deepEqual(node.props.style.transform, [{ translateX: 0 }, { rotate: '45deg' }]);
  mock.reset();
  let done = null;
  Animated.timing(v, { toValue: 1, duration: 160, easing: Easing.linear, useNativeDriver: true }).start((r) => (done = r));
  mock.frame(80);
  near(node.props.style.opacity, 0.5);
  near(node.props.style.transform[0].translateX, 50);
  assert.equal(node.props.style.position, 'absolute', 'non-animated keys kept');
  mock.advance(200);
  assert.deepEqual(done, { finished: true });
  assert.equal(node.props.style.opacity, 1);
  assert.equal(mock.count('setProps'), 0);
  // One patch per frame for both animated keys.
  assert.ok(mock.calls.every((c) => c[0] === 'patchStyle'));
  assert.equal(renders, 1);
  assert.equal(activeAnimationCount(), 0);
  render(null);
});

test('spring settles on target; sequence/parallel/loop/stagger complete', () => {
  const v = new Animated.Value(0);
  let r1;
  Animated.spring(v, { toValue: 10, useNativeDriver: false }).start((r) => (r1 = r));
  mock.advance(3000);
  assert.deepEqual(r1, { finished: true });
  assert.equal(v.__getValue(), 10);

  const a = new Animated.Value(0);
  const b = new Animated.Value(0);
  const order = [];
  a.addListener(({ value }) => value === 1 && order.push('a'));
  b.addListener(({ value }) => value === 1 && order.push('b'));
  let seqDone;
  Animated.sequence([Animated.timing(a, { toValue: 1, duration: 50 }), Animated.delay(30), Animated.timing(b, { toValue: 1, duration: 50 })]).start((r) => (seqDone = r));
  mock.advance(300);
  assert.deepEqual(order, ['a', 'b']);
  assert.deepEqual(seqDone, { finished: true });

  const c = new Animated.Value(0);
  const d = new Animated.Value(0);
  let parDone;
  Animated.parallel([Animated.timing(c, { toValue: 1, duration: 50 }), Animated.spring(d, { toValue: 1 })]).start((r) => (parDone = r));
  mock.advance(3000);
  assert.deepEqual(parDone, { finished: true });

  const e = new Animated.Value(0);
  let iterations = 0;
  e.addListener(({ value }) => value === 1 && iterations++);
  let loopDone;
  Animated.loop(Animated.timing(e, { toValue: 1, duration: 32, easing: Easing.linear }), { iterations: 3 }).start((r) => (loopDone = r));
  mock.advance(400);
  assert.equal(iterations, 3);
  assert.deepEqual(loopDone, { finished: true });

  const vals = [new Animated.Value(0), new Animated.Value(0)];
  let stDone;
  Animated.stagger(50, vals.map((x) => Animated.timing(x, { toValue: 1, duration: 50 }))).start((r) => (stDone = r));
  mock.frame(60);
  assert.ok(vals[0].__getValue() > 0 && vals[1].__getValue() === 0);
  mock.advance(200);
  assert.deepEqual(stDone, { finished: true });
});

test('stop() reports finished:false; setValue patches immediately', () => {
  const v = new Animated.Value(0);
  render(h(Animated.View, { nativeID: 's', style: { opacity: v } }));
  const node = mock.find((n) => n.props.nativeID === 's');
  let res;
  const anim = Animated.timing(v, { toValue: 1, duration: 1000 });
  anim.start((r) => (res = r));
  mock.frame(100);
  anim.stop();
  assert.deepEqual(res, { finished: false });
  v.setValue(0.25);
  assert.equal(node.props.style.opacity, 0.25);
  render(null);
});

test('Animated.event maps scroll offsets to values', () => {
  const y = new Animated.Value(0);
  const seen = [];
  const handler = Animated.event([{ nativeEvent: { contentOffset: { y } } }], { useNativeDriver: true, listener: (e) => seen.push(e.nativeEvent.contentOffset.y) });
  handler({ nativeEvent: { contentOffset: { x: 0, y: 42 } } });
  assert.equal(y.__getValue(), 42);
  assert.deepEqual(seen, [42]);
});

test('ValueXY layout + translate, Animated.add, unmount detaches', () => {
  const xy = new Animated.ValueXY({ x: 1, y: 2 });
  const sum = Animated.add(xy.x, xy.y);
  function App() {
    return h(Animated.View, { nativeID: 'xy', style: { ...xy.getLayout(), width: sum } });
  }
  render(h(App));
  const node = mock.find((n) => n.props.nativeID === 'xy');
  assert.deepEqual([node.props.style.left, node.props.style.top, node.props.style.width], [1, 2, 3]);
  xy.setValue({ x: 10, y: 20 });
  assert.deepEqual([node.props.style.left, node.props.style.top, node.props.style.width], [10, 20, 30]);
  render(null);
  assert.equal(xy.x.__getChildren().length, 0, 'subscription removed');
  assert.deepEqual(xy.getTranslateTransform().map((t) => Object.keys(t)[0]), ['translateX', 'translateY']);
});

test('ActivityIndicator spins with patchStyle only', () => {
  render(h(ActivityIndicator, { size: 'large', color: 'white' }));
  mock.reset();
  mock.advance(400);
  assert.equal(mock.count('setProps'), 0);
  assert.ok(mock.count('patchStyle') >= 20);
  const rot = mock.calls.at(-1)[2].transform[0].rotate;
  assert.match(rot, /^\d+(\.\d+)?deg$/);
  render(null);
  assert.equal(activeAnimationCount(), 0, 'loop stopped on unmount');
});
