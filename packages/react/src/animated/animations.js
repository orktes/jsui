// Animation drivers: timing, spring, decay and the composites. All running
// animations share one requestAnimationFrame loop; component style patches
// are flushed once at the end of each frame.

import { now, requestFrame } from '../native.js';
import { Easing } from './Easing.js';
import { AnimatedValue, AnimatedValueXY, AnimatedColor, AnimatedNode, batch, colorChannels } from './nodes.js';

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

class Animation {
  start(fromValue, onUpdate, onEnd, previous, value) {
    this._onUpdate = onUpdate;
    this._onEnd = onEnd;
    this._value = value;
    this._done = false;
    this.__iterations = this.__iterations || 1;
    this.begin(fromValue, previous);
  }
  _run() {
    active.add(this);
    schedule();
  }
  _finish(finished) {
    if (this._done) return;
    this._done = true;
    active.delete(this);
    const cb = this._onEnd;
    this._onEnd = null;
    if (cb) cb({ finished });
  }
  stop() {
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

function single(value, makeAnimation, config) {
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
    _isUsingNativeDriver: () => false,
  };
  return result;
}

export const timing = (value, config) => single(value, (c) => new TimingAnimation(c), config || {});
export const spring = (value, config) => single(value, (c) => new SpringAnimation(c), config || {});
export const decay = (value, config) => single(value, (c) => new DecayAnimation(c), config || {});

export function sequence(animations) {
  let index = 0;
  let stopped = false;
  return {
    start(callback) {
      stopped = false;
      index = 0;
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
      if (index < animations.length) animations[index].stop();
    },
    reset() {
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
  const result = {
    start(callback) {
      finishedCount = 0;
      doneAll = false;
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
      animations.forEach((a, i) => {
        if (!hasEnded[i]) a.stop();
      });
    },
    reset() {
      for (const a of animations) a.reset();
    },
    _isUsingNativeDriver: () => false,
  };
  return result;
}

export function delay(time) {
  return timing(new AnimatedValue(0), { toValue: 0, delay: time, duration: 0 });
}

export function stagger(time, animations) {
  return parallel(animations.map((a, i) => sequence([delay(time * i), a])));
}

export function loop(animation, config) {
  const iterations = config && config.iterations !== undefined ? config.iterations : -1;
  const resetBeforeIteration = !(config && config.resetBeforeIteration === false);
  let stopped = false;
  let count = 0;
  return {
    start(callback) {
      stopped = false;
      count = 0;
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
      } else restart();
    },
    stop() {
      stopped = true;
      animation.stop();
    },
    reset() {
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
