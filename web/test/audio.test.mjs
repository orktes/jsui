// Unit tests of web/jsui-audio.js (UI audio on Web Audio) against a mock
// AudioContext, fetch, <audio> element and page: no browser or WASM build needed.
//
//   node --test web/test/audio.test.mjs
import assert from 'node:assert/strict';
import test from 'node:test';
import { AudioOp, AudioStatus, JsuiAudio, WebAudioBackend } from '../jsui-audio.js';

const { FINISHED, STOPPED, FAILED } = AudioStatus;
const tick = () => new Promise((r) => setTimeout(r, 0));

class Param {
  constructor(v) { this.value = v; this.log = []; }
  setValueAtTime(v, t) { this.log.push(['set', v, t]); this.value = v; }
  linearRampToValueAtTime(v, t) { this.log.push(['ramp', v, t]); this.value = v; }
  setTargetAtTime(v, t, c) { this.log.push(['target', v, t, c]); this.value = v; }
  cancelScheduledValues(t) { this.log.push(['cancel', t]); }
  cancelAndHoldAtTime(t) { this.log.push(['hold', t]); }
  get last() { return this.log.at(-1); }
}

class Node {
  constructor(ctx, kind) { this.ctx = ctx; this.kind = kind; this.outputs = new Set(); ctx.nodes.push(this); }
  connect(n) { this.outputs.add(n); return n; }
  disconnect() { this.outputs.clear(); this.disconnected = true; }
}

class MockContext {
  constructor(opts) {
    this.opts = opts;
    this.state = 'suspended';
    this.currentTime = 0;
    this.nodes = [];
    this.sources = [];
    this.listeners = [];
    this.destination = new Node(this, 'destination');
    this.closed = false;
    MockContext.made.push(this);
  }
  addEventListener(t, cb) { if (t === 'statechange') this.listeners.push(cb); }
  setState(s) { this.state = s; for (const cb of this.listeners) cb(); }
  resume() { this.resumes = (this.resumes || 0) + 1; if (this.state !== 'closed') this.setState('running'); return Promise.resolve(); }
  close() { this.closed = true; this.setState('closed'); return Promise.resolve(); }
  createGain() { const n = new Node(this, 'gain'); n.gain = new Param(1); return n; }
  createStereoPanner() { const n = new Node(this, 'panner'); n.pan = new Param(0); return n; }
  createBufferSource() {
    const n = new Node(this, 'source');
    n.playbackRate = new Param(1);
    n.start = (when, offset) => { n.started = { when, offset }; };
    n.stop = (when = 0) => { n.stopped = when; };
    n.end = () => n.onended && n.onended();  // the test plays the 'ended' event
    this.sources.push(n);
    return n;
  }
  createMediaElementSource(el) { const n = new Node(this, 'media'); n.el = el; return n; }
  decodeAudioData(bytes) { return decode(bytes); }
}
MockContext.made = [];

class MockOffline {
  constructor(ch, len, rate) { this.rate = rate; MockOffline.made++; }
  decodeAudioData(bytes) { return decode(bytes); }
}
MockOffline.made = 0;

// "Files": the bytes' first value is the duration in seconds (0xff: not audio).
function decode(bytes) {
  const d = new Uint8Array(bytes)[0];
  if (d === 0xff) return Promise.reject(new DOMException('Unable to decode audio data', 'EncodingError'));
  return Promise.resolve({ duration: d, numberOfChannels: 2 });
}

