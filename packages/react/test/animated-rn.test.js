// Animated behaves like React Native's JS driver: easing curves, spring
// physics (tension/friction, bounciness/speed, stiffness/damping/mass),
// decay, vector / color animations, tracking, operators, interpolation
// edge cases, Animated.event through a real ScrollView, layout props.
import { mock } from './mock-native.js';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createElement as h } from 'react';
import { Animated, Easing, render, View } from '../src/index.js';
import { createInterpolation } from '../src/animated/nodes.js';
import { activeAnimationCount } from '../src/animated/animations.js';

const near = (a, b, eps = 1e-3) => assert.ok(Math.abs(a - b) < eps, `${a} != ${b} (±${eps})`);

test('Easing curves match React Native', () => {
  near(Easing.ease(0.5), 0.3153568, 1e-4);  // cubic-bezier(0.42, 0, 1, 1)
  near(Easing.inOut(Easing.ease)(0.25), Easing.ease(0.5) / 2, 1e-9);
  near(Easing.inOut(Easing.ease)(0.75), 1 - Easing.ease(0.5) / 2, 1e-9);
  near(Easing.back()(0.5), -0.0876975, 1e-6);
  near(Easing.bounce(0.5), 0.765625, 1e-6);
  near(Easing.elastic(1)(0.5), 1, 1e-6);
  near(Easing.exp(0.5), 0.03125, 1e-6);
  near(Easing.circle(0.5), 0.1339746, 1e-6);
  near(Easing.sin(0.5), 0.2928932, 1e-6);
  near(Easing.poly(4)(0.5), 0.0625, 1e-9);
  near(Easing.out(Easing.cubic)(0.5), 0.875, 1e-9);
  near(Easing.inOut(Easing.cubic)(0.25), 0.0625, 1e-9);
  assert.equal(Easing.step0(0), 0);
  assert.equal(Easing.step0(0.01), 1);
  assert.equal(Easing.step1(0.99), 0);
  assert.equal(Easing.step1(1), 1);
});

test('timing: default easeInOut, custom easing, delay', () => {
  const v = new Animated.Value(0);
  Animated.timing(v, { toValue: 100, duration: 160, useNativeDriver: true }).start();
  mock.advance(80);
  near(v.__getValue(), 100 * Easing.inOut(Easing.ease)(0.5), 1e-6);
  mock.advance(200);
  assert.equal(v.__getValue(), 100);

  const w = new Animated.Value(0);
  Animated.timing(w, { toValue: 1, duration: 100, delay: 64, easing: Easing.out(Easing.quad) }).start();
  mock.advance(64);
  assert.equal(w.__getValue(), 0, 'still delayed');
  mock.advance(48);  // 48 of 100 ms
  near(w.__getValue(), Easing.out(Easing.quad)(0.48), 1e-6);
  mock.advance(100);
  assert.equal(w.__getValue(), 1);
});

// Closed form of a damped spring from 0 to 1 at rest (RN SpringAnimation).
function springAt(k, c, m, t) {
  const zeta = c / (2 * Math.sqrt(k * m));
  const w0 = Math.sqrt(k / m);
  if (zeta < 1) {
    const w1 = w0 * Math.sqrt(1 - zeta * zeta);
    return 1 - Math.exp(-zeta * w0 * t) * ((zeta * w0 / w1) * Math.sin(w1 * t) + Math.cos(w1 * t));
  }
  return 1 - Math.exp(-w0 * t) * (1 + w0 * t);
}

test('spring: stiffness/damping/mass follow the analytic solution', () => {
  const v = new Animated.Value(0);
  Animated.spring(v, { toValue: 1, stiffness: 100, damping: 10, mass: 1, useNativeDriver: true }).start();
  mock.advance(160);
  near(v.__getValue(), springAt(100, 10, 1, 0.16), 1e-6);
  near(v.__getValue(), 0.662245, 1e-4);
  mock.advance(4000);
  assert.equal(v.__getValue(), 1);

  // Heavier mass is slower; critically damped never overshoots.
  const heavy = new Animated.Value(0);
  const crit = new Animated.Value(0);
  let peak = 0;
  crit.addListener(({ value }) => (peak = Math.max(peak, value)));
  Animated.spring(heavy, { toValue: 1, stiffness: 100, damping: 10, mass: 4 }).start();
  Animated.spring(crit, { toValue: 1, stiffness: 100, damping: 20, mass: 1 }).start();
  mock.advance(160);
  near(heavy.__getValue(), springAt(100, 10, 4, 0.16), 1e-6);
  near(crit.__getValue(), springAt(100, 20, 1, 0.16), 1e-6);
  mock.advance(4000);
  assert.ok(peak <= 1 + 1e-9, 'critically damped: no overshoot');
});

