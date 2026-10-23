// UI audio: sounds the UI asks for, played by the host (include/jsui/jsui.h
// "Audio", docs/AUDIO.md). The runtime hands every request to the host's
// audio handler (Godot: AudioStreamPlayers on its buses; the browser: Web
// Audio; otherwise the native library's built-in miniaudio backend).
//
//   const voice = Sound.play(require('./ui/confirm.wav'), { bus: 'ui', volume: 0.8 });
//   voice.stop({ fade: 200 });  voice.setVolume(0.5, { fade: 300 });  await voice.ended;
//   Sound.preload(src) -> Promise<{ duration }>     Sound.setBusVolume('music', 0.4, { fade: 500 })
//   const click = useSound(require('./click.wav'), { volume: 0.7 });  click();  // preloaded on mount
//   useMusic(require('./menu.ogg'), { fadeIn: 1500, fadeOut: 800 });     // plays while mounted, crossfades
//   <Pressable sounds={{ hoverIn: tick, press: confirm }} />           // per element
//   <UISounds sounds={{ focus: tick, press: confirm }}>...</UISounds>  // defaults for a screen
//
// Sources are URLs (relative ones resolve against the bundle; imported audio
// files evaluate to one) or { uri }. Without the native bridge (Node tests,
// react-native-web) every voice ends at once.

import { createContext, createElement as h, useContext, useEffect, useMemo, useRef } from 'react';
import { native } from './native.js';

const g = globalThis;
const hasAudio = typeof native.audioPlay === 'function';

// JsuiAudioOp
const OP_STOP = 3;
const OP_PAUSE = 4;
const OP_RESUME = 5;
const OP_SET = 6;

export const SoundEndReason = Object.freeze({ finished: 'finished', stopped: 'stopped', failed: 'failed' });
const REASONS = { 0: 'finished', 1: 'stopped', '-1': 'failed' };

function sourceUrl(source) {
  if (source == null || source === false) return null;
  if (typeof source === 'string') return source || null;
  if (typeof source === 'object' && typeof source.uri === 'string') return source.uri || null;
  return null;
}

function num(v, fallback) {
  const n = v == null ? fallback : +v;
  return n === n ? n : fallback;
}

function fadeOf(opts) {
  return opts ? Math.max(0, num(opts.fade, 0)) : 0;
}

function reportError(e) {
  if (typeof g.__jsui_reportError === 'function') g.__jsui_reportError(e);
  else setTimeout(() => {
    throw e;
  }, 0);
}

const voices = new Map(); // id -> SoundInstance (until it ends)
const loads = new Map(); // absolute url -> { promise, resolve, info }
const buses = new Map(); // name -> { volume, muted }
let localId = 1;

/** A playing (or paused) sound. Ends exactly once: `ended` resolves with
 * { reason: 'finished' | 'stopped' | 'failed' }. */
export class SoundInstance {
  constructor(id, opts) {
    this.id = id;
    this.bus = opts.bus;
    this.loop = opts.loop;
    this.volume = opts.volume;
    this.pitch = opts.pitch;
    this.pan = opts.pan;
    this.paused = false;
    this.playing = true;
    this.endReason = null;
    this._listeners = null;
    this.ended = new Promise((resolve) => {
      this._resolve = resolve;
    });
  }

  /** Stops the voice; `fade` ms fades it out first. */
  stop(opts) {
    if (!this.playing) return;
    if (hasAudio) native.audioVoice(OP_STOP, this.id, this.volume, this.pitch, this.pan, fadeOf(opts));
    else this._end(1);
  }

  pause() {
    if (!this.playing || this.paused) return;
    this.paused = true;
    if (hasAudio) native.audioVoice(OP_PAUSE, this.id, this.volume, this.pitch, this.pan, 0);
  }

  resume() {
    if (!this.playing || !this.paused) return;
    this.paused = false;
    if (hasAudio) native.audioVoice(OP_RESUME, this.id, this.volume, this.pitch, this.pan, 0);
  }

  /** Linear gain (1: as recorded); `fade` ms ramps to it. */
  setVolume(volume, opts) {
    this._set('volume', Math.max(0, num(volume, 1)), opts);
  }

  /** Playback rate (1: normal; pitch and speed change together). */
  setPitch(pitch, opts) {
    this._set('pitch', Math.max(0.01, num(pitch, 1)), opts);
  }

  /** -1 left .. 1 right. */
  setPan(pan, opts) {
    this._set('pan', Math.max(-1, Math.min(1, num(pan, 0))), opts);
  }

  /** cb({ reason }) when the voice ends (at once if it already has). Returns an unsubscribe function. */
  onEnd(cb) {
    if (this.endReason) {
      cb({ reason: this.endReason });
      return () => {};
    }
    (this._listeners ||= new Set()).add(cb);
    return () => this._listeners && this._listeners.delete(cb);
  }

