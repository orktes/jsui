// UI audio for web/jsui-web.js: plays the sounds the UI asks for (Sound.play,
// useSound, useMusic, <UISounds>, Pressable `sounds`; include/jsui/jsui.h
// "Audio", docs/AUDIO.md) with Web Audio.
//
// The runtime turns every request into a command (JsuiAudioCommand) and
// hands it to the host (jsui_web.cpp's on_audio trampoline -> onAudio); the
// host reports back the end of every voice (jsui_web_audio_voice_ended) and
// every preload (jsui_audio_sound_loaded). This module is that host side:
//
//   page        one AudioContext shared by every runtime on the page (closed
//               when the last one is destroyed) and one cache of decoded
//               sounds, keyed by URL (fetch + decodeAudioData, one load per
//               URL however many voices / runtimes ask).
//   per runtime a master GainNode -> the context's destination, and one
//               GainNode per bus ('ui', 'sfx', 'music', ... the UI's
//               Sound.setBusVolume / setBusMuted) -> master.
//   per voice   AudioBufferSourceNode (pitch = playbackRate, loop, offset)
//               -> GainNode (volume, fades) -> StereoPannerNode (pan) -> bus.
//               Long files (more than `streamBytes`, 2 MiB by default, by
//               their Content-Length: minutes of music) are not decoded into
//               memory; they stream through an <audio> element and a
//               MediaElementAudioSourceNode instead.
//
// Fades and volume changes are AudioParam ramps (linearRampToValueAtTime;
// "instant" changes use a 4 ms setTargetAtTime so they do not click).
// PAUSE remembers the position and RESUME starts a new source there.
//
// Autoplay: browsers keep audio silent until the page gets a user gesture.
// The AudioContext is created (or resumed) inside the first pointerdown /
// pointerup / mousedown / keydown / touchend / click on the page, and resumed
// again when the page becomes visible. Until then the runtime is "locked":
//
//   one-shot voices (UI sounds, effects) are dropped: they end at once as
//   FAILED (Sound.play(...).ended -> { reason: 'failed' }), so nothing plays
//   late and nothing piles up. Except right after a gesture (GESTURE_GRACE
//   ms) while the context is starting: the click that unlocks audio still
//   gets its press sound.
//   looping voices (music, ambience: useMusic) wait and start, with their
//   fade in, once audio unlocks. Their sound is fetched only then.
//   PRELOAD decodes at once (with an OfflineAudioContext), so UI sounds are
//   ready for the first gesture.
//
// Gamepad input is not a user activation in any browser: a player who only
// uses a controller hears nothing until they click, tap or press a key. Pages
// can call jsui.audio.unlock() from their own gesture handlers ("Press any
// key" screens) and listen for jsui.on('audiounlock', ...).
//
// Without Web Audio (Node, old browsers) every voice fails and every preload
// reports not loaded; headless Node hosts create the runtime with audio off.
//
// Commands for a runtime can go elsewhere instead (createJsui({ audio })):
// `false` (silence: the runtime is created with audio_disabled), or a
// function (cmd, audio) like the C API's on_audio, which must report through
// audio.voiceEnded(voice, status) / audio.soundLoaded(url, ok, duration).

export const AudioOp = Object.freeze({
  PRELOAD: 0, UNLOAD: 1, PLAY: 2, STOP: 3, PAUSE: 4, RESUME: 5, SET: 6, BUS: 7, STOP_ALL: 8,
});
export const AudioStatus = Object.freeze({ FINISHED: 0, STOPPED: 1, FAILED: -1 });
const { FINISHED, STOPPED, FAILED } = AudioStatus;

const STREAM_BYTES = 2 << 20;
const DEZIPPER = 0.004;   // s, time constant of "instant" gain changes
const RELEASE = 0.012;    // s, fade of an immediate stop / pause (no click)
const GESTURES = ['pointerdown', 'pointerup', 'mousedown', 'keydown', 'touchend', 'click'];
const GESTURE_GRACE = 400;  // ms

