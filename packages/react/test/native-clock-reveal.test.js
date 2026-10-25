// Animated.clock({...}) and Animated.Text reveal with the native driver,
// against the recording native module: what is serialized (a "clock"
// animation per run, pause = stop, reset = setValue + a new run, the
// `running` node's tag; the reveal as the textReveal style key) and that no
// JS frames run. The runtime side: tests/native/native_animated_test.cpp,
// text_reveal_test.cpp, react_reveal_e2e_test.cpp.
import { mock, animated } from './mock-native-animated.js';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createElement as h } from 'react';
import { Animated, Easing, render, Text, useHostStateBind } from '../src/index.js';
import { activeAnimationCount } from '../src/animated/animations.js';

const config = (tag) => animated.nodes.get(tag).config;

test('native clock: one clock animation per run; pause / resume / reset / setSpeed are commands', () => {
  animated.reset();
  const c = Animated.clock({ start: 2, speed: 1.5 });
  const tag = c.__getNativeTag();
  assert.deepEqual(config(tag), { type: 'value', value: 2, offset: 0 });
  let starts = animated.of('startAnimation');
  assert.equal(starts.length, 1);
  assert.equal(starts[0][1], tag);
  assert.deepEqual(starts[0][2], { type: 'clock', speed: 1.5 });
  const firstId = starts[0][0];

  // The runtime moved it to 7: pause stops the animation, the value stays.
  animated.frame(tag, 7);
  c.pause();
  assert.deepEqual(animated.of('stopAnimation'), [[firstId]]);
  assert.equal(c.__getValue(), 7);
  assert.equal(c.isPaused(), true);
  c.pause();
  assert.equal(animated.count('stopAnimation'), 1);

  c.resume();
  starts = animated.of('startAnimation');
  assert.equal(starts.length, 2);
  assert.deepEqual(starts[1][2], { type: 'clock', speed: 1.5 });

  // setSpeed while running: a new run at the new speed.
  c.setSpeed(-2);
  starts = animated.of('startAnimation');
  assert.equal(starts.length, 3);
  assert.deepEqual(starts[2][2], { type: 'clock', speed: -2 });
  assert.equal(animated.count('stopAnimation'), 2);

  // reset: stop, setValue, start again.
  animated.reset();
  c.reset(10);
  assert.deepEqual(
    animated.calls.map((x) => x[0]),
    ['stopAnimation', 'setValue', 'startAnimation'],
  );
  assert.deepEqual(animated.of('setValue'), [[tag, 10]]);
  // While paused: reset only sets the value; setSpeed is remembered.
  c.pause();
  animated.reset();
  c.reset();
  c.setSpeed(3);
  assert.deepEqual(
    animated.calls.map((x) => x[0]),
    ['setValue'],
  );
  assert.deepEqual(animated.of('setValue'), [[tag, 2]]);
  c.resume();
  assert.deepEqual(lastStart()[2], { type: 'clock', speed: 3 });
  assert.equal(activeAnimationCount(), 0);  // nothing on the JS frame loop

  // Stopped by the runtime (another animation on the value): paused.
  const id = lastStart()[0];
  animated.end(id, false, 4);
  assert.equal(c.isPaused(), true);
  assert.equal(c.__getValue(), 4);
  // A timing on the clock stops it (as any animation would).
  c.resume();
  const runId = lastStart()[0];
  Animated.timing(c, { toValue: 0, duration: 100, easing: Easing.linear, useNativeDriver: true }).start();
  assert.ok(animated.of('stopAnimation').some(([i]) => i === runId));
  assert.equal(c.isPaused(), true);
});

const lastStart = () => animated.of('startAnimation').at(-1);

test('native clock: running={node} sends that node with the clock', () => {
  animated.reset();
  let clock = null;
  function App() {
    const running = useHostStateBind('game', 'running');
    if (!clock) clock = Animated.clock({ running });
    return h(Animated.Text, { nativeID: 't', text: Animated.format(clock, { time: 'mm:ss' }) });
  }
  render(h(App));
  const start = animated.of('startAnimation').at(-1);
  assert.equal(start[2].type, 'clock');
  const gate = config(start[2].running);
  assert.deepEqual(gate, { type: 'hostState', key: 'game', path: ['running'] });
  render(null);
});

test('native Animated.Text reveal: a style node with textReveal, timing runs natively', () => {
  animated.reset();
  const v = new Animated.Value(0);
  let renders = 0;
  function App() {
    renders++;
    return h(Animated.Text, { nativeID: 't', reveal: v, revealMode: 'words' }, 'Hello ', h(Text, { style: { color: 'red' } }, 'world'));
  }
  render(h(App));
  Animated.timing(v, { toValue: 2, duration: 100, useNativeDriver: true }).start();
  const t = mock.find((n) => n.props.nativeID === 't');
  assert.equal(t.props.style.textRevealMode, 'words');
  assert.equal(t.props.style.textReveal, 0);
  const style = animated.of('createNode').find(([, c]) => c.type === 'style');
  assert.ok(style, 'a style node');
  assert.deepEqual(Object.keys(style[1].style), ['textReveal']);
  assert.equal(style[1].style.textReveal, v.__getNativeTag());
  const props = animated.of('createNode').find(([, c]) => c.type === 'props');
  assert.equal(animated.nodes.get(props[0]).view, t.id);
  assert.equal(activeAnimationCount(), 0);
  assert.equal(renders, 1);
  render(null);
});
