// Animated value graph. Values notify dependents (interpolations, math
// nodes, mounted animated components) when they change. Components are
// collected into a dirty set and flushed once per animation frame with
// direct style patches, so animations never re-render React.

import { parseColor } from '../color.js';
import {
  NativeAnimatedAPI,
  generateNewNodeTag,
  trackNativeNode,
  untrackNativeNode,
  startListening,
  stopListening,
  sampleCurve,
  generateNewAnimationId,
  startAnimatingNode,
  stopAnimation as stopNativeAnimationById,
} from './NativeAnimatedHelper.js';
import { HostBuffer } from '../hostBuffer.js';
import { HostState } from '../hostData.js';
import { now, requestFrame, cancelFrame } from '../native.js';
import { compileExpression, evaluateExpression, ExpressionError } from './expression.js';

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
      // A native node's dependents are native too (RN AnimatedWithChildren).
      if (this._isNative) child.__makeNative();
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

  // --- native driver (useNativeDriver: true; see NativeAnimatedHelper.js) ---
  // Once native, a node stays native (like React Native): its value lives
  // in the runtime and JS-driven animations on it throw.
  __isNative() {
    return !!this._isNative;
  }
  /** Inputs this node is computed from (made native with it). */
  __getInputs() {
    return [];
  }
  __makeNative() {
    if (this._isNative || !NativeAnimatedAPI) return;
    this._isNative = true;
    for (const input of this.__getInputs()) input.__makeNative();
    this.__getNativeTag();
    for (const child of this.__getChildren().slice()) child.__makeNative();
  }
  __getNativeConfig() {
    throw new Error('This node cannot be made a "native" animated node');
  }
  /** Creates the native node on first use (inputs first) and returns its tag. */
  __getNativeTag() {
    if (this._nativeTag == null) {
      if (!NativeAnimatedAPI) throw new Error('Animated: the native animated module is not available');
      if (!this._isNative) {
        this.__makeNative();  // creates the node
        if (this._nativeTag != null) return this._nativeTag;
      }
      const tag = generateNewNodeTag();
      const config = this.__getNativeConfig();
      NativeAnimatedAPI.createNode(tag, config);
      this._nativeTag = tag;
      trackNativeNode(this, tag);
      for (const input of this.__getInputs()) NativeAnimatedAPI.connect(input.__getNativeTag(), tag);
      this.__nativeCreated(tag);
    }
    return this._nativeTag;
  }
  __nativeCreated() {}
  /** Frees the native node (it is created again when needed). */
  __dropNative() {
    if (this._nativeTag == null) return;
    const tag = this._nativeTag;
    this._nativeTag = null;
    untrackNativeNode(this);
    NativeAnimatedAPI.dropNode(tag);
  }
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
    // Native: the runtime owns the value (read synchronously).
    if (this._nativeTag != null) this._value = NativeAnimatedAPI.getValue(this._nativeTag);
    return this._value + this._offset;
  }
  __getNativeConfig() {
    return { type: 'value', value: this._value, offset: this._offset };
  }
  __nativeCreated(tag) {
    if (this.hasListeners()) startListening(tag, this);
  }
  __dropNative() {
    if (this._nativeTag == null) return;
    this.__getValue();
    stopListening(this._nativeTag);
    super.__dropNative();
  }
  /** A natively driven change (animation frame, event): sync and notify listeners. */
  __onAnimatedValueUpdateReceived(value) {
    this._value = value;
    if (this._listeners) {
      const v = this._value + this._offset;
      for (const id in this._listeners) this._listeners[id]({ value: v });
    }
  }
  addListener(callback) {
    const id = super.addListener(callback);
    if (this._nativeTag != null) startListening(this._nativeTag, this);
    return id;
  }
  removeListener(id) {
    super.removeListener(id);
    if (this._nativeTag != null && !this.hasListeners()) stopListening(this._nativeTag);
  }
  removeAllListeners() {
    super.removeAllListeners();
    if (this._nativeTag != null) stopListening(this._nativeTag);
  }
  setValue(value) {
    if (this._animation) {
      this._animation.stop();
      this._animation = null;
    }
    if (this._nativeTag != null) NativeAnimatedAPI.setValue(this._nativeTag, +value);
    this._updateValue(+value, true);
  }
  setOffset(offset) {
    if (this._nativeTag != null) {
      this.__getValue();
      NativeAnimatedAPI.setOffset(this._nativeTag, offset);
    }
    this._offset = offset;
    this._updateValue(this._value, true);
  }
  flattenOffset() {
    if (this._nativeTag != null) {
      this.__getValue();
      NativeAnimatedAPI.flattenOffset(this._nativeTag);
    }
    this._value += this._offset;
    this._offset = 0;
  }
  extractOffset() {
    if (this._nativeTag != null) {
      this.__getValue();
      NativeAnimatedAPI.extractOffset(this._nativeTag);
    }
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
    // React Native: a native value is reset natively too.
    if (this._nativeTag != null) NativeAnimatedAPI.setValue(this._nativeTag, this._startingValue);
    this._updateValue(this._value, true);
  }
  animate(animation, callback) {
    const previous = this._animation;
    if (previous) previous.stop();
    this._animation = animation;
    if (this._nativeTag != null) this.__getValue();
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
  // Native: the channels are the native values (no node of its own).
  __makeNative() {
    this.x.__makeNative();
    this.y.__makeNative();
  }
}

