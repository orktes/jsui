// <Video> against the mock bridge: what reaches native (the source
// alternatives, texture, playback props; SET on prop changes, SEEK from the
// ref, CLOSE on unmount and on a new source), the view it draws (the host
// texture, the poster until the first frame), and react-native-video's
// callbacks from the status reports. The runtime side is
// tests/native/video_test.cpp.
import { mock } from './mock-native.js';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createElement as h, createRef } from 'react';
import { render, Video, VideoState } from '../src/index.js';
import ShimVideo from '../src/shims/react-native-video.js';

const wait = (ms = 0) => new Promise((r) => setTimeout(r, ms));
const byId = (id) => mock.find((x) => x.props.nativeID === id);
const take = () => mock.video.splice(0);

test('opens the sources into its texture, follows props, seeks, closes', async () => {
  take();
  const ref = createRef();
  const el = (props) => h(Video, { nativeID: 'v', ref, textureName: 'trailer', style: { width: 160, height: 90 },
    source: [{ uri: 'a.webm', type: 'video/webm' }, { uri: 'https://cdn/a.m3u8', type: 'application/vnd.apple.mpegurl' }],
    ...props });
  render(el({ paused: true, muted: true }));
  await wait();
  const [open, ...rest] = take();
  assert.equal(open[0], 'open');
  assert.deepEqual(open[1], { id: 1, urls: ['http://bundle/a.webm', 'https://cdn/a.m3u8'],
    types: ['video/webm', 'application/vnd.apple.mpegurl'], texture: 'trailer', paused: true, muted: true, volume: 1,
    loop: false, rate: 1, progressMs: 250 });
  assert.deepEqual(rest.map((c) => c[0]), ['set']);  // the initial props once more (harmless)
  // The view is the host texture.
  assert.equal(byId('v').props.texture, 'trailer');
  render(el({ paused: false, muted: true, repeat: true }));
  await wait();
  assert.deepEqual(take(), [['set', { id: 1, paused: false, muted: true, volume: 1, loop: true, rate: 1 }]]);
  ref.current.seek(12.5);
  assert.deepEqual(take(), [['seek', { id: 1, time: 12.5 }]]);
  assert.equal(ref.current.textureName, 'trailer');
  // A new source: the old video closes, a new one opens.
  render(el({ source: 'b.mp4', paused: false, muted: true, repeat: true }));
  await wait();
  const cmds = take();
  assert.deepEqual(cmds[0], ['close', { id: 1 }]);
  assert.equal(cmds[1][0], 'open');
  assert.deepEqual(cmds[1][1].urls, ['http://bundle/b.mp4']);
  render(h('View'));
  await wait();
  assert.deepEqual(take().filter((c) => c[0] === 'close'), [['close', { id: 2 }]]);
});

test('status reports become react-native-video callbacks', async () => {
  take();
  const heard = [];
  render(h(Video, {
    source: { uri: 'c.mp4' },
    progressUpdateInterval: 100,
    onLoad: (e) => heard.push(['load', e.duration, e.naturalSize.width]),
    onProgress: (e) => heard.push(['progress', e.currentTime, e.playableDuration]),
    onBuffer: (e) => heard.push(['buffer', e.isBuffering]),
    onPlaybackStateChanged: (e) => heard.push(['playing', e.isPlaying]),
    onReadyForDisplay: () => heard.push(['ready']),
    onEnd: () => heard.push(['end']),
    onError: (e) => heard.push(['error', e.error.errorString]),
  }));
  await wait();
  const id = take().find((c) => c[0] === 'open')[1].id;
  mock.videoStatus(id, VideoState.buffering, 0, 0, 0);
  mock.videoStatus(id, VideoState.playing, 10, 0, 2, 640, 360);
  mock.videoStatus(id, VideoState.playing, 10, 1.5, 3, 640, 360);
  mock.videoStatus(id, VideoState.ended, 10, 10, 10, 640, 360);
  mock.videoStatus(id, VideoState.failed, 10, 10, 10, 640, 360, 'gone');
  await wait();
  assert.deepEqual(heard, [
    ['load', 10, 640], ['ready'], ['buffer', false], ['playing', true], ['progress', 0, 2],
    ['progress', 1.5, 3],
    ['playing', false], ['progress', 10, 10], ['end'],
    ['error', 'gone'],
  ]);
});

test('the poster shows until the first frame; the shim is the same component', async () => {
  take();
  render(h(Video, { nativeID: 'p', source: 'd.mp4', poster: 'poster.jpg', textureName: 'd' }));
  await wait();
  const id = take().find((c) => c[0] === 'open')[1].id;
  assert.equal(byId('p').type, 'Image');
  mock.videoStatus(id, VideoState.paused, 5, 0, 1, 320, 180);
  await wait();
  assert.equal(byId('p').props.texture, 'd');
  assert.equal(ShimVideo, Video);
});
