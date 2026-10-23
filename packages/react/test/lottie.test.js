// LottieView against the mock bridge: what reaches native (source keys for
// inline compositions, URIs, playback props, progress as the lottieProgress
// style key, also from Animated with and without the native driver), the
// ref's play / pause / resume / reset, and the load / error / finish
// callbacks. Playback itself is native: tests/native/lottie_test.cpp.
import { mock, animated } from './mock-native-animated.js';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createElement as h, createRef } from 'react';
import { Animated, LottieView, render } from '../src/index.js';
import ShimLottieView from '../src/shims/lottie-react-native.js';

const wait = (ms = 0) => new Promise((r) => setTimeout(r, ms));
const byId = (id) => mock.find((x) => x.props.nativeID === id);
const comp = { v: '5.7.4', fr: 30, ip: 0, op: 60, w: 100, h: 50, layers: [] };

test('inline compositions go to native once, by key; playback props and style', async () => {
  mock.reset();
  render(
    h(LottieView, {
      nativeID: 'a',
      source: comp,
      autoPlay: true,
      speed: 2,
      resizeMode: 'cover',
      colorFilters: [{ keypath: 'box', color: '#f00' }],
      style: { width: 100 },
    }),
  );
  await wait();
  const n = byId('a');
  assert.equal(n.type, 'LottieView');
  assert.equal(mock.lottieData.get(n.props.source), JSON.stringify(comp));
  assert.equal(n.props.autoPlay, true);
  assert.equal(n.props.loop, true); // lottie-react-native's default
  assert.equal(n.props.speed, 2);
  assert.equal(n.props.resizeMode, 'cover');
  assert.deepEqual(n.props.colorFilters, [{ keypath: 'box', color: '#f00' }]);
  assert.deepEqual(n.props.style, { width: 100 });
  // Re-rendered with the same object: no new lottieData call.
  render(h(LottieView, { nativeID: 'a', source: comp, loop: false, style: { width: 100 } }));
  await wait();
  assert.equal(mock.count('lottieData'), 1);
  assert.equal(byId('a').props.loop, false);
  assert.equal(byId('a').props.autoPlay, undefined);
  render(null);
  await wait();
});

test('sources: {uri}, URI strings, JSON strings, ES module objects', async () => {
  const sources = [{ uri: 'https://example.com/a.json' }, 'anims/b.json', JSON.stringify(comp), { default: comp }];
  render(h('View', null, sources.map((source, i) => h(LottieView, { key: i, nativeID: 's' + i, source }))));
  await wait();
  assert.equal(byId('s0').props.source, 'https://example.com/a.json');
  assert.equal(byId('s1').props.source, 'anims/b.json');
  assert.equal(mock.lottieData.get(byId('s2').props.source), JSON.stringify(comp));
  assert.equal(mock.lottieData.get(byId('s3').props.source), JSON.stringify(comp));
  render(null);
  await wait();
});

test('progress: a number, an Animated value (JS driver) and the native driver write lottieProgress', async () => {
  render(h(LottieView, { nativeID: 'p', source: comp, progress: 0.25, style: { height: 20 } }));
  await wait();
  assert.deepEqual(byId('p').props.style, { height: 20, lottieProgress: 0.25 });
  // Animated value: patched straight into the style, no re-render.
  const v = new Animated.Value(0);
  render(h(LottieView, { nativeID: 'p', source: comp, progress: v }));
  await wait();
  assert.equal(byId('p').props.style.lottieProgress, 0);
  mock.reset();
  v.setValue(0.75);
  await wait(20);
  assert.equal(byId('p').props.style.lottieProgress, 0.75);
  assert.equal(mock.count('setProps'), 0);
  render(null);
  await wait();
  // createAnimatedComponent(LottieView) + useNativeDriver: a style node for
  // lottieProgress connected to the view.
  const AnimatedLottie = Animated.createAnimatedComponent(ShimLottieView);
  const nv = new Animated.Value(0);
  render(h(AnimatedLottie, { nativeID: 'n', source: comp, progress: nv }));
  await wait();
  animated.reset();
  Animated.timing(nv, { toValue: 1, duration: 100, useNativeDriver: true }).start();
  const style = animated.of('createNode').find(([, c]) => c.type === 'style');
  assert.ok(style, 'a native style node');
  assert.deepEqual(Object.keys(style[1].style), ['lottieProgress']);
  assert.deepEqual(animated.of('connectToView').map(([, view]) => view), [byId('n').id]);
  render(null);
  await wait();
});

test('ref: the host instance with play / pause / resume / reset', async () => {
  const ref = createRef();
  render(h(LottieView, { nativeID: 'r', ref, source: comp }));
  await wait();
  const id = byId('r').id;
  assert.equal(ref.current.id, id);
  mock.reset();
  ref.current.play(10, 20);
  ref.current.play();
  ref.current.pause();
  ref.current.resume();
  ref.current.reset();
  assert.deepEqual(
    mock.calls.filter((c) => c[0] === 'lottieCommand').map((c) => c.slice(2)),
    [
      ['play', 10, 20],
      ['play', undefined, undefined],
      ['pause', undefined, undefined],
      ['resume', undefined, undefined],
      ['reset', undefined, undefined],
    ],
  );
  render(null);
  await wait();
});

test('onAnimationLoaded / onAnimationFailure / onAnimationFinish', async () => {
  const seen = [];
  render(
    h(LottieView, {
      nativeID: 'e',
      source: comp,
      onAnimationLoaded: () => seen.push('loaded'),
      onAnimationFailure: (err) => seen.push('failed:' + err),
      onAnimationFinish: (cancelled) => seen.push('finish:' + cancelled),
    }),
  );
  await wait();
  const n = byId('e');
  for (const t of ['load', 'error', 'animationFinish']) assert.ok(n.props.__events.includes(t), t);
  mock.dispatch({ type: 'load', target: n.id });
  mock.dispatch({ type: 'error', target: n.id, error: 'bad json' });
  mock.dispatch({ type: 'animationfinish', target: n.id, isCancelled: true });
  mock.dispatch({ type: 'animationfinish', target: n.id, isCancelled: false });
  assert.deepEqual(seen, ['loaded', 'failed:bad json', 'finish:true', 'finish:false']);
  render(null);
  await wait();
});