/** What this module uses from the browser (tests pass their own). */
export function browserEnv() {
  const g = globalThis;
  return {
    AudioContext: g.AudioContext ?? g.webkitAudioContext ?? null,
    OfflineAudioContext: g.OfflineAudioContext ?? g.webkitOfflineAudioContext ?? null,
    fetch: g.fetch ? (url) => g.fetch(url) : null,
    target: typeof g.addEventListener === 'function' ? g : null,       // gesture events
    document: typeof g.document !== 'undefined' ? g.document : null,  // visibilitychange
    createAudio: typeof g.Audio === 'function' ? () => new g.Audio() : null,
    hasBeenActive: () => !!(g.navigator && g.navigator.userActivation && g.navigator.userActivation.hasBeenActive),
    now: () => (g.performance ? g.performance.now() : Date.now()),
  };
}

let defaultEnv = null;
const pages = new Map();  // env or external AudioContext -> AudioPage

// ------------------------------------------------------------------- page

/** The page's audio: one AudioContext and one sound cache for every runtime. */
class AudioPage {
  constructor(env, key, opts) {
    this.env = env;
    this.key = key;
    this.ctx = opts.context ?? null;
    this.external = !!opts.context;
    this.streamBytes = opts.streamBytes ?? STREAM_BYTES;
    this.backends = new Set();
    this.cache = new Map();  // url -> { holders: Set<backend>, ready: Promise, value }
    this._offline = null;
    this._running = false;
    this._gestureAt = -Infinity;
    this._abort = null;
    if (this.ctx) this._watch();
  }

  get supported() { return !!(this.ctx || this.env.AudioContext); }
  now() { return this.env.now ? this.env.now() : Date.now(); }
  // A gesture just happened: the context is starting (one-shots may wait for it).
  get starting() { return !!this.ctx && this.now() - this._gestureAt < GESTURE_GRACE; }
  get unlocked() { return !!this.ctx && this.ctx.state === 'running'; }

  attach(backend) {
    this.backends.add(backend);
    if (this.unlocked) backend._unlocked();  // a runtime made after the gesture
    if (this.backends.size > 1) return;
    const { target, document } = this.env;
    if (typeof AbortController !== 'undefined') this._abort = new AbortController();
    const o = { capture: true, passive: true, signal: this._abort?.signal };
    this._onGesture = () => {
      if (this.unlocked) return;
      this._gestureAt = this.now();
      this.unlock();
    };
    if (target) for (const t of GESTURES) target.addEventListener(t, this._onGesture, o);
    this._onVisible = () => {
      if (document.visibilityState === 'visible' && this.ctx && this.ctx.state !== 'running') this._resume();
    };
    if (document && document.addEventListener) document.addEventListener('visibilitychange', this._onVisible, o);
    // A page that already had a gesture (the runtime was made after a click)
    // may start at once; otherwise the context waits for one, which also
    // keeps Chrome's "AudioContext was not allowed to start" warning away.
    if (this.env.hasBeenActive && this.env.hasBeenActive()) this.unlock();
  }

  detach(backend) {
    this.backends.delete(backend);
    for (const [url, e] of this.cache) {
      e.holders.delete(backend);
      if (!e.holders.size) this.cache.delete(url);
    }
    if (this.backends.size) return;
    if (this._abort) this._abort.abort();
    else {
      const { target, document } = this.env;
      if (target) for (const t of GESTURES) target.removeEventListener(t, this._onGesture, true);
      if (document && document.removeEventListener) document.removeEventListener('visibilitychange', this._onVisible, true);
    }
    if (this.ctx && !this.external && this.ctx.state !== 'closed') this.ctx.close().catch(() => {});
    this.ctx = null;
    this.cache.clear();
    pages.delete(this.key);
  }

  /** Creates / resumes the AudioContext; call it from a user gesture. */
  unlock() {
    if (this.unlocked) return;
    if (!this.ctx) {
      if (!this.env.AudioContext) return;
      try {
        this.ctx = new this.env.AudioContext({ latencyHint: 'interactive' });
      } catch (e) {
        this.ctx = null;
        return;
      }
      this._watch();
    }
    this._resume();
  }

  _resume() {
    const ctx = this.ctx;
    if (!ctx || ctx.state === 'closed') return;
    if (ctx.state !== 'running') {
      const p = ctx.resume();
      if (p && p.then) p.then(() => this._stateChanged(), () => {});
    }
    this._stateChanged();
  }

  _watch() {
    const ctx = this.ctx;
    const on = () => { if (this.ctx === ctx) this._stateChanged(); };
    if (ctx.addEventListener) ctx.addEventListener('statechange', on);
    else ctx.onstatechange = on;
    this._stateChanged();
  }

