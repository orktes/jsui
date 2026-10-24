// Animated.createAnimatedComponent: resolves Animated nodes in props at
// render time and afterwards applies value changes as direct style patches
// on the host node (__jsui_native.patchStyle), without re-rendering React.

import { useCallback, useLayoutEffect, useReducer, useRef } from 'react';
import { jsx } from 'react/jsx-runtime';
import { flatten } from '../StyleSheet.js';
import { AnimatedNode } from './nodes.js';
import {
  NativeAnimatedAPI,
  generateNewNodeTag,
  validateStyle,
  validateTransform,
  eventTypeOfProp,
} from './NativeAnimatedHelper.js';

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

// The render path's single walk over a prop or style value (it used to be
// three: contains / collect / resolve): returns `v` with Animated nodes
// replaced by their values (arrays and plain objects copied on write; other
// objects searched but kept, as resolveAnimated keeps them), adds each node
// to `out` once, and sets `found` when it met one.
let found = false;
function resolveCollect(v, out, depth, copy) {
  if (v instanceof AnimatedNode) {
    found = true;
    if (out.indexOf(v) < 0) out.push(v);
    return copy ? v.__getValue() : v;
  }
  if (depth > 4 || v === null || typeof v !== 'object') return v;
  if (Array.isArray(v)) {
    let res = null;
    for (let i = 0; i < v.length; i++) {
      const r = resolveCollect(v[i], out, depth + 1, copy);
      if (r !== v[i] && !res) res = v.slice(0, i);
      if (res) res.push(r);
    }
    return res || v;
  }
  if (copy) {
    const proto = Object.getPrototypeOf(v);
    if (proto !== Object.prototype && proto !== null) copy = false;
  }
  let res = null;
  for (const k in v) {
    const r = resolveCollect(v[k], out, depth + 1, copy);
    if (r !== v[k] && !res) res = Object.assign({}, v);
    if (res) res[k] = r;
  }
  return res || v;
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
    this.isNative = false;  // driven by the native animated module
    this.native = null;     // {viewId, propsTag, tags, nodes, sig}
    this.nativeEvents = []; // [[prop, handler, detach]]
    this.textNode = null;   // the `text` prop's Animated node
    this.frozenText = null; // the children React keeps once the runtime writes the text
  }

  // --- native driver: a props -> style -> (transform) graph connected to the view
  __makeNative() {
    if (this.isNative) return;
    this.isNative = true;
    this.syncNative();
  }

  syncNative() {
    if (!this.isNative) return;
    const host = this.host;
    const viewId = host && typeof host.id === 'number' ? host.id : null;
    const sig = viewId == null ? null : nativeSignature(viewId, this.style, this.animatedKeys);
    const textNode = this.textNode;
    const fullSig = sig == null ? null : sig + (textNode ? '|text' : '');
    const cur = this.native;
    if (cur && fullSig === cur.sig && cur.nodes.length === this.nodes.length && cur.nodes.every((n, i) => n === this.nodes[i])) return;
    this.disconnectNative();
    if (fullSig == null) return;
    if (this.propsAnimated) throw new Error('Animated: only style props can be driven by the native animated module (useNativeDriver)');
    const api = NativeAnimatedAPI;
    const tags = [];
    const create = (config, inputs) => {
      const tag = generateNewNodeTag();
      api.createNode(tag, config);
      for (const t of inputs) api.connect(t, tag);
      tags.push(tag);
      return tag;
    };
    const nativeTag = (n) => {
      n.__makeNative();
      return n.__getNativeTag();
    };
    const style = {};
    for (const k of this.animatedKeys) {
      validateStyle(k);
      const v = this.style[k];
      if (v instanceof AnimatedNode) {
        style[k] = nativeTag(v);
      } else if (k === 'transform' && Array.isArray(v)) {
        const inputs = [];
        const transforms = v.map((t) => {
          const property = Object.keys(t)[0];
          validateTransform(property);
          const tv = t[property];
          if (!(tv instanceof AnimatedNode)) return { property, value: tv };
          const tag = nativeTag(tv);
          inputs.push(tag);
          return { property, nodeTag: tag };
        });
        style[k] = create({ type: 'transform', transforms }, inputs);
      } else {
        throw new Error(`Style property '${k}' is not supported by native animated module (nested animated value)`);
      }
    }
    const styleTag = create({ type: 'style', style }, Object.values(style));
    const props = { style: styleTag };
    const inputs = [styleTag];
    if (textNode) {
      props.text = nativeTag(textNode);
      inputs.push(props.text);
      // React keeps rendering the text it has; the runtime writes the rest.
      if (this.frozenText == null) this.frozenText = this.renderedText;
    }
    const propsTag = create({ type: 'props', props }, inputs);
    api.connectToView(propsTag, viewId);
    this.native = { viewId, propsTag, tags, nodes: this.nodes.slice(), sig: fullSig };
  }

  disconnectNative() {
    const cur = this.native;
    if (!cur) return;
    this.native = null;
    NativeAnimatedAPI.disconnectFromView(cur.propsTag, cur.viewId);
    for (const t of cur.tags) NativeAnimatedAPI.dropNode(t);
  }

  // Animated.event(..., {useNativeDriver: true}) props: mapped natively.
  setNativeEvents(list) {
    const viewId = this.host && typeof this.host.id === 'number' ? this.host.id : null;
    const old = this.nativeEvents;
    const same =
      old.length === list.length && old.every((e, i) => e[0] === list[i][0] && e[1] === list[i][1] && e[3] === viewId);
    if (same) return;
    for (const e of old) if (e[2]) e[2]();
    this.nativeEvents = list.map(([prop, handler]) => [
      prop,
      handler,
      viewId == null ? null : handler.__attachNative(viewId, eventTypeOfProp(prop)),
      viewId,
    ]);
  }

  setNodes(nodes) {
    const old = this.nodes;
    if (old.length === nodes.length && old.every((n, i) => n === nodes[i])) return;
    this.nodes = nodes;
    for (const n of old) if (nodes.indexOf(n) < 0) n.__removeChild(this);
    for (const n of nodes) if (old.indexOf(n) < 0) n.__addChild(this);
  }

  detach() {
    if (this.native) this.disconnectNative();
    this.setNativeEvents([]);
    for (const n of this.nodes) n.__removeChild(this);
    this.nodes = [];
  }

  update() {
    if (this.isNative) return;  // the runtime writes the styles
    const host = this.host;
    if (this.propsAnimated || this.textNode || !host || typeof host.patchStyle !== 'function') {
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

function nativeSignature(viewId, style, keys) {
  let sig = String(viewId);
  for (const k of keys) {
    const v = style[k];
    sig += '|' + k;
    if (k === 'transform' && Array.isArray(v)) {
      for (const t of v) {
        const p = Object.keys(t)[0];
        sig += ',' + p + (t[p] instanceof AnimatedNode ? '*' : '=' + JSON.stringify(t[p]));
      }
    }
  }
  return sig;
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
    const nativeEvents = [];
    let propsAnimated = false;
    // Props the component forwards as a style key (LottieView's progress):
    // animated like style, so the native driver can write them.
    const styleProps = Component.__animatedStyleProps;
    let mapped = null;
    let textNode = null;
    for (const k in props) {
      if (k === 'ref') continue;
      const v = props[k];
      if (k === 'style') continue;
      if (k === 'text' && v instanceof AnimatedNode) {
        // Animated.Text's text={node}: its value is the content.
        textNode = v;
        if (nodes.indexOf(v) < 0) nodes.push(v);
        continue;
      }
      if (styleProps && styleProps[k] && v instanceof AnimatedNode) {
        if (!mapped) mapped = {};
        mapped[styleProps[k]] = v;
        continue;
      }
      if (typeof v === 'function' && v.__isNative) {
        // Native Animated.event: the runtime applies the mapping; JS only
        // runs the listener, if any.
        nativeEvents.push([k, v]);
        if (v.__hasListener) next[k] = v;
        continue;
      }
      if (typeof v === 'object' && v !== null && k !== 'children') {
        found = false;
        const r = resolveCollect(v, nodes, 0, true);
        next[k] = found ? r : v;
        if (found) propsAnimated = true;
      } else {
        next[k] = v;
      }
    }
    const style = flatten(mapped ? [props.style, mapped] : props.style);
    const keys = [];
    if (style) {
      let resolved = null;
      for (const k in style) {
        const v = style[k];
        if (typeof v === 'object' && v !== null) {
          found = false;
          const r = resolveCollect(v, nodes, 0, true);
          if (!found) continue;
          keys.push(k);
          if (!resolved) resolved = Object.assign({}, style);
          resolved[k] = r;
        }
      }
      next.style = resolved || props.style;
    }
    sub.style = style;
    sub.animatedKeys = keys;
    sub.propsAnimated = propsAnimated;
    if (sub.textNode !== textNode) sub.frozenText = null;
    sub.textNode = textNode;
    if (textNode) {
      const v = textNode.__getValue();
      sub.renderedText = sub.frozenText != null && sub.isNative ? sub.frozenText : typeof v === 'string' ? v : String(v);
      next.children = sub.renderedText;
    }

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
      sub.syncNative();
      sub.setNativeEvents(nativeEvents);
      // A value may have changed between render and commit.
      if (nodes.length && !propsAnimated && !textNode) sub.update();
    });
    useLayoutEffect(() => () => sub.detach(), []);

    // jsx() keeps `next` as the props (createElement would copy it).
    return jsx(Component, next);
  }
  const name = typeof Component === 'string' ? Component : Component.displayName || Component.name || 'Component';
  AnimatedComponent.displayName = 'Animated(' + name + ')';
  return AnimatedComponent;
}