test('spring: tension/friction and bounciness/speed configs (Origami conversion)', () => {
  // Defaults: tension 40, friction 7 -> stiffness 230.2, damping 22.
  const a = new Animated.Value(0);
  const b = new Animated.Value(0);
  Animated.spring(a, { toValue: 1 }).start();
  Animated.spring(b, { toValue: 1, stiffness: 230.2, damping: 22, mass: 1 }).start();
  mock.advance(96);
  near(a.__getValue(), b.__getValue(), 1e-9);
  near(a.__getValue(), springAt(230.2, 22, 1, 0.096), 1e-9);

  // A bouncy spring overshoots its target, overshootClamping prevents it.
  const bouncy = new Animated.Value(0);
  const clamped = new Animated.Value(0);
  let peak = 0;
  let clampedPeak = 0;
  bouncy.addListener(({ value }) => (peak = Math.max(peak, value)));
  clamped.addListener(({ value }) => (clampedPeak = Math.max(clampedPeak, value)));
  let clampedDone;
  Animated.spring(bouncy, { toValue: 1, friction: 3, tension: 40 }).start();
  Animated.spring(clamped, { toValue: 1, friction: 3, tension: 40, overshootClamping: true }).start((r) => (clampedDone = r));
  mock.advance(4000);
  assert.ok(peak > 1.2, 'bouncy spring overshoots: ' + peak);
  assert.equal(clampedPeak, 1);
  assert.deepEqual(clampedDone, { finished: true });
  assert.equal(bouncy.__getValue(), 1);

  // bounciness / speed: more bounciness -> larger overshoot.
  let p1 = 0;
  let p2 = 0;
  const c1 = new Animated.Value(0);
  const c2 = new Animated.Value(0);
  c1.addListener(({ value }) => (p1 = Math.max(p1, value)));
  c2.addListener(({ value }) => (p2 = Math.max(p2, value)));
  Animated.spring(c1, { toValue: 1, bounciness: 4, speed: 12 }).start();
  Animated.spring(c2, { toValue: 1, bounciness: 18, speed: 12 }).start();
  mock.advance(4000);
  assert.ok(p2 > p1 && p1 > 1, `${p1} < ${p2}`);
  assert.equal(activeAnimationCount(), 0);
});

test('spring retargeting keeps the running velocity; initial velocity is honored', () => {
  const v = new Animated.Value(0);
  Animated.spring(v, { toValue: 100, stiffness: 200, damping: 20 }).start();
  mock.advance(64);
  const before = v.__getValue();
  mock.frame(16);
  const speed = v.__getValue() - before;
  // Retarget further away: continues moving forward smoothly (no stop).
  Animated.spring(v, { toValue: 200, stiffness: 200, damping: 20 }).start();
  const at = v.__getValue();
  mock.frame(16);
  assert.ok(v.__getValue() - at > speed * 0.8, 'momentum carried into the new spring');
  mock.advance(4000);
  assert.equal(v.__getValue(), 200);

  const w = new Animated.Value(0);
  Animated.spring(w, { toValue: 0, velocity: 500, stiffness: 100, damping: 10 }).start();  // units/s
  mock.frame(16);
  assert.ok(w.__getValue() > 5, 'kicked forward by the initial velocity: ' + w.__getValue());
  mock.advance(4000);
  assert.equal(w.__getValue(), 0);
});