  _stateChanged() {
    const running = this.unlocked;
    if (running === this._running) return;
    this._running = running;
    if (running) for (const b of [...this.backends]) b._unlocked();
  }

  // decodeAudioData before there is an AudioContext: an offline one (no gesture needed).
  _decoder() {
    if (this.ctx) return this.ctx;
    if (!this._offline && this.env.OfflineAudioContext) this._offline = new this.env.OfflineAudioContext(1, 1, 48000);
    return this._offline;
  }

  /** Loads `url` for `backend`: { kind: 'buffer', buffer, duration } or
   * { kind: 'stream', url, duration: -1 }. One load per URL. */
  load(url, backend) {
    let e = this.cache.get(url);
    if (!e) {
      e = { holders: new Set(), value: null, ready: null };
      e.ready = this._fetch(url).then((v) => (e.value = v), (err) => {
        if (this.cache.get(url) === e) this.cache.delete(url);  // a later play tries again
        throw err;
      });
      this.cache.set(url, e);
    }
    e.holders.add(backend);
    return e.ready;
  }

  unload(url, backend) {
    const e = this.cache.get(url);
    if (!e) return;
    e.holders.delete(backend);
    if (!e.holders.size) this.cache.delete(url);  // voices keep their buffer
  }

  async _fetch(url) {
    const { fetch } = this.env;
    if (!fetch) throw new Error('no fetch');
    let res;
    try {
      res = await fetch(url);
    } catch (e) {
      throw new Error('network');
    }
    if (!res.ok) throw new Error('HTTP ' + res.status);
    const len = +(res.headers && res.headers.get('content-length')) || 0;
    if (len > this.streamBytes && this.env.createAudio) {
      try { res.body && res.body.cancel(); } catch { /* not started */ }
      return { kind: 'stream', url, duration: -1 };
    }
    const bytes = await res.arrayBuffer();
    const dec = this._decoder();
    if (!dec) throw new Error('no decoder');
    let buffer;
    try {
      buffer = await new Promise((resolve, reject) => {
        const p = dec.decodeAudioData(bytes, resolve, reject);
        if (p && p.then) p.then(resolve, reject);
      });
    } catch (e) {
      throw new Error('could not decode');
    }
    return { kind: 'buffer', buffer, duration: buffer.duration };
  }

  // The duration of a streamed sound, from its metadata (PRELOAD answers).
  probeDuration(url) {
    return new Promise((resolve) => {
      const el = this.env.createAudio();
      const done = (d) => {
        el.onloadedmetadata = el.onerror = null;
        el.removeAttribute?.('src');
        resolve(d);
      };
      el.onloadedmetadata = () => done(isFinite(el.duration) ? el.duration : -1);
      el.onerror = () => done(-1);
      el.crossOrigin = 'anonymous';
      el.preload = 'metadata';
      el.src = url;
    });
  }
}

function pageFor(env, opts) {
  const key = opts.context ?? env;
  let p = pages.get(key);
  if (!p) pages.set(key, (p = new AudioPage(env, key, opts)));
  return p;
}

// Ramps an AudioParam from where it is now to `value` over `ms` (0: a few ms).
function ramp(param, value, ctx, ms) {
  const now = ctx.currentTime;
  if (param.cancelAndHoldAtTime) param.cancelAndHoldAtTime(now);
  else {
    const cur = param.value;
    param.cancelScheduledValues(now);
    param.setValueAtTime(cur, now);
  }
  if (ms > 0) param.linearRampToValueAtTime(value, now + ms / 1000);
  else param.setTargetAtTime(value, now, DEZIPPER);
}

// ---------------------------------------------------------------- backend

/** Web Audio for one runtime. `report`: { voiceEnded(voice, status),
 * soundLoaded(url, ok, duration), unlocked(), log(level, text) }. */
export class WebAudioBackend {
  constructor(report, opts = {}) {
    this.report = report;
    this.env = opts.env ?? (defaultEnv ??= browserEnv());
    this.page = pageFor(this.env, opts);
    this.voices = new Map();  // id -> voice
    this.buses = new Map();   // name -> { volume, muted, node }
    this.master = { volume: 1, muted: false, node: null };
    this.destroyed = false;
    this.page.attach(this);
  }

  get unlocked() { return this.page.unlocked; }
  get context() { return this.page.ctx; }

