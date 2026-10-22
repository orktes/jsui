// RN-compatible Easing functions (same formulas as react-native/Libraries/Animated/Easing).

// Cubic bezier solver (as in CSS / bezier-easing).
function bezier(x1, y1, x2, y2) {
  const A = (a1, a2) => 1 - 3 * a2 + 3 * a1;
  const B = (a1, a2) => 3 * a2 - 6 * a1;
  const C = (a1) => 3 * a1;
  const calc = (t, a1, a2) => ((A(a1, a2) * t + B(a1, a2)) * t + C(a1)) * t;
  const slope = (t, a1, a2) => 3 * A(a1, a2) * t * t + 2 * B(a1, a2) * t + C(a1);
  if (x1 === y1 && x2 === y2) return (x) => x;
  const N = 11;
  const step = 1 / (N - 1);
  const samples = new Float32Array(N);
  for (let i = 0; i < N; i++) samples[i] = calc(i * step, x1, x2);
  const tForX = (x) => {
    let start = 0;
    let i = 1;
    for (; i !== N - 1 && samples[i] <= x; i++) start += step;
    i--;
    const dist = (x - samples[i]) / (samples[i + 1] - samples[i]);
    let t = start + dist * step;
    const s = slope(t, x1, x2);
    if (s >= 0.001) {
      for (let k = 0; k < 4; k++) {
        const sl = slope(t, x1, x2);
        if (sl === 0) break;
        t -= (calc(t, x1, x2) - x) / sl;
      }
      return t;
    }
    if (s === 0) return t;
    let a = start;
    let b = start + step;
    for (let k = 0; k < 10; k++) {
      t = a + (b - a) / 2;
      const v = calc(t, x1, x2) - x;
      if (Math.abs(v) < 1e-7) break;
      if (v > 0) b = t;
      else a = t;
    }
    return t;
  };
  return (x) => (x <= 0 ? 0 : x >= 1 ? 1 : calc(tForX(x), y1, y2));
}

let easeFn;

export const Easing = {
  step0: (n) => (n > 0 ? 1 : 0),
  step1: (n) => (n >= 1 ? 1 : 0),
  linear: (t) => t,
  ease(t) {
    if (!easeFn) easeFn = bezier(0.42, 0, 1, 1);
    return easeFn(t);
  },
  quad: (t) => t * t,
  cubic: (t) => t * t * t,
  poly: (n) => (t) => Math.pow(t, n),
  sin: (t) => 1 - Math.cos((t * Math.PI) / 2),
  circle: (t) => 1 - Math.sqrt(1 - t * t),
  exp: (t) => Math.pow(2, 10 * (t - 1)),
  elastic(bounciness = 1) {
    const p = bounciness * Math.PI;
    return (t) => 1 - Math.pow(Math.cos((t * Math.PI) / 2), 3) * Math.cos(t * p);
  },
  back(s = 1.70158) {
    return (t) => t * t * ((s + 1) * t - s);
  },
  bounce(t) {
    if (t < 1 / 2.75) return 7.5625 * t * t;
    if (t < 2 / 2.75) {
      const t2 = t - 1.5 / 2.75;
      return 7.5625 * t2 * t2 + 0.75;
    }
    if (t < 2.5 / 2.75) {
      const t2 = t - 2.25 / 2.75;
      return 7.5625 * t2 * t2 + 0.9375;
    }
    const t2 = t - 2.625 / 2.75;
    return 7.5625 * t2 * t2 + 0.984375;
  },
  bezier,
  in: (easing) => easing,
  out: (easing) => (t) => 1 - easing(1 - t),
  inOut: (easing) => (t) => (t < 0.5 ? easing(t * 2) / 2 : 1 - easing((1 - t) * 2) / 2),
};

export default Easing;
