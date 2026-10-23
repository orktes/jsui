// Animated with useNativeDriver: true against a recording native module:
// what @orktes/jsui-react serializes (nodes, animations, event mappings), the end /
// stop / listener / setValue sync, React Native's errors, and that nothing
// runs on the JS frame loop. The runtime side is tested natively
// (tests/native/native_animated_test.cpp, react_native_animated_e2e_test.cpp).
import { mock, animated } from './mock-native-animated.js';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createElement as h } from 'react';
import { Animated, Easing, render, View } from '../src/index.js';
import { activeAnimationCount } from '../src/animated/animations.js';

const near = (a, b, eps = 1e-9) => assert.ok(Math.abs(a - b) < eps, `${a} != ${b} (±${eps})`);
const lastStart = () => animated.of('startAnimation').at(-1);
const config = (tag) => animated.nodes.get(tag).config;

test('timing: graph value -> style -> props connected to the view; sampled easing; no JS frames', () => {
  const v = new Animated.Value(0.25);
  render(h(Animated.View, { nativeID: 'box', style: { width: 10, opacity: v } }));
  const box = mock.find((n) => n.props.nativeID === 'box');
  assert.equal(box.props.style.opacity, 0.25);
  animated.reset();
  const results = [];
  Animated.timing(v, { toValue: 1, duration: 100, delay: 20, easing: Easing.out(Easing.quad), useNativeDriver: true }).start((r) =>
    results.push(r),
  );
  // value, style, props nodes; props connected to the host view.
  const created = animated.of('createNode');
  const types = created.map(([, c]) => c.type);
  assert.deepEqual(types, ['value', 'style', 'props']);
  assert.deepEqual(created[0][1], { type: 'value', value: 0.25, offset: 0 });
  const [styleTag, styleConfig] = created[1];
  assert.deepEqual(styleConfig, { type: 'style', style: { opacity: created[0][0] } });
  assert.deepEqual(created[2][1], { type: 'props', props: { style: styleTag } });
  assert.deepEqual(animated.of('connectToView'), [[created[2][0], box.id]]);
  // The animation: frames = easing sampled every 2 ms (51 samples over 100 ms).
  const [id, tag, cfg] = lastStart();
  assert.equal(tag, created[0][0]);
  assert.equal(cfg.type, 'frames');
  assert.equal(cfg.frames.length, 51);
  near(cfg.frames[25], Easing.out(Easing.quad)(0.5));
  assert.equal(cfg.frames[50], 1);
  assert.deepEqual([cfg.duration, cfg.toValue, cfg.delay, cfg.iterations], [100, 1, 20, 1]);
  // Nothing on the JS frame loop, no style patches from JS.
  assert.equal(activeAnimationCount(), 0);
  mock.reset();
  mock.advance(200);
  assert.equal(mock.count('patchStyle') + mock.count('setProps'), 0);
  assert.deepEqual(results, []);
  // The runtime ends it: {finished, value}; the JS value is synced.
  animated.end(id, true, 1);
  assert.deepEqual(results, [{ finished: true, value: 1 }]);
  assert.equal(v.__getValue(), 1);
  render(null);
  assert.ok(animated.count('disconnectFromView') >= 1);
});

