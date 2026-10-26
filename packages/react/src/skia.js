// @shopify/react-native-skia on JSUI: <Canvas> with declarative drawing
// children, drawn natively by one VectorView per Canvas
// (render/vector_view.h; vector/host.js has the mechanism). The CLI aliases
// '@shopify/react-native-skia' here:
//
//   import { Canvas, Circle, Group, LinearGradient, vec, Skia } from '@shopify/react-native-skia';
//   <Canvas style={{ width: 256, height: 256 }}>
//     <Group blendMode="multiply">
//       <Circle cx={128} cy={96} r={64} color="cyan" />
//       <Circle cx={96} cy={160} r={64}><LinearGradient start={vec(0, 0)} end={vec(256, 256)} colors={['#f0f', '#ff0']} /></Circle>
//     </Group>
//   </Canvas>
//
// Covered (same prop names and semantics): Group (transform / origin /
// matrix, clip / invertClip, layer, opacity and paint props inherited by
// its children), Circle, Rect, RoundedRect, Oval, Line, Path (SVG strings or
// Skia.Path, start / end trimming, fillType), Points, DiffRect, Fill, Image,
// Text, Paint, Mask; shaders LinearGradient, RadialGradient, SweepGradient,
// TwoPointConicalGradient, Shader (SkSL runtime effects, compiled once by
// the runtime like ShaderView), ImageShader, ColorShader, FractalNoise,
// Turbulence, Blend; BlurMask; image filters Blur, Shadow (also inner),
// Offset, Morphology; color filters ColorMatrix, BlendColor; path effects
// DashPathEffect, CornerPathEffect, DiscretePathEffect. The imperative
// `Skia` object builds paths, matrices, paints, shaders and filters as plain
// descriptions, so they can be passed as props (path={Skia.Path...},
// paint={...}, matrix={...}). Animated values (Animated.Value,
// interpolations, Animated.clock(), useClock()) work as drawing props, on
// the JS and the native driver. Not supported: Paragraph, Glyphs,
// TextPath, TextBlob, Picture / createPicture, Atlas, Vertices, Patch,
// Box / BoxShadow, backdrop filters, path ops, makeImageSnapshot, and
// Reanimated shared values.

import { useEffect, useMemo, useRef, useState } from 'react';
import { jsx } from 'react/jsx-runtime';
import { native, warnOnce } from './native.js';
import { resolveAssetSource } from './components/basic.js';
import { compileShader } from './shader.js';
import { Animated } from './animated/index.js';
import { DrawNode, animKey, drawComponent, registerSerializer, visibleChildren } from './vector/host.js';

// --- enums (strings in the declarative API, numbers in the imperative one) -------

export const PaintStyle = { Fill: 0, Stroke: 1 };
export const StrokeCap = { Butt: 0, Round: 1, Square: 2 };
export const StrokeJoin = { Bevel: 0, Miter: 1, Round: 2 };
export const FillType = { Winding: 0, EvenOdd: 1, InverseWinding: 2, InverseEvenOdd: 3 };
export const TileMode = { Clamp: 0, Repeat: 1, Mirror: 2, Decal: 3 };
export const BlurStyle = { Normal: 0, Solid: 1, Outer: 2, Inner: 3 };
export const PointMode = { Points: 0, Lines: 1, Polygon: 2 };
export const ClipOp = { Difference: 0, Intersect: 1 };
const BLEND_NAMES = [
  'clear', 'src', 'dst', 'srcOver', 'dstOver', 'srcIn', 'dstIn', 'srcOut', 'dstOut', 'srcATop', 'dstATop', 'xor',
  'plus', 'modulate', 'screen', 'overlay', 'darken', 'lighten', 'colorDodge', 'colorBurn', 'hardLight', 'softLight',
  'difference', 'exclusion', 'multiply', 'hue', 'saturation', 'color', 'luminosity',
];
export const BlendMode = {};
BLEND_NAMES.forEach((n, i) => (BlendMode[n[0].toUpperCase() + n.slice(1)] = i));

const enumName = (v, names) => (typeof v === 'number' ? names[v] : v);
const STYLE_NAMES = ['fill', 'stroke'];
const CAP_NAMES = ['butt', 'round', 'square'];
const JOIN_NAMES = ['bevel', 'miter', 'round'];
const FILL_NAMES = ['winding', 'evenOdd', 'inverseWinding', 'inverseEvenOdd'];
const TILE_NAMES = ['clamp', 'repeat', 'mirror', 'decal'];
const BLUR_NAMES = ['normal', 'solid', 'outer', 'inner'];
const POINT_NAMES = ['points', 'lines', 'polygon'];

// --- values ------------------------------------------------------------------

export const vec = (x = 0, y) => ({ x, y: y == null ? x : y });
export const point = vec;
export const rect = (x, y, width, height) => ({ x, y, width, height });
export const rrect = (r, rx, ry) => ({ rect: r, rx, ry: ry == null ? rx : ry });

/** A color as [r, g, b, a] floats (Skia.Color). */
function SkColor(c) {
  if (c == null) return new Float32Array([0, 0, 0, 0]);
  if (typeof c === 'object' && c.length >= 3) return new Float32Array([c[0], c[1], c[2], c.length > 3 ? c[3] : 1]);
  if (typeof c === 'number') {
    return new Float32Array([((c >>> 16) & 255) / 255, ((c >>> 8) & 255) / 255, (c & 255) / 255, ((c >>> 24) & 255) / 255]);
  }
  const s = String(c).trim();
  let m = /^#([0-9a-f]{3,8})$/i.exec(s);
  if (m) {
    let h = m[1];
    if (h.length <= 4) h = h.split('').map((x) => x + x).join('');
    const n = parseInt(h.slice(0, 6), 16);
    const a = h.length === 8 ? parseInt(h.slice(6, 8), 16) / 255 : 1;
    return new Float32Array([((n >> 16) & 255) / 255, ((n >> 8) & 255) / 255, (n & 255) / 255, a]);
  }
  m = /^rgba?\(([^)]*)\)$/i.exec(s);
  if (m) {
    const p = m[1].split(/[\s,/]+/).filter(Boolean).map(parseFloat);
    return new Float32Array([p[0] / 255, p[1] / 255, p[2] / 255, p.length > 3 ? p[3] : 1]);
  }
  // Named and other CSS colors: native parses the string.
  const out = new Float32Array([0, 0, 0, 1]);
  out.css = s;
  return out;
}

// A color prop for native: strings stay, typed arrays become arrays.
function color(c) {
  if (c == null) return c;
  if (typeof c === 'string' || typeof c === 'number') return c;
  if (c.css) return c.css;
  if (typeof c === 'object' && c.length >= 3) return Array.from(c);
  return c;
}
const colors = (list) => (Array.isArray(list) ? list.map(color) : []);

const pt = (v, def) => (v == null ? def : Array.isArray(v) ? [+v[0], +v[1]] : [+v.x, +v.y]);

