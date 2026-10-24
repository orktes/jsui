// ShaderView against the mock bridge: the source, plain uniforms as a prop,
// Animated uniforms as `uniform:` style keys (whole values and array
// components, JS and native driver), images (require / uri / host
// textures), paused and onError. Rendering itself: tests/native/shader_test.cpp.
import { mock, animated } from './mock-native-animated.js';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createElement as h } from 'react';
import { Animated, ShaderView, sksl, compileShader, precompileShaders, render } from '../src/index.js';

const wait = (ms = 0) => new Promise((r) => setTimeout(r, ms));
const byId = (id) => mock.find((x) => x.props.nativeID === id);
const SRC = sksl`uniform float glow;
uniform float2 center;
layout(color) uniform half4 tint;
uniform shader image;
half4 main(float2 p) { return image.eval(p) * tint * glow; }`;

test('sksl and compileShader', () => {
  assert.equal(sksl`a${1}b${'c'}`, 'a1bc');
  const info = compileShader(SRC);
  assert.deepEqual(info.children, ['image']);
  assert.deepEqual(
    info.uniforms.map((u) => [u.name, u.color]),
    [
      ['glow', false],
      ['center', false],
      ['tint', true],
    ],
  );
  assert.ok(compileShader('nope').error);
});

test('source, plain uniforms, images and paused reach native', async () => {
  mock.reset();
  render(
    h(ShaderView, {
      nativeID: 'a',
      source: SRC,
      uniforms: { glow: 0.5, center: [10, 20], tint: '#7cf', skipped: null },
      images: {
        image: { uri: 'https://example.com/a.png' },
        noise: { source: 'noise.png', fit: 'repeat', tile: 'mirror' },
        video: { texture: 'trailer', fit: 'contain' },
      },
      paused: true,
      style: { width: 100, height: 50 },
    }),
  );
  await wait();
  const n = byId('a');
  assert.equal(n.type, 'ShaderView');
  assert.equal(n.props.source, SRC);
  assert.deepEqual(n.props.uniforms, { glow: 0.5, center: [10, 20], tint: '#7cf' });
  assert.deepEqual(n.props.images, {
    image: { uri: 'https://example.com/a.png' },
    noise: { uri: 'noise.png', fit: 'repeat', tile: 'mirror' },
    video: { texture: 'trailer', fit: 'contain' },
  });
  assert.equal(n.props.paused, true);
  assert.deepEqual(n.props.style, { width: 100, height: 50 });
  // `sksl` is an alias of `source`.
  render(h(ShaderView, { nativeID: 'a', sksl: SRC }));
  await wait();
  assert.equal(byId('a').props.source, SRC);
  assert.equal(byId('a').props.paused, undefined);
  render(null);
  await wait();
});

test('Animated uniforms: style keys patched without re-rendering', async () => {
  const glow = new Animated.Value(0);
  const x = new Animated.Value(1);
  render(h(ShaderView, { nativeID: 'u', source: SRC, uniforms: { glow, center: [x, 7] }, style: { flex: 1 } }));
  await wait();
  let n = byId('u');
  assert.deepEqual(n.props.style, { flex: 1, 'uniform:glow': 0, 'uniform:center.0': 1 });
  assert.deepEqual(n.props.uniforms, { center: [0, 7] });
  mock.reset();
  glow.setValue(0.8);
  x.setValue(3);
  await wait(20);
  n = byId('u');
  assert.equal(n.props.style['uniform:glow'], 0.8);
  assert.equal(n.props.style['uniform:center.0'], 3);
  assert.equal(mock.count('setProps'), 0);
  // Back to a plain number: the style key goes away.
  render(h(ShaderView, { nativeID: 'u', source: SRC, uniforms: { glow: 1 }, style: { flex: 1 } }));
  await wait();
  assert.deepEqual(byId('u').props.style, { flex: 1 });
  assert.deepEqual(byId('u').props.uniforms, { glow: 1 });
  render(null);
  await wait();
});

test('native driver: a style node for the uniform keys', async () => {
  const v = new Animated.Value(0);
  render(h(ShaderView, { nativeID: 'nd', source: SRC, uniforms: { glow: v } }));
  await wait();
  animated.reset();
  Animated.timing(v, { toValue: 1, duration: 100, useNativeDriver: true }).start();
  const style = animated.of('createNode').find(([, c]) => c.type === 'style');
  assert.ok(style, 'a native style node');
  assert.deepEqual(Object.keys(style[1].style), ['uniform:glow']);
  assert.deepEqual(animated.of('connectToView').map(([, view]) => view), [byId('nd').id]);
  render(null);
  await wait();
});

test('onError gets the compile error', async () => {
  const seen = [];
  render(h(ShaderView, { nativeID: 'e', source: 'bad', onError: (e) => seen.push(e.nativeEvent.error) }));
  await wait();
  const n = byId('e');
  assert.ok(n.props.__events.includes('error'));
  mock.dispatch({ type: 'error', target: n.id, error: 'error: 1: missing main()' });
  assert.deepEqual(seen, ['error: 1: missing main()']);
  render(null);
  await wait();
});

test('precompileShaders: compiles, queues the warm-up, resolves after it ran', async () => {
  mock.reset();
  mock.warming = 0;
  assert.equal(ShaderView.preload, precompileShaders);
  let done = null;
  precompileShaders([SRC, { source: SRC, images: { image: { uri: 'a.png' }, video: { texture: 'v', tile: 'mirror' } } }, 'bad']).then(
    (r) => (done = r),
  );
  assert.deepEqual(
    mock.calls.filter((c) => c[0] === 'shaderPreload').map((c) => c.slice(2)),
    [[], [{ image: { uri: 'a.png' }, video: { texture: 'v', tile: 'mirror' } }], []],
  );
  assert.equal(mock.warming, 2);
  mock.frame();
  await wait();
  assert.equal(done, null, 'waits for the warm-up frame');
  mock.warming = 0;
  mock.frame();
  await wait();
  assert.equal(done.length, 3);
  assert.deepEqual(done[0].children, ['image']);
  assert.ok(done[2].error);
  // One source on its own.
  const pending = precompileShaders(SRC);
  assert.equal(mock.warming, 1);
  mock.warming = 0;
  mock.frame();
  assert.equal((await pending).length, 1);
});
