// Animation drivers: timing, spring, decay and the composites. All running
// animations share one requestAnimationFrame loop; component style patches
// are flushed once at the end of each frame.

import { now, requestFrame } from '../native.js';
import { Easing } from './Easing.js';
import { AnimatedValue, AnimatedValueXY, AnimatedColor, AnimatedNode, batch, colorChannels } from './nodes.js';
import {
  NativeAnimatedAPI,
  shouldUseNativeDriver,
  generateNewAnimationId,
  startAnimatingNode,
  stopAnimation as stopNativeAnimation,
  sampleCurve,
} from './NativeAnimatedHelper.js';

// --- frame loop --------------------------------------------------------------
const active = new Set();
let frameRequested = false;

function frame(ts) {
  frameRequested = false;
  const t = now();
  const time = typeof ts === 'number' && ts > 0 && Math.abs(ts - t) < 1000 ? ts : t;
  batch(() => {
    for (const a of Array.from(active)) if (active.has(a)) a.onFrame(time);
  });
  if (active.size) schedule();
}

function schedule() {
  if (frameRequested) return;
  frameRequested = true;
  requestFrame(frame);
}

// --- native composites --------------------------------------------------------
// A sequence / parallel / stagger / loop whose steps are all native-driven
// animations (or delays) runs as one native group: the runtime advances its
// steps on the frames they end, with no JS between them (so they keep going
// while JS is busy). Built from the composites' __nativeSpec().

function specOf(animation) {
  return animation && typeof animation.__nativeSpec === 'function' ? animation.__nativeSpec() : null;
}

function nativeChildren(animations) {
  if (!NativeAnimatedAPI || animations.length === 0) return null;
  const specs = [];
  const values = [];
  let native = false;
  for (const a of animations) {
    const r = specOf(a);
    if (!r) return null;
    specs.push(r.spec);
    values.push(...r.values);
    if (r.spec.type !== 'wait') native = true;
  }
  return native || specs.length ? { specs, values, native } : null;
}

// Starts a group; returns its stop().
function startNativeGroup(root, values, callback) {
  const id = generateNewAnimationId();
  let done = false;
  const stopper = {
    stop() {
      if (done) return;
      done = true;
      stopNativeAnimation(id);
      release();
      if (callback) callback({ finished: false });
    },
  };
  const release = () => {
    for (const v of values) if (v._animation === stopper) v._animation = null;
  };
  // value.stopAnimation() / a new animation on one of its values stops the group.
  for (const v of values) {
    if (v._animation && v._animation !== stopper) v._animation.stop();
    v._animation = stopper;
  }
  startAnimatingNode(id, 0, { type: 'group', root }, (finished) => {
    if (done) return;
    done = true;
    release();
    if (callback) callback({ finished });
  });
  return () => stopper.stop();
}

class Animation {
  start(fromValue, onUpdate, onEnd, previous, value) {
    this._onUpdate = onUpdate;
    this._onEnd = onEnd;
    this._value = value;
    this._done = false;
    this.__iterations = this.__iterations || 1;
    if (this._useNativeDriver && value instanceof AnimatedValue) {
      this._startNative(fromValue, previous, value);
      return;
    }
    if (value && value.__isNative()) {
      throw new Error(
        'Attempting to run JS driven animation on animated node that has been moved to "native" earlier by starting an animation with `useNativeDriver: true`',
      );
    }
    this.begin(fromValue, previous);
  }
  // Native driver: the runtime runs the animation on its frame clock and
  // reports the end ({finished, value}); see NativeAnimatedHelper.js.
  _startNative(from, previous, value) {
    this._from = from;
    value.__makeNative();
    const config = this.__getNativeAnimationConfig(previous);
    config.iterations = this.__iterations;
    const id = generateNewAnimationId();
    this._nativeId = id;
    startAnimatingNode(id, value.__getNativeTag(), config, (finished, raw) => {
      this._nativeId = null;
      value._value = raw;
      this._finish(finished, raw + value._offset);
    });
  }
  _run() {
    active.add(this);
    schedule();
  }
  _finish(finished, value) {
    if (this._done) return;
    this._done = true;
    active.delete(this);
    const cb = this._onEnd;
    this._onEnd = null;
    // Native ends carry the final value, like React Native's.
    if (cb) cb(value === undefined ? { finished } : { finished, value });
  }
  stop() {
    if (this._nativeId != null) {
      const r = stopNativeAnimation(this._nativeId);
      this._nativeId = null;
      if (r) this._lastVelocity = r.velocity;
      this._finish(false, this._value.__getValue());
      return;
    }
    this._finish(false);
  }
}