function mockEnv(files = {}) {
  const listeners = new Map();
  const target = {
    addEventListener: (t, cb, o) => { listeners.set(t, cb); if (o && o.signal) o.signal.addEventListener('abort', () => listeners.delete(t)); },
    removeEventListener: (t) => listeners.delete(t),
  };
  const doc = { visibilityState: 'visible', addEventListener: (t, cb, o) => { listeners.set(t, cb); if (o && o.signal) o.signal.addEventListener('abort', () => listeners.delete(t)); } };
  const fetches = [];
  const elements = [];
  const env = {
    AudioContext: MockContext,
    OfflineAudioContext: MockOffline,
    target,
    document: doc,
    listeners,
    fetches,
    elements,
    fetch: async (url) => {
      fetches.push(url);
      const f = files[url];
      if (!f) return { ok: false, status: 404, headers: { get: () => null } };
      return {
        ok: true, status: 200,
        headers: { get: (h) => (h === 'content-length' ? String(f.size ?? 1) : null) },
        body: { cancel() { f.cancelled = true; } },
        arrayBuffer: async () => Uint8Array.of(f.duration).buffer,
      };
    },
    createAudio: () => {
      const el = {
        currentTime: 0, paused: true, duration: 120, attrs: {},
        play() { this.paused = false; return Promise.resolve(); },
        pause() { this.paused = true; },
        load() {},
        removeAttribute(k) { delete this.attrs[k]; this.src = ''; },
      };
      elements.push(el);
      return el;
    },
    hasBeenActive: () => false,
    time: 0,
    now() { return this.time; },
    gesture(type = 'pointerdown') { listeners.get(type)(); },
  };
  return env;
}

function backend(env, opts = {}) {
  const r = { ended: [], loaded: [], unlocks: 0, logs: [] };
  const b = new WebAudioBackend({
    voiceEnded: (id, s) => r.ended.push([id, s]),
    soundLoaded: (url, ok, d) => r.loaded.push([url, ok, d]),
    unlocked: () => r.unlocks++,
    log: (l, t) => r.logs.push(t),
  }, { env, ...opts });
  return { b, r };
}

const play = (voice, sound, o = {}) => ({ op: AudioOp.PLAY, voice, sound, bus: 'sfx', volume: 1, pitch: 1, pan: 0,
  fadeMs: 0, offset: 0, loop: false, ...o });
const cmd = (op, o) => ({ op, voice: 0, sound: null, bus: null, volume: 1, pitch: 1, pan: 0, fadeMs: 0, offset: 0,
  loop: false, muted: false, ...o });
const FILES = { 'https://x/tick.wav': { duration: 1 }, 'https://x/music.ogg': { duration: 8 },
  'https://x/bad.wav': { duration: 0xff }, 'https://x/long.ogg': { duration: 200, size: 5 << 20 } };

async function unlocked(env) {
  const t = backend(env);
  env.gesture();
  await tick();
  return t;
}

// The node a voice's source feeds, down to the destination.
function chain(node) {
  const out = [];
  for (let n = node; n; n = [...n.outputs][0]) out.push(n.kind);
  return out;
}

test('no Web Audio (Node): every voice fails, every preload is not loaded, no context', async () => {
  const env = { ...mockEnv(FILES), AudioContext: null, OfflineAudioContext: null };
  const { b, r } = backend(env);
  b.command(play(1, 'https://x/tick.wav'));
  b.command(play(2, 'https://x/music.ogg', { loop: true }));
  b.command(cmd(AudioOp.PRELOAD, { sound: 'https://x/tick.wav' }));
  b.command(cmd(AudioOp.BUS, { bus: 'music', volume: 0.5 }));
  b.command(cmd(AudioOp.STOP_ALL, {}));
  await tick();
  assert.deepEqual(r.ended, [[1, FAILED], [2, FAILED]]);
  assert.deepEqual(r.loaded, [['https://x/tick.wav', false, -1]]);
  assert.equal(env.fetches.length, 0);
  env.gesture();
  assert.equal(b.context, null);
  b.destroy();

  // The host API in plain Node (globalThis has no AudioContext): audio off, so
  // the runtime is created with audio_disabled (jsui_web_create(..., 0)).
  const host = fakeHost();
  const a = new JsuiAudio(host, undefined);
  assert.equal(a.mode, 'off');
  assert.equal(a.enabled, false);
  assert.equal(a.unlocked, false);
  a.command(play(1, 'https://x/tick.wav'));  // ignored, never throws
  a.unlock();
  a.setVolume(0.3);
  a.destroy();
});

