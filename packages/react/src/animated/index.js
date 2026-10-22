// Animated: RN-compatible API. JS-driven animations run outside React (one
// rAF loop + direct style patches); with useNativeDriver: true the runtime
// evaluates them natively on its frame clock (NativeAnimatedHelper.js).

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
import { NativeAnimatedAPI, shouldUseNativeDriver, eventTypeOfProp } from './NativeAnimatedHelper.js';

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

// Native Animated.event: maps argMapping[0].nativeEvent paths onto native
// values for the view's events of `type` ("scroll", ...). Returns detach().
function attachNativeMapping(argMapping, viewId, type) {
  const tags = [];
  const traverse = (mapping, path) => {
    if (mapping instanceof AnimatedValue) {
      mapping.__makeNative();
      const tag = mapping.__getNativeTag();
      NativeAnimatedAPI.addEventMapping(viewId, type, path, tag);
      tags.push(tag);
    } else if (mapping instanceof AnimatedValueXY) {
      traverse(mapping.x, path.concat('x'));
      traverse(mapping.y, path.concat('y'));
    } else if (mapping && typeof mapping === 'object') {
      for (const k in mapping) traverse(mapping[k], path.concat(k));
    }
  };
  const first = argMapping[0];
  if (!first || typeof first !== 'object' || !first.nativeEvent) {
    throw new Error('Animated.event with useNativeDriver: true maps only the first argument, as {nativeEvent: ...}');
  }
  traverse(first.nativeEvent, []);
  return () => {
    for (const tag of tags) NativeAnimatedAPI.removeEventMapping(viewId, type, tag);
  };
}

/** Animated.event([{nativeEvent: {contentOffset: {y: value}}}], {listener, useNativeDriver}) */
function event(argMapping, config) {
  const listener = config && config.listener;
  if (shouldUseNativeDriver(config)) {
    // The runtime applies the mapping before the event reaches JS (it keeps
    // working while JS is busy); JS only runs the listener.
    const handler = function (...args) {
      if (listener) listener.apply(this, args);
    };
    handler.__isAnimatedEvent = true;
    handler.__isNative = true;
    handler.__hasListener = !!listener;
    handler.__attachNative = (viewId, type) => attachNativeMapping(argMapping, viewId, type);
    return handler;
  }
  const handler = function (...args) {
    batch(() => {
      for (let i = 0; i < argMapping.length; i++) applyMapping(argMapping[i], args[i]);
    });
    if (listener) listener.apply(this, args);
  };
  handler.__isAnimatedEvent = true;
  return handler;
}

/** Animated.attachNativeEvent(viewRef, 'onScroll', argMapping) -> {detach} */
function attachNativeEvent(viewRef, eventName, argMapping) {
  const host = viewRef && viewRef.current !== undefined ? viewRef.current : viewRef;
  const viewId = host && (typeof host.id === 'number' ? host.id : host._nativeTag);
  if (!NativeAnimatedAPI || typeof viewId !== 'number') return { detach() {} };
  return { detach: attachNativeMapping(argMapping, viewId, eventTypeOfProp(eventName)) };
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
  attachNativeEvent,
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