test('spring configs are converted like the JS driver; stop syncs and continues the velocity', () => {
  const v = new Animated.Value(0);
  Animated.spring(v, { toValue: 10, tension: 40, friction: 7, useNativeDriver: true }).start();
  let cfg = lastStart()[2];
  near(cfg.stiffness, (40 - 30) * 3.62 + 194);
  near(cfg.damping, (7 - 8) * 3 + 25);
  assert.equal(cfg.mass, 1);
  assert.equal(cfg.toValue, 10);
  assert.equal(cfg.restDisplacementThreshold, 0.001);
  // Retarget while running: the stopped spring's native velocity carries over.
  const first = lastStart()[0];
  const tag = lastStart()[1];
  animated.nodes.get(tag).value = 4;
  animated.stopVelocity = 12.5;
  const ends = [];
  Animated.spring(v, { toValue: 0, stiffness: 300, damping: 20, mass: 2, useNativeDriver: true }).start((r) => ends.push(r));
  cfg = lastStart()[2];
  assert.deepEqual(animated.of('stopAnimation').at(-1), [first]);
  assert.equal(cfg.initialVelocity, 12.5);
  assert.deepEqual([cfg.stiffness, cfg.damping, cfg.mass], [300, 20, 2]);
  // stopAnimation(cb): stops natively and reports the native value.
  animated.nodes.get(tag).value = 2.5;
  let stoppedAt = null;
  v.stopAnimation((x) => (stoppedAt = x));
  assert.equal(stoppedAt, 2.5);
  assert.deepEqual(ends, [{ finished: false, value: 2.5 }]);
  animated.stopVelocity = 0;

  Animated.spring(v, { toValue: 1, bounciness: 12, speed: 8, velocity: 3, overshootClamping: true, useNativeDriver: true }).start();
  cfg = lastStart()[2];
  assert.equal(cfg.initialVelocity, 3);
  assert.equal(cfg.overshootClamping, true);
  assert.ok(cfg.stiffness > 0 && cfg.damping > 0);
  Animated.decay(v, { velocity: 0.5, deceleration: 0.99, useNativeDriver: true }).start();
  assert.deepEqual(lastStart()[2], { type: 'decay', velocity: 0.5, deceleration: 0.99, delay: 0, iterations: 1 });
  v.stopAnimation();
});

test('loops run natively (iterations); sequences chain native animations', () => {
  const v = new Animated.Value(0);
  const done = [];
  Animated.loop(Animated.timing(v, { toValue: 1, duration: 50, useNativeDriver: true }), { iterations: 3 }).start((r) => done.push(r));
  const [id, , cfg] = lastStart();
  assert.equal(cfg.iterations, 3);
  assert.equal(animated.count('startAnimation') > 0, true);
  const before = animated.count('startAnimation');
  animated.end(id, true, 1);
  assert.deepEqual(done, [{ finished: true, value: 1 }]);
  assert.equal(animated.count('startAnimation'), before, 'no JS restarts');
  const forever = Animated.loop(Animated.timing(v, { toValue: 0, duration: 50, useNativeDriver: true }));
  forever.start((r) => done.push(r));
  assert.equal(lastStart()[2].iterations, -1);
  forever.stop();
  assert.equal(done.at(-1).finished, false);

  const w = new Animated.Value(0);
  const seq = [];
  Animated.sequence([
    Animated.timing(w, { toValue: 5, duration: 10, useNativeDriver: true }),
    Animated.timing(w, { toValue: 2, duration: 10, useNativeDriver: true }),
  ]).start((r) => seq.push(r.finished));
  const a1 = lastStart();
  assert.equal(a1[2].toValue, 5);
  animated.end(a1[0], true, 5);
  const a2 = lastStart();
  assert.equal(a2[2].toValue, 2);
  animated.end(a2[0], true, 2);
  assert.deepEqual(seq, [true]);
});

test('loop(sequence([timing, zero-duration reset])) repeats; reset restores the native value (RN)', () => {
  const v = new Animated.Value(0);
  const l = Animated.loop(
    Animated.sequence([
      Animated.timing(v, { toValue: 1, duration: 800, useNativeDriver: true }),
      Animated.timing(v, { toValue: 0, duration: 0, useNativeDriver: true }),
    ]),
  );
  l.start();
  for (let i = 0; i < 3; i++) {
    const up = lastStart();
    assert.equal(up[2].toValue, 1);
    assert.equal(up[2].duration, 800);
    animated.end(up[0], true, 1);
    const reset = lastStart();
    assert.equal(reset[2].toValue, 0);
    assert.equal(reset[2].duration, 0);
    animated.end(reset[0], true, 0);
  }
  assert.equal(lastStart()[2].toValue, 1, 'the loop keeps going');
  l.stop();

  // resetAnimation writes the starting value to the native node too.
  const w = new Animated.Value(0.5);
  Animated.timing(w, { toValue: 1, duration: 100, useNativeDriver: true }).start();
  animated.end(lastStart()[0], true, 1);
  assert.equal(w.__getValue(), 1);
  animated.reset();
  w.resetAnimation();
  assert.deepEqual(animated.of('setValue'), [[w.__getNativeTag(), 0.5]]);
  assert.equal(w.__getValue(), 0.5);
});