test('decay: value, ValueXY with {x, y} velocity', () => {
  const v = new Animated.Value(10);
  let done;
  Animated.decay(v, { velocity: 1, deceleration: 0.998, useNativeDriver: true }).start((r) => (done = r));
  mock.advance(160);
  near(v.__getValue(), 10 + (1 / 0.002) * (1 - Math.exp(-0.002 * 160)), 1e-6);
  mock.advance(10000);
  assert.deepEqual(done, { finished: true });
  // Ends once a frame moves < 0.1 (RN), just short of from + v / (1 - deceleration).
  assert.ok(v.__getValue() > 505 && v.__getValue() < 510, String(v.__getValue()));

  const xy = new Animated.ValueXY({ x: 0, y: 0 });
  Animated.decay(xy, { velocity: { x: 1, y: -0.5 }, deceleration: 0.99 }).start();
  mock.advance(3000);
  near(xy.x.__getValue(), 100, 1);
  near(xy.y.__getValue(), -50, 1);
});

test('ValueXY spring / timing to {x, y}; Animated.Color timing', () => {
  const xy = new Animated.ValueXY();
  Animated.timing(xy, { toValue: { x: 100, y: -40 }, duration: 100, easing: Easing.linear }).start();
  mock.advance(48);
  near(xy.x.__getValue(), 48, 1e-6);
  near(xy.y.__getValue(), -19.2, 1e-6);
  mock.advance(100);
  assert.deepEqual(xy.__getValue(), { x: 100, y: -40 });

  const c = new Animated.Color('red');
  render(h(Animated.View, { nativeID: 'col', style: { backgroundColor: c } }));
  const node = mock.find((n) => n.props.nativeID === 'col');
  assert.equal(node.props.style.backgroundColor, 'rgba(255, 0, 0, 1)');
  Animated.timing(c, { toValue: 'rgba(0, 0, 255, 0.5)', duration: 100, easing: Easing.linear }).start();
  mock.advance(48);
  mock.frame(2);  // 50 ms
  assert.equal(node.props.style.backgroundColor, 'rgba(128, 0, 128, 0.75)');
  mock.advance(100);
  assert.equal(node.props.style.backgroundColor, 'rgba(0, 0, 255, 0.5)');
  c.setValue({ r: 10, g: 20, b: 30, a: 1 });
  assert.equal(node.props.style.backgroundColor, 'rgba(10, 20, 30, 1)');
  render(null);
});

test('tracking: toValue bound to another Animated.Value', () => {
  const leader = new Animated.Value(0);
  const follower = new Animated.Value(0);
  Animated.spring(follower, { toValue: leader, stiffness: 300, damping: 30 }).start();
  leader.setValue(50);
  mock.advance(2000);
  near(follower.__getValue(), 50, 1e-3);
  leader.setValue(-20);
  mock.advance(2000);
  near(follower.__getValue(), -20, 1e-3);
  follower.stopAnimation();
  leader.setValue(100);
  mock.advance(500);
  near(follower.__getValue(), -20, 1e-3, 'stopAnimation stops tracking');
});

test('stopAnimation / resetAnimation / reset() callbacks and values', () => {
  const v = new Animated.Value(5);
  const anim = Animated.timing(v, { toValue: 15, duration: 160, easing: Easing.linear });
  let result;
  anim.start((r) => (result = r));
  mock.advance(80);
  let stoppedAt;
  v.stopAnimation((value) => (stoppedAt = value));
  near(stoppedAt, 10, 1e-6);
  assert.deepEqual(result, { finished: false });
  mock.advance(100);
  near(v.__getValue(), 10, 1e-6, 'stays where it stopped');
  let resetTo;
  v.resetAnimation((value) => (resetTo = value));
  near(resetTo, 10, 1e-6, 'callback gets the value before the reset (RN)');
  assert.equal(v.__getValue(), 5, 'back to the initial value');
  anim.start();
  mock.advance(80);
  anim.reset();
  assert.equal(v.__getValue(), 5);
  assert.equal(activeAnimationCount(), 0);
});

