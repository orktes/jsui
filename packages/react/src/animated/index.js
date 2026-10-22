// Animated: RN-compatible API. useNativeDriver is accepted and ignored: every
// animation already runs outside React (one rAF loop + direct style patches).

import { useRef } from 'react';
import {
  AnimatedNode,
  AnimatedValue,
  AnimatedValueXY,
  AnimatedColor,
  AnimatedInterpolation,
  add,
  subtract,
  multiply,
  divide,
  modulo,
  diffClamp,
  batch,
} from './nodes.js';
import { timing, spring, decay, sequence, parallel, stagger, delay, loop } from './animations.js';
import { createAnimatedComponent } from './createAnimatedComponent.js';
import { Easing } from './Easing.js';

function applyMapping(mapping, value) {
  if (mapping instanceof AnimatedValue) {
    if (typeof value === 'number') mapping.setValue(value);
    return;
  }
  if (mapping instanceof AnimatedValueXY) {
    if (value && typeof value === 'object') mapping.setValue({ x: +value.x || 0, y: +value.y || 0 });
    return;
  }
  if (!mapping || typeof mapping !== 'object' || value == null || typeof value !== 'object') return;
  for (const k in mapping) applyMapping(mapping[k], value[k]);
}

/** Animated.event([{nativeEvent: {contentOffset: {y: value}}}], {listener}) */
function event(argMapping, config) {
  const listener = config && config.listener;
  const handler = function (...args) {
    batch(() => {
      for (let i = 0; i < argMapping.length; i++) applyMapping(argMapping[i], args[i]);
    });
    if (listener) listener.apply(this, args);
  };
  handler.__isAnimatedEvent = true;
  return handler;
}

export function useAnimatedValue(initialValue, config) {
  const ref = useRef(null);
  if (ref.current === null) ref.current = new AnimatedValue(initialValue, config);
  return ref.current;
}

// Called once by the package index with the base components (avoids an
// import cycle between Animated and the components that use it).
export function makeAnimated(base) {
  const components = {
    View: createAnimatedComponent(base.View),
    Text: createAnimatedComponent(base.Text),
    Image: createAnimatedComponent(base.Image),
    ScrollView: createAnimatedComponent(base.ScrollView),
    FlatList: createAnimatedComponent(base.FlatList),
    SectionList: createAnimatedComponent(base.SectionList),
  };
  Object.assign(Animated, components);
  return Animated;
}

export const Animated = {
  Value: AnimatedValue,
  ValueXY: AnimatedValueXY,
  Color: AnimatedColor,
  Interpolation: AnimatedInterpolation,
  Node: AnimatedNode,
  event,
  timing,
  spring,
  decay,
  sequence,
  parallel,
  stagger,
  delay,
  loop,
  add,
  subtract,
  multiply,
  divide,
  modulo,
  diffClamp,
  createAnimatedComponent,
  attachNativeEvent: () => ({ detach() {} }),
  forkEvent: (event1, listener) => {
    if (!event1) return listener;
    return (...args) => {
      event1(...args);
      listener(...args);
    };
  },
  unforkEvent() {},
  useAnimatedValue,
  Easing,
};

export { Easing, AnimatedValue, AnimatedNode, createAnimatedComponent };
export default Animated;
