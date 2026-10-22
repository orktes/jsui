// Host instances: the JS mirror of native nodes. Holds the latest React props
// (for event handlers), the last props sent over the bridge (for diffing),
// and the parent link used for capture/bubble propagation.

import { native, warnOnce } from './native.js';
import { flatten, deepEqual } from './StyleSheet.js';

/** @type {Map<number, HostInstance|TextInstance>} */
export const instances = new Map();

// React prop -> tokens in props.__events. Native matches them by substring
// (docs/ARCHITECTURE.md "Input routing details"): "press"/"click" make a node
// pressable (focusable, pointer cursor), "move"/"responder" request
// pointermove, "hover"/"enter"/"leave" request pointerenter/leave, "wheel"
// wheel events. Plain pointer down/up listeners deliberately avoid "press"
// so a View with onPointerDown does not become a focus target.
const RESPONDER = ['responder', 'pointerMove'];
const EVENT_TOKENS = {
  onClick: ['click'],
  onPress: ['press'],
  onPointerDown: ['pointerDown'],
  onPointerUp: ['pointerUp'],
  onPointerMove: ['pointerMove'],
  onPointerCancel: ['pointerCancel'],
  onPointerEnter: ['hoverIn', 'pointerEnter'],
  onPointerLeave: ['hoverOut', 'pointerLeave'],
  onMouseEnter: ['hoverIn', 'pointerEnter'],
  onMouseLeave: ['hoverOut', 'pointerLeave'],
  onWheel: ['wheel'],
  onKeyDown: ['keyDown'],
  onKeyUp: ['keyUp'],
  onFocus: ['focus'],
  onBlur: ['blur'],
  onScroll: ['scroll'],
  onTextInput: ['textInput'],
  onGamepadButton: ['gamepadButton'],
  onGamepadAxis: ['gamepadAxis'],
  onLayout: ['layout'],
  onLoad: ['load'],
  onError: ['error'],
  onStartShouldSetResponder: RESPONDER,
  onMoveShouldSetResponder: RESPONDER,
  onStartShouldSetResponderCapture: RESPONDER,
  onMoveShouldSetResponderCapture: RESPONDER,
  onResponderGrant: RESPONDER,
  onResponderMove: RESPONDER,
  onResponderRelease: RESPONDER,
};
for (const k of Object.keys(EVENT_TOKENS)) {
  if (!k.includes('Responder')) EVENT_TOKENS[k + 'Capture'] = EVENT_TOKENS[k];
}

// Props that never cross the bridge.
const SKIP = { children: 1, style: 1, ref: 1, key: 1 };

/**
 * Builds the props object sent to native: functions removed, style
 * flattened, __events list attached.
 */
export function toNativeProps(props, hidden) {
  const out = {};
  let events = null;
  for (const key in props) {
    if (SKIP[key] === 1) continue;
    const v = props[key];
    if (v === undefined) continue;
    if (typeof v === 'function') {
      const toks = EVENT_TOKENS[key];
      if (toks) {
        if (!events) events = [];
        for (let i = 0; i < toks.length; i++) if (events.indexOf(toks[i]) < 0) events.push(toks[i]);
      }
      continue;
    }
    if (typeof v === 'symbol') continue;
    out[key] = v;
  }
  let style = flatten(props.style);
  // RN accepts pointerEvents as a prop; native reads it from style.
  if (props.pointerEvents && (!style || style.pointerEvents === undefined)) {
    style = Object.assign({}, style, { pointerEvents: props.pointerEvents });
  }
  if (hidden) style = Object.assign({}, style, { display: 'none' });
  if (style !== undefined) out.style = style;
  if (events) out.__events = events;
  return out;
}

/** True if two native prop objects differ (shallow per key, deep for objects). */
export function nativePropsChanged(a, b) {
  if (a === b) return false;
  let n = 0;
  for (const k in a) {
    n++;
    const x = a[k];
    const y = b[k];
    if (x === y) continue;
    if (typeof x === 'object' && typeof y === 'object' && deepEqual(x, y)) continue;
    return true;
  }
  for (const k in b) n--;
  return n !== 0;
}

// Cheap pre-check on the React props: identical values (or a handler swapped
// for another handler) cannot change what native sees, so most re-renders
// skip building and diffing native props entirely.
function mayDiffer(a, b) {
  let n = 0;
  for (const k in b) {
    n++;
    const x = b[k];
    const y = a[k];
    if (x === y || k === 'children') continue;
    if (typeof x === 'function' && typeof y === 'function') continue;
    return true;
  }
  for (const k in a) n--;
  return n !== 0;
}

let bridgeCalls = 0;
/**
 * Number of setProps + patchStyle bridge calls so far, and of live host
 * nodes (each one a native node), for benchmarks and tests.
 */
export function bridgeStats() {
  return { calls: bridgeCalls, nodes: instances.size };
}