  command(c) {
    if (this.destroyed) return;
    switch (c.op) {
      case AudioOp.PLAY: return this._play(c);
      case AudioOp.STOP: return this._withVoice(c, (v) => this._stop(v, c.fadeMs));
      case AudioOp.PAUSE: return this._withVoice(c, (v) => this._pause(v));
      case AudioOp.RESUME: return this._withVoice(c, (v) => this._resume(v));
      case AudioOp.SET: return this._withVoice(c, (v) => this._set(v, c));
      case AudioOp.BUS: return this._bus(c);
      case AudioOp.STOP_ALL:
        for (const v of [...this.voices.values()]) if (c.bus == null || v.bus === c.bus) this._stop(v, c.fadeMs);
        return;
      case AudioOp.PRELOAD: return this._preload(c.sound);
      case AudioOp.UNLOAD: return this.page.unload(c.sound, this);
      default:
    }
  }

  /** The runtime's master volume (multiplies every bus). */
  setMaster(volume, muted, fadeMs = 0) {
    this.master.volume = volume;
    this.master.muted = muted;
    const ctx = this.page.ctx;
    if (this.master.node && ctx) ramp(this.master.node.gain, muted ? 0 : volume, ctx, fadeMs);
  }

  stats() {
    let playing = 0, waiting = 0;
    for (const v of this.voices.values()) {
      if (v.state === 'locked' || v.state === 'loading') waiting++;
      else playing++;
    }
    return { voices: this.voices.size, playing, waiting, sounds: this.page.cache.size };
  }

  destroy() {
    if (this.destroyed) return;
    this.destroyed = true;
    for (const v of this.voices.values()) this._release(v, 0);
    this.voices.clear();
    for (const b of this.buses.values()) b.node && b.node.disconnect();
    this.master.node && this.master.node.disconnect();
    this.buses.clear();
    this.master.node = null;
    this.page.detach(this);
  }

  // ------------------------------------------------------------ voices

  _withVoice(c, fn) {
    const v = this.voices.get(c.voice);
    if (v && v.state !== 'ended') fn(v);
  }

  _play(c) {
    const v = {
      id: c.voice, url: c.sound, bus: c.bus || 'sfx', volume: c.volume, pitch: c.pitch, pan: c.pan,
      loop: !!c.loop, fadeMs: c.fadeMs, offset: c.offset || 0, paused: false, stopping: false,
      state: 'locked', sound: null, src: null, el: null, elNode: null, gain: null, panner: null,
      startPos: 0, startedAt: 0, rate: c.pitch, position: c.offset || 0, timer: 0, deadline: 0,
    };
    this.voices.set(v.id, v);
    if (!this.page.supported) return this._end(v, FAILED);
    if (!this.page.unlocked) {
      // Locked by the autoplay policy: loops wait, one-shots are dropped
      // unless the context is starting after a gesture.
      if (v.loop) return;
      if (!this.page.starting) return this._end(v, FAILED);
      v.deadline = this.page.now() + GESTURE_GRACE;
      v.timer = setTimeout(() => { if (v.state === 'locked') this._end(v, FAILED); }, GESTURE_GRACE);
      return;
    }
    this._load(v);
  }

  _load(v) {
    clearTimeout(v.timer);
    v.state = 'loading';
    this.page.load(v.url, this).then((sound) => {
      if (v.state !== 'loading' || this.destroyed) return;
      v.sound = sound;
      this._start(v);
    }, (err) => {
      if (v.state !== 'loading' || this.destroyed) return;
      this.report.log(2, `audio: could not load ${v.url} (${err && err.message})`);
      this._end(v, FAILED);
    });
  }

  // Audio unlocked: the graph exists now, waiting loops start.
  _unlocked() {
    if (this.destroyed) return;
    this._graph();
    const now = this.page.now();
    for (const v of [...this.voices.values()]) {
      if (v.state !== 'locked') continue;
      if (!v.loop && !(now <= v.deadline)) this._end(v, FAILED);
      else this._load(v);
    }
    this.report.unlocked();
  }

  _graph() {
    const ctx = this.page.ctx;
    if (!ctx) return null;
    if (!this.master.node) {
      const m = (this.master.node = ctx.createGain());
      m.gain.value = this.master.muted ? 0 : this.master.volume;
      m.connect(ctx.destination);
    }
    return ctx;
  }

