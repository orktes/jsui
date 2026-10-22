// LayoutAnimation: React Native's API. configureNext() stores the config;
// the renderer arms it natively (__jsui_native.configureLayoutAnimation) at
// the start of the next commit, so the layout pass after that commit
// animates: changed frames (update), inserted views (create: opacity /
// scale) and removed views (delete: fade / scale out). See
// src/core/layout_animation.cpp.

import { native } from './native.js';

let pending = null;

function maxDuration(config) {
  let max = 0;
  const base = +config.duration || 0;
  for (const k of ['create', 'update', 'delete']) {
    const s = config[k];
    if (!s) continue;
    max = Math.max(max, (s.duration != null ? +s.duration : base) + (+s.delay || 0));
  }
  return max || base;
}

/** Called by the renderer at the start of every commit. */
export function armPendingLayoutAnimation() {
  if (!pending) return;
  const config = pending;
  pending = null;
  if (typeof native.configureLayoutAnimation === 'function') native.configureLayoutAnimation(config);
}

function configureNext(config, onAnimationDidEnd, onAnimationDidFail) {
  if (!config || typeof config !== 'object') {
    if (onAnimationDidFail) setTimeout(onAnimationDidFail, 0);
    return;
  }
  pending = config;
  if (onAnimationDidEnd) setTimeout(onAnimationDidEnd, maxDuration(config));
}

function create(duration, type, property) {
  return { duration, create: { type, property }, update: { type }, delete: { type, property } };
}

const Presets = {
  easeInEaseOut: create(300, 'easeInEaseOut', 'opacity'),
  linear: create(500, 'linear', 'opacity'),
  spring: {
    duration: 700,
    create: { type: 'linear', property: 'opacity' },
    update: { type: 'spring', springDamping: 0.4 },
    delete: { type: 'linear', property: 'opacity' },
  },
};

export const LayoutAnimation = {
  configureNext,
  create,
  Types: Object.freeze({ spring: 'spring', linear: 'linear', easeInEaseOut: 'easeInEaseOut', easeIn: 'easeIn', easeOut: 'easeOut', keyboard: 'keyboard' }),
  Properties: Object.freeze({ opacity: 'opacity', scaleX: 'scaleX', scaleY: 'scaleY', scaleXY: 'scaleXY' }),
  Presets,
  easeInEaseOut: (onEnd) => configureNext(Presets.easeInEaseOut, onEnd),
  linear: (onEnd) => configureNext(Presets.linear, onEnd),
  spring: (onEnd) => configureNext(Presets.spring, onEnd),
  setEnabled() {},
  checkConfig() {},
};