const defaultEasing = Easing.inOut(Easing.ease);

function resolveTo(v) {
  return v instanceof AnimatedNode ? v.__getValue() : v;
}

class TimingAnimation extends Animation {
  constructor(config) {
    super();
    this._to = config.toValue;
    this._duration = config.duration !== undefined ? config.duration : 500;
    this._delay = config.delay || 0;
    this._easing = config.easing || defaultEasing;
  }
  begin(from) {
    this._from = from;
    this._toNum = +resolveTo(this._to);
    this._start = now() + this._delay;
    if (this._duration === 0 && this._delay === 0) {
      this._onUpdate(this._toNum);
      this._finish(true);
      return;
    }
    this._run();
  }
  __getNativeAnimationConfig() {
    // The easing as a curve sampled every ~2 ms (linearly interpolated
    // natively; React Native samples at 60 Hz).
    const count = Math.max(2, Math.min(4097, Math.ceil(this._duration / 2) + 1));
    return {
      type: 'frames',
      frames: sampleCurve(this._easing, count),
      duration: this._duration,
      toValue: +resolveTo(this._to),
      delay: this._delay,
    };
  }
  onFrame(t) {
    if (t < this._start) return;
    const p = this._duration > 0 ? (t - this._start) / this._duration : 1;
    if (p >= 1) {
      this._onUpdate(this._duration === 0 ? this._toNum : this._from + this._easing(1) * (this._toNum - this._from));
      this._finish(true);
      return;
    }
    this._onUpdate(this._from + this._easing(p) * (this._toNum - this._from));
  }
}

// Spring parameter conversions from React Native (Origami / bounciness).
const stiffnessFromOrigami = (o) => (o - 30) * 3.62 + 194;
const dampingFromOrigami = (o) => (o - 8) * 3 + 25;
function fromBouncinessAndSpeed(bounciness, speed) {
  const normalize = (v, s, e) => (v - s) / (e - s);
  const project = (n, s, e) => s + n * (e - s);
  const lin = (t, s, e) => t * e + (1 - t) * s;
  const quadOut = (t, s, e) => lin(2 * t - t * t, s, e);
  const f1 = (x) => 0.0007 * x ** 3 - 0.031 * x ** 2 + 0.64 * x + 1.28;
  const f2 = (x) => 0.000044 * x ** 3 - 0.006 * x ** 2 + 0.36 * x + 2;
  const f3 = (x) => 0.00000045 * x ** 3 - 0.000332 * x ** 2 + 0.1078 * x + 5.84;
  const noBounce = (t) => (t <= 18 ? f1(t) : t <= 44 ? f2(t) : f3(t));
  const b = project(normalize(bounciness / 1.7, 0, 20), 0, 0.8);
  const s = normalize(speed / 1.7, 0, 20);
  const tension = project(s, 0.5, 200);
  const friction = quadOut(b, noBounce(tension), 0.01);
  return { stiffness: stiffnessFromOrigami(tension), damping: dampingFromOrigami(friction) };
}