// --- Skia.Path (as SVG path data) ---------------------------------------------------

const K = 0.5522847498;

function arcToCubics(x1, y1, rx, ry, phiDeg, large, sweep, x2, y2, out) {
  // SVG arc -> cubic Béziers (SVG implementation notes, F.6.5).
  if (rx === 0 || ry === 0) {
    out.push(['L', x2, y2]);
    return;
  }
  const phi = (phiDeg * Math.PI) / 180, cos = Math.cos(phi), sin = Math.sin(phi);
  const dx = (x1 - x2) / 2, dy = (y1 - y2) / 2;
  const xp = cos * dx + sin * dy, yp = -sin * dx + cos * dy;
  rx = Math.abs(rx);
  ry = Math.abs(ry);
  const lam = (xp * xp) / (rx * rx) + (yp * yp) / (ry * ry);
  if (lam > 1) {
    rx *= Math.sqrt(lam);
    ry *= Math.sqrt(lam);
  }
  const num = rx * rx * ry * ry - rx * rx * yp * yp - ry * ry * xp * xp;
  const den = rx * rx * yp * yp + ry * ry * xp * xp;
  let co = Math.sqrt(Math.max(0, num / den));
  if (large === sweep) co = -co;
  const cxp = (co * rx * yp) / ry, cyp = (-co * ry * xp) / rx;
  const cx = cos * cxp - sin * cyp + (x1 + x2) / 2, cy = sin * cxp + cos * cyp + (y1 + y2) / 2;
  const ang = (ux, uy, vx, vy) => {
    const a = Math.atan2(ux * vy - uy * vx, ux * vx + uy * vy);
    return a;
  };
  const t1 = ang(1, 0, (xp - cxp) / rx, (yp - cyp) / ry);
  let dt = ang((xp - cxp) / rx, (yp - cyp) / ry, (-xp - cxp) / rx, (-yp - cyp) / ry);
  if (!sweep && dt > 0) dt -= 2 * Math.PI;
  else if (sweep && dt < 0) dt += 2 * Math.PI;
  const n = Math.max(1, Math.ceil(Math.abs(dt) / (Math.PI / 2) - 1e-9));
  const step = dt / n, k = (4 / 3) * Math.tan(step / 4);
  let t = t1;
  for (let i = 0; i < n; i++) {
    const c1 = Math.cos(t), s1 = Math.sin(t), c2 = Math.cos(t + step), s2 = Math.sin(t + step);
    const p = (x, y) => [cx + rx * (cos * x) - ry * (sin * y), cy + rx * (sin * x) + ry * (cos * y)];
    const a = p(c1 - k * s1, s1 + k * c1), b = p(c2 + k * s2, s2 - k * c2), e = p(c2, s2);
    out.push(['C', a[0], a[1], b[0], b[1], e[0], e[1]]);
    t += step;
  }
}

