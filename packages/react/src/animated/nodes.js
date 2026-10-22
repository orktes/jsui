// Animated value graph. Values notify dependents (interpolations, math
// nodes, mounted animated components) when they change. Components are
// collected into a dirty set and flushed once per animation frame with
// direct style patches, so animations never re-render React.

import { parseColor } from '../color.js';

let nextListenerId = 1;

// --- dirty component batching ----------------------------------------------
const dirty = new Set();
let batchDepth = 0;

/** Runs fn with component flushes deferred to the end (one patch per component). */
export function batch(fn) {
  batchDepth++;
  try {
    fn();
  } finally {
    batchDepth--;
    if (batchDepth === 0) flushDirty();
  }
}

export function flushDirty() {
  if (dirty.size === 0) return;
  for (const c of dirty) {
    dirty.delete(c);
    c.update();
  }
}

export class AnimatedNode {
  constructor() {
    this._children = null;  // dependent nodes / component subscriptions
    this._listeners = null;
  }
  __getValue() {
    return 0;
  }
  __addChild(child) {
    if (!this._children) this._children = [];
    if (this._children.indexOf(child) < 0) {
      this._children.push(child);
      if (this._children.length === 1) this.__attach();
    }
  }
  __removeChild(child) {
    if (!this._children) return;
    const i = this._children.indexOf(child);
    if (i >= 0) {
      this._children.splice(i, 1);
      if (this._children.length === 0) this.__detach();
    }
  }
  __getChildren() {
    return this._children || [];
  }
  __attach() {}
  __detach() {}
  __isNative() {
    return false;
  }
  __makeNative() {}
  // Called when an input changed: propagate to dependents.
  __changed() {
    if (this._listeners) {
      const value = this.__getValue();
      for (const id in this._listeners) this._listeners[id]({ value });
    }
    const kids = this._children;
    if (!kids) return;
    for (let i = 0; i < kids.length; i++) {
      const k = kids[i];
      if (k.__isComponent) dirty.add(k);
      else k.__changed();
    }
  }
  addListener(callback) {
    const id = String(nextListenerId++);
    if (!this._listeners) this._listeners = {};
    this._listeners[id] = callback;
    return id;
  }
  removeListener(id) {
    if (this._listeners) delete this._listeners[id];
  }
  removeAllListeners() {
    this._listeners = null;
  }
  hasListeners() {
    return !!this._listeners && Object.keys(this._listeners).length > 0;
  }
  interpolate(config) {
    return new AnimatedInterpolation(this, config);
  }
  toJSON() {
    return this.__getValue();
  }
}

export class AnimatedValue extends AnimatedNode {
  constructor(value = 0, config) {
    super();
    if (typeof value !== 'number') value = +value || 0;
    this._value = value;
    this._startingValue = value;
    this._offset = 0;
    this._animation = null;
    this._tracking = null;
    if (config && config.useNativeDriver) this.__makeNative();
  }
  __getValue() {
    return this._value + this._offset;
  }
  setValue(value) {
    if (this._animation) {
      this._animation.stop();
      this._animation = null;
    }
    this._updateValue(+value, true);
  }
  setOffset(offset) {
    this._offset = offset;
    this._updateValue(this._value, true);
  }
  flattenOffset() {
    this._value += this._offset;
    this._offset = 0;
  }
  extractOffset() {
    this._offset += this._value;
    this._value = 0;
  }
  stopAnimation(callback) {
    this.stopTracking();
    if (this._animation) this._animation.stop();
    this._animation = null;
    if (callback) callback(this.__getValue());
  }
  resetAnimation(callback) {
    this.stopAnimation(callback);
    this._value = this._startingValue;
    this._updateValue(this._value, true);
  }
  animate(animation, callback) {
    const previous = this._animation;
    if (previous) previous.stop();
    this._animation = animation;
    animation.start(
      this._value,
      (v) => this._updateValue(v, true),
      (result) => {
        if (this._animation === animation) this._animation = null;
        if (callback) callback(result);
      },
      previous,
      this,
    );
  }
  stopTracking() {
    if (this._tracking) this._tracking.__detach();
    this._tracking = null;
  }
  track(tracking) {
    this.stopTracking();
    this._tracking = tracking;
  }
  _updateValue(value, flush) {
    this._value = value;
    if (flush && batchDepth === 0) {
      batchDepth++;
      try {
        this.__changed();
      } finally {
        batchDepth--;
      }
      flushDirty();
    } else {
      this.__changed();
    }
  }
}