class SpringAnimation extends Animation {
  constructor(config) {
    super();
    this._to = config.toValue;
    this._overshootClamping = !!config.overshootClamping;
    this._restDisplacement = config.restDisplacementThreshold !== undefined ? config.restDisplacementThreshold : 0.001;
    this._restSpeed = config.restSpeedThreshold !== undefined ? config.restSpeedThreshold : 0.001;
    this._initialVelocity = config.velocity !== undefined ? +config.velocity : 0;
    this._delay = config.delay || 0;
    if (config.stiffness !== undefined || config.damping !== undefined || config.mass !== undefined) {
      this._k = config.stiffness !== undefined ? config.stiffness : 100;
      this._c = config.damping !== undefined ? config.damping : 10;
      this._m = config.mass !== undefined ? config.mass : 1;
    } else if (config.bounciness !== undefined || config.speed !== undefined) {
      const p = fromBouncinessAndSpeed(config.bounciness !== undefined ? config.bounciness : 8, config.speed !== undefined ? config.speed : 12);
      this._k = p.stiffness;
      this._c = p.damping;
      this._m = 1;
    } else {
      this._k = stiffnessFromOrigami(config.tension !== undefined ? config.tension : 40);
      this._c = dampingFromOrigami(config.friction !== undefined ? config.friction : 7);
      this._m = 1;
    }
  }
  begin(from, previous) {
    this._from = from;
    this._toNum = +resolveTo(this._to);
    // Continue with the previous spring's velocity for seamless retargeting
    // (RN takes over the running spring's state the same way).
    if (previous instanceof SpringAnimation && previous._lastVelocity !== undefined) {
      this._initialVelocity = previous._lastVelocity;
    }
    this._start = now() + this._delay;
    this._lastVelocity = this._initialVelocity;
    this._run();
  }
  __getNativeAnimationConfig(previous) {
    let velocity = this._initialVelocity;
    if (previous instanceof SpringAnimation && previous._lastVelocity !== undefined) velocity = previous._lastVelocity;
    this._initialVelocity = velocity;
    return {
      type: 'spring',
      stiffness: this._k,
      damping: this._c,
      mass: this._m,
      initialVelocity: velocity,
      toValue: +resolveTo(this._to),
      overshootClamping: this._overshootClamping,
      restDisplacementThreshold: this._restDisplacement,
      restSpeedThreshold: this._restSpeed,
      delay: this._delay,
    };
  }
  onFrame(t) {
    if (t < this._start) return;
    const elapsed = (t - this._start) / 1000;
    const c = this._c;
    const m = this._m;
    const k = this._k;
    const v0 = -this._initialVelocity;
    const zeta = c / (2 * Math.sqrt(k * m));
    const omega0 = Math.sqrt(k / m);
    const omega1 = omega0 * Math.sqrt(1 - zeta * zeta);
    const x0 = this._toNum - this._from;
    let position;
    let velocity;
    if (zeta < 1) {
      const env = Math.exp(-zeta * omega0 * elapsed);
      const sin = Math.sin(omega1 * elapsed);
      const cos = Math.cos(omega1 * elapsed);
      position = this._toNum - env * (((v0 + zeta * omega0 * x0) / omega1) * sin + x0 * cos);
      velocity = zeta * omega0 * env * ((sin * (v0 + zeta * omega0 * x0)) / omega1 + x0 * cos) - env * (cos * (v0 + zeta * omega0 * x0) - omega1 * x0 * sin);
    } else {
      const env = Math.exp(-omega0 * elapsed);
      position = this._toNum - env * (x0 + (v0 + omega0 * x0) * elapsed);
      velocity = env * (v0 * (elapsed * omega0 - 1) + elapsed * x0 * (omega0 * omega0));
    }
    this._lastVelocity = velocity;
    let overshot = false;
    if (this._overshootClamping && k !== 0) {
      overshot = this._from < this._toNum ? position > this._toNum : position < this._toNum;
    }
    const resting = Math.abs(velocity) <= this._restSpeed && (k === 0 || Math.abs(this._toNum - position) <= this._restDisplacement);
    if (overshot || resting) {
      if (k !== 0) position = this._toNum;
      this._onUpdate(position);
      this._lastVelocity = 0;
      this._finish(true);
      return;
    }
    this._onUpdate(position);
  }
}

class DecayAnimation extends Animation {
  constructor(config) {
    super();
    this._velocity = +config.velocity || 0;
    this._deceleration = config.deceleration !== undefined ? config.deceleration : 0.998;
    this._delay = config.delay || 0;
  }
  __getNativeAnimationConfig() {
    return { type: 'decay', velocity: this._velocity, deceleration: this._deceleration, delay: this._delay };
  }
  begin(from) {
    this._from = from;
    this._last = from;
    this._start = now() + this._delay;
    this._run();
  }
  onFrame(t) {
    if (t < this._start) return;
    const k = 1 - this._deceleration;
    const value = this._from + (this._velocity / k) * (1 - Math.exp(-k * (t - this._start)));
    this._onUpdate(value);
    if (Math.abs(this._last - value) < 0.1) {
      this._finish(true);
      return;
    }
    this._last = value;
  }
}

// --- public animation objects -------------------------------------------------

// Splits a ValueXY / Color config into per-channel configs: every key whose
// value carries the channel keys ({x, y} toValue / velocity, {r, g, b, a})
// is split, like RN's maybeVectorAnim.
function splitConfig(config, keys, resolveTo) {
  return keys.map((k, i) => {
    const out = Object.assign({}, config);
    for (const key in config) {
      const v = config[key];
      if (key === 'toValue' && resolveTo) out.toValue = resolveTo(v)[i];
      else if (v instanceof AnimatedValueXY) out[key] = v[k];
      else if (v && typeof v === 'object' && !(v instanceof AnimatedNode) && v[k] !== undefined) out[key] = v[k];
    }
    return out;
  });
}