/** Parses SVG path data into absolute M / L / Q / C / Z commands (null when invalid). */
function parsePathData(d) {
  const tokens = String(d).match(/[a-df-zA-DF-Z]|[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?/g);
  if (!tokens) return [];
  const out = [];
  let i = 0, cmd = '', x = 0, y = 0, sx = 0, sy = 0, lcx = 0, lcy = 0, lq = null;
  const n = () => {
    const v = parseFloat(tokens[i++]);
    if (v !== v) throw new Error('bad number');
    return v;
  };
  const isNum = () => i < tokens.length && !/^[a-zA-Z]$/.test(tokens[i]);
  try {
    while (i < tokens.length) {
      if (/^[a-zA-Z]$/.test(tokens[i])) cmd = tokens[i++];
      else if (!cmd) return null;
      const rel = cmd === cmd.toLowerCase();
      const C = cmd.toUpperCase();
      const ox = rel ? x : 0, oy = rel ? y : 0;
      let prev = out.length ? out[out.length - 1][0] : '';
      switch (C) {
        case 'M':
          x = n() + ox;
          y = n() + oy;
          out.push(['M', x, y]);
          sx = x;
          sy = y;
          cmd = rel ? 'l' : 'L';
          break;
        case 'L':
          x = n() + ox;
          y = n() + oy;
          out.push(['L', x, y]);
          break;
        case 'H':
          x = n() + ox;
          out.push(['L', x, y]);
          break;
        case 'V':
          y = n() + oy;
          out.push(['L', x, y]);
          break;
        case 'C': {
          const a = [n() + ox, n() + oy, n() + ox, n() + oy, n() + ox, n() + oy];
          out.push(['C', ...a]);
          lcx = a[2];
          lcy = a[3];
          x = a[4];
          y = a[5];
          break;
        }
        case 'S': {
          const c1x = prev === 'C' ? 2 * x - lcx : x, c1y = prev === 'C' ? 2 * y - lcy : y;
          const a = [n() + ox, n() + oy, n() + ox, n() + oy];
          out.push(['C', c1x, c1y, ...a]);
          lcx = a[0];
          lcy = a[1];
          x = a[2];
          y = a[3];
          break;
        }
        case 'Q': {
          const a = [n() + ox, n() + oy, n() + ox, n() + oy];
          out.push(['Q', ...a]);
          lq = [a[0], a[1]];
          x = a[2];
          y = a[3];
          break;
        }
        case 'T': {
          const c = prev === 'Q' && lq ? [2 * x - lq[0], 2 * y - lq[1]] : [x, y];
          const e = [n() + ox, n() + oy];
          out.push(['Q', c[0], c[1], e[0], e[1]]);
          lq = c;
          x = e[0];
          y = e[1];
          break;
        }
        case 'A': {
          const rx = n(), ry = n(), rot = n();
          const flag = () => {
            // Flags may be packed ("a1 1 0 00.5.5").
            const t = tokens[i];
            if (t.length > 1 && (t[0] === '0' || t[0] === '1') && !/[eE.]/.test(t.slice(0, 2)) && t[1] !== '.') {
              tokens[i] = t.slice(1);
              return t[0] === '1';
            }
            return n() !== 0;
          };
          const large = flag(), sweep = flag();
          const ex = n() + ox, ey = n() + oy;
          arcToCubics(x, y, rx, ry, rot, large, sweep, ex, ey, out);
          x = ex;
          y = ey;
          break;
        }
        case 'Z':
          out.push(['Z']);
          x = sx;
          y = sy;
          break;
        default:
          return null;
      }
      if (C !== 'Z' && !isNum() && i < tokens.length && !/^[a-zA-Z]$/.test(tokens[i])) return null;
      prev = C;
    }
  } catch {
    return null;
  }
  return out;
}

const fmt = (v) => {
  const r = Math.round(v * 1e4) / 1e4;
  return Object.is(r, -0) ? '0' : String(r);
};

class SkPathJS {
  constructor(cmds) {
    this.cmds = cmds || [];
    this.fillType = 0;
    this.__skPath = true;
  }
  _last() {
    for (let i = this.cmds.length - 1; i >= 0; i--) {
      const c = this.cmds[i];
      if (c[0] === 'Z') {
        for (let j = i - 1; j >= 0; j--) if (this.cmds[j][0] === 'M') return [this.cmds[j][1], this.cmds[j][2]];
        return [0, 0];
      }
      return [c[c.length - 2], c[c.length - 1]];
    }
    return [0, 0];
  }
  moveTo(x, y) { this.cmds.push(['M', x, y]); return this; }
  lineTo(x, y) {
    if (!this.cmds.length) this.moveTo(0, 0);
    this.cmds.push(['L', x, y]);
    return this;
  }
  quadTo(x1, y1, x2, y2) { this.cmds.push(['Q', x1, y1, x2, y2]); return this; }
  cubicTo(x1, y1, x2, y2, x3, y3) { this.cmds.push(['C', x1, y1, x2, y2, x3, y3]); return this; }
  conicTo(x1, y1, x2, y2, w) {
    // A conic as a cubic (exact for w = 1, close for circular arcs).
    const [x0, y0] = this._last();
    const k = (4 * w) / (3 * (1 + w));
    return this.cubicTo(x0 + k * (x1 - x0), y0 + k * (y1 - y0), x2 + k * (x1 - x2), y2 + k * (y1 - y2), x2, y2);
  }
  rMoveTo(dx, dy) { const [x, y] = this._last(); return this.moveTo(x + dx, y + dy); }
  rLineTo(dx, dy) { const [x, y] = this._last(); return this.lineTo(x + dx, y + dy); }
  rQuadTo(a, b, c, d) { const [x, y] = this._last(); return this.quadTo(x + a, y + b, x + c, y + d); }
  rCubicTo(a, b, c, d, e, f) { const [x, y] = this._last(); return this.cubicTo(x + a, y + b, x + c, y + d, x + e, y + f); }
  rConicTo(a, b, c, d, w) { const [x, y] = this._last(); return this.conicTo(x + a, y + b, x + c, y + d, w); }
  close() { this.cmds.push(['Z']); return this; }
  arcToRotated(rx, ry, xAxisRotate, useSmallArc, isCCW, x, y) {
    const [x0, y0] = this._last();
    arcToCubics(x0, y0, rx, ry, xAxisRotate, !useSmallArc, !isCCW, x, y, this.cmds);
    return this;
  }
  arcToOval(oval, startDeg, sweepDeg, forceMoveTo) {
    const cx = oval.x + oval.width / 2, cy = oval.y + oval.height / 2, rx = oval.width / 2, ry = oval.height / 2;
    const a0 = (startDeg * Math.PI) / 180;
    const p0 = [cx + rx * Math.cos(a0), cy + ry * Math.sin(a0)];
    if (forceMoveTo || !this.cmds.length) this.moveTo(p0[0], p0[1]);
    else this.lineTo(p0[0], p0[1]);
    const n = Math.max(1, Math.ceil(Math.abs(sweepDeg) / 90 - 1e-9));
    const step = (sweepDeg * Math.PI) / 180 / n, k = (4 / 3) * Math.tan(step / 4);
    let t = a0;
    for (let i = 0; i < n; i++) {
      const c1 = Math.cos(t), s1 = Math.sin(t), c2 = Math.cos(t + step), s2 = Math.sin(t + step);
      this.cubicTo(cx + rx * (c1 - k * s1), cy + ry * (s1 + k * c1), cx + rx * (c2 + k * s2), cy + ry * (s2 - k * c2), cx + rx * c2, cy + ry * s2);
      t += step;
    }
    return this;
  }
  addArc(oval, start, sweep) { return this.arcToOval(oval, start, sweep, true); }
  addRect(r, ccw) {
    const { x, y, width: w, height: h } = r;
    this.moveTo(x, y);
    if (ccw) this.lineTo(x, y + h).lineTo(x + w, y + h).lineTo(x + w, y);
    else this.lineTo(x + w, y).lineTo(x + w, y + h).lineTo(x, y + h);
    return this.close();
  }
  addRRect(rr) {
    const r = rr.rect || rr;
    const rx = Math.min(rr.rx || 0, r.width / 2), ry = Math.min(rr.ry == null ? rr.rx || 0 : rr.ry, r.height / 2);
    if (!rx || !ry) return this.addRect(r);
    const { x, y, width: w, height: h } = r;
    this.moveTo(x + rx, y).lineTo(x + w - rx, y);
    this.cubicTo(x + w - rx + K * rx, y, x + w, y + ry - K * ry, x + w, y + ry).lineTo(x + w, y + h - ry);
    this.cubicTo(x + w, y + h - ry + K * ry, x + w - rx + K * rx, y + h, x + w - rx, y + h).lineTo(x + rx, y + h);
    this.cubicTo(x + rx - K * rx, y + h, x, y + h - ry + K * ry, x, y + h - ry).lineTo(x, y + ry);
    this.cubicTo(x, y + ry - K * ry, x + rx - K * rx, y, x + rx, y);
    return this.close();
  }
  addOval(r) {
    const cx = r.x + r.width / 2, cy = r.y + r.height / 2, rx = r.width / 2, ry = r.height / 2;
    this.moveTo(cx + rx, cy);
    this.cubicTo(cx + rx, cy + K * ry, cx + K * rx, cy + ry, cx, cy + ry);
    this.cubicTo(cx - K * rx, cy + ry, cx - rx, cy + K * ry, cx - rx, cy);
    this.cubicTo(cx - rx, cy - K * ry, cx - K * rx, cy - ry, cx, cy - ry);
    this.cubicTo(cx + K * rx, cy - ry, cx + rx, cy - K * ry, cx + rx, cy);
    return this.close();
  }
  addCircle(x, y, r) { return this.addOval({ x: x - r, y: y - r, width: 2 * r, height: 2 * r }); }
  addPoly(points, close) {
    points.forEach((p, i) => (i ? this.lineTo(p.x, p.y) : this.moveTo(p.x, p.y)));
    if (close) this.close();
    return this;
  }
  addPath(other, matrix) {
    const o = matrix ? other.copy().transform(matrix) : other;
    for (const c of o.cmds) this.cmds.push(c.slice());
    return this;
  }
  reset() { this.cmds = []; return this; }
  rewind() { return this.reset(); }
  isEmpty() { return this.cmds.length === 0; }
  countPoints() { return this.cmds.reduce((n, c) => n + (c.length - 1) / 2, 0); }
  getLastPt() { const [x, y] = this._last(); return { x, y }; }
  getPoint(i) {
    let k = 0;
    for (const c of this.cmds)
      for (let j = 1; j + 1 < c.length; j += 2) if (k++ === i) return { x: c[j], y: c[j + 1] };
    return { x: 0, y: 0 };
  }
  getBounds() {
    let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
    for (const c of this.cmds)
      for (let j = 1; j + 1 < c.length; j += 2) {
        x0 = Math.min(x0, c[j]);
        x1 = Math.max(x1, c[j]);
        y0 = Math.min(y0, c[j + 1]);
        y1 = Math.max(y1, c[j + 1]);
      }
    return x0 > x1 ? rect(0, 0, 0, 0) : rect(x0, y0, x1 - x0, y1 - y0);
  }
  computeTightBounds() { return this.getBounds(); }
  setFillType(t) { this.fillType = typeof t === 'string' ? Math.max(0, FILL_NAMES.indexOf(t)) : t; return this; }
  getFillType() { return this.fillType; }
  offset(dx, dy) { return this.transform([1, 0, dx, 0, 1, dy, 0, 0, 1]); }
  transform(m) {
    const a = m && m.__skMatrix ? m.get() : m;
    if (!a) return this;
    for (const c of this.cmds)
      for (let j = 1; j + 1 < c.length; j += 2) {
        const x = c[j], y = c[j + 1];
        c[j] = a[0] * x + a[1] * y + a[2];
        c[j + 1] = a[3] * x + a[4] * y + a[5];
      }
    return this;
  }
  copy() {
    const p = new SkPathJS(this.cmds.map((c) => c.slice()));
    p.fillType = this.fillType;
    return p;
  }
  isInterpolatable(other) {
    return other && other.cmds.length === this.cmds.length && this.cmds.every((c, i) => c[0] === other.cmds[i][0]);
  }
  interpolate(end, weight, output) {
    if (!this.isInterpolatable(end)) return null;
    const out = output || new SkPathJS();
    out.cmds = this.cmds.map((c, i) => c.map((v, j) => (j === 0 ? v : v * weight + end.cmds[i][j] * (1 - weight))));
    out.fillType = this.fillType;
    return out;
  }
  trim() {
    warnOnce('skia-path-trim', '[jsui] Skia.Path.trim is not supported; use <Path start end>');
    return this;
  }
  toSVGString() {
    return this.cmds.map((c) => c[0] + c.slice(1).map(fmt).join(' ')).join('');
  }
  equals(o) { return !!o && o.toSVGString() === this.toSVGString(); }
  toCmds() { return this.cmds.map((c) => c.slice()); }
}

function makePath(source) {
  if (source == null) return null;
  if (source.__skPath) return source;
  const cmds = parsePathData(source);
  return cmds ? new SkPathJS(cmds) : null;
}

// --- Skia.Matrix / Paint / shaders / filters (descriptions) ---------------------------

class SkMatrixJS {
  constructor(m) {
    this.m = m ? Array.from(m) : [1, 0, 0, 0, 1, 0, 0, 0, 1];
    this.__skMatrix = true;
  }
  concat(o) {
    const a = this.m, b = o.__skMatrix ? o.m : o, r = new Array(9);
    for (let i = 0; i < 3; i++) for (let j = 0; j < 3; j++) r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
    this.m = r;
    return this;
  }
  translate(x, y) { return this.concat([1, 0, x, 0, 1, y, 0, 0, 1]); }
  scale(x, y = x) { return this.concat([x, 0, 0, 0, y, 0, 0, 0, 1]); }
  rotate(rad) { const c = Math.cos(rad), s = Math.sin(rad); return this.concat([c, -s, 0, s, c, 0, 0, 0, 1]); }
  skew(x, y) { return this.concat([1, x, 0, y, 1, 0, 0, 0, 1]); }
  identity() { this.m = [1, 0, 0, 0, 1, 0, 0, 0, 1]; return this; }
  get() { return this.m.slice(); }
}

class SkPaintJS {
  constructor() {
    this.attrs = {};
    this.__skPaint = true;
  }
  copy() { const p = new SkPaintJS(); p.attrs = Object.assign({}, this.attrs); return p; }
  setColor(c) { this.attrs.color = c; }
  getColor() { return SkColor(this.attrs.color); }
  setAlphaf(a) { this.attrs.opacity = a; }
  setStyle(s) { this.attrs.style = s; }
  setStrokeWidth(w) { this.attrs.strokeWidth = w; }
  setStrokeCap(c) { this.attrs.strokeCap = c; }
  setStrokeJoin(j) { this.attrs.strokeJoin = j; }
  setStrokeMiter(m) { this.attrs.strokeMiter = m; }
  setBlendMode(b) { this.attrs.blendMode = b; }
  setAntiAlias(a) { this.attrs.antiAlias = a; }
  setDither() {}
  setShader(s) { this.attrs.shader = s; }
  setMaskFilter(f) { this.attrs.maskFilter = f; }
  setPathEffect(e) { this.attrs.pathEffect = e; }
  setColorFilter(f) { this.attrs.colorFilter = f; }
  setImageFilter(f) { this.attrs.imageFilter = f; }
}

function gradientSpec(t, o) {
  return Object.assign({ t }, o);
}

class SkRuntimeEffectJS {
  constructor(source, info) {
    this.source = source;
    this.info = info;
  }
  getUniformCount() { return this.info && this.info.uniforms ? this.info.uniforms.length : 0; }
  getUniformName(i) { return this.info.uniforms[i].name; }
  getUniform(i) { return this.info.uniforms[i]; }
  makeShader(uniforms) { return { t: 'shader', source: this.source, uniforms: uniformsOf(uniforms, this), children: [] }; }
  makeShaderWithChildren(uniforms, children) {
    return { t: 'shader', source: this.source, uniforms: uniformsOf(uniforms, this), children: children || [] };
  }
}

// Uniforms as {name: value}; an array is matched to the declared uniforms in order.
function uniformsOf(u, effect) {
  if (u == null) return {};
  if (Array.isArray(u) || ArrayBuffer.isView(u)) {
    const out = {};
    let i = 0;
    const list = (effect && effect.info && effect.info.uniforms) || [];
    for (const d of list) {
      const n = (d.count || 1) * ({ float: 1, float2: 2, float3: 3, float4: 4, float2x2: 4, float3x3: 9, float4x4: 16, int: 1, int2: 2, int3: 3, int4: 4, half: 1, half2: 2, half3: 3, half4: 4 }[d.type] || 1);
      out[d.name] = Array.from(u).slice(i, i + n);
      i += n;
    }
    return out;
  }
  const out = {};
  for (const k in u) out[k] = uniformValue(u[k]);
  return out;
}

function uniformValue(v) {
  if (v == null) return 0;
  if (typeof v === 'number' || typeof v === 'boolean') return v;
  if (ArrayBuffer.isView(v)) return Array.from(v);
  if (Array.isArray(v)) return v.map(uniformValue);
  if (typeof v === 'object' && v.x != null) return [+v.x, +v.y];
  if (typeof v === 'object' && v.__skMatrix) return v.get();
  return v;
}

let fontSeq = 0;
class SkFontJS {
  constructor(family, size, weight, style) {
    this.family = family || '';
    this.size = size == null ? 14 : size;
    this.weight = weight;
    this.style = style;
  }
  getSize() { return this.size; }
  setSize(s) { this.size = s; }
  // Approximate (no synchronous native text measurement): 0.55 em per character.
  getTextWidth(text) { return String(text).length * this.size * 0.55; }
  measureText(text) { return rect(0, -this.size * 0.8, this.getTextWidth(text), this.size); }
  getMetrics() { return { ascent: -this.size * 0.93, descent: this.size * 0.24, leading: 0 }; }
  getGlyphIDs(text) { return Array.from(String(text)).map((_c, i) => i); }
}

class SkImageJS {
  constructor(uri, w, h) {
    this.uri = uri;
    this._w = w || 0;
    this._h = h || 0;
    this.__skImage = true;
  }
  width() { return this._w; }
  height() { return this._h; }
  getImageInfo() { return { width: this._w, height: this._h }; }
  makeShaderOptions(tx, ty) { return { t: 'image', image: { uri: this.uri }, tx: enumName(tx, TILE_NAMES), ty: enumName(ty, TILE_NAMES) }; }
  makeShaderCubic(tx, ty) { return this.makeShaderOptions(tx, ty); }
}

function imageOf(src) {
  if (src == null) return null;
  if (src.__skImage) return src;
  const a = resolveAssetSource(src);
  return a && a.uri ? new SkImageJS(String(a.uri), a.width, a.height) : null;
}

export const Skia = {
  Color: SkColor,
  Point: (x, y) => ({ x, y }),
  XYWHRect: (x, y, width, height) => ({ x, y, width, height }),
  RRectXY: (r, rx, ry) => ({ rect: r, rx, ry }),
  Matrix: (m) => new SkMatrixJS(m),
  Paint: () => new SkPaintJS(),
  Path: {
    Make: () => new SkPathJS(),
    MakeFromSVGString: (s) => makePath(String(s)),
    MakeFromCmds: (cmds) => new SkPathJS(cmds.map((c) => c.slice())),
  },
  Font: (typeface, size) => new SkFontJS(typeface && typeface.family, size),
  Typeface: { MakeFreeTypeFaceFromData: () => null },
  RuntimeEffect: {
    Make: (source) => {
      const info = compileShader(source);
      if (info && info.error) {
        console.warn('[jsui] RuntimeEffect: ' + info.error);
        return null;
      }
      return new SkRuntimeEffectJS(String(source), info);
    },
  },
  Shader: {
    MakeLinearGradient: (start, end, cs, pos, mode, localMatrix) =>
      gradientSpec('linear', { start: pt(start), end: pt(end), colors: colors(cs), positions: pos || undefined, mode: enumName(mode, TILE_NAMES), m: localMatrix ? matrixOf(localMatrix) : undefined }),
    MakeRadialGradient: (c, r, cs, pos, mode, localMatrix) =>
      gradientSpec('radial', { c: pt(c), r, colors: colors(cs), positions: pos || undefined, mode: enumName(mode, TILE_NAMES), m: localMatrix ? matrixOf(localMatrix) : undefined }),
    MakeSweepGradient: (cx, cy, cs, pos, mode, localMatrix, _flags, start = 0, end = 360) =>
      gradientSpec('sweep', { cx, cy, colors: colors(cs), positions: pos || undefined, mode: enumName(mode, TILE_NAMES), start, end, m: localMatrix ? matrixOf(localMatrix) : undefined }),
    MakeTwoPointConicalGradient: (s, sr, e, er, cs, pos, mode) =>
      gradientSpec('conical', { start: pt(s), startR: sr, end: pt(e), endR: er, colors: colors(cs), positions: pos || undefined, mode: enumName(mode, TILE_NAMES) }),
    MakeColor: (c) => ({ t: 'color', color: color(c) }),
    MakeFractalNoise: (fx, fy, octaves, seed, tw, th) => ({ t: 'fractalNoise', freqX: fx, freqY: fy, octaves, seed, tileWidth: tw, tileHeight: th }),
    MakeTurbulence: (fx, fy, octaves, seed, tw, th) => ({ t: 'turbulence', freqX: fx, freqY: fy, octaves, seed, tileWidth: tw, tileHeight: th }),
    MakeBlend: (mode, a, b) => ({ t: 'blend', blendMode: enumName(mode, BLEND_NAMES), children: [a, b] }),
  },
  MaskFilter: {
    MakeBlur: (style, sigma, respectCTM) => ({ blur: sigma, style: enumName(style, BLUR_NAMES), respectCTM: respectCTM !== false }),
  },
  PathEffect: {
    MakeDash: (intervals, phase) => [{ t: 'dash', intervals: Array.from(intervals), phase: phase || 0 }],
    MakeCorner: (r) => [{ t: 'corner', r }],
    MakeDiscrete: (length, deviation, seed) => [{ t: 'discrete', length, deviation, seed }],
  },
  ColorFilter: {
    MakeMatrix: (m) => [{ t: 'matrix', matrix: Array.from(m) }],
    MakeBlend: (c, mode) => [{ t: 'blend', color: color(c), blendMode: enumName(mode, BLEND_NAMES) }],
  },
  ImageFilter: {
    MakeBlur: (sx, sy, mode) => [{ t: 'blur', blur: [sx, sy], mode: enumName(mode, TILE_NAMES) }],
    MakeDropShadow: (dx, dy, sx, _sy, c) => [{ t: 'shadow', dx, dy, blur: sx, color: color(c) }],
    MakeDropShadowOnly: (dx, dy, sx, _sy, c) => [{ t: 'shadow', dx, dy, blur: sx, color: color(c), shadowOnly: true }],
    MakeOffset: (x, y) => [{ t: 'offset', x, y }],
  },
  Image: {
    MakeImageFromEncoded: () => {
      warnOnce('skia-encoded', '[jsui] Skia.Image.MakeImageFromEncoded is not supported; use useImage(require(...) | uri)');
      return null;
    },
  },
  Data: { fromURI: (uri) => Promise.resolve({ uri }) },
};

function matrixOf(m) {
  if (m == null) return undefined;
  if (m.__skMatrix) return m.get();
  if (ArrayBuffer.isView(m)) return Array.from(m);
  return m;
}

// --- hooks ---------------------------------------------------------------------

/** The image of a source (require(), {uri}, URI); loaded by the runtime when drawn. */
export function useImage(source) {
  return useMemo(() => imageOf(source), [typeof source === 'object' && source ? source.uri : source]);
}

/** A font described for <Text>: a registered family by name. */
export function matchFont(style = {}) {
  return new SkFontJS(style.fontFamily, style.fontSize == null ? 14 : style.fontSize, style.fontWeight, style.fontStyle);
}

/** Loads a font file (require() / URI) and returns it at `size` (null while loading). */
export function useFont(source, size = 14) {
  const [family, setFamily] = useState(null);
  const a = resolveAssetSource(source);
  const uri = a && a.uri;
  useEffect(() => {
    if (!uri) return undefined;
    let live = true;
    const fam = '__skia_font_' + ++fontSeq;
    if (typeof native.loadFont !== 'function') {
      setFamily('');
      return undefined;
    }
    Promise.resolve(native.loadFont(fam, String(uri), 0, -1)).then(
      () => live && setFamily(fam),
      (e) => console.warn('[jsui] useFont: ' + (e && e.message)),
    );
    return () => {
      live = false;
    };
  }, [uri]);
  return useMemo(() => (family == null ? null : new SkFontJS(family, size)), [family, size]);
}

export function useFonts() {
  warnOnce('skia-usefonts', '[jsui] useFonts / Paragraph are not supported');
  return null;
}

/** A ref to the Canvas (its view). makeImageSnapshot is not supported. */
export function useCanvasRef() {
  return useRef(null);
}

/** Milliseconds since mount, as an Animated value driven by the runtime clock. */
export function useClock() {
  return useMemo(() => Animated.multiply(Animated.clock(), 1000), []);
}

export const mix = (t, a, b) => a + (b - a) * t;
export const clamp = (v, lo, hi) => Math.min(Math.max(v, lo), hi);
export function interpolate(x, input, output) {
  let i = 1;
  while (i < input.length - 1 && x > input[i]) i++;
  const t = (x - input[i - 1]) / (input[i] - input[i - 1] || 1);
  return output[i - 1] + (output[i] - output[i - 1]) * t;
}

// --- components ------------------------------------------------------------------

const P = 'draw:sk.';
const dc = (name, normalize) => drawComponent(P + name, name, normalize);

export const Circle = dc('Circle');
export const Rect = dc('Rect');
export const RoundedRect = dc('RoundedRect');
export const Oval = dc('Oval');
export const Line = dc('Line');
export const Path = dc('Path');
export const Points = dc('Points');
export const DiffRect = dc('DiffRect');
export const Fill = dc('Fill');
export const Image = dc('Image');
export const Text = dc('Text');
export const Paint = dc('Paint');
export const LinearGradient = dc('LinearGradient');
export const RadialGradient = dc('RadialGradient');
export const SweepGradient = dc('SweepGradient');
export const TwoPointConicalGradient = dc('TwoPointConicalGradient');
export const Shader = dc('Shader');
export const ImageShader = dc('ImageShader');
export const ColorShader = dc('ColorShader');
export const FractalNoise = dc('FractalNoise');
export const Turbulence = dc('Turbulence');
export const Blend = dc('Blend');
export const BlurMask = dc('BlurMask');
export const Blur = dc('Blur');
export const Shadow = dc('Shadow');
export const Offset = dc('Offset');
export const Morphology = dc('Morphology');
export const ColorMatrix = dc('ColorMatrix');
export const BlendColor = dc('BlendColor');
export const DashPathEffect = dc('DashPathEffect');
export const CornerPathEffect = dc('CornerPathEffect');
export const DiscretePathEffect = dc('DiscretePathEffect');
// A Group whose `layer` is a <Paint> element renders it as a hidden child.
export const Group = dc('Group', (props) => {
  const layer = props.layer;
  if (!layer || typeof layer !== 'object' || layer.$$typeof == null) return props;
  const out = Object.assign({}, props);
  out.layer = true;
  out.children = [jsx(P + 'Layer', { children: layer }, '__layer'), props.children];
  return out;
});
// <Mask mask={<Group>...</Group>} mode="alpha"|"luminance">content</Mask>
export const Mask = dc('Mask', (props) => {
  const out = Object.assign({}, props);
  out.children = [jsx(P + 'MaskContent', { children: props.mask }, '__mask'), props.children];
  delete out.mask;
  return out;
});

function unsupported(name) {
  const C = (props) => {
    warnOnce('skia-' + name, `[jsui] react-native-skia: <${name}> is not supported (not drawn)`);
    return jsx(P + 'Unsupported', { children: props.children });
  };
  C.displayName = name;
  return C;
}
export const Paragraph = unsupported('Paragraph');
export const Glyphs = unsupported('Glyphs');
export const TextPath = unsupported('TextPath');
export const TextBlob = unsupported('TextBlob');
export const Picture = unsupported('Picture');
export const Atlas = unsupported('Atlas');
export const Vertices = unsupported('Vertices');
export const Patch = unsupported('Patch');
export const Box = unsupported('Box');
export const BoxShadow = unsupported('BoxShadow');
export const BackdropBlur = unsupported('BackdropBlur');
export const BackdropFilter = unsupported('BackdropFilter');

/** The drawing surface: a view whose children draw into it (0, 0 at its top-left). */
export function Canvas(props) {
  const { style, children, onSize, onLayout, mode: _mode, debug: _debug, opaque: _opaque, ...rest } = props;
  const out = Object.assign({}, rest, { mode: 'skia', style: [{ overflow: 'hidden' }, style], children });
  if (onSize || onLayout) {
    out.onLayout = (e) => {
      if (onSize && typeof onSize === 'object' && 'value' in onSize) {
        const { width, height } = e.nativeEvent.layout;
        onSize.value = { width, height };
      }
      if (onLayout) onLayout(e);
    };
  }
  return jsx('VectorView', out);
}
Canvas.displayName = 'Canvas';

// --- serialization ---------------------------------------------------------------

const SHADERS = {
  LinearGradient: 1, RadialGradient: 1, SweepGradient: 1, TwoPointConicalGradient: 1, Shader: 1, ImageShader: 1,
  ColorShader: 1, FractalNoise: 1, Turbulence: 1, Blend: 1,
};
const IMAGE_FILTERS = { Blur: 1, Shadow: 1, Offset: 1, Morphology: 1 };
const COLOR_FILTERS = { ColorMatrix: 1, BlendColor: 1 };
const PATH_EFFECTS = { DashPathEffect: 1, CornerPathEffect: 1, DiscretePathEffect: 1 };

// The transform props of a group, a shape or a shader: `transform` (RN-style,
// radians), `origin`, `matrix`.
function transformProps(p, out) {
  if (p.matrix != null) out.m = matrixOf(p.matrix);
  if (Array.isArray(p.transform) && p.transform.length) out.transform = p.transform;
  if (p.origin != null) out.origin = pt(p.origin);
}

function paintProps(p, out) {
  if (p.paint && p.paint.__skPaint) paintProps(p.paint.attrs, out);
  if (p.color != null) out.color = color(p.color);
  const ps = p.paintStyle != null ? p.paintStyle : typeof p.style !== 'object' ? p.style : null;
  if (ps != null) out.style = enumName(ps, STYLE_NAMES);
  if (p.strokeWidth != null) out.strokeWidth = p.strokeWidth;
  if (p.strokeCap != null) out.strokeCap = enumName(p.strokeCap, CAP_NAMES);
  if (p.strokeJoin != null) out.strokeJoin = enumName(p.strokeJoin, JOIN_NAMES);
  if (p.strokeMiter != null) out.strokeMiter = p.strokeMiter;
  if (p.opacity != null && typeof p.opacity !== 'object') out.opacity = p.opacity;
  if (p.blendMode != null) out.blendMode = enumName(p.blendMode, BLEND_NAMES);
  if (p.antiAlias != null) out.antiAlias = !!p.antiAlias;
  if (p.shader != null) out.shader = p.shader;
  if (p.maskFilter != null) out.maskFilter = p.maskFilter;
  if (p.pathEffect != null) out.pathEffect = p.pathEffect;
  if (p.colorFilter != null) out.colorFilter = p.colorFilter;
  if (p.imageFilter != null) out.imageFilter = p.imageFilter;
}

function shaderOf(n) {
  const p = n.props;
  const out = {};
  switch (n.name) {
    case 'LinearGradient':
      out.t = 'linear';
      out.start = pt(p.start, [0, 0]);
      out.end = pt(p.end, [0, 0]);
      break;
    case 'RadialGradient':
      out.t = 'radial';
      out.c = pt(p.c, [0, 0]);
      out.r = p.r;
      break;
    case 'SweepGradient':
      out.t = 'sweep';
      out.c = pt(p.c, [0, 0]);
      if (p.start != null) out.start = p.start;
      if (p.end != null) out.end = p.end;
      break;
    case 'TwoPointConicalGradient':
      out.t = 'conical';
      out.start = pt(p.start, [0, 0]);
      out.startR = p.startR;
      out.end = pt(p.end, [0, 0]);
      out.endR = p.endR;
      break;
    case 'ColorShader':
      return { t: 'color', color: color(p.color) };
    case 'FractalNoise':
    case 'Turbulence':
      return { t: n.name === 'FractalNoise' ? 'fractalNoise' : 'turbulence', freqX: p.freqX, freqY: p.freqY, octaves: p.octaves, seed: p.seed, tileWidth: p.tileWidth, tileHeight: p.tileHeight };
    case 'Shader': {
      const src = p.source && p.source.source != null ? p.source.source : p.source;
      out.t = 'shader';
      out.source = src == null ? '' : String(src);
      out.uniforms = uniformsOf(p.uniforms, p.source && p.source.info ? p.source : null);
      out.children = childShaders(n);
      break;
    }
    case 'ImageShader': {
      const img = imageOf(p.image);
      out.t = 'image';
      out.image = img ? { uri: img.uri } : null;
      out.fit = p.fit || 'none';
      const r = p.rect || (p.width != null ? { x: p.x || 0, y: p.y || 0, width: p.width, height: p.height } : null);
      if (r) out.rect = [r.x, r.y, r.width, r.height];
      out.tx = enumName(p.tx, TILE_NAMES) || 'decal';
      out.ty = enumName(p.ty, TILE_NAMES) || 'decal';
      break;
    }
    case 'Blend':
      return { t: 'blend', blendMode: enumName(p.mode, BLEND_NAMES), children: childShaders(n) };
    default:
      return null;
  }
  if (p.colors) out.colors = colors(p.colors);
  if (p.positions) out.positions = Array.from(p.positions);
  if (p.mode != null) out.mode = enumName(p.mode, TILE_NAMES);
  if (p.matrix != null) out.m = matrixOf(p.matrix);
  if (Array.isArray(p.transform)) {
    const o = pt(p.origin, null);
    out.transform = o ? [{ translate: o }].concat(p.transform, [{ translate: [-o[0], -o[1]] }]) : p.transform;
  }
  return out;
}

function childShaders(n) {
  const out = [];
  for (const c of visibleChildren(n)) if (c instanceof DrawNode && SHADERS[c.name] === 1) out.push(shaderOf(c));
  return out;
}

function imageFilterOf(n) {
  const p = n.props;
  switch (n.name) {
    case 'Blur':
      return { t: 'blur', blur: p.blur && typeof p.blur === 'object' ? pt(p.blur) : p.blur, mode: enumName(p.mode, TILE_NAMES) };
    case 'Shadow':
      return { t: 'shadow', dx: p.dx || 0, dy: p.dy || 0, blur: p.blur || 0, color: color(p.color == null ? 'black' : p.color), inner: !!p.inner, shadowOnly: !!p.shadowOnly };
    case 'Offset':
      return { t: 'offset', x: p.x || 0, y: p.y || 0 };
    case 'Morphology':
      return { t: 'morphology', op: p.operator || 'dilate', radius: typeof p.radius === 'object' ? p.radius.x : p.radius };
  }
  return null;
}

function colorFilterOf(n) {
  const p = n.props;
  if (n.name === 'ColorMatrix') return { t: 'matrix', matrix: Array.from(p.matrix || []) };
  if (n.name === 'BlendColor') return { t: 'blend', color: color(p.color), blendMode: enumName(p.mode, BLEND_NAMES) };
  return null;
}

function pathEffectOf(n) {
  const p = n.props;
  if (n.name === 'DashPathEffect') return { t: 'dash', intervals: Array.from(p.intervals || []), phase: p.phase || 0 };
  if (n.name === 'CornerPathEffect') return { t: 'corner', r: p.r || 0 };
  if (n.name === 'DiscretePathEffect') return { t: 'discrete', length: p.length, deviation: p.deviation, seed: p.seed };
  return null;
}

// Effect children (shaders, filters, path effects, paints) onto `out`;
// drawing children returned.
function effects(n, out, ctx) {
  const draws = [];
  for (const c of visibleChildren(n)) {
    if (!(c instanceof DrawNode)) continue;
    const name = c.name;
    if (SHADERS[name] === 1) out.shader = shaderOf(c);
    else if (name === 'BlurMask') {
      const p = c.props;
      out.maskFilter = { blur: p.blur, style: enumName(p.style, BLUR_NAMES), respectCTM: p.respectCTM !== false };
    } else if (IMAGE_FILTERS[name] === 1) (out.imageFilter || (out.imageFilter = [])).push(imageFilterOf(c));
    else if (COLOR_FILTERS[name] === 1) (out.colorFilter || (out.colorFilter = [])).push(colorFilterOf(c));
    else if (PATH_EFFECTS[name] === 1) {
      (out.pathEffect || (out.pathEffect = [])).push(pathEffectOf(c));
      // Nested path effects compose.
      for (const g of visibleChildren(c)) if (g instanceof DrawNode && PATH_EFFECTS[g.name] === 1) out.pathEffect.unshift(pathEffectOf(g));
    } else if (name === 'Paint') {
      const po = {};
      paintProps(c.props, po);
      effects(c, po, ctx);
      (out.paints || (out.paints = [])).push(po);
    } else if (name === 'Layer') {
      const lo = {};
      for (const g of visibleChildren(c)) {
        if (!(g instanceof DrawNode) || g.name !== 'Paint') continue;
        paintProps(g.props, lo);
        effects(g, lo, ctx);
      }
      out.layer = layerOf(lo);
    } else if (name === 'MaskContent') {
      out.mask = { c: elems(visibleChildren(c), ctx) };
    } else draws.push(c);
  }
  return draws;
}

function layerOf(p) {
  const l = {};
  if (p.opacity != null) l.opacity = p.opacity;
  if (p.imageFilter) l.imageFilter = p.imageFilter;
  if (p.colorFilter) l.colorFilter = p.colorFilter;
  if (p.blendMode) l.blendMode = p.blendMode;
  return l;
}

function rectOf(p) {
  if (p.rect) {
    const r = p.rect.rect || p.rect;
    return [r.x, r.y, r.width, r.height];
  }
  return [p.x || 0, p.y || 0, p.width || 0, p.height || 0];
}

function rrectD(r) {
  const rr = r.rect || r;
  return new SkPathJS().addRRect({ rect: rr, rx: r.rx || 0, ry: r.ry == null ? r.rx || 0 : r.ry }).toSVGString();
}

function clipOf(clip, invert) {
  let shape = null;
  if (typeof clip === 'string') shape = { t: 'path', d: clip };
  else if (clip.__skPath) shape = { t: 'path', d: clip.toSVGString(), fillType: FILL_NAMES[clip.fillType] };
  else if (clip.rect) {
    const r = clip.rect;
    shape = { t: 'rect', x: r.x, y: r.y, width: r.width, height: r.height, rx: clip.rx || 0, ry: clip.ry == null ? clip.rx || 0 : clip.ry };
  } else if (clip.width != null) shape = { t: 'rect', x: clip.x || 0, y: clip.y || 0, width: clip.width, height: clip.height };
  if (!shape) return null;
  const out = { c: [shape] };
  if (invert) out.invert = true;
  return out;
}

function elems(nodes, ctx) {
  const out = [];
  for (const n of nodes) {
    if (!(n instanceof DrawNode) || n.hidden) continue;
    const e = elem(n, ctx);
    if (e) out.push(e);
  }
  return out;
}

function elem(n, ctx) {
  const p = n.props;
  const out = {};
  const k = animKey(n, ctx.anim);
  if (k) out.k = k;
  switch (n.name) {
    case 'Group':
    case 'Mask':
    case 'Unsupported':
      out.t = 'g';
      break;
    case 'Circle': {
      out.t = 'circle';
      const c = p.c || p.center;
      if (c) {
        out.cx = c.x;
        out.cy = c.y;
      }
      if (p.cx != null) out.cx = p.cx;
      if (p.cy != null) out.cy = p.cy;
      if (p.r != null) out.r = p.r;
      break;
    }
    case 'Rect':
    case 'RoundedRect':
    case 'Oval': {
      out.t = n.name === 'Oval' ? 'oval' : 'rect';
      const r = rectOf(p);
      if (p.x != null || !p.rect) {
        // (Animated x / y / width / height are style keys: absent here.)
        if (p.x != null) out.x = p.x;
        if (p.y != null) out.y = p.y;
        if (p.width != null) out.width = p.width;
        if (p.height != null) out.height = p.height;
      } else {
        [out.x, out.y, out.width, out.height] = r;
      }
      if (n.name === 'RoundedRect') {
        if (p.rect && p.rect.rect) {
          out.rx = p.rect.rx || 0;
          out.ry = p.rect.ry == null ? out.rx : p.rect.ry;
        } else if (p.r != null && typeof p.r === 'object') {
          out.rx = p.r.x;
          out.ry = p.r.y;
        } else if (p.r != null) out.r = p.r;
      }
      break;
    }
    case 'Line': {
      out.t = 'line';
      const a = pt(p.p1, [0, 0]), b = pt(p.p2, [0, 0]);
      [out.x1, out.y1, out.x2, out.y2] = [a[0], a[1], b[0], b[1]];
      break;
    }
    case 'Path': {
      out.t = 'path';
      const path = p.path;
      if (path != null && path.__skPath) {
        out.d = path.toSVGString();
        if (path.fillType) out.fillType = FILL_NAMES[path.fillType];
      } else if (path != null) out.d = String(path);
      if (p.fillType != null) out.fillType = enumName(p.fillType, FILL_NAMES);
      if (p.start != null) out.start = p.start;
      if (p.end != null) out.end = p.end;
      break;
    }
    case 'DiffRect':
      out.t = 'path';
      out.d = rrectD(p.outer) + rrectD(p.inner);
      out.fillType = 'evenOdd';
      break;
    case 'Points': {
      out.t = 'points';
      const pts = [];
      for (const q of p.points || []) pts.push(q.x, q.y);
      out.points = pts;
      out.mode = enumName(p.mode, POINT_NAMES) || 'points';
      break;
    }
    case 'Fill':
      out.t = 'fill';
      break;
    case 'Image': {
      out.t = 'image';
      const img = imageOf(p.image);
      if (img) out.image = { uri: img.uri, width: img._w || undefined, height: img._h || undefined };
      const r = p.rect;
      if (r) [out.x, out.y, out.width, out.height] = [r.x, r.y, r.width, r.height];
      else {
        if (p.x != null) out.x = p.x;
        if (p.y != null) out.y = p.y;
        if (p.width != null) out.width = p.width;
        if (p.height != null) out.height = p.height;
      }
      out.fit = p.fit || 'contain';
      break;
    }
    case 'Text': {
      out.t = 'text';
      if (p.text != null) out.text = String(p.text);
      if (p.x != null) out.x = p.x;
      if (p.y != null) out.y = p.y;
      const f = p.font;
      if (f) out.font = { family: f.family || undefined, size: f.size, weight: f.weight, style: f.style };
      else out.font = { size: 14 };
      break;
    }
    default:
      return null;  // shaders, filters, paints: read by their parent
  }
  paintProps(p, out);
  transformProps(p, out);
  if (p.clip != null) {
    const c = clipOf(p.clip, p.invertClip);
    if (c) out.clip = c;
  }
  if (p.layer === true && !out.layer) out.layer = {};
  else if (p.layer && p.layer.__skPaint) out.layer = layerOf(Object.assign({}, p.layer.attrs));
  const draws = effects(n, out, ctx);
  if (n.name === 'Mask') {
    if (out.mask) out.mask.mode = p.mode === 'luminance' ? 'luminance' : 'alpha';
  }
  if (draws.length) out.c = elems(draws, ctx);
  return out;
}

registerSerializer('skia', (owner, anim) => {
  const ctx = { anim };
  return { c: elems(visibleChildren(owner), ctx) };
});

export default Canvas;