  _busNode(name) {
    let b = this.buses.get(name);
    if (!b) this.buses.set(name, (b = { volume: 1, muted: false, node: null }));
    const ctx = this._graph();
    if (!b.node && ctx) {
      b.node = ctx.createGain();
      b.node.gain.value = b.muted ? 0 : b.volume;
      b.node.connect(this.master.node);
    }
    return b.node;
  }

  _start(v) {
    const ctx = this._graph();
    if (!ctx) return this._end(v, FAILED);
    v.gain = ctx.createGain();
    const now = ctx.currentTime;
    if (v.fadeMs > 0) {
      v.gain.gain.setValueAtTime(0, now);
      v.gain.gain.linearRampToValueAtTime(v.volume, now + v.fadeMs / 1000);
    } else v.gain.gain.value = v.volume;
    let out = v.gain;
    if (ctx.createStereoPanner) {
      v.panner = ctx.createStereoPanner();
      v.panner.pan.value = v.pan;
      v.gain.connect(v.panner);
      out = v.panner;
    }
    out.connect(this._busNode(v.bus));
    if (v.paused) {
      v.state = 'paused';
      v.position = v.offset;
      return;
    }
    this._startSource(v, v.offset);
  }

  _startSource(v, position) {
    const ctx = this.page.ctx;
    v.state = 'playing';
    if (v.sound.kind === 'stream') return this._startStream(v, position);
    const buf = v.sound.buffer, d = buf.duration;
    const src = ctx.createBufferSource();
    src.buffer = buf;
    src.loop = v.loop;
    src.playbackRate.value = v.pitch;
    src.connect(v.gain);
    src.onended = () => {
      if (v.src !== src) return;  // replaced (pause / resume)
      v.src = null;
      this._end(v, v.stopping ? STOPPED : FINISHED);
    };
    const offset = v.loop && d > 0 ? position % d : Math.min(position, d);
    v.src = src;
    v.startPos = offset;
    v.startedAt = ctx.currentTime;
    v.rate = v.pitch;
    src.start(0, offset);
  }

  _startStream(v, position) {
    const ctx = this.page.ctx;
    let el = v.el;
    if (!el) {
      el = v.el = this.env.createAudio();
      el.crossOrigin = 'anonymous';  // else the element source outputs silence
      el.preload = 'auto';
      el.src = v.url;
      el.onended = () => { if (!v.loop) this._end(v, v.stopping ? STOPPED : FINISHED); };
      el.onerror = () => {
        this.report.log(2, `audio: could not stream ${v.url}`);
        this._end(v, FAILED);
      };
      v.elNode = ctx.createMediaElementSource(el);
      v.elNode.connect(v.gain);
    }
    el.loop = v.loop;
    el.preservesPitch = false;
    el.playbackRate = v.pitch;
    if (position > 0 || el.currentTime > 0) el.currentTime = position;
    const p = el.play();
    // Refused (autoplay, unsupported source); AbortError is a pause() of ours.
    if (p && p.catch) p.catch((e) => {
      if (v.el === el && v.state === 'playing' && !(e && e.name === 'AbortError')) this._end(v, FAILED);
    });
  }

  // Seconds into the sound.
  _position(v) {
    if (v.el) return v.el.currentTime;
    if (!v.src) return v.position;
    const p = v.startPos + (this.page.ctx.currentTime - v.startedAt) * v.rate;
    const d = v.sound.buffer.duration;
    return v.loop && d > 0 ? p % d : Math.min(p, d);
  }

  _pause(v) {
    if (v.paused) return;
    v.paused = true;
    if (v.state !== 'playing' || v.stopping) return;
    v.position = this._position(v);
    v.state = 'paused';
    const ctx = this.page.ctx;
    ramp(v.gain.gain, 0, ctx, RELEASE * 1000);
    if (v.el) {
      const el = v.el;
      v.timer = setTimeout(() => { if (v.state === 'paused') el.pause(); }, RELEASE * 1000);
    } else if (v.src) {
      const src = v.src;
      v.src = null;
      src.onended = () => src.disconnect();
      try { src.stop(ctx.currentTime + RELEASE); } catch { /* already stopped */ }
    }
  }

  _resume(v) {
    if (!v.paused) return;
    v.paused = false;
    if (v.state !== 'paused') return;  // still loading / locked: starts unpaused
    clearTimeout(v.timer);
    const ctx = this.page.ctx;
    v.gain.gain.cancelScheduledValues(ctx.currentTime);
    v.gain.gain.setValueAtTime(0, ctx.currentTime);
    v.gain.gain.linearRampToValueAtTime(v.volume, ctx.currentTime + RELEASE);
    this._startSource(v, v.position);
  }

