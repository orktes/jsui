// UI audio: Sound / useSound / useMusic / UISounds against the mock bridge.
// The runtime side (routing to the handler, voice bookkeeping, reports) is
// in tests/native/audio_test.cpp.
import { mock } from './mock-native.js';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createElement as h, useState } from 'react';
import { View, Pressable, render, Sound, useSound, useMusic, UISounds } from '../src/index.js';

const wait = (ms = 0) => new Promise((r) => setTimeout(r, ms));
const byId = (id) => mock.find((x) => x.props.nativeID === id);
const plays = () => mock.audio.filter((c) => c[0] === 'play').map((c) => c[1]);
const take = () => mock.audio.splice(0);

test('Sound.play sends the options and ends once with the reported reason', async () => {
  take();
  const v = Sound.play('click.wav', { bus: 'ui', volume: 0.5, pitch: 1.2, pan: -0.5, fadeIn: 30, offset: 0.25 });
  const [p] = plays();
  assert.deepEqual(p, { id: v.id, url: 'http://bundle/click.wav', bus: 'ui', volume: 0.5, pitch: 1.2, pan: -0.5, loop: false, fadeMs: 30, offset: 0.25 });
  assert.equal(v.playing, true);
  assert.ok(Sound.voices().includes(v));
  let heard = 0;
  v.onEnd(() => heard++);
  mock.audioEnded(v.id, 0);
  mock.audioEnded(v.id, 1);  // a second report is ignored
  assert.deepEqual(await v.ended, { reason: 'finished' });
  assert.equal(heard, 1);
  assert.equal(v.playing, false);
  assert.ok(!Sound.voices().includes(v));
  // Defaults: the sfx bus, unit gain.
  take();
  const w = Sound.play({ uri: 'boom.ogg' });
  assert.equal(plays()[0].bus, 'sfx');
  assert.equal(plays()[0].volume, 1);
  mock.audioEnded(w.id, -1);
  assert.equal((await w.ended).reason, 'failed');
});

test('voice controls: stop with fade, pause / resume once, set only on change', () => {
  take();
  const v = Sound.play('loop.ogg', { loop: true });
  v.setVolume(0.3, { fade: 200 });
  v.setVolume(0.3);
  v.pause();
  v.pause();
  v.resume();
  v.setPan(-5);
  v.stop({ fade: 500 });
  const voice = mock.audio.filter((c) => c[0] === 'voice').map((c) => [c[1].op, c[1].volume, c[1].pan, c[1].fadeMs]);
  assert.deepEqual(voice, [[6, 0.3, 0, 200], [4, 0.3, 0, 0], [5, 0.3, 0, 0], [6, 0.3, -1, 0], [3, 0.3, -1, 500]]);
  mock.audioEnded(v.id, 1);
  assert.equal(v.endReason, 'stopped');
  take();
  v.stop();  // ended: nothing is sent
  v.setVolume(1);
  assert.equal(mock.audio.length, 0);
});

test('Sound.preload resolves with the duration once per sound, rejects on failure', async () => {
  take();
  const a = Sound.preload('music/menu.ogg');
  const b = Sound.preload('./music/menu.ogg');
  assert.equal(a, b);
  assert.deepEqual(mock.audio, [['preload', 'http://bundle/music/menu.ogg']]);
  mock.audioLoaded('music/menu.ogg', 1, 92.5);
  assert.deepEqual(await a, { duration: 92.5 });
  const c = Sound.preload('missing.wav');
  mock.audioLoaded('missing.wav', 0);
  await assert.rejects(c, /could not be loaded/);
});

test('buses and stopAll', () => {
  take();
  Sound.setBusVolume('music', 0.4, { fade: 300 });
  Sound.setBusMuted('music', true);
  assert.deepEqual(Sound.getBus('music'), { volume: 0.4, muted: true });
  assert.deepEqual(Sound.getBus('other'), { volume: 1, muted: false });
  Sound.stopAll({ bus: 'ui', fade: 100 });
  Sound.stopAll();
  assert.deepEqual(take(), [
    ['bus', { bus: 'music', volume: 0.4, muted: false, fadeMs: 300 }],
    ['bus', { bus: 'music', volume: 0.4, muted: true, fadeMs: 0 }],
    ['stopAll', { bus: 'ui', fadeMs: 100 }],
    ['stopAll', { bus: null, fadeMs: 0 }],
  ]);
});

