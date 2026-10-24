// Generates the Lottie (Bodymovin) animations of the LottieView showcase:
//
//   node examples/assets/lottie/make.mjs
//
// Hand-built compositions (no After Effects export), so they are ours to
// ship (MIT, like the repo) and stay small. Each one uses Lottie features a
// game UI needs: shape layers, gradients, strokes with trim paths, polystars,
// repeaters, parenting, eased keyframes and named layers for colorFilters.
//
//   victory-emblem.json  512 x 512, 60 fps. Frames 0-110: the intro (rays
//                        unfold, the shield lands, its outline draws, the
//                        star spins in, rings burst). 110-230: a seamless
//                        idle loop (rays turn, the star breathes, sparkles).
//                        Solid colors named for colorFilters: "Shield Fill",
//                        "Inner Fill", "Inner Stroke", "Outline Stroke".
//   coin.json            160 x 160, a spinning coin (loop).
//   xp-orb.json          160 x 160, an orb with orbiting motes (loop).
//   gem.json             160 x 160, a bobbing gem with a glint (loop).
//   checkmark.json       160 x 160, a ring and a check drawing on (once).
//   spinner.json         96 x 96, a loading ring (loop).
import { writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const OUT = dirname(fileURLToPath(import.meta.url));

// ---------------------------------------------------------------- helpers
const hex = (h, a = 1) => {
  const n = parseInt(h.slice(1), 16);
  return [((n >> 16) & 255) / 255, ((n >> 8) & 255) / 255, (n & 255) / 255, a].map((v) => +v.toFixed(4));
};
// Cubic-bezier easings as Lottie keyframe tangents.
const EASE = {
  linear: [0, 0, 1, 1],
  out: [0.16, 1, 0.3, 1],
  inOut: [0.65, 0, 0.35, 1],
  in: [0.55, 0, 1, 0.45],
  back: [0.34, 1.56, 0.64, 1],
  soft: [0.33, 0, 0.67, 1],
};
const st = (v) => ({ a: 0, k: v });
// anim([[frame, value, easing?], ...]): the easing of a keyframe shapes the
// segment that starts there.
function anim(kfs) {
  return {
    a: 1,
    k: kfs.map(([t, s, e], i) => {
      const v = Array.isArray(s) ? s : [s];
      if (i === kfs.length - 1) return { t, s: v };
      const [x1, y1, x2, y2] = EASE[e || 'inOut'];
      return { t, s: v, o: { x: [x1], y: [y1] }, i: { x: [x2], y: [y2] } };
    }),
  };
}
const val = (v) => (v && v.a !== undefined ? v : st(v));
function tr({ p = [0, 0], a = [0, 0], s = [100, 100], r = 0, o = 100 } = {}) {
  return { ty: 'tr', p: val(p), a: val(a), s: val(s), r: val(r), o: val(o), sk: st(0), sa: st(0), nm: 'Transform' };
}
const group = (nm, items, t) => ({ ty: 'gr', nm, it: [...items, tr(t)] });
const ellipse = (w, h = w, p = [0, 0]) => ({ ty: 'el', nm: 'Ellipse', d: 1, p: val(p), s: val([w, h]) });
const rect = (w, h, r = 0, p = [0, 0]) => ({ ty: 'rc', nm: 'Rect', d: 1, p: val(p), s: val([w, h]), r: val(r) });
// Polystar: star (points, inner / outer radius) or polygon (inner unused).
const star = (pts, outer, inner, rot = 0, roundness = 0) => ({
  ty: 'sr', nm: 'Star', sy: 1, d: 1, pt: val(pts), p: st([0, 0]), r: val(rot), ir: val(inner), is: st(roundness), or: val(outer), os: st(roundness),
});
const polygon = (pts, radius, rot = 0, roundness = 0) => ({
  ty: 'sr', nm: 'Polygon', sy: 2, d: 1, pt: val(pts), p: st([0, 0]), r: val(rot), or: val(radius), os: st(roundness),
});
const path = (v, closed = false, i, o) => ({
  ty: 'sh', nm: 'Path', d: 1,
  ks: st({ c: closed, v, i: i || v.map(() => [0, 0]), o: o || v.map(() => [0, 0]) }),
});
const fill = (c, o = 100, nm = 'Fill') => ({ ty: 'fl', nm, c: val(hex(c)), o: val(o), r: 1 });
const stroke = (c, w, o = 100, nm = 'Stroke') => ({ ty: 'st', nm, c: val(hex(c)), o: val(o), w: val(w), lc: 2, lj: 2, ml: 4 });
// Gradient fill: stops [[offset, '#rrggbb', alpha?], ...]; type 1 linear, 2 radial.
function gradient(stops, s, e, type = 1, nm = 'Gradient') {
  const k = [];
  for (const [off, c] of stops) k.push(off, ...hex(c).slice(0, 3));
  for (const [off, , a = 1] of stops) k.push(off, a);
  return { ty: 'gf', nm, o: st(100), r: 1, t: type, s: val(s), e: val(e), g: { p: stops.length, k: st(k) }, h: st(0), a: st(0) };
}
const trim = (s, e, o = 0) => ({ ty: 'tm', nm: 'Trim', s: val(s), e: val(e), o: val(o), m: 1 });
const repeater = (copies, { r = 0, p = [0, 0], s = [100, 100], so = 100, eo = 100 } = {}) => ({
  ty: 'rp', nm: 'Repeater', c: val(copies), o: st(0), m: 1,
  tr: { ty: 'tr', p: val(p), a: st([0, 0]), s: val(s), r: val(r), so: val(so), eo: val(eo), nm: 'Repeat' },
});

let nextInd = 1;
// Lottie draws the first shape of a list on top; callers list groups
// bottom-up. Modifiers (repeaters) stay after the groups they repeat.
const stack = (shapes) => [...shapes.filter((x) => x.ty === 'gr').reverse(), ...shapes.filter((x) => x.ty !== 'gr')];
function layer(nm, shapes, { p, a = [0, 0], s = [100, 100], r = 0, o = 100, ip = 0, op, parent, st: start = 0, bm = 0 } = {}) {
  const l = { ddd: 0, ind: nextInd++, ty: 4, nm, sr: 1, ks: { o: val(o), r: val(r), p: val(p), a: val(a), s: val(s) }, ao: 0, shapes: stack(shapes), ip, op, st: start, bm };
  if (parent) l.parent = parent.ind;
  return l;
}
function comp(nm, w, h, fr, op, layers) {
  // Lottie draws the first layer on top: callers list them bottom-up.
  return { v: '5.7.4', fr, ip: 0, op, w, h, nm, ddd: 0, assets: [], layers: layers.slice().reverse(), markers: [] };
}
function save(name, c) {
  const json = JSON.stringify(c);
  writeFileSync(join(OUT, name), json + '\n');
  console.log(`${name}: ${(json.length / 1024).toFixed(1)} KB`);
}
const reset = () => (nextInd = 1);

// --------------------------------------------------------- victory emblem
{
  reset();
  const OP = 230, C = [256, 256];
  const L = (nm, shapes, o) => layer(nm, shapes, { op: OP, p: C, ...o });
  // Idle loop 110 -> 230 (2 s): every idle keyframe returns to its frame-110 value.
  const glow = L('Glow', [
    group('glow', [ellipse(400), gradient([[0, '#ffd36b', 0.55], [0.55, '#ff9f3a', 0.16], [1, '#ff7a1a', 0]], [0, 0], [200, 0], 2)]),
  ], {
    s: anim([[0, [20, 20], 'out'], [40, [110, 110], 'soft'], [110, [100, 100], 'soft'], [170, [108, 108], 'soft'], [230, [100, 100]]]),
    o: anim([[0, 0, 'out'], [30, 100], [230, 100]]),
  });
  const rays = L('Rays', [
    group('ray', [
      path([[-9, -128], [9, -128], [2, -236], [-2, -236]], true),
      gradient([[0, '#ffe3a1', 0.9], [1, '#ffb347', 0]], [0, -128], [0, -236], 1, 'Ray Fill'),
      repeater(18, { r: 20 }),
    ]),
  ], {
    r: anim([[0, -40, 'out'], [110, 0, 'linear'], [230, 20]]),
    s: anim([[8, [0, 0], 'out'], [60, [100, 100]], [230, [100, 100]]]),
    o: anim([[8, 0, 'out'], [40, 85], [230, 85]]),
  });
  const ring = (nm, start, delay) =>
    L(nm, [group('ring', [ellipse(300), stroke('#ffe7b0', 6, 100, 'Ring Stroke')])], {
      s: anim([[start, [55, 55], 'out'], [start + 50, [150, 150]], [OP, [150, 150]]]),
      o: anim([[start, 0, 'linear'], [start + 4, 90, 'out'], [start + 50, 0], [OP, 0]]),
      ip: start - delay, op: OP,
    });
  const shield = L('Shield', [
    group('shadow', [polygon(6, 150, 0, 8), fill('#000000', 35)], { p: [0, 14] }),
    group('body', [polygon(6, 150, 0, 8), fill('#2b1d63', 100, 'Shield Fill')]),
    group('bevel', [polygon(6, 150, 0, 8), stroke('#140c33', 6, 60, 'Shield Bevel')]),
  ], {
    s: anim([[18, [0, 0], 'back'], [52, [100, 100]], [110, [100, 100], 'soft'], [170, [103, 103], 'soft'], [230, [100, 100]]]),
    r: anim([[18, -30, 'out'], [52, 0], [230, 0]]),
  });
  const inner = L('Shield Inner', [
    group('inner', [polygon(6, 118, 0, 6), fill('#5b3fd9', 100, 'Inner Fill')]),
    // A soft top-lit sheen over the solid (recolorable) fill.
    group('sheen', [polygon(6, 118, 0, 6), gradient([[0, '#ffffff', 0.22], [0.55, '#ffffff', 0], [1, '#000000', 0.25]], [0, -118], [0, 118], 1, 'Sheen')]),
    group('edge', [polygon(6, 118, 0, 6), stroke('#c9b8ff', 3, 80, 'Inner Stroke')]),
  ], { parent: shield, p: [0, 0], s: anim([[30, [0, 0], 'back'], [60, [100, 100]], [230, [100, 100]]]) });
  inner.ks.p = st([0, 0]);
  const outline = L('Outline', [
    group('outline', [polygon(6, 162, 0, 10), stroke('#ffd36b', 7, 100, 'Outline Stroke'), trim(0, anim([[34, 0, 'inOut'], [74, 100], [230, 100]]), anim([[34, -60, 'inOut'], [74, 0], [230, 0]]))]),
  ], { o: anim([[34, 0, 'linear'], [36, 100], [230, 100]]) });
  const starL = L('Star', [
    group('star glow', [star(5, 96, 42, 0, 0), fill('#fff3c4', 25)], { s: [118, 118] }),
    group('star', [star(5, 82, 36, 0, 0), gradient([[0, '#fff6d1'], [0.5, '#ffd36b'], [1, '#ff9a2e']], [0, -82], [0, 60], 1, 'Star Fill'), stroke('#fff8e1', 3, 80, 'Star Stroke')]),
  ], {
    p: [256, 250],
    s: anim([[44, [0, 0], 'back'], [80, [100, 100]], [110, [100, 100], 'soft'], [140, [92, 92], 'soft'], [170, [104, 104], 'soft'], [200, [96, 96], 'soft'], [230, [100, 100]]]),
    r: anim([[44, -220, 'out'], [92, 0], [110, 0, 'soft'], [170, 6, 'soft'], [230, 0]]),
  });
  const sparkle = (nm, x, y, t0, size) =>
    L(nm, [group('sparkle', [star(4, size, size * 0.22, 45), fill('#fffbe6')])], {
      p: [x, y],
      s: anim([[t0, [0, 0], 'out'], [t0 + 14, [100, 100], 'in'], [t0 + 30, [0, 0]], [t0 + 120, [0, 0], 'out'], [t0 + 134, [100, 100], 'in'], [t0 + 150, [0, 0]]]),
      r: anim([[t0, 0, 'linear'], [t0 + 150, 180]]),
    });
  save('victory-emblem.json', comp('Victory emblem', 512, 512, 60, OP, [
    glow, rays, ring('Ring 1', 40, 0), ring('Ring 2', 52, 0), shield, inner, outline, starL,
    sparkle('Sparkle 1', 120, 140, 70, 18), sparkle('Sparkle 2', 400, 170, 84, 14), sparkle('Sparkle 3', 380, 390, 96, 20), sparkle('Sparkle 4', 140, 360, 104, 12),
  ]));
}

// ----------------------------------------------------------------- coin
{
  reset();
  const OP = 90, C = [80, 80];
  const shadow = layer('Shadow', [group('shadow', [ellipse(80, 14), fill('#000000', 30)])], {
    op: OP, p: [80, 142], s: anim([[0, [100, 100], 'soft'], [45, [70, 70], 'soft'], [90, [100, 100]]]),
  });
  // Spin: x scale 100 -> 8 -> -100 (the back) -> 8 -> 100.
  const spin = anim([[0, [100, 100], 'inOut'], [22, [6, 100], 'inOut'], [45, [-100, 100], 'inOut'], [67, [6, 100], 'inOut'], [90, [100, 100]]]);
  const coin = layer('Coin', [
    group('rim', [ellipse(112), fill('#b8741a', 100, 'Rim Fill')]),
    group('face', [ellipse(100), gradient([[0, '#ffe9a3'], [0.5, '#ffc53d'], [1, '#e08a12']], [-40, -40], [40, 50], 1, 'Face Fill')]),
    group('inset', [ellipse(74), stroke('#fff1c2', 4, 70, 'Inset Stroke')]),
    group('mark', [star(5, 26, 11), fill('#c97a10', 100, 'Mark Fill')]),
  ], { op: OP, p: anim([[0, [80, 74], 'soft'], [45, [80, 64], 'soft'], [90, [80, 74]]]), s: spin });
  const shine = layer('Shine', [group('shine', [star(4, 14, 3, 0), fill('#ffffff')])], {
    op: OP, p: [112, 46], s: anim([[0, [0, 0], 'out'], [10, [100, 100], 'in'], [24, [0, 0]], [90, [0, 0]]]), r: anim([[0, 0, 'linear'], [24, 90], [90, 90]]),
  });
  save('coin.json', comp('Coin', 160, 160, 60, OP, [shadow, coin, shine]));
}

// --------------------------------------------------------------- xp orb
{
  reset();
  const OP = 120, C = [80, 80];
  const halo = layer('Halo', [group('halo', [ellipse(150), gradient([[0, '#7ee8ff', 0.6], [1, '#2bb3ff', 0]], [0, 0], [75, 0], 2)])], {
    op: OP, p: C, s: anim([[0, [90, 90], 'soft'], [60, [104, 104], 'soft'], [120, [90, 90]]]),
  });
  const orb = layer('Orb', [
    group('orb', [ellipse(76), gradient([[0, '#e8fbff'], [0.35, '#6fdcff'], [1, '#1e6bd6']], [-14, -18], [40, 0], 2, 'Orb Fill')]),
    group('rim', [ellipse(76), stroke('#bff3ff', 2.5, 80)]),
  ], { op: OP, p: C, s: anim([[0, [100, 100], 'soft'], [60, [94, 94], 'soft'], [120, [100, 100]]]) });
  const motes = layer('Motes', [
    group('mote', [ellipse(9, 9, [0, -54]), fill('#dff8ff')], { s: [100, 100] }),
    repeater(6, { r: 60, so: 100, eo: 35 }),
  ], { op: OP, p: C, r: anim([[0, 0, 'linear'], [120, 360]]), s: [100, 62] });
  const ring = layer('Ring', [group('ring', [ellipse(118), stroke('#9be9ff', 3, 70), trim(0, 28, anim([[0, 0, 'linear'], [120, 360]]))])], {
    op: OP, p: C, s: [100, 62], r: -14,
  });
  save('xp-orb.json', comp('XP orb', 160, 160, 60, OP, [halo, ring, orb, motes]));
}

// ------------------------------------------------------------------ gem
{
  reset();
  const OP = 120;
  const bob = anim([[0, [80, 82], 'soft'], [60, [80, 72], 'soft'], [120, [80, 82]]]);
  const shadow = layer('Shadow', [group('shadow', [ellipse(70, 12), fill('#000000', 30)])], {
    op: OP, p: [80, 142], s: anim([[0, [100, 100], 'soft'], [60, [78, 78], 'soft'], [120, [100, 100]]]),
  });
  const facet = (v, c, nm) => group(nm, [path(v, true), fill(c, 100, nm + ' Fill')]);
  const gem = layer('Gem', [
    facet([[-46, -14], [-24, -40], [24, -40], [46, -14], [0, 52]], '#7a1fd6', 'Body'),
    facet([[-24, -40], [24, -40], [12, -14], [-12, -14]], '#d9a8ff', 'Table'),
    facet([[-46, -14], [-24, -40], [-12, -14]], '#b06bff', 'Crown L'),
    facet([[46, -14], [24, -40], [12, -14]], '#9440f0', 'Crown R'),
    facet([[-46, -14], [-12, -14], [0, 52]], '#a24dff', 'Pavilion L'),
    facet([[-12, -14], [12, -14], [0, 52]], '#c27dff', 'Pavilion M'),
    facet([[12, -14], [46, -14], [0, 52]], '#6a14c4', 'Pavilion R'),
  ], { op: OP, p: bob, r: anim([[0, -4, 'soft'], [60, 4, 'soft'], [120, -4]]) });
  const glint = layer('Glint', [group('glint', [star(4, 16, 3, 0), fill('#ffffff')])], {
    op: OP, p: [62, 50], parent: undefined,
    s: anim([[30, [0, 0], 'out'], [40, [100, 100], 'in'], [54, [0, 0]], [120, [0, 0]]]),
    r: anim([[30, 0, 'linear'], [54, 90], [120, 90]]),
  });
  save('gem.json', comp('Gem', 160, 160, 60, OP, [shadow, gem, glint]));
}

// ------------------------------------------------------------ checkmark
{
  reset();
  const OP = 60, C = [80, 80];
  const disc = layer('Disc', [group('disc', [ellipse(120), fill('#22c55e', 100, 'Disc Fill')])], {
    op: OP, p: C, s: anim([[10, [0, 0], 'back'], [30, [100, 100]], [60, [100, 100]]]),
  });
  const ring = layer('Ring', [group('ring', [ellipse(136), stroke('#86efac', 6, 100, 'Ring Stroke'), trim(0, anim([[0, 0, 'out'], [24, 100], [60, 100]]), -90)])], {
    op: OP, p: C,
  });
  const check = layer('Check', [group('check', [path([[-26, 2], [-8, 20], [28, -18]]), stroke('#ffffff', 11, 100, 'Check Stroke'), trim(0, anim([[24, 0, 'out'], [44, 100], [60, 100]]))])], {
    op: OP, p: C,
  });
  const burst = layer('Burst', [group('dot', [ellipse(8, 8, [0, -84]), fill('#86efac')]), repeater(8, { r: 45 })], {
    op: OP, p: C, s: anim([[26, [60, 60], 'out'], [50, [110, 110]], [60, [110, 110]]]), o: anim([[25, 0, 'linear'], [26, 100, 'in'], [50, 0], [60, 0]]),
  });
  save('checkmark.json', comp('Checkmark', 160, 160, 60, OP, [burst, ring, disc, check]));
}

// -------------------------------------------------------------- spinner
{
  reset();
  const OP = 60, C = [48, 48];
  const track = layer('Track', [group('track', [ellipse(70), stroke('#ffffff', 7, 15)])], { op: OP, p: C });
  const arc = layer('Arc', [
    group('arc', [ellipse(70), stroke('#ffd36b', 7, 100, 'Arc Stroke'), trim(anim([[0, 0, 'inOut'], [30, 0, 'inOut'], [60, 75]]), anim([[0, 5, 'inOut'], [30, 80, 'inOut'], [60, 80]]))]),
  ], { op: OP, p: C, r: anim([[0, 0, 'linear'], [60, 360]]) });
  save('spinner.json', comp('Spinner', 96, 96, 60, OP, [track, arc]));
}
