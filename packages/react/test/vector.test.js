// react-native-svg (svg.js) and @shopify/react-native-skia (skia.js) against
// the mock bridge: drawing elements are JS-side instances serialized into one
// VectorView's `commands` per commit (custom components and hooks inside
// work), SVG references (gradients, clip paths, use / symbol) are resolved,
// Skia's effect children land on their shape, the imperative Skia objects
// describe themselves as props, and Animated drawing props become
// draw:<k>.<prop> style keys of the VectorView (JS and native driver).
// Rendering itself: tests/native/vector_test.cpp, react_vector_e2e_test.cpp.
import { mock, animated } from './mock-native-animated.js';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createElement as h, useState } from 'react';
import { Animated, render } from '../src/index.js';
import Svg, { Circle, ClipPath, Defs, G, LinearGradient, Path, Rect, Stop, Symbol, Text, TSpan, Use, SvgXml, SvgCss, parse } from '../src/svg.js';
import * as Sk from '../src/skia.js';

const wait = (ms = 0) => new Promise((r) => setTimeout(r, ms));
const byId = (id) => mock.find((x) => x.props.nativeID === id);

test('Svg: one VectorView, elements serialized with their references resolved', async () => {
  render(
    h(Svg, { nativeID: 's', width: 48, height: 24, viewBox: '0 0 24 12', preserveAspectRatio: 'xMinYMid slice', color: '#123', fill: 'red' },
      h(Defs, null,
        h(LinearGradient, { id: 'g', x1: '0', y1: '0', x2: '100%', y2: '0', gradientTransform: 'rotate(90)' },
          h(Stop, { offset: '0', stopColor: '#f90' }), h(Stop, { offset: '50%', stopColor: '#f30', stopOpacity: 0.5 })),
        h(ClipPath, { id: 'c', clipPathUnits: 'objectBoundingBox' }, h(Rect, { width: 0.5, height: 1 })),
        h(Symbol, { id: 'sym', viewBox: '0 0 10 10' }, h(Circle, { cx: 5, cy: 5, r: 4 }))),
      h(G, { transform: 'translate(2 3) scale(2)', opacity: 0.5, clipPath: 'url(#c)' },
        h(Circle, { cx: 6, cy: 6, r: '50%', fill: 'url(#g)', stroke: 'currentColor', strokeDasharray: '2, 1' })),
      h(Use, { href: '#sym', x: 1, y: 2, width: 20, height: 20 }),
      h(Text, { x: 12, y: 10, textAnchor: 'middle', fontSize: 8 }, 'Hi ', h(TSpan, { fill: 'blue' }, 'there'))),
  );
  await wait();
  const n = byId('s');
  assert.equal(n.type, 'VectorView');
  assert.deepEqual(mock.findAll((x) => x.type !== 'Root' && x.type !== 'View', n.id).length, 1, 'no native nodes for the elements');
  assert.equal(n.children.length, 0);
  assert.deepEqual(n.props.style, { overflow: 'hidden', width: 48, height: 24 });
  const c = n.props.commands;
  assert.equal(c.mode, 'svg');
  assert.deepEqual(c.vb, [0, 0, 24, 12]);
  assert.equal(c.align, 'xMinYMid');
  assert.equal(c.slice, true);
  assert.equal(c.color, '#123');
  assert.equal(c.fill, 'red');
  assert.equal(c.c.length, 3, 'Defs are not drawn');
  const [g, use, text] = c.c;
  assert.equal(g.t, 'g');
  assert.deepEqual(g.m, [2, 0, 0, 2, 2, 3]);
  assert.equal(g.opacity, 0.5);
  assert.deepEqual(g.clip, { c: [{ t: 'rect', x: 0, y: 0, width: 0.5, height: 1 }], units: 'bbox' });
  const circle = g.c[0];
  assert.equal(circle.t, 'circle');
  assert.equal(circle.r, Math.sqrt((24 * 24 + 12 * 12) / 2) / 2);
  assert.equal(circle.stroke, 'currentColor');
  assert.deepEqual(circle.strokeDasharray, [2, 1]);
  assert.equal(circle.fill.t, 'linear');
  assert.equal(circle.fill.units, 'bbox');
  assert.deepEqual([circle.fill.x1, circle.fill.x2], [0, 1]);
  assert.deepEqual(circle.fill.stops, [[0, '#f90', 1], [0.5, '#f30', 0.5]]);
  assert.ok(circle.fill.m && Math.abs(circle.fill.m[1] - 1) < 1e-9, 'gradientTransform as a matrix');
  assert.deepEqual(use.m, [1, 0, 0, 1, 1, 2]);
  assert.deepEqual(use.c[0].m, [2, 0, 0, 2, 0, 0], 'symbol viewBox into 20 x 20');
  assert.equal(use.c[0].c[0].t, 'circle');
  assert.equal(text.t, 'text');
  assert.equal(text.text, 'Hi ');
  assert.deepEqual(text.runs, [{ text: 'there', fill: 'blue' }]);
  assert.equal(text.textAnchor, 'middle');
  render(null);
  await wait();
});