// Color value: four channels animated together, outputs rgba(). Accepts a
// color string, {r, g, b, a} numbers or {r, g, b, a} Animated.Values (RN).
export function colorChannels(value) {
  if (value instanceof AnimatedColor) return [value.r, value.g, value.b, value.a].map((n) => n.__getValue());
  if (value && typeof value === 'object' && !(value instanceof AnimatedNode) && 'r' in value) {
    const ch = (v, d) => (v instanceof AnimatedNode ? v.__getValue() : v === undefined ? d : +v);
    return [ch(value.r, 0), ch(value.g, 0), ch(value.b, 0), ch(value.a, 1)];
  }
  return parseColor(value == null ? 'black' : value) || [0, 0, 0, 1];
}

const CHANNELS = ['r', 'g', 'b', 'a'];

export class AnimatedColor extends AnimatedNode {
  constructor(value) {
    super();
    const given = value && typeof value === 'object' && !(value instanceof AnimatedNode) && 'r' in value ? value : null;
    const c = colorChannels(value);
    CHANNELS.forEach((k, i) => {
      this[k] = given && given[k] instanceof AnimatedValue ? given[k] : new AnimatedValue(c[i]);
    });
    for (const k of CHANNELS) this[k].__addChild(this);
  }
  __getInputs() {
    return [this.r, this.g, this.b, this.a];
  }
  __getNativeConfig() {
    return { type: 'color', r: this.r.__getNativeTag(), g: this.g.__getNativeTag(), b: this.b.__getNativeTag(), a: this.a.__getNativeTag() };
  }
  __getValue() {
    return `rgba(${Math.round(this.r.__getValue())}, ${Math.round(this.g.__getValue())}, ${Math.round(this.b.__getValue())}, ${this.a.__getValue()})`;
  }
  setValue(value) {
    const c = colorChannels(value);
    batch(() => CHANNELS.forEach((k, i) => this[k].setValue(c[i])));
  }
  setOffset(value) {
    const c = colorChannels(value);
    batch(() => CHANNELS.forEach((k, i) => this[k].setOffset(c[i])));
  }
  flattenOffset() {
    for (const k of CHANNELS) this[k].flattenOffset();
  }
  extractOffset() {
    for (const k of CHANNELS) this[k].extractOffset();
  }
  stopAnimation(cb) {
    for (const k of CHANNELS) this[k].stopAnimation();
    if (cb) cb(this.__getValue());
  }
  resetAnimation(cb) {
    batch(() => {
      for (const k of CHANNELS) this[k].resetAnimation();
    });
    if (cb) cb(this.__getValue());
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

/** The interpolation as the native driver evaluates it (same formulas as createInterpolation). */
export function interpolationNativeConfig(config) {
  const outputRange = config.outputRange;
  const out = {
    type: 'interpolation',
    inputRange: config.inputRange.slice(),
    extrapolateLeft: config.extrapolateLeft || config.extrapolate || 'extend',
    extrapolateRight: config.extrapolateRight || config.extrapolate || 'extend',
  };
  if (config.easing) out.easing = sampleCurve(config.easing, 257);
  if (typeof outputRange[0] === 'number') {
    out.outputType = 'number';
    out.outputs = [outputRange.slice()];
    return out;
  }
  const colors = outputRange.map(parseColor);
  if (colors.every(Boolean)) {
    out.outputType = 'color';
    out.outputs = [0, 1, 2, 3].map((c) => colors.map((col) => col[c]));
    return out;
  }
  const template = String(outputRange[0]);
  const numbers = outputRange.map((s) => (String(s).match(NUMBER_RE) || []).map(Number));
  out.outputType = 'string';
  out.parts = template.split(NUMBER_RE);
  out.outputs = numbers[0].map((_, i) => numbers.map((n) => (n[i] !== undefined ? n[i] : 0)));
  return out;
}

// Element `index` of frame data buffer `key` (jsui_set_buffer;
// Animated.hostBuffer). Native whenever the runtime has the native module:
// the runtime then writes the views that use it as the buffer changes,
// without JS. Otherwise (JS driver) it follows HostBuffer.subscribe.
export class AnimatedHostBuffer extends AnimatedNode {
  constructor(key, index) {
    super();
    this._key = String(key);
    this._index = Math.max(0, Math.floor(+index || 0));
    this._sub = null;
    this._listenSub = null;
    if (NativeAnimatedAPI) this._isNative = true;
  }
  __getValue() {
    const b = HostBuffer.get(this._key);
    const v = b && this._index < b.length ? b[this._index] : 0;
    return v === v ? v : 0;
  }
  __getNativeConfig() {
    return { type: 'hostBuffer', key: this._key, index: this._index };
  }
  __attach() {
    if (!this._isNative) this._sub = HostBuffer.subscribe(this._key, () => batch(() => this.__changed()));
  }
  __detach() {
    if (this._sub) this._sub();
    this._sub = null;
    this.__dropNative();
  }
  addListener(callback) {
    const id = super.addListener(callback);
    if (!this._listenSub) {
      this._listenSub = HostBuffer.subscribe(this._key, () => {
        // An attached JS-driven node tells its listeners in __changed().
        if (this._sub || !this._listeners) return;
        const value = this.__getValue();
        for (const k in this._listeners) this._listeners[k]({ value });
      });
    }
    return id;
  }
  removeListener(id) {
    super.removeListener(id);
    if (this._listenSub && !this.hasListeners()) {
      this._listenSub();
      this._listenSub = null;
    }
  }
  removeAllListeners() {
    super.removeAllListeners();
    if (this._listenSub) this._listenSub();
    this._listenSub = null;
  }
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
    this.__dropNative();
  }
  __getInputs() {
    return [this._parent];
  }
  __getNativeConfig() {
    return interpolationNativeConfig(this._config);
  }
}

class AnimatedOp extends AnimatedNode {
  constructor(inputs, fn, nativeConfig) {
    super();
    this._inputs = inputs.map((v) => (v instanceof AnimatedNode ? v : new AnimatedValue(+v)));
    this._fn = fn;
    this._nativeConfig = nativeConfig;  // () => extra config ({type, ...})
  }
  __getInputs() {
    return this._inputs;
  }
  __getNativeConfig() {
    const config = this._nativeConfig();
    const tags = this._inputs.map((n) => n.__getNativeTag());
    config.input = tags.length === 1 ? tags[0] : tags;
    return config;
  }
  __getValue() {
    return this._fn(this._inputs.map((n) => n.__getValue()));
  }
  __attach() {
    for (const n of this._inputs) n.__addChild(this);
  }
  __detach() {
    for (const n of this._inputs) n.__removeChild(this);
    this.__dropNative();
  }
}

const opType = (type) => () => ({ type });
export const add = (a, b) => new AnimatedOp([a, b], (v) => v[0] + v[1], opType('addition'));
export const subtract = (a, b) => new AnimatedOp([a, b], (v) => v[0] - v[1], opType('subtraction'));
export const multiply = (a, b) => new AnimatedOp([a, b], (v) => v[0] * v[1], opType('multiplication'));
export const divide = (a, b) =>
  new AnimatedOp(
    [a, b],
    (v) => {
      if (v[1] === 0) console.error('Animated.divide: detected division by zero');
      return v[0] / v[1];
    },
    opType('division'),
  );
export const modulo = (a, m) => new AnimatedOp([a], (v) => ((v[0] % m) + m) % m, () => ({ type: 'modulus', modulus: m }));
export function diffClamp(a, min, max) {
  // Like RN's AnimatedDiffClamp: starts at the input's current value and
  // then follows its changes, clamped to [min, max].
  const input = a instanceof AnimatedNode ? a : new AnimatedValue(+a);
  let last = input.__getValue();
  let value = last;
  return new AnimatedOp([input], (v) => {
    const cur = v[0];
    const diff = cur - last;
    last = cur;
    value = Math.min(Math.max(value + diff, min), max);
    return value;
  }, () => ({ type: 'diffclamp', min, max, last, value }));
}

// Animated.derive(inputs, fn): fn's body compiled to an expression program
// (expression.js). Native when any input is (the runtime evaluates it every
// frame: no JS, no lag); otherwise the JS driver runs the same program. A
// function outside the expression subset still works with the JS driver
// (fn is called) but cannot go native: making it native throws.
const compiledBySource = new Map();

/** Animated.expr(fn): an expression function kept for Animated.derive(inputs, expr). */
export class AnimatedExpression {
  constructor(fn, meta) {
    if (typeof fn !== 'function') throw new TypeError('Animated.expr(fn): fn must be a function');
    this.fn = fn;
    this.meta = meta;
  }
}  // source -> {code, slots, free} (scope-free programs)

export class AnimatedDerived extends AnimatedNode {
  constructor(inputs, fn, meta) {
    super();
    if (!Array.isArray(inputs)) inputs = [inputs];
    if (fn instanceof AnimatedExpression) {
      meta = fn.meta;
      fn = fn.fn;
    }
    if (typeof fn !== 'function') throw new TypeError('Animated.derive(inputs, fn): fn must be a function or an Animated.expr()');
    this._fn = fn;
    let inputNodes = inputs.map((v) => (v instanceof AnimatedNode ? v : new AnimatedValue(+v || 0)));
    this._program = null;
    this._error = null;
    const source = meta && typeof meta.source === 'string' ? meta.source : Function.prototype.toString.call(fn);
    if (/\{\s*\[native code\]\s*\}\s*$/.test(source)) {
      this._error = 'its source is not available (build the bundle with jsui build, which keeps it)';
    } else {
      try {
        const scope = meta && meta.scope;
        let prog = !scope ? compiledBySource.get(source) : null;
        if (!prog || prog.inputCount !== inputNodes.length) {
          prog = compileExpression(source, inputNodes.length, scope);
          prog.inputCount = inputNodes.length;
          if (!scope && prog.inputs.length === 0) compiledBySource.set(source, prog);
        }
        this._program = prog;
        inputNodes = inputNodes.concat(prog.inputs);
      } catch (e) {
        if (!(e instanceof ExpressionError)) throw e;
        this._error = e.message;
      }
    }
    this._inputs = inputNodes;
    this._values = new Array(inputNodes.length);
    if (this._inputs.some((n) => n.__isNative())) this.__makeNative();
  }
  __getInputs() {
    return this._inputs;
  }
  __getValue() {
    const vals = this._values;
    for (let i = 0; i < vals.length; i++) vals[i] = this._inputs[i].__getValue();
    if (this._program) return evaluateExpression(this._program.code, vals, this._program.slots);
    const v = +this._fn(...vals.slice(0, vals.length));
    return v === v ? v : 0;
  }
  __makeNative() {
    if (!this._program && NativeAnimatedAPI) {
      throw new Error(`Animated.derive: this function cannot run natively: ${this._error}`);
    }
    super.__makeNative();
  }
  __getNativeConfig() {
    return {
      type: 'expression',
      input: this._inputs.map((n) => n.__getNativeTag()),
      code: this._program.code,
      slots: this._program.slots,
    };
  }
  __attach() {
    for (const n of this._inputs) n.__addChild(this);
  }
  __detach() {
    for (const n of this._inputs) n.__removeChild(this);
    this.__dropNative();
  }
}

// Animated.clock(): the frame clock in seconds. Native whenever the native
// module is there (the runtime advances it every frame while something
// uses it); with the JS driver it ticks on requestAnimationFrame.
export class AnimatedClock extends AnimatedNode {
  constructor() {
    super();
    this._raf = null;
    if (NativeAnimatedAPI) this._isNative = true;
  }
  __getValue() {
    return now() / 1000;
  }
  __getNativeConfig() {
    return { type: 'clock' };
  }
  __attach() {
    if (this._isNative) return;
    const loop = () => {
      this._raf = requestFrame(loop);
      batch(() => this.__changed());
    };
    this._raf = requestFrame(loop);
  }
  __detach() {
    if (this._raf != null) cancelFrame(this._raf);
    this._raf = null;
    this.__dropNative();
  }
}

// Animated.clock({start, speed, paused, running}): a clock you control, an
// Animated.Value counting seconds (times `speed`) from `start`. pause(),
// resume(), reset(t) and setSpeed(x) are single commands: natively the
// runtime runs it as a "clock" animation on the value (no JS per frame),
// and a paused clock, or one nothing reads, does not keep the host ticking.
// `running`: an Animated node (host state bind, interaction, value, ...)
// that holds the clock while it is 0, natively too. With the JS driver it
// follows requestAnimationFrame while something reads it.
export class AnimatedClockValue extends AnimatedValue {
  constructor(options) {
    const o = options || {};
    const start = typeof o.start === 'number' && o.start === o.start ? o.start : 0;
    super(start);
    this._clockStart = start;
    this._speed = typeof o.speed === 'number' && o.speed === o.speed ? o.speed : 1;
    this._paused = !!o.paused;
    this._running = o.running instanceof AnimatedNode ? o.running : o.running != null ? new AnimatedValue(o.running ? 1 : 0) : null;
    this._run = null;  // {id, stop} while counting
    this._held = false;  // JS driver: `running` is 0
    this._v0 = start;
    this._t0 = 0;
    this._raf = null;
    this._rafToken = null;
    this._gate = null;
    if (NativeAnimatedAPI) this.__makeNative();
    if (!this._paused) this._go();
  }
  /** Stops counting; the value stays where it is (natively: the last frame's). */
  pause() {
    this._paused = true;
    this._stopRun();
  }
  /** Counts on from where it was paused. */
  resume() {
    if (!this._paused) return;
    this._paused = false;
    this._go();
  }
  /** Sets the time (seconds; default: the start time); keeps running or paused. */
  reset(t) {
    const v = typeof t === 'number' && t === t ? t : this._clockStart;
    this._stopRun();
    if (this._nativeTag != null) NativeAnimatedAPI.setValue(this._nativeTag, v);
    this._v0 = v;
    this._updateValue(v, true);
    if (!this._paused) this._go();
  }
  /** Seconds per second from now on (0 holds, negative counts down). */
  setSpeed(speed) {
    const s = +speed;
    if (!(s === s) || s === this._speed) return;
    if (this._run) {
      this._stopRun();
      this._speed = s;
      this._go();
    } else {
      this._speed = s;
    }
  }
  isPaused() {
    return this._paused;
  }
  getSpeed() {
    return this._speed;
  }
  setValue(value) {
    this.reset(+value);
  }
  __getValue() {
    if (this._nativeTag == null && this._run) this._value = this._jsTime();
    return super.__getValue();
  }
  _jsTime() {
    return this._held ? this._v0 : this._v0 + ((now() - this._t0) / 1000) * this._speed;
  }
  _go() {
    if (this._run) return;
    const run = { id: null, stop: () => this.pause() };
    // An animation running on the value (Animated.timing(clock, ...)) stops.
    if (this._animation && this._animation !== run) this._animation.stop();
    this._run = run;
    this._animation = run;
    if (this._isNative) {
      const id = generateNewAnimationId();
      run.id = id;
      const config = { type: 'clock', speed: this._speed };
      if (this._running) {
        this._running.__makeNative();
        config.running = this._running.__getNativeTag();
      }
      startAnimatingNode(id, this.__getNativeTag(), config, (finished, raw) => {
        // Stopped by the runtime (a native setValue, another animation).
        if (this._run !== run) return;
        this._run = null;
        this._paused = true;
        this._value = raw;
        if (this._animation === run) this._animation = null;
      });
      return;
    }
    this._v0 = this._value;
    this._t0 = now();
    this._held = false;
    if (this._running) {
      if (!this._gate) {
        this._gate = { __changed: () => this._syncGate(), __makeNative() {} };
        this._running.__addChild(this._gate);
      }
      this._held = !+this._running.__getValue();
    }
    this._kick();
  }
  _stopRun() {
    const run = this._run;
    if (!run) return;
    this._run = null;
    if (this._animation === run) this._animation = null;
    if (run.id != null) {
      stopNativeAnimationById(run.id);
      this.__getValue();
      return;
    }
    const v = this._jsTime();
    this._held = false;
    this._cancelFrame();
    this._v0 = v;
    this._updateValue(v, true);
  }
  // JS driver: `running` changed (an Animated node notifies its dependents).
  _syncGate() {
    if (!this._run || this._isNative) return;
    const on = !!+this._running.__getValue();
    if (!on && !this._held) {
      this._v0 = this._jsTime();
      this._held = true;
      this._updateValue(this._v0, true);
    } else if (on && this._held) {
      this._held = false;
      this._t0 = now();
      this._kick();
    }
  }
  _kick() {
    if (this._raf != null || !this._run || this._held || this._isNative) return;
    if (!this.__getChildren().length && !this.hasListeners()) return;  // read lazily
    const token = (this._rafToken = {});
    const loop = () => {
      if (this._rafToken !== token) return;  // cancelled
      this._raf = this._rafToken = null;
      if (!this._run || this._held) return;
      this._updateValue(this._jsTime(), true);
      this._kick();
    };
    this._raf = requestFrame(loop);
  }
  _cancelFrame() {
    if (this._raf != null) cancelFrame(this._raf);
    this._raf = this._rafToken = null;
  }
  __attach() {
    this._kick();
  }
  __detach() {
    this._cancelFrame();
  }
  addListener(callback) {
    const id = super.addListener(callback);
    this._kick();
    return id;
  }
}

// One state of an element's interaction (useInteraction): 0..1, eased by
// the element's `transition`, computed by the runtime from input
// (core/state_style.h). Native only: a JS-driven consumer sees 0.
export class AnimatedInteraction extends AnimatedNode {
  constructor(key, state, instant) {
    super();
    this._key = key;
    this._state = state;
    this._instant = !!instant;
    if (NativeAnimatedAPI) this._isNative = true;
  }
  __getValue() {
    return 0;
  }
  __getNativeConfig() {
    return { type: 'interaction', key: this._key, state: this._state, instant: this._instant };
  }
  __detach() {
    this.__dropNative();
  }
}

let interactionKeys = 0;

/**
 * An element's interaction state as native Animated values: pass it as the
 * `interaction` prop of a host element (View, Pressable, ...) and use
 * `.hovered`, `.pressed`, `.focused`, `.focusVisible`, `.disabled` (0..1,
 * following that element's `transition`) in native-driven styles,
 * interpolations or Animated.derive. Only the `key` crosses the bridge.
 */
export class Interaction {
  constructor() {
    this.key = 'ix' + ++interactionKeys;
    Object.defineProperty(this, '_values', { value: {}, enumerable: false });
    Object.defineProperty(this, '_instant', { value: null, writable: true, enumerable: false });
  }
  _value(state, instant) {
    const k = instant ? state + '!' : state;
    return this._values[k] || (this._values[k] = new AnimatedInteraction(this.key, state, instant));
  }
  /** The states themselves, 0 or 1 the frame they change (no transition). */
  get instant() {
    if (!this._instant) {
      const ix = this;
      this._instant = {
        get hovered() { return ix._value('hovered', true); },
        get pressed() { return ix._value('pressed', true); },
        get focused() { return ix._value('focused', true); },
        get focusVisible() { return ix._value('focusVisible', true); },
        get disabled() { return ix._value('disabled', true); },
      };
    }
    return this._instant;
  }
  get hovered() {
    return this._value('hovered');
  }
  get pressed() {
    return this._value('pressed');
  }
  get focused() {
    return this._value('focused');
  }
  get focusVisible() {
    return this._value('focusVisible');
  }
  get disabled() {
    return this._value('disabled');
  }
}

// Animated.hostState(key, path) / useHostStateBind: a value inside shared
// host state (jsui_set_state), e.g. ('player', 'health.current'). Native:
// the runtime re-reads it the moment the host changes the key, before any
// JS runs. Numbers as is, booleans as 1 / 0, strings (for Animated.Text's
// `text`) as is, anything else 0.
function readPath(value, path) {
  let v = value;
  for (let i = 0; i < path.length && v != null; i++) v = typeof v === 'object' ? v[path[i]] : undefined;
  if (typeof v === 'number') return v === v ? v : 0;
  if (typeof v === 'boolean') return v ? 1 : 0;
  if (typeof v === 'string') return v;
  return 0;
}

export class AnimatedHostState extends AnimatedNode {
  constructor(key, path) {
    super();
    this._key = String(key);
    this._path = path == null || path === '' ? [] : Array.isArray(path) ? path.map(String) : String(path).split('.');
    this._sub = null;
    if (NativeAnimatedAPI) this._isNative = true;
  }
  __getValue() {
    return readPath(HostState.get(this._key), this._path);
  }
  __getNativeConfig() {
    return { type: 'hostState', key: this._key, path: this._path };
  }
  __attach() {
    if (!this._isNative) this._sub = HostState.subscribe(this._key, () => batch(() => this.__changed()));
  }
  __detach() {
    if (this._sub) this._sub();
    this._sub = null;
    this.__dropNative();
  }
}

// Animated.format(node, {decimals, grouping, prefix, suffix, minDigits}): a
// number as text, for Animated.Text's `text` prop. Native with its input
// (the runtime formats it and sets the text without JS); the C++ twin is
// NativeAnimated::formatNumber.
// time: 'h:mm:ss' | 'mm:ss' | ...: whole seconds (floored, at least 0) as
// h / m / s fields; a doubled letter pads to two digits. Without an h
// field the minutes are the total minutes, without m the seconds the total.
function formatTime(v, pattern) {
  const total = isFinite(v) && v > 0 ? Math.floor(v) : 0;
  const hasH = pattern.indexOf('h') >= 0, hasM = pattern.indexOf('m') >= 0;
  const f = { h: Math.floor(total / 3600), m: hasH ? Math.floor(total / 60) % 60 : Math.floor(total / 60), s: hasM ? total % 60 : total };
  let out = '';
  for (let i = 0; i < pattern.length; ) {
    const c = pattern[i];
    if (c === 'h' || c === 'm' || c === 's') {
      let j = i;
      while (j < pattern.length && pattern[j] === c) j++;
      const d = String(f[c]);
      out += d.length < j - i ? '0'.repeat(j - i - d.length) + d : d;
      i = j;
    } else {
      out += c;
      i++;
    }
  }
  return out;
}

export function formatNumber(v, o) {
  const prefix = o.prefix || '', suffix = o.suffix || '';
  if (typeof o.time === 'string' && o.time) return prefix + formatTime(v, o.time) + suffix;
  if (!isFinite(v)) return prefix + String(v) + suffix;
  const decimals = typeof o.decimals === 'number' ? Math.max(-1, Math.min(20, Math.trunc(o.decimals))) : -1;
  let digits;
  if (decimals < 0) {
    digits = String(v === 0 ? 0 : v);
  } else {
    const scale = 10 ** decimals;
    const r = Math.round(Math.abs(v) * scale) / scale;
    digits = (v < 0 && r !== 0 ? '-' : '') + r.toFixed(decimals);
  }
  const neg = digits[0] === '-';
  const body = neg ? digits.slice(1) : digits;
  const dot = body.indexOf('.');
  let int = dot < 0 ? body : body.slice(0, dot);
  const minDigits = Math.max(0, Math.min(40, Math.trunc(o.minDigits || 0)));
  if (int.length < minDigits) int = '0'.repeat(minDigits - int.length) + int;
  if (o.grouping && int.indexOf('e') < 0) int = int.replace(/\B(?=(\d{3})+(?!\d))/g, o.grouping);
  return prefix + (neg ? '-' : '') + int + (dot < 0 ? '' : body.slice(dot)) + suffix;
}

export class AnimatedFormat extends AnimatedNode {
  constructor(input, options) {
    super();
    this._input = input instanceof AnimatedNode ? input : new AnimatedValue(+input || 0);
    this._options = Object.assign({}, options);
    if (this._input.__isNative()) this.__makeNative();
  }
  __getInputs() {
    return [this._input];
  }
  __getValue() {
    const v = this._input.__getValue();
    return formatNumber(typeof v === 'number' ? v : +v, this._options);
  }
  __getNativeConfig() {
    const o = this._options;
    return {
      type: 'format',
      input: this._input.__getNativeTag(),
      decimals: typeof o.decimals === 'number' ? o.decimals : -1,
      grouping: o.grouping || '',
      prefix: o.prefix || '',
      suffix: o.suffix || '',
      minDigits: o.minDigits || 0,
      time: typeof o.time === 'string' ? o.time : '',
    };
  }
  __attach() {
    this._input.__addChild(this);
  }
  __detach() {
    this._input.__removeChild(this);
    this.__dropNative();
  }
}