test('interpolations, math nodes, colors and transforms serialize for the native evaluator', () => {
  const v = new Animated.Value(0);
  const color = new Animated.Color('rgba(10, 20, 30, 0.5)');
  render(
    h(Animated.View, {
      nativeID: 'n',
      style: {
        backgroundColor: v.interpolate({ inputRange: [0, 1], outputRange: ['red', '#0000ff80'] }),
        borderColor: color,
        opacity: Animated.diffClamp(Animated.add(v, 1), 0, 1),
        transform: [
          { rotate: v.interpolate({ inputRange: [0, 1], outputRange: ['0deg', '90deg'], extrapolate: 'clamp' }) },
          { scale: 2 },
          { translateX: Animated.modulo(Animated.multiply(v, 3), 2) },
          { translateY: v.interpolate({ inputRange: [0, 1], outputRange: [0, 10], easing: Easing.quad, extrapolateLeft: 'identity' }) },
        ],
      },
    }),
  );
  animated.reset();
  Animated.timing(v, { toValue: 1, duration: 10, useNativeDriver: true }).start();
  const byType = (t) => animated.of('createNode').filter(([, c]) => c.type === t).map(([, c]) => c);
  const interps = byType('interpolation');
  const colorI = interps.find((c) => c.outputType === 'color');
  assert.deepEqual(colorI.outputs, [[255, 0], [0, 0], [0, 255], [1, 128 / 255]]);
  const deg = interps.find((c) => c.outputType === 'string');
  assert.deepEqual(deg.parts, ['', 'deg']);
  assert.deepEqual(deg.outputs, [[0, 90]]);
  assert.equal(deg.extrapolateRight, 'clamp');
  const eased = interps.find((c) => c.easing);
  assert.equal(eased.easing.length, 257);
  near(eased.easing[128], 0.25);
  assert.equal(eased.extrapolateLeft, 'identity');
  assert.equal(eased.extrapolateRight, 'extend');
  assert.equal(byType('addition').length, 1);
  assert.equal(byType('multiplication').length, 1);
  assert.equal(byType('modulus')[0].modulus, 2);
  const dc = byType('diffclamp')[0];
  assert.deepEqual([dc.min, dc.max, dc.last, dc.value], [0, 1, 1, 1]);
  assert.equal(byType('color').length, 1, 'Animated.Color -> color node of four values');
  const tr = byType('transform')[0];
  assert.deepEqual(
    tr.transforms.map((t) => (t.nodeTag ? t.property + '*' : t.property + '=' + t.value)),
    ['rotate*', 'scale=2', 'translateX*', 'translateY*'],
  );
  assert.deepEqual(Object.keys(byType('style')[0].style).sort(), ['backgroundColor', 'borderColor', 'opacity', 'transform']);
  // Every value the props read became native (the color's channels too).
  assert.ok(color.r.__isNative() && color.a.__isNative());
  render(null);
});

test('React Native errors: layout props, JS-driven animations on native values, non-style props', () => {
  const w = new Animated.Value(10);
  render(h(Animated.View, { style: { width: w } }));
  assert.throws(
    () => Animated.timing(w, { toValue: 20, duration: 10, useNativeDriver: true }).start(),
    /Style property 'width' is not supported by native animated module/,
  );
  render(null);
  const t = new Animated.Value(0);
  render(h(Animated.View, { style: { transform: [{ translateX: t }] } }));
  Animated.timing(t, { toValue: 20, duration: 10, useNativeDriver: true }).start();
  assert.throws(
    () => Animated.timing(t, { toValue: 0, duration: 10, useNativeDriver: false }).start(),
    /moved to "native" earlier/,
  );
  t.stopAnimation();
  render(null);
  const p = new Animated.Value(0);
  render(h(Animated.View, { accessibilityValue: { now: p } }));
  assert.throws(() => Animated.timing(p, { toValue: 1, duration: 10, useNativeDriver: true }).start(), /only style props/);
  render(null);
});