export class AnimatedValueXY extends AnimatedNode {
  constructor(value) {
    super();
    const v = value || { x: 0, y: 0 };
    this.x = v.x instanceof AnimatedValue ? v.x : new AnimatedValue(v.x || 0);
    this.y = v.y instanceof AnimatedValue ? v.y : new AnimatedValue(v.y || 0);
  }
  __getValue() {
    return { x: this.x.__getValue(), y: this.y.__getValue() };
  }
  setValue(v) {
    batch(() => {
      this.x.setValue(v.x);
      this.y.setValue(v.y);
    });
  }
  setOffset(o) {
    batch(() => {
      this.x.setOffset(o.x);
      this.y.setOffset(o.y);
    });
  }
  flattenOffset() {
    this.x.flattenOffset();
    this.y.flattenOffset();
  }
  extractOffset() {
    this.x.extractOffset();
    this.y.extractOffset();
  }
  stopAnimation(cb) {
    this.x.stopAnimation();
    this.y.stopAnimation();
    if (cb) cb(this.__getValue());
  }
  resetAnimation(cb) {
    this.x.resetAnimation();
    this.y.resetAnimation();
    if (cb) cb(this.__getValue());
  }
  addListener(cb) {
    const id = String(nextListenerId++);
    const fire = () => cb(this.__getValue());
    this._xyListeners = this._xyListeners || {};
    this._xyListeners[id] = [this.x.addListener(fire), this.y.addListener(fire)];
    return id;
  }
  removeListener(id) {
    const l = this._xyListeners && this._xyListeners[id];
    if (!l) return;
    this.x.removeListener(l[0]);
    this.y.removeListener(l[1]);
    delete this._xyListeners[id];
  }
  removeAllListeners() {
    for (const id in this._xyListeners || {}) this.removeListener(id);
  }
  getLayout() {
    return { left: this.x, top: this.y };
  }
  getTranslateTransform() {
    return [{ translateX: this.x }, { translateY: this.y }];
  }
}

// Color value: four channels animated together, outputs rgba().
export class AnimatedColor extends AnimatedNode {
  constructor(value) {
    super();
    const c = parseColor(value || 'black') || [0, 0, 0, 1];
    this.r = new AnimatedValue(c[0]);
    this.g = new AnimatedValue(c[1]);
    this.b = new AnimatedValue(c[2]);
    this.a = new AnimatedValue(c[3]);
    for (const ch of [this.r, this.g, this.b, this.a]) ch.__addChild(this);
  }
  __getValue() {
    return `rgba(${Math.round(this.r.__getValue())}, ${Math.round(this.g.__getValue())}, ${Math.round(this.b.__getValue())}, ${this.a.__getValue()})`;
  }
  setValue(value) {
    const c = parseColor(value) || [0, 0, 0, 1];
    batch(() => {
      this.r.setValue(c[0]);
      this.g.setValue(c[1]);
      this.b.setValue(c[2]);
      this.a.setValue(c[3]);
    });
  }
}

// --- interpolation -----------------------------------------------------------

