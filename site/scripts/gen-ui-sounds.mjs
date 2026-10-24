#!/usr/bin/env node
// Synthesizes the showcase site's UI sounds and music loops into
// site/app/assets/sounds/*.wav (16-bit PCM, mono). Everything is made from
// sine waves, filtered noise and a small reverb here, with a fixed random
// seed, so the files are reproducible and CC0 by construction (no samples,
// no third-party material).
//
//   node site/scripts/gen-ui-sounds.mjs
//
// UI sounds (44.1 kHz, a few KB each):
//   hover.wav    a soft high tick (pointer enters a button)
//   focus.wav    a glassy tock (keyboard / gamepad focus moves)
//   press.wav    a rising two-note chime (confirm)
//   back.wav     a falling two-note chime (back / close)
//   tick.wav     a tiny click (slider steps, toggles)
//   swipe.wav    a filtered noise swoosh (tabs, screen changes)
// Music (seamless loops, 16 s):
//   menu-loop.wav  warm pads, sub bass and a bell arpeggio (A minor - F - C - G), 22.05 kHz
//   rift-loop.wav  a darker pulse (D minor - B flat - G minor - A), 16 kHz
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const OUT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../app/assets/sounds');
fs.mkdirSync(OUT, { recursive: true });

const TAU = Math.PI * 2;
const midi = (n) => 440 * 2 ** ((n - 69) / 12);