test('setValue / offsets / listeners sync with the native value', () => {
  const v = new Animated.Value(0);
  render(h(Animated.View, { nativeID: 'x', style: { opacity: v } }));
  Animated.timing(v, { toValue: 1, duration: 100, useNativeDriver: true }).start();
  const tag = lastStart()[1];
  const seen = [];
  v.addListener(({ value }) => seen.push(value));
  assert.deepEqual(animated.of('startListening').at(-1), [tag]);
  animated.frame(tag, 0.4);  // the runtime moved it
  assert.deepEqual(seen, [0.4]);
  assert.equal(v.__getValue(), 0.4);
  animated.reset();
  v.setValue(0.75);
  assert.deepEqual(animated.of('setValue'), [[tag, 0.75]]);
  assert.equal(v.__getValue(), 0.75);
  v.setOffset(0.1);
  assert.deepEqual(animated.of('setOffset'), [[tag, 0.1]]);
  near(v.__getValue(), 0.85);
  v.flattenOffset();
  assert.equal(animated.count('flattenOffset'), 1);
  near(v.__getValue(), 0.85);
  v.removeAllListeners();
  assert.deepEqual(animated.of('stopListening').at(-1), [tag]);
  render(null);
});

test('Animated.event with useNativeDriver maps nativeEvent paths natively', () => {
  const y = new Animated.Value(0);
  const xy = new Animated.ValueXY();
  const heard = [];
  animated.reset();
  render(
    h(
      View,
      null,
      h(Animated.ScrollView, {
        nativeID: 'sv',
        onScroll: Animated.event([{ nativeEvent: { contentOffset: { y } } }], { useNativeDriver: true }),
        onMomentumScrollEnd: Animated.event([{ nativeEvent: { contentOffset: xy } }], {
          useNativeDriver: true,
          listener: (e) => heard.push(e.nativeEvent.contentOffset.y),
        }),
      }, h(View, { style: { height: 1000 } })),
      h(Animated.View, { style: { transform: [{ translateY: y }] } }),
    ),
  );
  const sv = mock.find((n) => n.type === 'ScrollView');
  const maps = animated.of('addEventMapping');
  assert.deepEqual(
    maps.map(([view, type, path]) => [view === sv.id, type, path.join('.')]),
    [
      [true, 'scroll', 'contentOffset.y'],
      [true, 'momentumscrollend', 'contentOffset.x'],
      [true, 'momentumscrollend', 'contentOffset.y'],
    ],
  );
  assert.ok(y.__isNative() && xy.x.__isNative());
  // JS no longer applies the mapping; only the listener runs.
  mock.dispatch({ type: 'momentumscrollend', target: sv.id, contentOffset: { x: 0, y: 70 } });
  assert.deepEqual(heard, [70]);
  assert.equal(xy.y.__getValue(), 0);
  render(null);
  assert.equal(animated.count('removeEventMapping'), 3);
});

test('Animated.hostBuffer: native from the start; the runtime reads the buffer (hostBuffer node -> transform -> view)', () => {
  animated.reset();
  const x = Animated.hostBuffer('enemies', 4);
  const o = Animated.hostBuffer('enemies', 7).interpolate({ inputRange: [0, 1], outputRange: [0.2, 1] });
  render(h(Animated.View, { nativeID: 'marker', style: { opacity: o, transform: [{ translateX: x }, { scale: 2 }] } }));
  const created = animated.of('createNode').map(([, c]) => c);
  const buf = created.filter((c) => c.type === 'hostBuffer');
  assert.deepEqual(buf, [
    { type: 'hostBuffer', key: 'enemies', index: 7 },
    { type: 'hostBuffer', key: 'enemies', index: 4 },
  ]);
  const transform = created.find((c) => c.type === 'transform');
  assert.equal(transform.transforms[0].property, 'translateX');
  assert.deepEqual(transform.transforms[1], { property: 'scale', value: 2 });
  assert.equal(animated.count('connectToView'), 1);
  assert.equal(animated.count('startAnimation'), 0);
  render(null);
});