const NUMBER_RE = /[+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?/g;

function findRange(input, inputRange) {
  let i;
  for (i = 1; i < inputRange.length - 1; ++i) if (inputRange[i] >= input) break;
  return i - 1;
}

function interpolateNumber(input, inMin, inMax, outMin, outMax, easing, extrapolateLeft, extrapolateRight) {
  let result = input;
  if (result < inMin) {
    if (extrapolateLeft === 'identity') return result;
    if (extrapolateLeft === 'clamp') result = inMin;
  }
  if (result > inMax) {
    if (extrapolateRight === 'identity') return result;
    if (extrapolateRight === 'clamp') result = inMax;
  }
  if (outMin === outMax) return outMin;
  if (inMin === inMax) return input <= inMin ? outMin : outMax;
  if (inMin === -Infinity) result = -result;
  else if (inMax === Infinity) result = result - inMin;
  else result = (result - inMin) / (inMax - inMin);
  result = easing(result);
  if (outMin === -Infinity) result = -result;
  else if (outMax === Infinity) result = result + outMin;
  else result = result * (outMax - outMin) + outMin;
  return result;
}

/** Builds the interpolation function for a config (numbers, colors, or strings with numbers). */
export function createInterpolation(config) {
  const inputRange = config.inputRange;
  const outputRange = config.outputRange;
  if (!inputRange || !outputRange || inputRange.length < 2 || inputRange.length !== outputRange.length) {
    throw new Error('Animated interpolate: inputRange and outputRange must be arrays of the same length (>= 2)');
  }
  const easing = config.easing || ((t) => t);
  const left = config.extrapolateLeft || config.extrapolate || 'extend';
  const right = config.extrapolateRight || config.extrapolate || 'extend';
  const num = (outs) => (input) => {
    const r = findRange(input, inputRange);
    return interpolateNumber(input, inputRange[r], inputRange[r + 1], outs[r], outs[r + 1], easing, left, right);
  };
  if (typeof outputRange[0] === 'number') return num(outputRange);

  const colors = outputRange.map(parseColor);
  if (colors.every(Boolean)) {
    const ch = [0, 1, 2, 3].map((c) => num(colors.map((col) => col[c])));
    return (input) => {
      const r = Math.round(Math.max(0, Math.min(255, ch[0](input))));
      const g = Math.round(Math.max(0, Math.min(255, ch[1](input))));
      const b = Math.round(Math.max(0, Math.min(255, ch[2](input))));
      const a = Math.max(0, Math.min(1, ch[3](input)));
      return `rgba(${r}, ${g}, ${b}, ${Math.round(a * 1000) / 1000})`;
    };
  }
  // Strings like '0deg' / '10px': interpolate each number, keep the template.
  const template = String(outputRange[0]);
  const numbers = outputRange.map((s) => (String(s).match(NUMBER_RE) || []).map(Number));
  const count = numbers[0].length;
  const fns = [];
  for (let i = 0; i < count; i++) fns.push(num(numbers.map((n) => (n[i] !== undefined ? n[i] : 0))));
  return (input) => {
    let i = 0;
    return template.replace(NUMBER_RE, () => {
      const v = fns[i++](input);
      return String(Math.round(v * 1e5) / 1e5);
    });
  };
}

export class AnimatedInterpolation extends AnimatedNode {
  constructor(parent, config) {
    super();
    this._parent = parent;
    this._config = config;
    this._fn = createInterpolation(config);
  }
  __getValue() {
    const v = this._parent.__getValue();
    return this._fn(typeof v === 'number' ? v : +v);
  }
  __attach() {
    this._parent.__addChild(this);
  }
  __detach() {
    this._parent.__removeChild(this);
  }
}

class AnimatedOp extends AnimatedNode {
  constructor(inputs, fn) {
    super();
    this._inputs = inputs.map((v) => (v instanceof AnimatedNode ? v : new AnimatedValue(+v)));
    this._fn = fn;
  }
  __getValue() {
    return this._fn(this._inputs.map((n) => n.__getValue()));
  }
  __attach() {
    for (const n of this._inputs) n.__addChild(this);
  }
  __detach() {
    for (const n of this._inputs) n.__removeChild(this);
  }
}

export const add = (a, b) => new AnimatedOp([a, b], (v) => v[0] + v[1]);
export const subtract = (a, b) => new AnimatedOp([a, b], (v) => v[0] - v[1]);
export const multiply = (a, b) => new AnimatedOp([a, b], (v) => v[0] * v[1]);
export const divide = (a, b) =>
  new AnimatedOp([a, b], (v) => {
    if (v[1] === 0) console.error('Animated.divide: detected division by zero');
    return v[0] / v[1];
  });
export const modulo = (a, m) => new AnimatedOp([a], (v) => ((v[0] % m) + m) % m);
export function diffClamp(a, min, max) {
  let last = null;
  let value = 0;
  return new AnimatedOp([a], (v) => {
    const cur = v[0];
    const diff = last === null ? 0 : cur - last;
    last = cur;
    value = Math.min(Math.max(value + diff, min), max);
    return value;
  });
}