  _set(v, c) {
    const ctx = this.page.ctx, fade = c.fadeMs;
    if (c.volume !== v.volume) {
      v.volume = c.volume;
      if (v.gain && v.state === 'playing' && !v.stopping) ramp(v.gain.gain, v.volume, ctx, fade);
    }
    if (c.pan !== v.pan) {
      v.pan = c.pan;
      if (v.panner) ramp(v.panner.pan, v.pan, ctx, fade);
    }
    if (c.pitch !== v.pitch) {
      if (v.src) {
        // The position estimate takes the new rate from now on (a ramp's
        // own duration is not integrated).
        v.startPos = this._position(v);
        v.startedAt = ctx.currentTime;
        v.rate = c.pitch;
        ramp(v.src.playbackRate, c.pitch, ctx, fade);
      } else if (v.el) v.el.playbackRate = c.pitch;
      v.pitch = c.pitch;
    }
  }

  _stop(v, fadeMs) {
    if (v.state === 'ended') return;
    const ctx = this.page.ctx;
    if (v.state !== 'playing' || !(fadeMs > 0)) return this._end(v, STOPPED);
    const at = ctx.currentTime + fadeMs / 1000;
    if (v.stopping && v.stopAt <= at) return;  // already ending sooner
    v.stopping = true;
    v.stopAt = at;
    ramp(v.gain.gain, 0, ctx, fadeMs);
    if (v.src) {
      try { v.src.stop(at); } catch { /* stopped */ }  // onended -> STOPPED
    } else {
      clearTimeout(v.timer);
      v.timer = setTimeout(() => this._end(v, STOPPED), fadeMs);
    }
  }

  // Ends voice `v` (reported once) and frees its nodes.
  _end(v, status) {
    if (v.state === 'ended') return;
    v.state = 'ended';
    this.voices.delete(v.id);
    this._release(v, RELEASE);
    this.report.voiceEnded(v.id, status);
  }

  // Stops a voice's sources (with a short release unless `release` is 0) and disconnects its nodes.
  _release(v, release) {
    v.state = 'ended';
    clearTimeout(v.timer);
    const ctx = this.page.ctx;
    const nodes = [v.gain, v.panner, v.elNode].filter(Boolean);
    const free = () => nodes.forEach((n) => n.disconnect());
    const fade = release > 0 && ctx && v.gain && ctx.state === 'running';
    if (fade) ramp(v.gain.gain, 0, ctx, release * 1000);
    if (v.src) {
      const src = v.src;
      v.src = null;
      src.onended = () => { src.disconnect(); free(); };
      try {
        src.stop(fade ? ctx.currentTime + release : 0);
      } catch {
        src.disconnect();
        free();
      }
    } else if (v.el) {
      const el = v.el;
      v.el = null;
      el.onended = el.onerror = null;
      const done = () => {
        el.pause();
        el.removeAttribute?.('src');
        el.load?.();
        free();
      };
      if (fade) setTimeout(done, release * 1000);
      else done();
    } else free();
  }

  // ----------------------------------------------------------- buses, loads

  _bus(c) {
    let b = this.buses.get(c.bus);
    if (!b) this.buses.set(c.bus, (b = { volume: 1, muted: false, node: null }));
    b.volume = c.volume;
    b.muted = !!c.muted;
    const ctx = this.page.ctx;
    if (b.node && ctx) ramp(b.node.gain, b.muted ? 0 : b.volume, ctx, c.fadeMs);
  }

  _preload(url) {
    if (!this.page.supported) return this.report.soundLoaded(url, false, -1);
    this.page.load(url, this).then(async (s) => {
      const d = s.kind === 'stream' ? await this.page.probeDuration(url) : s.duration;
      if (!this.destroyed) this.report.soundLoaded(url, true, d);
    }, (err) => {
      if (this.destroyed) return;
      this.report.log(2, `audio: could not load ${url} (${err && err.message})`);
      this.report.soundLoaded(url, false, -1);
    });
  }
}

// ------------------------------------------------------------ host API