// Mulberry32: a fixed seed keeps the noise (and so the files) identical run to run.
function rng(seed) {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

// `loop`: a seamless loop; otherwise the last 20 ms fade out (no click at the cut).
function wav(name, samples, rate, peak, loop = false) {
  if (!loop) {
    const n = Math.min(samples.length, Math.round(rate * 0.02));
    for (let i = 0; i < n; i++) samples[samples.length - 1 - i] *= i / n;
  }
  let max = 0;
  for (const s of samples) max = Math.max(max, Math.abs(s));
  const gain = max > 0 ? peak / max : 0;
  const data = Buffer.alloc(samples.length * 2);
  // TPDF dither, seeded.
  const r = rng(7);
  for (let i = 0; i < samples.length; i++) {
    const v = samples[i] * gain * 32767 + (r() - r());
    data.writeInt16LE(Math.max(-32768, Math.min(32767, Math.round(v))), i * 2);
  }
  const h = Buffer.alloc(44);
  h.write('RIFF', 0);
  h.writeUInt32LE(36 + data.length, 4);
  h.write('WAVE', 8);
  h.write('fmt ', 12);
  h.writeUInt32LE(16, 16);
  h.writeUInt16LE(1, 20);          // PCM
  h.writeUInt16LE(1, 22);          // mono
  h.writeUInt32LE(rate, 24);
  h.writeUInt32LE(rate * 2, 28);
  h.writeUInt16LE(2, 32);
  h.writeUInt16LE(16, 34);
  h.write('data', 36);
  h.writeUInt32LE(data.length, 40);
  fs.writeFileSync(path.join(OUT, name), Buffer.concat([h, data]));
  console.log(`${name}: ${(samples.length / rate).toFixed(2)} s, ${((44 + data.length) / 1024).toFixed(1)} KiB`);
}

// One-pole low-pass / high-pass, in place. `circular`: the filter state at
// the loop's start is the one at its end (a first pass primes it).
function lowpass(x, rate, hz, circular = false) {
  const a = 1 - Math.exp((-TAU * hz) / rate);
  let y = 0;
  if (circular) for (let i = 0; i < x.length; i++) y += a * (x[i] - y);
  for (let i = 0; i < x.length; i++) x[i] = y += a * (x[i] - y);
  return x;
}
function highpass(x, rate, hz) {
  const a = 1 - Math.exp((-TAU * hz) / rate);
  let y = 0;
  for (let i = 0; i < x.length; i++) {
    y += a * (x[i] - y);
    x[i] -= y;
  }
  return x;
}

// A small Schroeder reverb (4 combs, 2 allpasses). `circular`: the buffer is
// a loop, so tails wrap around its end (run twice, keep the second pass).
function reverb(x, rate, { mix = 0.3, decay = 0.82, damp = 0.35, circular = false }) {
  const combs = [1116, 1188, 1277, 1356].map((n) => ({ buf: new Float32Array(Math.round((n * rate) / 44100)), i: 0, f: 0 }));
  const alls = [556, 341].map((n) => ({ buf: new Float32Array(Math.round((n * rate) / 44100)), i: 0 }));
  const out = new Float32Array(x.length);
  for (let pass = circular ? 2 : 1; pass > 0; pass--) {
    for (let n = 0; n < x.length; n++) {
      const inp = x[n] * 0.25;
      let s = 0;
      for (const c of combs) {
        const y = c.buf[c.i];
        c.f = y * (1 - damp) + c.f * damp;
        c.buf[c.i] = inp + c.f * decay;
        c.i = (c.i + 1) % c.buf.length;
        s += y;
      }
      for (const a of alls) {
        const b = a.buf[a.i];
        const y = -s + b;
        a.buf[a.i] = s + b * 0.5;
        a.i = (a.i + 1) % a.buf.length;
        s = y;
      }
      out[n] = x[n] * (1 - mix) + s * mix;
    }
  }
  return out;
}

const env = (t, attack, decay) => (t < attack ? t / attack : Math.exp(-(t - attack) / decay));

// ------------------------------------------------------------------ UI sounds

const UI_RATE = 44100;
const buf = (sec) => new Float32Array(Math.round(sec * UI_RATE));

function hover() {
  const x = buf(0.06);
  for (let i = 0; i < x.length; i++) {
    const t = i / UI_RATE;
    x[i] = (Math.sin(TAU * 3520 * t) * 0.6 + Math.sin(TAU * 5280 * t) * 0.25) * env(t, 0.0015, 0.012);
  }
  return x;
}

function focus() {
  const x = buf(0.14);
  const r = rng(11);
  let ph = 0;
  for (let i = 0; i < x.length; i++) {
    const t = i / UI_RATE;
    ph += (TAU * 1568 * (1 + 0.04 * Math.exp(-t / 0.01))) / UI_RATE;  // a slight pitch drop
    x[i] = Math.sin(ph) * env(t, 0.002, 0.035) * 0.8 + Math.sin(ph * 2.01) * env(t, 0.001, 0.015) * 0.25 +
      (r() * 2 - 1) * env(t, 0.0005, 0.003) * 0.15;
  }
  return highpass(x, UI_RATE, 300);
}

// A soft FM bell note added at `at` seconds.
function bell(x, rate, at, freq, amp, decay, bright = 1) {
  const start = Math.round(at * rate);
  for (let i = start, n = 0; i < x.length; i++, n++) {
    const t = n / rate;
    const e = env(t, 0.003, decay);
    if (e < 1e-4 && t > 0.01) break;
    const mod = Math.sin(TAU * freq * 3.5 * t) * 1.2 * bright * Math.exp(-t / (decay * 0.4));
    x[i] += Math.sin(TAU * freq * t + mod) * e * amp;
  }
}

function press() {
  const x = buf(0.42);
  bell(x, UI_RATE, 0, midi(81), 0.7, 0.07);     // A5
  bell(x, UI_RATE, 0.055, midi(88), 0.8, 0.12); // E6
  return reverb(x, UI_RATE, { mix: 0.18, decay: 0.7 });
}

function back() {
  const x = buf(0.4);
  bell(x, UI_RATE, 0, midi(84), 0.6, 0.06, 0.6);     // C6
  bell(x, UI_RATE, 0.06, midi(77), 0.7, 0.11, 0.6);  // F5
  return reverb(x, UI_RATE, { mix: 0.15, decay: 0.65 });
}

function tick() {
  const x = buf(0.03);
  const r = rng(5);
  for (let i = 0; i < x.length; i++) {
    const t = i / UI_RATE;
    x[i] = Math.sin(TAU * 2200 * t) * env(t, 0.0008, 0.005) * 0.7 + (r() * 2 - 1) * env(t, 0.0003, 0.0015) * 0.3;
  }
  return highpass(x, UI_RATE, 800);
}

function swipe() {
  const x = buf(0.32);
  const r = rng(9);
  // Noise through a resonant band-pass sweeping up (a state-variable filter).
  let low = 0, band = 0;
  for (let i = 0; i < x.length; i++) {
    const t = i / UI_RATE, u = t / 0.32;
    const f = 2 * Math.sin((Math.PI * (500 + 3500 * u * u)) / UI_RATE);
    const q = 0.35;
    const inp = r() * 2 - 1;
    low += f * band;
    const high = inp - low - q * band;
    band += f * high;
    x[i] = band * Math.sin(Math.PI * Math.min(1, u * 1.15)) ** 2;
  }
  return reverb(x, UI_RATE, { mix: 0.2, decay: 0.6 });
}

wav('hover.wav', hover(), UI_RATE, 0.32);
wav('focus.wav', focus(), UI_RATE, 0.4);
wav('press.wav', press(), UI_RATE, 0.5);
wav('back.wav', back(), UI_RATE, 0.45);
wav('tick.wav', tick(), UI_RATE, 0.3);
wav('swipe.wav', swipe(), UI_RATE, 0.35);

// --------------------------------------------------------------------- music

// Adds `fn(t)` (t in seconds from `at`) for `len` seconds into the loop `x`,
// wrapping around its end.
function addWrapped(x, rate, at, len, fn) {
  const n0 = Math.round(at * rate), n = Math.round(len * rate);
  for (let k = 0; k < n; k++) x[(n0 + k) % x.length] += fn(k / rate);
}

// A pad voice: a few detuned, band-limited saws (additive), soft attack / release.
function pad(x, rate, at, len, notes, { amp, bright, attack = 1.2, release = 1.6 }) {
  const harmonics = Math.max(1, bright);
  for (const note of notes) {
    for (const det of [-0.08, 0.0, 0.07]) {
      const f = midi(note + det);
      const phase0 = (note * 0.37 + det * 5) % 1;
      addWrapped(x, rate, at, len + release, (t) => {
        const e = t < attack ? (1 - Math.cos((Math.PI * t) / attack)) / 2 : t < len ? 1 : Math.cos((Math.PI / 2) * Math.min(1, (t - len) / release));
        let s = 0;
        for (let h = 1; h <= harmonics; h++) {
          if (f * h > rate / 2.2) break;
          s += Math.sin(TAU * (f * h * t + phase0 * h)) / (h * h * 0.6 + h * 0.4);
        }
        return s * e * amp;
      });
    }
  }
}

function sub(x, rate, at, len, note, amp) {
  const f = midi(note);
  addWrapped(x, rate, at, len + 0.8, (t) => {
    const e = Math.min(1, t / 0.6) * (t < len ? 1 : Math.max(0, 1 - (t - len) / 0.8));
    return Math.sin(TAU * f * t) * e * amp;
  });
}

function bellWrapped(x, rate, at, note, amp, decay) {
  const f = midi(note);
  addWrapped(x, rate, at, decay * 7, (t) => {
    const e = env(t, 0.004, decay);
    const mod = Math.sin(TAU * f * 2 * t) * 0.8 * Math.exp(-t / (decay * 0.5));
    return (Math.sin(TAU * f * t + mod) + 0.3 * Math.sin(TAU * f * 3 * t) * Math.exp(-t / (decay * 0.3))) * e * amp;
  });
}

function menuLoop() {
  const rate = 22050, bar = 4, chords = [
    { root: 45, pad: [57, 60, 64, 69], arp: [69, 72, 76, 79, 76, 72, 69, 64] },  // A minor
    { root: 41, pad: [57, 60, 65, 69], arp: [69, 72, 77, 81, 77, 72, 69, 65] },  // F
    { root: 48, pad: [55, 60, 64, 67], arp: [67, 72, 76, 79, 76, 72, 67, 64] },  // C
    { root: 43, pad: [55, 59, 62, 67], arp: [67, 71, 74, 79, 74, 71, 67, 62] },  // G
  ];
  const x = new Float32Array(rate * bar * chords.length);
  chords.forEach((c, i) => {
    const at = i * bar;
    pad(x, rate, at, bar, c.pad, { amp: 0.05, bright: 6 });
    sub(x, rate, at, bar, c.root, 0.22);
    c.arp.forEach((n, k) => bellWrapped(x, rate, at + k * 0.5, n, k % 2 ? 0.05 : 0.08, 0.35));
  });
  lowpass(x, rate, 5000, true);
  return reverb(x, rate, { mix: 0.35, decay: 0.84, circular: true });
}

function riftLoop() {
  const rate = 16000, bar = 4, chords = [
    { root: 38, pad: [50, 53, 57, 62] },  // D minor
    { root: 34, pad: [50, 53, 58, 62] },  // B flat
    { root: 43, pad: [50, 55, 58, 62] },  // G minor
    { root: 45, pad: [49, 52, 57, 61] },  // A
  ];
  const x = new Float32Array(rate * bar * chords.length);
  chords.forEach((c, i) => {
    const at = i * bar;
    pad(x, rate, at, bar, c.pad, { amp: 0.05, bright: 4, attack: 1.8, release: 2 });
    sub(x, rate, at, bar, c.root, 0.25);
    // A pulsing low octave, eighth notes (120 BPM feel over the slow chords).
    const f = midi(c.root + 12);
    for (let k = 0; k < 8; k++) {
      addWrapped(x, rate, at + k * 0.5, 0.45, (t) => Math.sin(TAU * f * t + Math.sin(TAU * f * t) * 0.7) * env(t, 0.01, 0.09) * 0.07);
    }
    bellWrapped(x, rate, at + 1.5, c.pad[3] + 12, 0.05, 0.6);
  });
  lowpass(x, rate, 2800, true);
  return reverb(x, rate, { mix: 0.4, decay: 0.86, damp: 0.5, circular: true });
}

wav('menu-loop.wav', menuLoop(), 22050, 0.6, true);
wav('rift-loop.wav', riftLoop(), 16000, 0.6, true);
