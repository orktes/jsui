// main.gd's self-test probe for UI audio (JsuiView plays the runtime's sounds
// with AudioStreamPlayers). Godot drives it with call_ui:
//   audio.play {sound | url, bus, volume, pitch, loop, fadeIn, offset} -> {id, url}
//   audio.stop {id, fade}   audio.pause {id, paused}   audio.set {id, volume, pitch, fade}
//   audio.bus {bus, volume, muted, fade}   audio.stopAll {bus, fade}
//   audio.preload {sound | url} -> {ok, duration}
// and hears `audio:ready` on mount and `audio:ended` {id, reason} when a
// voice's `ended` resolves.
// `sound` names one of the demo's sounds (bundled: the bundler emits them
// next to the bundle and the import is their URL); `url` is any URL
// (relative ones resolve against the bundle).
import React, { useEffect } from 'react';
import { Host, Sound, View, useHostHandler } from 'react-native';
import back from './sounds/back.wav';
import blip from './sounds/blip.mp3';
import confirm from './sounds/confirm.wav';
import menuLoop from './sounds/menu-loop.ogg';
import tick from './sounds/tick.wav';

const SOUNDS = { tick, confirm, back, blip, loop: menuLoop };
const voices = new Map();

const source = (a) => (a.url != null ? a.url : SOUNDS[a.sound]);

export function AudioProbe() {
  useEffect(() => Host.postMessage('audio:ready', {}), []);
  useHostHandler('audio.play', (a) => {
    const v = Sound.play(source(a), a);
    voices.set(v.id, v);
    v.ended.then(({ reason }) => {
      voices.delete(v.id);
      Host.postMessage('audio:ended', { id: v.id, reason });
    });
    return { id: v.id, url: source(a) };
  });
  useHostHandler('audio.stop', ({ id, fade }) => {
    voices.get(id)?.stop({ fade });
    return {};
  });
  useHostHandler('audio.pause', ({ id, paused }) => {
    const v = voices.get(id);
    if (v && paused) v.pause();
    else if (v) v.resume();
    return {};
  });
  useHostHandler('audio.set', ({ id, volume, pitch, fade }) => {
    const v = voices.get(id);
    if (v && volume != null) v.setVolume(volume, { fade });
    if (v && pitch != null) v.setPitch(pitch, { fade });
    return {};
  });
  useHostHandler('audio.bus', ({ bus, volume, muted, fade }) => {
    if (volume != null) Sound.setBusVolume(bus, volume, { fade });
    if (muted != null) Sound.setBusMuted(bus, muted, { fade });
    return Sound.getBus(bus);
  });
  useHostHandler('audio.stopAll', ({ bus, fade }) => {
    Sound.stopAll({ bus, fade });
    return {};
  });
  useHostHandler('audio.preload', (a) =>
    Sound.preload(source(a)).then(
      ({ duration }) => ({ ok: true, duration }),
      () => ({ ok: false }),
    ),
  );
  return <View style={{ width: 8, height: 8 }} />;
}