/**
 * `jsui.audio`: the runtime's audio. `options` (createJsui({ audio })):
 *   undefined / true / {...}  Web Audio. Object fields: context (an
 *                             AudioContext of the page's own to play into;
 *                             it is not closed), streamBytes (files larger
 *                             than this stream through an <audio> element
 *                             instead of being decoded; default 2 MiB).
 *   false                     no sound (the runtime is made with audio off).
 *   function (cmd, audio)     a handler of your own (see the file comment).
 */
export class JsuiAudio {
  constructor(host, options) {
    this.host = host;
    this._queue = [];
    this._flushing = false;
    this._volume = 1;
    this._muted = false;
    this.backend = null;
    this.handler = null;
    if (options === false) this.mode = 'off';
    else if (typeof options === 'function') {
      this.mode = 'custom';
      this.handler = options;
    } else {
      const opts = options && typeof options === 'object' ? options : {};
      const env = opts.env ?? (defaultEnv ??= browserEnv());
      // No Web Audio (Node): the runtime gets audio off, like `false`.
      if (!opts.context && !env.AudioContext) this.mode = 'off';
      else {
        this.mode = 'webaudio';
        this.backend = new WebAudioBackend({
          voiceEnded: (id, status) => this.voiceEnded(id, status),
          soundLoaded: (url, ok, d) => this.soundLoaded(url, ok, d),
          unlocked: () => host._emit('audiounlock'),
          log: (level, text) => host._log(level, text),
        }, { ...opts, env });
      }
    }
  }

  /** True when the runtime should be created with audio on. */
  get enabled() { return this.mode !== 'off'; }

  /** Whether sound can play now (Web Audio: the page had its user gesture). */
  get unlocked() {
    if (this.mode === 'custom') return true;
    return !!this.backend && this.backend.unlocked;
  }

  /** The AudioContext (null until a user gesture, and without Web Audio). */
  get context() { return this.backend ? this.backend.context : null; }

  /** Creates / resumes the AudioContext. Call it from a user gesture handler of
   * your own (the host already listens for pointer and key events). */
  unlock() { if (this.backend) this.backend.page.unlock(); }

  get volume() { return this._volume; }
  set volume(v) { this.setVolume(v); }
  /** The runtime's master volume (linear gain, multiplies every bus); `fade` ms ramps. */
  setVolume(volume, { fade = 0 } = {}) {
    this._volume = Math.max(0, +volume || 0);
    this.backend && this.backend.setMaster(this._volume, this._muted, fade);
  }

  get muted() { return this._muted; }
  set muted(m) { this.setMuted(m); }
  setMuted(muted, { fade = 0 } = {}) {
    this._muted = !!muted;
    this.backend && this.backend.setMaster(this._volume, this._muted, fade);
  }

  /** { voices, playing, waiting, sounds } (Web Audio), for tests and debugging. */
  get stats() { return this.backend ? this.backend.stats() : null; }

  // From the runtime (jsui_web.cpp's on_audio trampoline).
  command(cmd) {
    if (this.handler) {
      try {
        this.handler(cmd, this);
      } catch (e) {
        console.error(e);
      }
    } else if (this.backend) this.backend.command(cmd);
  }

  /** Reports: voice `voice` ended (AudioStatus). Custom handlers call these;
   * delivered after the current task (never re-entering the runtime). */
  voiceEnded(voice, status) {
    this._report([0, voice, status]);
  }

  /** Reports: sound `url` loaded (ok, duration in seconds, < 0 unknown) or not. */
  soundLoaded(url, ok, duration = -1) {
    this._report([1, url, ok ? 1 : 0, duration]);
  }

  _report(r) {
    this._queue.push(r);
    if (this._flushing) return;
    this._flushing = true;
    queueMicrotask(() => this._flush());
  }

  _flush() {
    this._flushing = false;
    const q = this._queue;
    this._queue = [];
    const h = this.host, M = h.module;
    if (h._destroyed || !h.rt) return;
    for (const r of q) {
      if (r[0] === 0) M._jsui_web_audio_voice_ended(h.rt, r[1], r[2]);
      else {
        const p = h._newString(String(r[1]));
        try {
          M._jsui_audio_sound_loaded(h.rt, p, r[2], typeof r[3] === 'number' && isFinite(r[3]) ? r[3] : -1);
        } finally {
          M._free(p);
        }
      }
    }
  }

  destroy() {
    this._queue = [];
    if (this.backend) this.backend.destroy();
    this.backend = null;
  }
}