test('locked: one-shots are dropped (FAILED), loops wait for the gesture, preloads decode offline', async () => {
  const env = mockEnv(FILES);
  const { b, r } = backend(env);
  assert.equal(MockContext.made.filter((c) => c.opts).length >= 0, true);
  assert.equal(b.context, null, 'no AudioContext before a gesture');
  b.command(play(1, 'https://x/tick.wav', { bus: 'ui' }));
  b.command(play(2, 'https://x/music.ogg', { bus: 'music', loop: true, fadeMs: 1500, volume: 0.7 }));
  b.command(cmd(AudioOp.PRELOAD, { sound: 'https://x/tick.wav' }));
  await tick();
  assert.deepEqual(r.ended, [[1, FAILED]]);
  assert.deepEqual(r.loaded, [['https://x/tick.wav', true, 1]]);
  assert.deepEqual(env.fetches, ['https://x/tick.wav'], 'the waiting loop is not fetched yet');
  assert.ok(MockOffline.made > 0);
  assert.deepEqual(b.stats(), { voices: 1, playing: 0, waiting: 1, sounds: 1 });

  env.gesture('keydown');
  await tick();
  const ctx = b.context;
  assert.ok(ctx && ctx.state === 'running');
  assert.equal(ctx.opts.latencyHint, 'interactive');
  assert.equal(r.unlocks, 1);
  const src = ctx.sources[0];
  assert.deepEqual(src.started, { when: 0, offset: 0 });
  assert.equal(src.loop, true);
  assert.deepEqual(chain(src), ['source', 'gain', 'panner', 'gain', 'gain', 'destination']);
  const gain = [...src.outputs][0].gain;
  assert.deepEqual(gain.log, [['set', 0, 0], ['ramp', 0.7, 1.5]], 'fade in');
  // Unlocked: one-shots play now.
  b.command(play(3, 'https://x/tick.wav', { bus: 'ui', pitch: 1.2, pan: -0.5 }));
  await tick();
  const s3 = ctx.sources[1];
  assert.equal(s3.playbackRate.value, 1.2);
  assert.equal([...[...s3.outputs][0].outputs][0].pan.value, -0.5);
  assert.equal(env.fetches.filter((u) => u === 'https://x/tick.wav').length, 1, 'cached');
  s3.end();
  assert.deepEqual(r.ended, [[1, FAILED], [3, FINISHED]]);
  s3.end();
  assert.equal(r.ended.length, 2, 'reported once');
  b.destroy();
  assert.ok(ctx.closed, 'the last runtime closes the context');
});

test('the gesture that unlocks audio keeps its own one-shots (while the context starts)', async () => {
  class SlowContext extends MockContext {
    resume() { this.pending = () => this.setState('running'); return new Promise(() => {}); }
  }
  const env = { ...mockEnv(FILES), AudioContext: SlowContext };
  const { b, r } = backend(env);
  env.time = 1000;
  b.command(play(1, 'https://x/tick.wav'));  // a hover before any gesture: dropped
  env.gesture('pointerdown');
  env.time = 1100;
  b.command(play(2, 'https://x/tick.wav'));  // the click's press sound: waits
  env.time = 1600;
  b.command(play(3, 'https://x/tick.wav'));  // long after: dropped
  assert.deepEqual(r.ended, [[1, FAILED], [3, FAILED]]);
  env.time = 1200;
  b.context.pending();
  await tick();
  assert.equal(b.context.sources.length, 1, 'voice 2 plays');
  // Waiting past the grace period: dropped when audio unlocks.
  b.context.setState('suspended');
  env.gesture('keydown');
  b.command(play(4, 'https://x/tick.wav'));
  env.time = 2000;
  b.context.pending();
  await tick();
  assert.deepEqual(r.ended.at(-1), [4, FAILED]);
  assert.equal(b.context.sources.length, 1);
  b.destroy();
});