test('custom components and hooks inside an Svg; one update per commit', async () => {
  let set = null;
  function Dots({ n }) {
    const [r] = useState(2);
    return Array.from({ length: n }, (_, i) => h(Circle, { key: i, cx: i * 10, cy: 5, r }));
  }
  function App() {
    const [n, setN] = useState(2);
    set = setN;
    return h(Svg, { nativeID: 'h', width: 100, height: 10 }, h(G, null, h(Dots, { n })));
  }
  render(h(App));
  await wait();
  assert.equal(byId('h').props.commands.c[0].c.length, 2);
  mock.reset();
  set(5);
  await wait(20);
  assert.equal(byId('h').props.commands.c[0].c.length, 5);
  assert.equal(mock.count('setProps') + mock.count('updateProps'), 1);
  assert.equal(mock.count('createNode'), 0);
  render(null);
  await wait();
});

test('Animated drawing props: draw:<k>.<prop> keys on the VectorView, no re-render', async () => {
  const r = new Animated.Value(3);
  const op = new Animated.Value(1);
  let renders = 0;
  const AnimatedRect = Animated.createAnimatedComponent(Rect);
  function App() {
    renders++;
    return h(Svg, { nativeID: 'a', width: 10, height: 10 }, h(Circle, { r, cx: 1 }), h(AnimatedRect, { width: op, height: 2 }));
  }
  render(h(App));
  await wait();
  let n = byId('a');
  const keys = Object.keys(n.props.style).filter((k) => k.startsWith('draw:'));
  assert.equal(keys.length, 2);
  const [rk, wk] = keys;
  assert.match(rk, /^draw:\d+\.r$/);
  assert.match(wk, /^draw:\d+\.width$/);
  const circle = n.props.commands.c[0];
  assert.equal(circle.k, +rk.slice(5, rk.indexOf('.')));
  assert.equal(circle.r, undefined, 'the animated value is not in the element');
  assert.equal(circle.cx, 1);
  mock.reset();
  r.setValue(7);
  op.setValue(0.5);
  await wait(20);
  n = byId('a');
  assert.equal(mock.count('setProps'), 0);
  assert.equal(n.props.style[rk], 7);
  assert.equal(n.props.style[wk], 0.5);
  assert.equal(renders, 1);
  // Native driver: a style node with the draw key, connected to the VectorView.
  animated.reset();
  Animated.timing(r, { toValue: 10, duration: 100, useNativeDriver: true }).start();
  const style = animated.of('createNode').find(([, c]) => c.type === 'style');
  assert.ok(style, 'a native style node');
  assert.deepEqual(Object.keys(style[1].style), [rk]);
  assert.deepEqual(animated.of('connectToView').map(([, view]) => view), [n.id]);
  render(null);
  await wait();
});

