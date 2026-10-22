// Animated.createAnimatedComponent: resolves Animated nodes in props at
// render time and afterwards applies value changes as direct style patches
// on the host node (__jsui_native.patchStyle), without re-rendering React.

import { createElement, useCallback, useLayoutEffect, useReducer, useRef } from 'react';
import { flatten } from '../StyleSheet.js';
import { AnimatedNode } from './nodes.js';

function containsAnimated(v, depth) {
  if (v instanceof AnimatedNode) return true;
  if (depth > 4 || v === null || typeof v !== 'object') return false;
  if (Array.isArray(v)) {
    for (let i = 0; i < v.length; i++) if (containsAnimated(v[i], depth + 1)) return true;
    return false;
  }
  for (const k in v) if (containsAnimated(v[k], depth + 1)) return true;
  return false;
}

function collectNodes(v, out, depth) {
  if (v instanceof AnimatedNode) {
    if (out.indexOf(v) < 0) out.push(v);
    return;
  }
  if (depth > 4 || v === null || typeof v !== 'object') return;
  if (Array.isArray(v)) for (let i = 0; i < v.length; i++) collectNodes(v[i], out, depth + 1);
  else for (const k in v) collectNodes(v[k], out, depth + 1);
}

/** Replaces Animated nodes with their current values (deep, small objects only). */
export function resolveAnimated(v, depth = 0) {
  if (v instanceof AnimatedNode) return v.__getValue();
  if (depth > 4 || v === null || typeof v !== 'object') return v;
  if (Array.isArray(v)) {
    let out = null;
    for (let i = 0; i < v.length; i++) {
      const r = resolveAnimated(v[i], depth + 1);
      if (r !== v[i] && !out) out = v.slice(0, i);
      if (out) out.push(r);
    }
    return out || v;
  }
  if (Object.getPrototypeOf(v) !== Object.prototype && Object.getPrototypeOf(v) !== null) return v;
  let out = null;
  for (const k in v) {
    const r = resolveAnimated(v[k], depth + 1);
    if (r !== v[k] && !out) out = Object.assign({}, v);
    if (out) out[k] = r;
  }
  return out || v;
}

// One per mounted animated component: the subscriber that Animated nodes
// mark dirty; update() runs at the end of the frame (see nodes.js flushDirty).
class Subscription {
  constructor(forceUpdate) {
    this.__isComponent = true;
    this.forceUpdate = forceUpdate;
    this.host = null;
    this.nodes = [];
    this.style = null;  // flattened style holding animated values
    this.animatedKeys = [];
    this.propsAnimated = false;
  }

  setNodes(nodes) {
    const old = this.nodes;
    if (old.length === nodes.length && old.every((n, i) => n === nodes[i])) return;
    for (const n of old) if (nodes.indexOf(n) < 0) n.__removeChild(this);
    for (const n of nodes) if (old.indexOf(n) < 0) n.__addChild(this);
    this.nodes = nodes;
  }

  detach() {
    for (const n of this.nodes) n.__removeChild(this);
    this.nodes = [];
  }

  update() {
    const host = this.host;
    if (this.propsAnimated || !host || typeof host.patchStyle !== 'function') {
      this.forceUpdate();
      return;
    }
    const style = this.style;
    const keys = this.animatedKeys;
    const partial = {};
    for (let i = 0; i < keys.length; i++) partial[keys[i]] = resolveAnimated(style[keys[i]]);
    host.patchStyle(partial);
  }
}

export function createAnimatedComponent(Component) {
  function AnimatedComponent(props) {
    const userRef = props.ref;
    const [, forceUpdate] = useReducer((x) => x + 1, 0);
    const subRef = useRef(null);
    if (!subRef.current) subRef.current = new Subscription(forceUpdate);
    const sub = subRef.current;

    const nodes = [];
    const next = {};
    let propsAnimated = false;
    for (const k in props) {
      if (k === 'ref') continue;
      const v = props[k];
      if (k === 'style') continue;
      if (typeof v === 'object' && v !== null && k !== 'children' && containsAnimated(v, 0)) {
        collectNodes(v, nodes, 0);
        next[k] = resolveAnimated(v);
        propsAnimated = true;
      } else {
        next[k] = v;
      }
    }
    const style = flatten(props.style);
    const keys = [];
    if (style) {
      let resolved = null;
      for (const k in style) {
        const v = style[k];
        if (typeof v === 'object' && v !== null && containsAnimated(v, 0)) {
          keys.push(k);
          collectNodes(v, nodes, 0);
          if (!resolved) resolved = Object.assign({}, style);
          resolved[k] = resolveAnimated(v);
        }
      }
      next.style = resolved || props.style;
    }
    sub.style = style;
    sub.animatedKeys = keys;
    sub.propsAnimated = propsAnimated;

    const setRef = useCallback(
      (instance) => {
        sub.host = instance;
        if (typeof userRef === 'function') userRef(instance);
        else if (userRef) userRef.current = instance;
      },
      [userRef],
    );
    next.ref = setRef;

    useLayoutEffect(() => {
      sub.setNodes(nodes);
      // A value may have changed between render and commit.
      if (nodes.length && !propsAnimated) sub.update();
    });
    useLayoutEffect(() => () => sub.detach(), []);

    return createElement(Component, next);
  }
  const name = typeof Component === 'string' ? Component : Component.displayName || Component.name || 'Component';
  AnimatedComponent.displayName = 'Animated(' + name + ')';
  return AnimatedComponent;
}