test('useSound preloads on mount and returns a stable player', async () => {
  take();
  const fns = [];
  function C({ n }) {
    const play = useSound('tick.wav', { bus: 'ui', volume: 0.6 });
    fns.push(play);
    return h(View, { nativeID: 'c' + n });
  }
  render(h(C, { n: 1 }));
  await wait();
  render(h(C, { n: 2 }));
  await wait();
  assert.equal(fns[0], fns[fns.length - 1]);
  assert.deepEqual(mock.audio.filter((c) => c[0] === 'preload'), [['preload', 'http://bundle/tick.wav']]);
  fns[0]({ pitch: 1.5 });
  const p = plays().pop();
  assert.equal(p.bus, 'ui');
  assert.equal(p.volume, 0.6);
  assert.equal(p.pitch, 1.5);
  render(null);
  await wait();
});

test('useMusic plays while mounted, crossfades a new source, follows volume and paused', async () => {
  take();
  let setTrack, setVol;
  function M() {
    const [track, st] = useState('a.ogg');
    const [vol, sv] = useState(1);
    setTrack = st;
    setVol = sv;
    useMusic(track, { volume: vol, fadeIn: 1000, fadeOut: 800 });
    return null;
  }
  render(h(M));
  await wait();
  let p = plays();
  assert.equal(p.length, 1);
  assert.equal(p[0].url, 'http://bundle/a.ogg');
  assert.equal(p[0].bus, 'music');
  assert.equal(p[0].loop, true);
  assert.equal(p[0].fadeMs, 1000);
  const first = p[0].id;
  take();
  setVol(0.5);
  await wait(20);
  assert.deepEqual(take().map((c) => [c[0], c[1].op, c[1].volume, c[1].fadeMs]), [['voice', 6, 0.5, 250]]);
  setTrack('b.ogg');
  await wait(20);
  const cmds = take();
  assert.deepEqual(cmds[0], ['voice', { op: 3, voice: first, volume: 0.5, pitch: 1, pan: 0, fadeMs: 800 }]);
  assert.equal(cmds[1][0], 'play');
  assert.equal(cmds[1][1].url, 'http://bundle/b.ogg');
  assert.equal(cmds[1][1].volume, 0.5);
  render(null);
  await wait();
  assert.equal(take()[0][1].op, 3);
});

test('Pressable sounds: own prop, UISounds defaults, opt-out, focus only from navigation', async () => {
  take();
  render(
    h(UISounds, { sounds: { hoverIn: 'hover.wav', press: 'press.wav', focus: 'focus.wav' }, volume: 0.5 },
      h(Pressable, { nativeID: 'a', onPress() {} }),
      h(Pressable, { nativeID: 'b', onPress() {}, sounds: { press: { source: 'special.wav', volume: 0.8, bus: 'sfx' } } }),
      h(Pressable, { nativeID: 'c', onPress() {}, sounds: false }),
      h(Pressable, { nativeID: 'd' })),  // does nothing when pressed: no default sounds
  );
  await wait();
  const a = byId('a').id, b = byId('b').id, c = byId('c').id, d = byId('d').id;
  const urls = () => take().filter((x) => x[0] === 'play').map((x) => [x[1].url.replace('http://bundle/', ''), x[1].bus, x[1].volume]);
  mock.dispatch({ type: 'pointerenter', target: a, x: 1, y: 1, pointerType: 'mouse' });
  mock.dispatch({ type: 'pointerenter', target: c, x: 1, y: 1, pointerType: 'mouse' });
  mock.dispatch({ type: 'pointerenter', target: d, x: 1, y: 1, pointerType: 'mouse' });
  mock.dispatch({ type: 'pointerenter', target: b, x: 1, y: 1, pointerType: 'touch' });
  assert.deepEqual(urls(), [['hover.wav', 'ui', 0.5]]);
  mock.dispatch({ type: 'click', target: b, pointerType: 'gamepad' });
  assert.deepEqual(urls(), [['special.wav', 'sfx', 0.4]]);
  mock.dispatch({ type: 'click', target: c, pointerType: 'gamepad' });
  assert.deepEqual(urls(), []);
  // Focus: only keyboard / gamepad moves from another element.
  mock.dispatch({ type: 'focus', target: a, focusVisible: false, relatedTarget: b });
  mock.dispatch({ type: 'blur', target: a });
  mock.dispatch({ type: 'focus', target: a, focusVisible: true, relatedTarget: null });
  mock.dispatch({ type: 'blur', target: a });
  assert.deepEqual(urls(), []);
  mock.dispatch({ type: 'focus', target: a, focusVisible: true, relatedTarget: b });
  assert.deepEqual(urls(), [['focus.wav', 'ui', 0.5]]);
  render(null);
  await wait();
});