  _set(key, value, opts) {
    if (this[key] === value || !this.playing) return;
    this[key] = value;
    if (hasAudio) native.audioVoice(OP_SET, this.id, this.volume, this.pitch, this.pan, fadeOf(opts));
  }

  _end(status) {
    if (!this.playing) return;
    this.playing = false;
    voices.delete(this.id);
    this.endReason = REASONS[status] || 'failed';
    const info = { reason: this.endReason };
    this._resolve(info);
    const ls = this._listeners;
    this._listeners = null;
    if (ls) for (const cb of ls) {
      try {
        cb(info);
      } catch (e) {
        reportError(e);
      }
    }
  }
}

function playOptions(opts, defaults) {
  const o = Object.assign({}, defaults, opts);
  return {
    bus: o.bus ? String(o.bus) : 'sfx',
    volume: Math.max(0, num(o.volume, 1)),
    pitch: Math.max(0.01, num(o.pitch, 1)),
    pan: Math.max(-1, Math.min(1, num(o.pan, 0))),
    loop: !!o.loop,
    fadeIn: Math.max(0, num(o.fadeIn, 0)),
    offset: Math.max(0, num(o.offset, 0)),
  };
}

function play(source, opts, defaults) {
  const url = sourceUrl(source);
  const o = playOptions(opts, defaults);
  if (!url || !hasAudio) {
    const v = new SoundInstance(-localId++, o);
    if (o.loop && url) {
      voices.set(v.id, v);  // a silent loop still runs until stopped
    } else {
      Promise.resolve().then(() => v._end(url ? 0 : -1));
    }
    return v;
  }
  const id = native.audioPlay(url, o.bus, o.volume, o.pitch, o.pan, o.loop, o.fadeIn, o.offset);
  const v = new SoundInstance(id, o);
  voices.set(id, v);
  return v;
}

function preload(source) {
  const url = sourceUrl(source);
  if (!url) return Promise.reject(new Error('Sound.preload: no source'));
  if (!hasAudio) return Promise.resolve({ duration: null });
  const abs = native.audioResolve(url);
  const known = loads.get(abs);
  if (known) return known.promise;
  const entry = { info: null };
  entry.promise = new Promise((resolve, reject) => {
    entry.resolve = resolve;
    entry.reject = reject;
  });
  loads.set(abs, entry);
  native.audioPreload(url);
  return entry.promise;
}

function busState(bus) {
  let b = buses.get(bus);
  if (!b) buses.set(bus, (b = { volume: 1, muted: false }));
  return b;
}

function sendBus(bus, b, opts) {
  if (hasAudio) native.audioBus(bus, b.volume, b.muted, fadeOf(opts));
}

export const Sound = {
  /** Plays `source`. Options: bus ('sfx'), volume (1), pitch (1), pan (0),
   * loop (false), fadeIn (ms), offset (seconds). Returns a SoundInstance. */
  play(source, opts) {
    return play(source, opts);
  },
  /** Loads / decodes ahead of time. Resolves { duration } (seconds, or null
   * when unknown); rejects when the sound cannot be loaded. */
  preload,
  /** The host may free the sound (playing voices finish). */
  unload(source) {
    const url = sourceUrl(source);
    if (!url || !hasAudio) return;
    loads.delete(native.audioResolve(url));
    native.audioUnload(url);
  },
  setBusVolume(bus, volume, opts) {
    const b = busState(String(bus));
    b.volume = Math.max(0, num(volume, 1));
    sendBus(String(bus), b, opts);
  },
  setBusMuted(bus, muted, opts) {
    const b = busState(String(bus));
    b.muted = !!muted;
    sendBus(String(bus), b, opts);
  },
  /** { volume, muted } as the UI set it (buses start at 1, unmuted). */
  getBus(bus) {
    const b = buses.get(String(bus));
    return { volume: b ? b.volume : 1, muted: b ? b.muted : false };
  },
  /** Stops every voice (opts.bus: of one bus), fading out over opts.fade ms. */
  stopAll(opts) {
    const bus = opts && opts.bus != null ? String(opts.bus) : null;
    if (hasAudio) native.audioStopAll(bus, fadeOf(opts));
    else for (const v of [...voices.values()]) if (!bus || v.bus === bus) v._end(1);
  },
  /** The voices playing now. */
  voices() {
    return [...voices.values()];
  },
  available: hasAudio,
};