function single(value, makeAnimationDriver, config) {
  const useNative = shouldUseNativeDriver(config);
  const makeAnimation = (c) => {
    const a = makeAnimationDriver(c);
    a._useNativeDriver = useNative;
    return a;
  };
  if (value instanceof AnimatedValueXY) {
    const [cx, cy] = splitConfig(config, ['x', 'y']);
    return parallel([single(value.x, makeAnimation, cx), single(value.y, makeAnimation, cy)], { stopTogether: false });
  }
  if (value instanceof AnimatedColor) {
    const chans = splitConfig(config, ['r', 'g', 'b', 'a'], colorChannels);
    return parallel(['r', 'g', 'b', 'a'].map((k, i) => single(value[k], makeAnimation, chans[i])), { stopTogether: false });
  }
  let current = null;
  const result = {
    start(callback) {
      // toValue given as another Animated node: follow it (RN AnimatedTracking).
      if (config.toValue instanceof AnimatedNode) {
        const target = config.toValue;
        const restart = () => {
          current = makeAnimation(Object.assign({}, config, { toValue: target.__getValue() }));
          value.animate(current, callback);
        };
        const id = target.addListener(restart);
        value.track({ __detach: () => target.removeListener(id) });
        restart();
        return;
      }
      const anim = makeAnimation(config);
      current = anim;
      const iterations = config.iterations !== undefined ? config.iterations : 1;
      if (useNative) {
        // Iterations run natively (restarting from the start value).
        anim.__iterations = iterations === 0 ? 1 : iterations;
        value.animate(anim, callback);
        return;
      }
      let done = 0;
      const run = () => {
        value.animate(current, (r) => {
          done++;
          if (r.finished && (iterations === -1 || done < iterations)) {
            value.setValue(anim._from);
            current = makeAnimation(config);
            run();
          } else if (callback) callback(r);
        });
      };
      run();
    },
    stop() {
      value.stopTracking();
      if (current) current.stop();
    },
    reset() {
      value.stopTracking();
      if (current) current.stop();
      value.resetAnimation();
    },
    _isUsingNativeDriver: () => useNative,
    // As one step of a composite the runtime runs as a whole (null: this
    // animation needs JS).
    __nativeSpec() {
      if (!useNative || !(value instanceof AnimatedValue) || config.toValue instanceof AnimatedNode) return null;
      const anim = makeAnimation(config);
      const iterations = config.iterations !== undefined ? config.iterations : 1;
      value.__makeNative();
      const c = anim.__getNativeAnimationConfig();
      c.iterations = iterations === 0 ? 1 : iterations;
      return { spec: { type: 'leaf', tag: value.__getNativeTag(), config: c }, values: [value] };
    },
    // Animated.loop of a native animation: the runtime repeats it (RN).
    _startNativeLoop(iterations, callback) {
      value.stopTracking();
      const anim = makeAnimation(config);
      anim.__iterations = iterations;
      current = anim;
      value.animate(anim, callback);
    },
  };
  return result;
}

export const timing = (value, config) => single(value, (c) => new TimingAnimation(c), config || {});
export const spring = (value, config) => single(value, (c) => new SpringAnimation(c), config || {});
export const decay = (value, config) => single(value, (c) => new DecayAnimation(c), config || {});

export function sequence(animations) {
  let index = 0;
  let stopped = false;
  let stopNative = null;
  return {
    __nativeSpec() {
      const r = nativeChildren(animations);
      return r && { spec: { type: 'sequence', children: r.specs }, values: r.values };
    },
    start(callback) {
      stopped = false;
      index = 0;
      const n = this.__nativeSpec();
      if (n && n.values.length) {
        stopNative = startNativeGroup(n.spec, n.values, callback);
        return;
      }
      const next = (result) => {
        if (!result.finished || stopped) {
          if (callback) callback(result);
          return;
        }
        index++;
        if (index >= animations.length) {
          if (callback) callback({ finished: true });
          return;
        }
        animations[index].start(next);
      };
      if (animations.length === 0) {
        if (callback) callback({ finished: true });
      } else animations[0].start(next);
    },
    stop() {
      stopped = true;
      if (stopNative) {
        const s = stopNative;
        stopNative = null;
        s();
        return;
      }
      if (index < animations.length) animations[index].stop();
    },
    reset() {
      if (stopNative) this.stop();
      for (const a of animations) a.reset();
      index = 0;
    },
    _isUsingNativeDriver: () => false,
  };
}

