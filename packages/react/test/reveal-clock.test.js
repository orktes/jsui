// <Text reveal> (typewriter) and Animated.clock({...}) on the JS driver (no
// native animated module): the reveal props become the textReveal* style
// keys, an animated reveal is patched without React renders, and a
// controllable clock counts, pauses, resumes, resets, changes speed and is
// held by a `running` node, frame for frame what the native driver does
// (native-clock-reveal.test.js, tests/native/native_animated_test.cpp).
import { mock } from './mock-native.js';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createElement as h } from 'react';
import { Animated, Easing, render, Text } from '../src/index.js';

const near = (a, b, eps = 1e-9) => assert.ok(Math.abs(a - b) < eps, `${a} != ${b} (±${eps})`);

test('Text reveal props become the textReveal* style keys', () => {
  render(
    h(Text, { nativeID: 't', reveal: 3, revealMode: 'words', revealFade: 2, revealFraction: false, style: { color: 'red' } }, 'Hello ', h(Text, { style: { color: 'blue' } }, 'world')),
  );
  const t = mock.find((n) => n.props.nativeID === 't');
  assert.equal(t.props.style.color, 'red');
  assert.equal(t.props.style.textReveal, 3);
  assert.equal(t.props.style.textRevealMode, 'words');
  assert.equal(t.props.style.textRevealFade, 2);
  assert.equal(t.props.style.textRevealFraction, undefined);
  assert.equal(t.props.reveal, undefined);
  assert.equal(mock.text(t.id), 'Hello world');
  render(h(Text, { nativeID: 't', revealFraction: true, reveal: 0.5 }, 'abc'));
  const u = mock.find((n) => n.props.nativeID === 't');
  assert.equal(u.props.style.textReveal, 0.5);
  assert.equal(u.props.style.textRevealFraction, true);
  // No reveal props: the style is passed as is.
  const style = { color: 'green' };
  render(h(Text, { nativeID: 't', style }, 'abc'));
  assert.deepEqual(mock.find((n) => n.props.nativeID === 't').props.style, { color: 'green' });
  render(null);
});

test('Animated.Text reveal={value}: patched per frame, no React renders', () => {
  const v = new Animated.Value(0);
  let renders = 0;
  function App() {
    renders++;
    return h(Animated.Text, { nativeID: 't', reveal: v, revealFade: 1, style: { fontSize: 18 } }, 'The gate did not ', h(Text, { style: { fontStyle: 'italic' } }, 'fall'), '.');
  }
  render(h(App));
  const t = mock.find((n) => n.props.nativeID === 't');
  assert.equal(t.props.style.textReveal, 0);
  assert.equal(t.props.style.textRevealFade, 1);
  assert.equal(t.props.style.fontSize, 18);
  mock.reset();
  let done = null;
  Animated.timing(v, { toValue: 22, duration: 160, easing: Easing.linear }).start((r) => (done = r));
  mock.frame(80);
  near(t.props.style.textReveal, 11, 1e-6);
  mock.advance(200);
  assert.deepEqual(done, { finished: true });
  assert.equal(t.props.style.textReveal, 22);
  assert.equal(mock.count('setProps'), 0);
  assert.ok(mock.calls.every((c) => c[0] === 'patchStyle'));
  assert.equal(renders, 1);
  render(null);
});

test('Animated.clock({...}) on the JS driver: count, pause, resume, reset, speed', () => {
  const c = Animated.clock({ start: 5, speed: 2 });
  render(h(Animated.View, { nativeID: 'c', style: { transform: [{ translateX: c }] } }));
  const box = mock.find((n) => n.props.nativeID === 'c');
  const x = () => box.props.style.transform[0].translateX;
  const t0 = mock.now();
  near(x(), 5 + ((mock.now() - t0) / 1000) * 2);
  mock.frame(100);
  near(x(), 5.2);
  mock.frame(400);
  near(x(), 6);
  c.pause();
  assert.equal(c.isPaused(), true);
  near(c.__getValue(), 6);
  mock.frame(500);
  near(x(), 6);
  near(c.__getValue(), 6);
  c.resume();
  mock.frame(250);
  near(x(), 6.5);
  c.setSpeed(-1);
  mock.frame(500);
  near(x(), 6);
  c.reset();  // back to start, still running
  near(x(), 5);
  mock.frame(1000);
  near(x(), 4);
  c.reset(100);
  c.pause();
  c.setSpeed(1);
  mock.frame(1000);
  near(x(), 100);
  c.resume();
  mock.frame(16);
  near(x(), 100.016);
  // setValue is reset.
  c.setValue(0);
  near(c.__getValue(), 0);
  render(null);
});

test('Animated.clock({running}): held while the node is 0', () => {
  const on = new Animated.Value(1);
  const c = Animated.clock({ running: on });
  render(h(Animated.View, { nativeID: 'c', style: { opacity: c } }));
  const box = mock.find((n) => n.props.nativeID === 'c');
  mock.frame(500);
  near(box.props.style.opacity, 0.5);
  on.setValue(0);
  mock.frame(1000);
  near(box.props.style.opacity, 0.5);
  near(c.__getValue(), 0.5);
  on.setValue(1);
  mock.frame(250);
  near(box.props.style.opacity, 0.75);
  // Paused as well as held, then resumed while held: still held.
  on.setValue(0);
  c.pause();
  c.resume();
  mock.frame(300);
  near(c.__getValue(), 0.75);
  on.setValue(1);
  mock.frame(100);
  near(c.__getValue(), 0.85);
  render(null);
});

test('Animated.clock({paused}) starts paused; nothing reads it: no frames', () => {
  const c = Animated.clock({ paused: true, start: 3 });
  mock.frame(100);
  near(c.__getValue(), 3);
  c.resume();
  // Not observed: computed when read, no frame loop of its own.
  mock.frame(100);
  near(c.__getValue(), 3.1);
  const values = [];
  const id = c.addListener(({ value }) => values.push(value));
  mock.frame(100);
  assert.equal(values.length, 1);
  near(values[0], 3.2);
  c.removeListener(id);
  c.pause();
  // Animated.clock() without options is still the shared frame clock.
  const frame = Animated.clock();
  assert.equal(typeof frame.pause, 'undefined');
  near(frame.__getValue(), mock.now() / 1000);
});