test('a page that already had a gesture starts at once; visibility resumes a suspended context', async () => {
  const env = mockEnv(FILES);
  env.hasBeenActive = () => true;
  const { b, r } = backend(env);
  await tick();
  assert.equal(b.unlocked, true);
  assert.equal(r.unlocks, 1);
  const ctx = b.context;
  ctx.setState('suspended');  // e.g. iOS interruption
  assert.equal(b.unlocked, false);
  env.listeners.get('visibilitychange')();
  await tick();
  assert.equal(b.unlocked, true);
  assert.equal(r.unlocks, 2);
  b.destroy();
  assert.equal(env.listeners.size, 0, 'listeners removed with the last runtime');
});

test('loads: one fetch per URL, failures report FAILED / not loaded without "error" in the log', async () => {
  const env = mockEnv(FILES);
  const { b, r } = await unlocked(env);
  b.command(play(1, 'https://x/music.ogg'));
  b.command(play(2, 'https://x/music.ogg'));
  b.command(cmd(AudioOp.PRELOAD, { sound: 'https://x/music.ogg' }));
  b.command(play(3, 'https://x/missing.wav'));
  b.command(play(4, 'https://x/bad.wav'));
  b.command(cmd(AudioOp.PRELOAD, { sound: 'https://x/bad.wav' }));
  await tick();
  assert.equal(env.fetches.filter((u) => u === 'https://x/music.ogg').length, 1);
  assert.deepEqual(r.ended.sort(), [[3, FAILED], [4, FAILED]]);
  assert.deepEqual(r.loaded, [['https://x/music.ogg', true, 8], ['https://x/bad.wav', false, -1]]);
  assert.ok(r.logs.length >= 2);
  for (const l of r.logs) assert.doesNotMatch(l, /error|exception/i);  // the site smoke test fails on those
  // A failed load is retried by the next play.
  b.command(play(5, 'https://x/bad.wav'));
  await tick();
  assert.equal(env.fetches.filter((u) => u === 'https://x/bad.wav').length, 2);
  // UNLOAD drops the cache entry; playing voices keep their buffer.
  assert.equal(b.stats().sounds, 1);
  b.command(cmd(AudioOp.UNLOAD, { sound: 'https://x/music.ogg' }));
  assert.equal(b.stats().sounds, 0);
  assert.equal(b.stats().playing, 2);
  b.destroy();
});

test('STOP fades out, then ends STOPPED once; STOP of a loading voice ends at once', async () => {
  const env = mockEnv(FILES);
  const { b, r } = await unlocked(env);
  const ctx = b.context;
  b.command(play(1, 'https://x/music.ogg', { loop: true }));
  await tick();
  const src = ctx.sources[0];
  const gain = [...src.outputs][0];
  ctx.currentTime = 2;
  b.command(cmd(AudioOp.STOP, { voice: 1, fadeMs: 500 }));
  assert.deepEqual(gain.gain.log.slice(-2), [['hold', 2], ['ramp', 0, 2.5]]);
  assert.equal(src.stopped, 2.5);
  b.command(cmd(AudioOp.STOP, { voice: 1, fadeMs: 2000 }));  // later: ignored
  assert.equal(src.stopped, 2.5);
  b.command(cmd(AudioOp.SET, { voice: 1, volume: 0.2 }));     // does not undo the fade out
  assert.deepEqual(gain.gain.last, ['ramp', 0, 2.5]);
  assert.deepEqual(r.ended, []);
  src.end();
  assert.deepEqual(r.ended, [[1, STOPPED]]);
  assert.ok(gain.disconnected);

  b.command(play(2, 'https://x/tick.wav'));
  b.command(cmd(AudioOp.STOP, { voice: 2, fadeMs: 100 }));  // still loading
  assert.deepEqual(r.ended.at(-1), [2, STOPPED]);
  await tick();
  assert.equal(ctx.sources.length, 1, 'never started');
  // Immediate stop: reported at once, the source gets a short release.
  b.command(play(3, 'https://x/tick.wav'));
  await tick();
  const s3 = ctx.sources[1];
  b.command(cmd(AudioOp.STOP, { voice: 3 }));
  assert.deepEqual(r.ended.at(-1), [3, STOPPED]);
  assert.ok(s3.stopped > ctx.currentTime);
  s3.end();
  assert.equal(r.ended.length, 3);
  b.destroy();
});

