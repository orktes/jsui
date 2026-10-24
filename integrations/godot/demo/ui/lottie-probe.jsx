// main.gd's self-test probe for Lottie: a 64 x 32 LottieView of a small
// composition (a 16 px blue square moving from x 8 to 56 over 1 s), either
// held at `progress` or playing in a loop (`progress` null).
import React from 'react';
import LottieView from 'lottie-react-native';

const COMP = {
  v: '5.7.4', fr: 60, ip: 0, op: 60, w: 64, h: 32, ddd: 0, assets: [],
  layers: [{
    ddd: 0, ind: 1, ty: 4, nm: 'square', sr: 1, ip: 0, op: 60, st: 0, bm: 0, ao: 0,
    ks: {
      o: { a: 0, k: 100 }, r: { a: 0, k: 0 }, a: { a: 0, k: [0, 0] }, s: { a: 0, k: [100, 100] },
      p: { a: 1, k: [{ t: 0, s: [8, 16], o: { x: [0], y: [0] }, i: { x: [1], y: [1] } }, { t: 60, s: [56, 16] }] },
    },
    shapes: [{ ty: 'gr', nm: 'g', it: [
      { ty: 'rc', nm: 'r', d: 1, p: { a: 0, k: [0, 0] }, s: { a: 0, k: [16, 16] }, r: { a: 0, k: 0 } },
      { ty: 'fl', nm: 'Fill 1', c: { a: 0, k: [0.16, 0.35, 0.9, 1] }, o: { a: 0, k: 100 }, r: 1 },
      { ty: 'tr', p: { a: 0, k: [0, 0] }, a: { a: 0, k: [0, 0] }, s: { a: 0, k: [100, 100] }, r: { a: 0, k: 0 }, o: { a: 0, k: 100 } },
    ] }],
  }],
};

export function LottieProbe({ progress = null }) {
  return progress == null ? (
    <LottieView source={COMP} autoPlay loop style={{ width: 64, height: 32 }} />
  ) : (
    <LottieView source={COMP} progress={progress} style={{ width: 64, height: 32 }} />
  );
}