test('Canvas: Skia elements, effect children, paints and the Skia object', async () => {
  const path = Sk.Skia.Path.Make().moveTo(0, 0).lineTo(10, 0).quadTo(10, 10, 0, 10).close();
  const effect = Sk.Skia.RuntimeEffect.Make('uniform float a; half4 main(float2 p) { return half4(a); }');
  render(
    h(Sk.Canvas, { nativeID: 'k', style: { width: 100, height: 100 } },
      h(Sk.Group, { transform: [{ rotate: Math.PI / 2 }], origin: Sk.vec(50, 50), color: 'red', clip: Sk.rrect(Sk.rect(0, 0, 90, 90), 8, 8), invertClip: true },
        h(Sk.Circle, { c: Sk.vec(5, 6), r: 4, style: 'stroke', strokeWidth: 2 },
          h(Sk.LinearGradient, { start: Sk.vec(0, 0), end: Sk.vec(10, 0), colors: ['#f00', Sk.Skia.Color('#00f')] }),
          h(Sk.BlurMask, { blur: 3, style: 'solid' }),
          h(Sk.DashPathEffect, { intervals: [4, 2] }),
          h(Sk.Paint, { color: 'green', style: 'stroke', strokeWidth: 1 }, h(Sk.Shadow, { dx: 1, dy: 2, blur: 3, color: 'black' }))),
        h(Sk.Path, { path, start: 0.25, end: 0.75 }),
        h(Sk.RoundedRect, { x: 1, y: 2, width: 30, height: 20, r: 5 }, h(Sk.Shader, { source: effect, uniforms: { a: 0.5 } })))),
  );
  await wait();
  const n = byId('k');
  const c = n.props.commands;
  assert.equal(c.mode, 'skia');
  const g = c.c[0];
  assert.deepEqual(g.origin, [50, 50]);
  assert.equal(g.color, 'red');
  assert.deepEqual(g.clip.c[0], { t: 'rect', x: 0, y: 0, width: 90, height: 90, rx: 8, ry: 8 });
  assert.equal(g.clip.invert, true);
  const [circle, p, rr] = g.c;
  assert.deepEqual([circle.t, circle.cx, circle.cy, circle.r, circle.style], ['circle', 5, 6, 4, 'stroke']);
  assert.equal(circle.shader.t, 'linear');
  assert.deepEqual(circle.shader.colors[1], [0, 0, 1, 1]);
  assert.deepEqual(circle.maskFilter, { blur: 3, style: 'solid', respectCTM: true });
  assert.deepEqual(circle.pathEffect, [{ t: 'dash', intervals: [4, 2], phase: 0 }]);
  assert.equal(circle.paints.length, 1);
  assert.equal(circle.paints[0].imageFilter[0].t, 'shadow');
  assert.equal(p.d, 'M0 0L10 0Q10 10 0 10Z');
  assert.deepEqual([p.start, p.end], [0.25, 0.75]);
  assert.deepEqual([rr.t, rr.r], ['rect', 5]);
  assert.equal(rr.shader.t, 'shader');
  assert.deepEqual(rr.shader.uniforms, { a: 0.5 });
  render(null);
  await wait();
});

test('Skia.Path: SVG strings (arcs), transforms, interpolation', () => {
  const p = Sk.Skia.Path.MakeFromSVGString('M10 10 h 10 v 10 z');
  assert.equal(p.toSVGString(), 'M10 10L20 10L20 20Z');
  assert.equal(Sk.Skia.Path.MakeFromSVGString('M 0 0 L x'), null);
  const arc = Sk.Skia.Path.MakeFromSVGString('M0 0a5 5 0 1 0 10 0');
  assert.ok(arc.cmds.length >= 3 && arc.cmds.slice(1).every((c) => c[0] === 'C'));
  const last = arc.getLastPt();
  assert.ok(Math.abs(last.x - 10) < 1e-6 && Math.abs(last.y) < 1e-6);
  const b = Sk.Skia.Path.MakeFromSVGString('M0 0L10 10');
  const a2 = Sk.Skia.Path.MakeFromSVGString('M10 10L20 30');
  assert.equal(a2.interpolate(b, 0.5).toSVGString(), 'M5 5L15 20');
  const m = Sk.Skia.Matrix().translate(5, 0);
  assert.equal(b.copy().transform(m).toSVGString(), 'M5 0L15 10');
  assert.deepEqual(Array.from(Sk.Skia.Color('#ff000080')).map((x) => Math.round(x * 100) / 100), [1, 0, 0, 0.5]);
});

test('SvgXml / SvgCss: markup becomes the same elements', async () => {
  const xml = `<?xml version="1.0"?><svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 10 10">
    <style>.a { fill: #0f0 } rect { stroke: red }</style>
    <rect class="a" x="1" y="1" width="8" height="8" style="stroke-width: 2"/>
    <path d="M0 0L5 5" stroke-linecap="round"/></svg>`;
  const tree = parse(xml);
  assert.equal(tree.tag, 'svg');
  render(h(SvgCss, { xml, width: 20, height: 20, nativeID: 'x' }));
  await wait();
  let c = byId('x').props.commands;
  assert.deepEqual(c.vb, [0, 0, 10, 10]);
  assert.deepEqual(
    [c.c[0].t, c.c[0].fill, c.c[0].stroke, c.c[0].strokeWidth],
    ['rect', '#0f0', 'red', 2],
  );
  assert.equal(c.c[1].strokeLinecap, 'round');
  render(h(SvgXml, { xml, width: 20, height: 20, nativeID: 'x' }));
  await wait();
  c = byId('x').props.commands;
  assert.equal(c.c[0].fill, undefined, 'SvgXml ignores <style>');
  render(null);
  await wait();
});
