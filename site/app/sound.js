// The site's sound: a UI sound set for every button (<UISounds> in
// index.jsx), music per game screen (Screens.jsx) and the mix the Settings
// screen's Audio tab edits. The sounds are synthesized by
// site/scripts/gen-ui-sounds.mjs (CC0).
//
// The page (site/index.html) plays them with Web Audio and reports through
// HostState 'audio': { unlocked, muted }. Browsers keep audio locked until
// the first click / tap / key press (gamepad buttons do not count), so the
// nav bar's sound button says so until then.
import { Sound, useHostState } from 'react-native';

export const SFX = {
  hover: require('./assets/sounds/hover.wav'),
  focus: require('./assets/sounds/focus.wav'),
  press: require('./assets/sounds/press.wav'),
  back: require('./assets/sounds/back.wav'),
  tick: require('./assets/sounds/tick.wav'),
  swipe: require('./assets/sounds/swipe.wav'),
};

export const MUSIC = {
  menu: require('./assets/sounds/menu-loop.wav'),
  rift: require('./assets/sounds/rift-loop.wav'),
};

// <UISounds sounds={UI_SOUNDS}>: what every pressable element plays.
export const UI_SOUNDS = {
  hoverIn: { source: SFX.hover, volume: 0.55 },
  focus: { source: SFX.focus, volume: 0.7 },
  press: SFX.press,
};

/** Decodes the UI sounds ahead (they are tiny), so the first hover is instant. */
export function preloadUISounds() {
  for (const s of Object.values(SFX)) Sound.preload(s).catch(() => {});
}

/** One-off UI sounds outside Pressables (back, tabs, slider steps). */
export function playUI(name, opts) {
  Sound.play(SFX[name], { bus: 'ui', ...opts });
}

// Slider steps tick, but not faster than a ticking clock would.
let lastTick = 0;
export function sliderTick(fraction) {
  const now = Date.now();
  if (now - lastTick < 45) return;
  lastTick = now;
  playUI('tick', { volume: 0.6, pitch: 0.85 + 0.4 * Math.max(0, Math.min(1, fraction)) });
}

// The mix (0..100 like the Settings sliders). Buses: 'music', 'sfx', 'ui', 'voice'.
export const DEFAULT_MIX = { master: 80, music: 60, sfx: 85, ui: 70, voice: 90, muteAll: false };
let mix = { ...DEFAULT_MIX };

export function currentMix() {
  return mix;
}

/** Applies a mix to the runtime's buses (ramped, so slider drags stay smooth). */
export function applyMix(next, fade = 120) {
  mix = { ...mix, ...next };
  const m = mix.master / 100;
  for (const bus of ['music', 'sfx', 'ui', 'voice']) {
    // Perceived loudness is roughly logarithmic: square the slider value.
    const v = (m * mix[bus]) / 100;
    Sound.setBusVolume(bus, v * v, { fade });
    Sound.setBusMuted(bus, !!mix.muteAll, { fade });
  }
}

/** { unlocked, muted } from the page (site/index.html). */
export function useAudioState() {
  return useHostState('audio', { unlocked: false, muted: false });
}