test('composition: sequence with delay, parallel stopTogether, stagger offsets, loop resets', () => {
  const a = new Animated.Value(0);
  const b = new Animated.Value(0);
  Animated.sequence([Animated.timing(a, { toValue: 1, duration: 48 }), Animated.delay(96), Animated.timing(b, { toValue: 1, duration: 48 })]).start();
  mock.advance(128);
  assert.equal(a.__getValue(), 1);
  assert.equal(b.__getValue(), 0, 'delay holds the next step');
  mock.advance(96);
  assert.equal(b.__getValue(), 1);

  const c = new Animated.Value(0);
  const d = new Animated.Value(0);
  let res;
  const par = Animated.parallel([Animated.timing(c, { toValue: 1, duration: 500 }), Animated.timing(d, { toValue: 1, duration: 500 })]);
  par.start((r) => (res = r));
  mock.advance(64);
  c.stopAnimation();
  assert.deepEqual(res, { finished: false });
  const dAt = d.__getValue();
  mock.advance(64);
  assert.equal(d.__getValue(), dAt, 'stopTogether stopped the sibling');

  const vals = [0, 1, 2].map(() => new Animated.Value(0));
  Animated.stagger(100, vals.map((v) => Animated.timing(v, { toValue: 1, duration: 50, easing: Easing.linear }))).start();
  mock.advance(128);
  assert.deepEqual(vals.map((v) => v.__getValue() > 0), [true, true, false]);
  mock.advance(400);
  assert.deepEqual(vals.map((v) => v.__getValue()), [1, 1, 1]);

  // Infinite loop keeps scheduling frames until stopped.
  const e = new Animated.Value(0);
  const lp = Animated.loop(Animated.sequence([Animated.timing(e, { toValue: 1, duration: 100, easing: Easing.linear }), Animated.timing(e, { toValue: 0, duration: 100, easing: Easing.linear })]));
  lp.start();
  mock.advance(1000);
  assert.ok(activeAnimationCount() > 0);
  mock.advance(48);
  mock.frame(2);  // 1050 ms: 50 ms into an iteration
  near(e.__getValue(), 0.5, 1e-6);
  lp.stop();
  assert.equal(activeAnimationCount(), 0);
});

test('interpolate: extrapolate modes, multi-segment, easing, colors, rotation strings', () => {
  const f = createInterpolation({ inputRange: [0, 100], outputRange: [0, 1], extrapolateLeft: 'clamp', extrapolateRight: 'identity' });
  assert.equal(f(-50), 0);
  assert.equal(f(250), 250);
  const g = createInterpolation({ inputRange: [0, 100], outputRange: [0, 1] });
  near(g(-50), -0.5, 1e-9);
  near(g(150), 1.5, 1e-9);
  const seg = createInterpolation({ inputRange: [-1, 0, 1, 2], outputRange: [10, 0, 0, 100], easing: Easing.quad });
  near(seg(-0.5), 7.5, 1e-9);
  assert.equal(seg(0.5), 0);
  near(seg(1.5), 25, 1e-9);
  const col = createInterpolation({ inputRange: [0, 1, 2], outputRange: ['#ff0000', 'rgb(0, 255, 0)', 'hsla(240, 100%, 50%, 0.5)'] });
  assert.equal(col(0.5), 'rgba(128, 128, 0, 1)');
  assert.equal(col(1.5), 'rgba(0, 128, 128, 0.75)');
  const rot = createInterpolation({ inputRange: [0, 1], outputRange: ['-45deg', '45deg'] });
  assert.equal(rot(0.75), '22.5deg');
  const rad = createInterpolation({ inputRange: [0, 1], outputRange: ['0rad', '3.14rad'] });
  assert.equal(rad(0.5), '1.57rad');
  // Chained interpolation of an operator node.
  const v = new Animated.Value(2);
  const chained = Animated.multiply(v, 10).interpolate({ inputRange: [0, 40], outputRange: ['0deg', '360deg'] });
  assert.equal(chained.__getValue(), '180deg');
});