export class HostInstance {
  constructor(id, type, props) {
    this.id = id;
    this.type = type;
    this.props = props;
    this.parent = null;
    this.hidden = false;
    this.nativeProps = toNativeProps(props, false);
    this.styleOwned = false;  // nativeProps.style is a private copy we may mutate
    this.responderState = null;
    bridgeCalls++;
    native.setProps(id, this.nativeProps);
  }

  get _nativeTag() {
    return this.id;
  }

  /** Re-sends props if anything the native side sees changed. */
  update(props) {
    const old = this.props;
    this.props = props;
    if (!mayDiffer(old, props)) return;
    const next = toNativeProps(props, this.hidden);
    if (nativePropsChanged(next, this.nativeProps)) {
      this.nativeProps = next;
      this.styleOwned = false;
      bridgeCalls++;
      native.setProps(this.id, next);
    }
  }

  setHidden(hidden) {
    if (this.hidden === hidden) return;
    this.hidden = hidden;
    this.nativeProps = toNativeProps(this.props, hidden);
    this.styleOwned = false;
    bridgeCalls++;
    native.setProps(this.id, this.nativeProps);
  }

  /**
   * Merges a partial style into the node without a React render (Animated,
   * setNativeProps). Uses the bridge's patchStyle when available.
   */
  patchStyle(partial) {
    const cur = this.nativeProps.style;
    let changed = false;
    for (const k in partial) {
      const v = partial[k];
      if (!cur || !(cur[k] === v || (typeof v === 'object' && deepEqual(cur[k], v)))) {
        changed = true;
        break;
      }
    }
    if (!changed) return;
    // Copy the style once (it may be a frozen StyleSheet object), then
    // mutate our own copy: no per-frame allocation for animations.
    let style = cur;
    if (!this.styleOwned || !style) {
      style = this.nativeProps.style = Object.assign({}, cur);
      this.styleOwned = true;
    }
    for (const k in partial) style[k] = partial[k];
    bridgeCalls++;
    if (typeof native.patchStyle === 'function') native.patchStyle(this.id, partial);
    else native.setProps(this.id, this.nativeProps);
  }

  setNativeProps(props) {
    if (!props) return;
    let rest = null;
    for (const k in props) {
      if (k === 'style') continue;
      if (!rest) rest = {};
      rest[k] = props[k];
    }
    if (rest) {
      // Non-style props: merge into the React props so later diffs agree.
      this.props = Object.assign({}, this.props, rest);
      const next = toNativeProps(Object.assign({}, this.props, { style: this.nativeProps.style }), this.hidden);
      this.nativeProps = next;
      this.styleOwned = false;
      bridgeCalls++;
      native.setProps(this.id, next);
    }
    if (props.style) this.patchStyle(flatten(props.style) || {});
  }

  measure(callback) {
    const m = measureNode(this.id);
    if (callback) callback(m.x, m.y, m.width, m.height, m.pageX, m.pageY);
  }

  measureInWindow(callback) {
    const m = measureNode(this.id);
    if (callback) callback(m.pageX, m.pageY, m.width, m.height);
  }

  measureLayout(relativeTo, onSuccess, onFail) {
    const other = typeof relativeTo === 'number' ? relativeTo : relativeTo && relativeTo.id;
    if (other == null) {
      if (onFail) onFail();
      return;
    }
    const a = measureNode(this.id);
    const b = measureNode(other);
    if (onSuccess) onSuccess(a.pageX - b.pageX, a.pageY - b.pageY, a.width, a.height);
  }

  focus() {
    if (typeof native.focus === 'function') native.focus(this.id);
  }

  blur() {
    if (typeof native.blur === 'function') native.blur(this.id);
  }

  // ScrollView methods (the ScrollView component forwards its ref here).
  scrollTo(yOrOptions, x, animated) {
    let opts = yOrOptions;
    if (typeof yOrOptions === 'number' || yOrOptions == null) opts = { x: x || 0, y: yOrOptions || 0, animated };
    if (typeof native.scrollTo === 'function') {
      native.scrollTo(this.id, +opts.x || 0, +opts.y || 0, opts.animated !== false);
    }
  }

  scrollToEnd(options) {
    const content = this._content;
    const self = measureNode(this.id);
    const inner = content ? measureNode(content.id) : self;
    const horizontal = !!this.props.horizontal;
    this.scrollTo({
      x: horizontal ? Math.max(0, inner.width - self.width) : 0,
      y: horizontal ? 0 : Math.max(0, inner.height - self.height),
      animated: !options || options.animated !== false,
    });
  }

  flashScrollIndicators() {}

  getScrollResponder() {
    return this;
  }

  getScrollableNode() {
    return this.id;
  }

  getNativeScrollRef() {
    return this;
  }
}

export class TextInstance {
  constructor(id, text) {
    this.id = id;
    this.type = 'RawText';
    this.text = text;
    this.parent = null;
    this.props = null;
  }
}

const ZERO = { x: 0, y: 0, width: 0, height: 0, pageX: 0, pageY: 0 };
export function measureNode(id) {
  if (typeof native.measure !== 'function') {
    warnOnce('measure', '[jsui] __jsui_native.measure is not available');
    return ZERO;
  }
  return native.measure(id) || ZERO;
}