// Native reports: [[0, voice, status] | [1, url, ok, duration], ...]
g.__jsui_onAudio = function (events) {
  for (const e of events) {
    if (e[0] === 0) {
      const v = voices.get(e[1]);
      if (v) v._end(e[2]);
    } else {
      const entry = loads.get(e[1]);
      if (!entry) continue;
      if (e[2]) {
        entry.info = { duration: e[3] >= 0 ? e[3] : null };
        entry.resolve(entry.info);
      } else {
        loads.delete(e[1]);
        entry.reject(new Error('Sound could not be loaded: ' + e[1]));
      }
    }
  }
};

/** Returns a stable play(opts?) function for `source`, preloaded on mount.
 * `defaults` are merged under each call's options. */
export function useSound(source, defaults) {
  const ref = useRef(null);
  ref.current = { source, defaults };
  const url = sourceUrl(source);
  useEffect(() => {
    if (url && hasAudio) preload(url).catch(() => {});
  }, [url]);
  return useMemo(() => (opts) => play(ref.current.source, opts, ref.current.defaults), []);
}

/** Plays `source` (looping by default, on the 'music' bus) while the
 * component is mounted; a new source crossfades (fadeOut for the old one,
 * fadeIn for the new). null / false: silence. Options: volume, bus, loop,
 * fadeIn, fadeOut (ms), pitch, paused. Returns the playing SoundInstance or null. */
export function useMusic(source, opts) {
  const o = opts || {};
  const url = sourceUrl(source);
  const ref = useRef(null);
  const optsRef = useRef(o);
  optsRef.current = o;
  const loop = o.loop !== false;
  const bus = o.bus || 'music';
  useEffect(() => {
    if (!url) return undefined;
    const cur = optsRef.current;
    const v = play(url, {
      bus,
      loop,
      volume: cur.volume,
      pitch: cur.pitch,
      fadeIn: cur.fadeIn,
    });
    if (cur.paused) v.pause();
    ref.current = v;
    return () => {
      v.stop({ fade: num(optsRef.current.fadeOut, 0) });
      if (ref.current === v) ref.current = null;
    };
  }, [url, bus, loop]);
  const volume = o.volume == null ? 1 : o.volume;
  const pitch = o.pitch == null ? 1 : o.pitch;
  const paused = !!o.paused;
  useEffect(() => {
    const v = ref.current;
    if (!v) return;
    v.setVolume(volume, { fade: num(optsRef.current.volumeFade, 250) });
    v.setPitch(pitch);
    if (paused) v.pause();
    else v.resume();
  }, [volume, pitch, paused]);
  return ref.current;
}

// ---------------------------------------------------------- UI sound sets

/** Interaction sounds of Pressables (and the Touchables, Switch, Button):
 *   hoverIn   pointer enters
 *   focus     keyboard / gamepad focus moves onto it
 *   pressIn   press starts
 *   press     press activated (onPress)
 *   longPress long press fired
 * Each entry is a source, or { source, volume, pitch, bus } (bus default 'ui'). */
export const UISoundsContext = createContext(null);

/** Default interaction sounds for the pressable elements inside it (an
 * element's own `sounds` prop overrides per event; `sounds={false}` mutes
 * one). Nested providers merge with their parent's sounds. */
export function UISounds({ sounds, volume, bus, children }) {
  const parent = useContext(UISoundsContext);
  const value = useMemo(() => {
    const merged = Object.assign({}, parent && parent.sounds, sounds);
    return {
      sounds: merged,
      volume: volume != null ? volume : parent ? parent.volume : 1,
      bus: bus || (parent ? parent.bus : 'ui'),
    };
  }, [parent, sounds, volume, bus]);
  return h(UISoundsContext.Provider, { value }, children);
}

/** Plays interaction sound `event` for an element (usePress). `own` is the
 * element's `sounds` prop, `ctx` the UISounds context (used only when
 * `interactive`). */
export function playUISound(event, own, ctx, interactive) {
  if (own === false || own === null) return;
  let entry = own && own[event];
  if (entry === undefined && ctx && interactive) entry = ctx.sounds[event];
  if (!entry) return;
  const isObj = typeof entry === 'object' && !('uri' in entry) && entry.source !== undefined;
  const source = isObj ? entry.source : entry;
  const base = ctx ? ctx.volume : 1;
  play(source, {
    bus: (isObj && entry.bus) || (ctx && ctx.bus) || 'ui',
    volume: base * (isObj && entry.volume != null ? entry.volume : 1),
    pitch: isObj && entry.pitch != null ? entry.pitch : 1,
  });
}

/** Plays a UISounds event outside a Pressable (custom widgets: sliders, carousels, tabs). */
export function useUISound() {
  const ctx = useContext(UISoundsContext);
  const ref = useRef(ctx);
  ref.current = ctx;
  return useMemo(() => (event, own) => playUISound(event, own, ref.current, true), []);
}