test('PAUSE remembers the position, RESUME starts a new source there; SET ramps', async () => {
  const env = mockEnv(FILES);
  const { b, r } = await unlocked(env);
  const ctx = b.context;
  ctx.currentTime = 10;
  b.command(play(1, 'https://x/music.ogg', { loop: true, offset: 1, pitch: 2 }));
  await tick();
  const s1 = ctx.sources[0];
  assert.deepEqual(s1.started, { when: 0, offset: 1 });
  ctx.currentTime = 11.5;  // 1 + 1.5 s * 2 = 4 s in
  b.command(cmd(AudioOp.PAUSE, { voice: 1 }));
  assert.ok(s1.stopped > 11.5);
  s1.end();  // the paused source ending is not the voice ending
  assert.deepEqual(r.ended, []);
  ctx.currentTime = 30;
  b.command(cmd(AudioOp.RESUME, { voice: 1, pitch: 2 }));
  const s2 = ctx.sources[1];
  assert.deepEqual(s2.started, { when: 0, offset: 4 });
  // Wraps around a loop: 4 + 3 s * 2 = 10 -> 2 s into the 8 s sound.
  ctx.currentTime = 33;
  b.command(cmd(AudioOp.PAUSE, { voice: 1 }));
  b.command(cmd(AudioOp.RESUME, { voice: 1 }));
  assert.equal(ctx.sources[2].started.offset, 2);

  const s = ctx.sources[2];
  const gain = [...s.outputs][0], panner = [...gain.outputs][0];
  b.command(cmd(AudioOp.SET, { voice: 1, volume: 0.5, pitch: 1, pan: 1, fadeMs: 250 }));
  assert.deepEqual(gain.gain.last, ['ramp', 0.5, 33.25]);
  assert.deepEqual(panner.pan.last, ['ramp', 1, 33.25]);
  assert.deepEqual(s.playbackRate.last, ['ramp', 1, 33.25]);
  b.command(cmd(AudioOp.SET, { voice: 1, volume: 0.6, pitch: 1, pan: 1 }));
  assert.deepEqual(gain.gain.last, ['target', 0.6, 33, 0.004], 'instant changes are dezippered');
  // Pausing while still loading starts it paused.
  b.command(play(2, 'https://x/tick.wav'));
  b.command(cmd(AudioOp.PAUSE, { voice: 2 }));
  await tick();
  assert.equal(ctx.sources.length, 3);
  b.command(cmd(AudioOp.RESUME, { voice: 2 }));
  assert.equal(ctx.sources.length, 4);
  // Commands for ended or unknown voices are ignored.
  b.command(cmd(AudioOp.SET, { voice: 99, volume: 0 }));
  b.destroy();
});