export function parallel(animations, config) {
  const stopTogether = !(config && config.stopTogether === false);
  let finishedCount = 0;
  let hasEnded = [];
  let doneAll = false;
  let stopNative = null;
  const result = {
    __nativeSpec() {
      const r = nativeChildren(animations);
      return r && { spec: { type: 'parallel', children: r.specs, stopTogether }, values: r.values };
    },
    start(callback) {
      finishedCount = 0;
      doneAll = false;
      const n = result.__nativeSpec();
      if (n && n.values.length) {
        stopNative = startNativeGroup(n.spec, n.values, callback);
        return;
      }
      hasEnded = animations.map(() => false);
      if (animations.length === 0) {
        if (callback) callback({ finished: true });
        return;
      }
      animations.forEach((a, i) => {
        a.start((r) => {
          if (hasEnded[i]) return;
          hasEnded[i] = true;
          finishedCount++;
          if (!r.finished && stopTogether) result.stop();
          if (finishedCount === animations.length && !doneAll) {
            doneAll = true;
            if (callback) callback(r.finished && hasEnded.every(Boolean) ? r : { finished: false });
          }
        });
      });
    },
    stop() {
      if (stopNative) {
        const s = stopNative;
        stopNative = null;
        s();
        return;
      }
      animations.forEach((a, i) => {
        if (!hasEnded[i]) a.stop();
      });
    },
    reset() {
      if (stopNative) result.stop();
      for (const a of animations) a.reset();
    },
    _isUsingNativeDriver: () => false,
  };
  return result;
}

export function delay(time) {
  const a = timing(new AnimatedValue(0), { toValue: 0, delay: time, duration: 0 });
  // Inside a native composite: a plain wait.
  a.__nativeSpec = () => ({ spec: { type: 'wait', duration: Math.max(0, +time || 0) }, values: [] });
  return a;
}

export function stagger(time, animations) {
  return parallel(animations.map((a, i) => sequence([delay(time * i), a])));
}

export function loop(animation, config) {
  const iterations = config && config.iterations !== undefined ? config.iterations : -1;
  const resetBeforeIteration = !(config && config.resetBeforeIteration === false);
  let stopped = false;
  let count = 0;
  let stopNative = null;
  const nativeSpec = () => {
    if (!NativeAnimatedAPI || !animation || !animation.__nativeSpec) return null;
    const r = animation.__nativeSpec();
    if (!r || !r.values.length) return null;
    // resetBeforeIteration: the values go back to their starting values.
    const resets = resetBeforeIteration ? r.values.map((v) => [v.__getNativeTag(), v._startingValue]) : [];
    return { spec: { type: 'loop', iterations, child: r.spec, resets }, values: r.values };
  };
  return {
    __nativeSpec: nativeSpec,
    start(callback) {
      stopped = false;
      count = 0;
      if (animation && iterations !== 0 && typeof animation.__nativeSpec === 'function' && !animation._startNativeLoop) {
        const n = nativeSpec();
        if (n) {
          stopNative = startNativeGroup(n.spec, n.values, callback);
          return;
        }
      }
      const restart = (result = { finished: true }) => {
        if (stopped || count === iterations || result.finished === false) {
          if (callback) callback(result);
          return;
        }
        count++;
        if (resetBeforeIteration) animation.reset();
        animation.start(restart);
      };
      if (!animation || iterations === 0) {
        if (callback) callback({ finished: true });
      } else if (animation._isUsingNativeDriver() && typeof animation._startNativeLoop === 'function') {
        animation._startNativeLoop(iterations, callback);
      } else restart();
    },
    stop() {
      stopped = true;
      if (stopNative) {
        const s = stopNative;
        stopNative = null;
        s();
        return;
      }
      animation.stop();
    },
    reset() {
      if (stopNative) this.stop();
      count = 0;
      stopped = false;
      animation.reset();
    },
    _isUsingNativeDriver: () => false,
  };
}

/** Number of running animations (tests / diagnostics). */
export function activeAnimationCount() {
  return active.size;
}
