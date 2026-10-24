// Native driver (useNativeDriver: true), like React Native's
// NativeAnimatedHelper: Animated nodes and animations are serialized to the
// runtime (__jsui_native.animated, src/animated/native_animated.cpp), which
// evaluates them on its own frame clock and writes the styles itself. JS
// hears back only for end callbacks and value listeners.
//
// Without the native module (Node tests, other hosts) useNativeDriver falls
// back to the JS driver, which also runs outside React.

import { native } from '../native.js';

const g = globalThis;
const api = native && native.animated && typeof native.animated.createNode === 'function' ? native.animated : null;

export const NativeAnimatedAPI = api;
export const isNativeAnimatedAvailable = () => api !== null;

/** True when an animation / event config asks for the native driver and the runtime has one. */
export function shouldUseNativeDriver(config) {
  return !!(config && config.useNativeDriver && api);
}

let nextTag = 1;
let nextAnimationId = 1;
export const generateNewNodeTag = () => nextTag++;
export const generateNewAnimationId = () => nextAnimationId++;

// Native nodes of Animated objects that were garbage collected.
const registry = typeof g.FinalizationRegistry === 'function' ? new g.FinalizationRegistry((tag) => api && api.dropNode(tag)) : null;
export function trackNativeNode(node, tag) {
  if (registry) registry.register(node, tag, node);
}
export function untrackNativeNode(node) {
  if (registry) registry.unregister(node);
}

// --- animations and listeners -------------------------------------------------
const endCallbacks = new Map();  // animation id -> (finished, value) => void
const listening = new Map();     // value tag -> AnimatedValue

export function startAnimatingNode(animationId, tag, config, onEnd) {
  endCallbacks.set(animationId, onEnd);
  try {
    api.startAnimation(animationId, tag, config);
  } catch (e) {
    endCallbacks.delete(animationId);
    throw e;
  }
}

/** Stops a native animation; returns {velocity} when it was running. No end callback. */
export function stopAnimation(animationId) {
  endCallbacks.delete(animationId);
  return api.stopAnimation(animationId);
}

export function startListening(tag, node) {
  listening.set(tag, node);
  api.startListening(tag);
}

export function stopListening(tag) {
  listening.delete(tag);
  if (api) api.stopListening(tag);
}

// Called by the runtime at the end of a tick (Runtime::deliverAnimatedEvents).
g.__jsui_onAnimatedEnd = (id, finished, value) => {
  const cb = endCallbacks.get(id);
  if (!cb) return;
  endCallbacks.delete(id);
  cb(!!finished, value);
};
g.__jsui_onAnimatedValueUpdate = (tag, value) => {
  const node = listening.get(tag);
  if (node) node.__onAnimatedValueUpdateReceived(value);
};

// --- what the native driver may animate ----------------------------------------
// Paint-only keys, as React Native's SUPPORTED_STYLES: no layout props.
export const SUPPORTED_STYLES = new Set([
  'opacity',
  'transform',
  'borderRadius',
  'borderBottomEndRadius',
  'borderBottomLeftRadius',
  'borderBottomRightRadius',
  'borderBottomStartRadius',
  'borderEndEndRadius',
  'borderEndStartRadius',
  'borderStartEndRadius',
  'borderStartStartRadius',
  'borderTopEndRadius',
  'borderTopLeftRadius',
  'borderTopRightRadius',
  'borderTopStartRadius',
  'elevation',
  'shadowOpacity',
  'shadowRadius',
  'scaleX',
  'scaleY',
  'translateX',
  'translateY',
  'backgroundColor',
  'borderBottomColor',
  'borderColor',
  'borderEndColor',
  'borderLeftColor',
  'borderRightColor',
  'borderStartColor',
  'borderTopColor',
  'color',
  'tintColor',
  'shadowColor',
  'textShadowColor',
  'textDecorationColor',
  // LottieView's progress (packages/react/src/lottie.js).
  'lottieProgress',
]);

export const SUPPORTED_TRANSFORMS = new Set([
  'translateX',
  'translateY',
  'scale',
  'scaleX',
  'scaleY',
  'rotate',
  'rotateX',
  'rotateY',
  'rotateZ',
  'perspective',
  'skewX',
  'skewY',
  'matrix',
]);

export function validateStyle(key) {
  // ShaderView uniforms (packages/react/src/shader.js): paint-only.
  if (!SUPPORTED_STYLES.has(key) && !key.startsWith('uniform:')) {
    throw new Error(`Style property '${key}' is not supported by native animated module`);
  }
}

export function validateTransform(property) {
  if (!SUPPORTED_TRANSFORMS.has(property)) {
    throw new Error(`Property '${property}' is not supported by native animated module`);
  }
}

/** Samples an easing function on [0, 1] (the native side interpolates linearly between samples). */
export function sampleCurve(fn, count) {
  const out = new Array(count);
  for (let i = 0; i < count; i++) out[i] = fn(i / (count - 1));
  return out;
}

/** 'onScroll' -> 'scroll': the native event type an Animated.event prop maps. */
export function eventTypeOfProp(prop) {
  return (prop.startsWith('on') ? prop.slice(2) : prop).toLowerCase();
}