test('buses: volume / mute ramps, STOP_ALL per bus and for all', async () => {
  const env = mockEnv(FILES);
  const { b, r } = backend(env);
  // Bus settings before the context exists apply when it is made.
  b.command(cmd(AudioOp.BUS, { bus: 'music', volume: 0.4 }));
  env.gesture();
  await tick();
  const ctx = b.context;
  b.command(play(1, 'https://x/music.ogg', { bus: 'music', loop: true }));
  b.command(play(2, 'https://x/tick.wav', { bus: 'ui', loop: true }));
  b.command(play(3, 'https://x/tick.wav', { bus: 'ui' }));
  await tick();
  const busOf = (src) => [...[...[...src.outputs][0].outputs][0].outputs][0];
  const music = busOf(ctx.sources[0]), ui = busOf(ctx.sources[1]);
  assert.equal(music.gain.value, 0.4);
  assert.equal(busOf(ctx.sources[2]), ui, 'one gain per bus');
  b.command(cmd(AudioOp.BUS, { bus: 'music', volume: 0.4, muted: true, fadeMs: 300 }));
  assert.deepEqual(music.gain.last, ['ramp', 0, 0.3]);
  b.command(cmd(AudioOp.BUS, { bus: 'music', volume: 0.8, muted: false }));
  assert.deepEqual(music.gain.last, ['target', 0.8, 0, 0.004]);
  b.command(cmd(AudioOp.STOP_ALL, { bus: 'ui' }));
  assert.deepEqual(r.ended.sort(), [[2, STOPPED], [3, STOPPED]]);
  b.command(cmd(AudioOp.STOP_ALL, { bus: null, fadeMs: 0 }));  // the JS context went away
  assert.deepEqual(r.ended.at(-1), [1, STOPPED]);
  assert.equal(b.stats().voices, 0);
  // Master volume (jsui.audio.setVolume / muted).
  b.setMaster(0.5, false, 100);
  const master = [...music.outputs][0];
  assert.deepEqual(master.gain.last, ['ramp', 0.5, 0.1]);
  b.destroy();
});

test('long files stream through an <audio> element instead of being decoded', async () => {
  const env = mockEnv(FILES);
  const { b, r } = await unlocked(env);
  const ctx = b.context;
  b.command(play(1, 'https://x/long.ogg', { bus: 'music', loop: true, offset: 3, pitch: 0.5 }));
  await tick();
  assert.ok(FILES['https://x/long.ogg'].cancelled, 'body not downloaded');
  const el = env.elements[0];
  assert.equal(el.src, 'https://x/long.ogg');
  assert.equal(el.crossOrigin, 'anonymous');
  assert.equal(el.loop, true);
  assert.equal(el.playbackRate, 0.5);
  assert.equal(el.preservesPitch, false);
  assert.equal(el.currentTime, 3);
  assert.equal(el.paused, false);
  const media = ctx.nodes.find((n) => n.kind === 'media');
  assert.equal(media.el, el);
  assert.equal([...media.outputs][0].kind, 'gain');
  b.command(cmd(AudioOp.PRELOAD, { sound: 'https://x/long.ogg' }));
  await tick();
  env.elements[1].onloadedmetadata();
  await tick();
  assert.deepEqual(r.loaded, [['https://x/long.ogg', true, 120]]);
  b.command(cmd(AudioOp.STOP, { voice: 1, fadeMs: 20 }));
  await new Promise((res) => setTimeout(res, 60));
  assert.deepEqual(r.ended, [[1, STOPPED]]);
  assert.equal(el.paused, true);
  b.destroy();
});

test('runtimes share the page context; destroy frees voices without reports', async () => {
  const env = mockEnv(FILES);
  const one = backend(env), two = backend(env);
  env.gesture();
  await tick();
  assert.equal(one.b.context, two.b.context);
  assert.equal(one.r.unlocks, 1);
  assert.equal(two.r.unlocks, 1);
  one.b.command(play(1, 'https://x/music.ogg', { loop: true }));
  two.b.command(play(1, 'https://x/music.ogg', { loop: true }));
  await tick();
  assert.equal(env.fetches.length, 1, 'one cache for the page');
  const ctx = one.b.context;
  one.b.destroy();
  assert.equal(ctx.closed, false);
  assert.deepEqual(one.r.ended, []);
  assert.ok(ctx.sources[0].stopped !== undefined);
  one.b.command(play(2, 'https://x/tick.wav'));  // after destroy: ignored
  two.b.destroy();
  assert.equal(ctx.closed, true);
  // A new runtime gets a new context.
  const three = await unlocked(env);
  assert.notEqual(three.b.context, ctx);
  three.b.destroy();
});

