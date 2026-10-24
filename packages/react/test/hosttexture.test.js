// Host textures: HostTexture (an Image host node with a `texture` prop) and
// useHostTexture / HostTextures against the mock bridge. The runtime side
// (sizing, painting, repaint on new contents) is in
// tests/native/host_textures_test.cpp.
import { mock } from './mock-native.js';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createElement as h } from 'react';
import { Text, render, HostTexture, HostTextures, useHostTexture } from '../src/index.js';

const wait = (ms = 0) => new Promise((r) => setTimeout(r, ms));
const byId = (id) => mock.find((x) => x.props.nativeID === id);

test('HostTexture renders an Image host node naming the texture, with Image paint props in style', async () => {
  render(h(HostTexture, { nativeID: 'tv', name: 'trailer', resizeMode: 'contain', style: [{ width: 64 }, { opacity: 0.5 }] }));
  await wait();
  const n = byId('tv');
  assert.equal(n.type, 'Image');
  assert.equal(n.props.texture, 'trailer');
  assert.equal(n.props.resizeMode, 'contain');
  assert.deepEqual(n.props.style, { width: 64, opacity: 0.5, resizeMode: 'contain' });
  render(null);
  await wait();
});

test('useHostTexture: null until set, the size after, re-rendered on resize and removal only', async () => {
  let renders = 0;
  function Size() {
    renders++;
    const s = useHostTexture('cam');
    return h(Text, { nativeID: 'size' }, s ? `${s.width}x${s.height}` : 'none');
  }
  const text = () => {
    const n = byId('size');
    return n && mock.nodes.get(n.children[0]).text;
  };
  render(h(Size));
  await wait();
  assert.equal(text(), 'none');
  assert.equal(HostTextures.get('cam'), null);
  mock.setHostTexture('cam', { width: 320, height: 180 });
  mock.tickTextures();
  await wait();
  assert.equal(text(), '320x180');
  assert.deepEqual(HostTextures.get('cam'), { width: 320, height: 180 });
  assert.equal(HostTextures.get('cam'), HostTextures.get('cam')); // stable while unchanged
  assert.deepEqual(HostTextures.keys(), ['cam']);
  const before = renders;
  mock.tickTextures(); // nothing changed: no re-render
  await wait();
  assert.equal(renders, before);
  const seen = [];
  const off = HostTextures.subscribe('*', (info, name) => seen.push([name, info && info.width]));
  mock.setHostTexture('cam', undefined);
  mock.tickTextures();
  await wait();
  assert.equal(text(), 'none');
  assert.deepEqual(seen, [['cam', null]]);
  off();
  assert.throws(() => HostTextures.set('x', { width: 1, height: 1 }), /set by the host/);
  render(null);
  await wait();
});