test('operators: add, subtract, multiply, divide, modulo, diffClamp', () => {
  const a = new Animated.Value(6);
  const b = new Animated.Value(4);
  assert.equal(Animated.add(a, b).__getValue(), 10);
  assert.equal(Animated.subtract(a, b).__getValue(), 2);
  assert.equal(Animated.multiply(a, 2).__getValue(), 12);
  assert.equal(Animated.divide(a, b).__getValue(), 1.5);
  assert.equal(Animated.modulo(new Animated.Value(-1), 4).__getValue(), 3);

  // diffClamp (collapsible header): follows deltas, clamped.
  const scrollY = new Animated.Value(0);
  const clamp = Animated.diffClamp(scrollY, 0, 50);
  const seen = [];
  const translate = clamp.interpolate({ inputRange: [0, 50], outputRange: [0, -50] });
  render(h(Animated.View, { nativeID: 'hdr', style: { transform: [{ translateY: translate }] } }));
  const node = mock.find((n) => n.props.nativeID === 'hdr');
  for (const y of [30, 100, 80, 0, 20]) {
    scrollY.setValue(y);
    seen.push(node.props.style.transform[0].translateY);
  }
  assert.deepEqual(seen, [-30, -50, -30, 0, -20]);
  render(null);
});

test('Animated.event on Animated.ScrollView onScroll drives an interpolated header', () => {
  const scrollY = new Animated.Value(0);
  const listened = [];
  function App() {
    return h(
      View,
      { style: { flex: 1 } },
      h(Animated.View, {
        nativeID: 'header',
        style: {
          opacity: scrollY.interpolate({ inputRange: [0, 100], outputRange: [1, 0], extrapolate: 'clamp' }),
          height: scrollY.interpolate({ inputRange: [0, 100], outputRange: [120, 60], extrapolate: 'clamp' }),
        },
      }),
      h(
        Animated.ScrollView,
        {
          nativeID: 'sv',
          scrollEventThrottle: 16,
          onScroll: Animated.event([{ nativeEvent: { contentOffset: { y: scrollY } } }], {
            useNativeDriver: true,
            listener: (e) => listened.push(e.nativeEvent.contentOffset.y),
          }),
        },
        h(View, { style: { height: 2000 } }),
      ),
    );
  }
  render(h(App));
  const sv = mock.find((n) => n.type === 'ScrollView');
  const header = mock.find((n) => n.props.nativeID === 'header');
  assert.equal(header.props.style.height, 120);
  mock.reset();
  mock.dispatch({ type: 'scroll', target: sv.id, contentOffset: { x: 0, y: 50 }, contentSize: { width: 100, height: 2000 }, layoutMeasurement: { width: 100, height: 300 } });
  assert.equal(scrollY.__getValue(), 50);
  assert.deepEqual(listened, [50]);
  assert.equal(header.props.style.opacity, 0.5);
  assert.equal(header.props.style.height, 90, 'layout prop (height) patched');
  assert.equal(mock.count('setProps'), 0, 'no React re-render');
  mock.dispatch({ type: 'scroll', target: sv.id, contentOffset: { x: 0, y: 400 }, contentSize: { width: 100, height: 2000 }, layoutMeasurement: { width: 100, height: 300 } });
  assert.equal(header.props.style.opacity, 0);
  assert.equal(header.props.style.height, 60);
  render(null);
});

test('transform, opacity, width/height animate through patchStyle', () => {
  const p = new Animated.Value(0);
  render(
    h(Animated.View, {
      nativeID: 'box',
      style: {
        width: p.interpolate({ inputRange: [0, 1], outputRange: [100, 300] }),
        height: 40,
        opacity: p,
        transform: [{ translateX: Animated.multiply(p, 50) }, { scale: p.interpolate({ inputRange: [0, 1], outputRange: [0.5, 1] }) }, { rotate: p.interpolate({ inputRange: [0, 1], outputRange: ['0deg', '90deg'] }) }],
      },
    }),
  );
  const node = mock.find((n) => n.props.nativeID === 'box');
  mock.reset();
  Animated.timing(p, { toValue: 1, duration: 100, easing: Easing.linear, useNativeDriver: false }).start();
  mock.advance(48);
  mock.frame(2);
  const s = node.props.style;
  assert.equal(s.width, 200);
  assert.equal(s.height, 40);
  assert.equal(s.opacity, 0.5);
  assert.deepEqual(s.transform, [{ translateX: 25 }, { scale: 0.75 }, { rotate: '45deg' }]);
  const patch = mock.calls.at(-1)[2];
  assert.deepEqual(Object.keys(patch).sort(), ['opacity', 'transform', 'width'], 'only animated keys are patched');
  mock.advance(100);
  render(null);
});