test('an AudioContext of the page\'s own is used and not closed', async () => {
  const env = mockEnv(FILES);
  const own = new MockContext({});
  own.state = 'running';
  const { b, r } = backend(env, { context: own });
  assert.equal(b.context, own);
  assert.equal(r.unlocks, 1);
  b.command(play(1, 'https://x/tick.wav'));
  await tick();
  assert.equal(own.sources.length, 1);
  b.destroy();
  assert.equal(own.closed, false);
});

// ------------------------------------------------------------ JsuiAudio (host API)

function fakeHost() {
  const calls = [];
  const events = [];
  const host = {
    rt: 1, _destroyed: false, calls, events,
    module: {
      _jsui_web_audio_voice_ended: (rt, v, s) => calls.push(['ended', v, s]),
      _jsui_audio_sound_loaded: (rt, url, ok, d) => calls.push(['loaded', url, ok, d]),
      _free: () => {},
    },
    _newString: (s) => s,
    _emit: (t, ...a) => events.push([t, ...a]),
    _log: (l, t) => events.push(['log', l, t]),
  };
  return host;
}

test('JsuiAudio: reports reach the runtime after the current task, not after destroy', async () => {
  const env = mockEnv(FILES);
  const host = fakeHost();
  const a = new JsuiAudio(host, { env });
  assert.equal(a.mode, 'webaudio');
  assert.equal(a.enabled, true);
  assert.equal(a.unlocked, false);
  a.command(play(1, 'https://x/tick.wav'));
  a.command(cmd(AudioOp.PRELOAD, { sound: 'https://x/tick.wav' }));
  assert.deepEqual(host.calls, [], 'never re-enters the runtime from inside on_audio');
  await tick();
  assert.deepEqual(host.calls, [['ended', 1, FAILED], ['loaded', 'https://x/tick.wav', 1, 1]]);
  env.gesture('touchend');
  await tick();
  assert.equal(a.unlocked, true);
  assert.ok(host.events.some((e) => e[0] === 'audiounlock'));
  assert.equal(a.context, a.backend.context);
  a.setVolume(0.25);
  a.muted = true;
  assert.equal(a.volume, 0.25);
  assert.equal(a.muted, true);
  assert.deepEqual(a.stats, { voices: 0, playing: 0, waiting: 0, sounds: 1 });
  a.voiceEnded(5, STOPPED);
  host._destroyed = true;
  host.rt = 0;
  await tick();
  assert.equal(host.calls.length, 2, 'dropped: the runtime is gone');
  a.destroy();
});

test('JsuiAudio: audio false is off, a function handles the commands itself', async () => {
  const host = fakeHost();
  const off = new JsuiAudio(host, false);
  assert.equal(off.enabled, false);
  off.destroy();

  const got = [];
  const a = new JsuiAudio(host, (c, audio) => {
    got.push(c.op);
    if (c.op === AudioOp.PLAY) audio.voiceEnded(c.voice, FINISHED);
    if (c.op === AudioOp.PRELOAD) audio.soundLoaded(c.sound, true, 2.5);
  });
  assert.equal(a.mode, 'custom');
  assert.equal(a.enabled, true);
  assert.equal(a.unlocked, true);
  a.command(play(7, 'https://x/a.wav'));
  a.command(cmd(AudioOp.PRELOAD, { sound: 'https://x/a.wav' }));
  a.command(cmd(AudioOp.BUS, { bus: 'ui', volume: 0 }));
  await tick();
  assert.deepEqual(got, [AudioOp.PLAY, AudioOp.PRELOAD, AudioOp.BUS]);
  assert.deepEqual(host.calls, [['ended', 7, FINISHED], ['loaded', 'https://x/a.wav', 1, 2.5]]);
  a.destroy();
});
