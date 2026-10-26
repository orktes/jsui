// react-native-svg on JSUI: the same components and props, drawn natively by
// one VectorView per <Svg> (render/vector_view.h; vector/host.js has the
// mechanism). The CLI aliases 'react-native-svg' here, so
//
//   import Svg, { Circle, Path, G, LinearGradient, Stop, Defs } from 'react-native-svg';
//   <Svg width={48} height={48} viewBox="0 0 24 24" color="#7cf">
//     <Defs><LinearGradient id="g" x1="0" y1="0" x2="1" y2="1">
//       <Stop offset="0" stopColor="#f90" /><Stop offset="1" stopColor="#f30" />
//     </LinearGradient></Defs>
//     <Circle cx={12} cy={12} r={10} fill="url(#g)" stroke="currentColor" strokeWidth={2} />
//   </Svg>
//
// works unchanged. Supported: Svg, G, Path, Rect, Circle, Ellipse, Line,
// Polyline, Polygon, Text / TSpan / TextPath (basic: no text on a path),
// Image, Defs, LinearGradient / RadialGradient / Stop, ClipPath, Mask
// (luminance), Use, Symbol, nested Svg; presentation props (fill, stroke,
// strokeWidth, strokeLinecap / Linejoin / Miterlimit, strokeDasharray /
// Dashoffset, fillRule, clipRule, opacity, fillOpacity, strokeOpacity,
// color / currentColor, font props, textAnchor, display), transforms (the
// SVG string, RN-style arrays, and the x / y / translate / scale / rotation
// / origin / skew props), SvgXml, SvgCss (simple selectors), SvgUri,
// SvgCssUri and LocalSvg. Animated values work as props (the runtime then
// writes them, also with useNativeDriver). Not supported: Pattern, Marker,
// Filter primitives, ForeignObject, per-shape touch events (put onPress on
// the Svg), and text on a path.

import { createContext, createElement, useContext, useEffect, useState } from 'react';
import { jsx } from 'react/jsx-runtime';
import { flatten } from './StyleSheet.js';
import { resolveAssetSource } from './components/basic.js';
import { warnOnce } from './native.js';
import { DrawNode, DrawText, animKey, drawComponent, registerSerializer, textOf, visibleChildren } from './vector/host.js';

// --- components --------------------------------------------------------------

const P = 'draw:svg.';

export const G = drawComponent(P + 'G', 'G');
export const Path = drawComponent(P + 'Path', 'Path');
export const Rect = drawComponent(P + 'Rect', 'Rect');
export const Circle = drawComponent(P + 'Circle', 'Circle');
export const Ellipse = drawComponent(P + 'Ellipse', 'Ellipse');
export const Line = drawComponent(P + 'Line', 'Line');
export const Polyline = drawComponent(P + 'Polyline', 'Polyline');
export const Polygon = drawComponent(P + 'Polygon', 'Polygon');
export const Text = drawComponent(P + 'Text', 'Text');
export const TSpan = drawComponent(P + 'TSpan', 'TSpan');
export const TextPath = drawComponent(P + 'TextPath', 'TextPath');
export const Image = drawComponent(P + 'Image', 'Image');
export const Defs = drawComponent(P + 'Defs', 'Defs');
export const LinearGradient = drawComponent(P + 'LinearGradient', 'LinearGradient');
export const RadialGradient = drawComponent(P + 'RadialGradient', 'RadialGradient');
export const Stop = drawComponent(P + 'Stop', 'Stop');
export const ClipPath = drawComponent(P + 'ClipPath', 'ClipPath');
export const Mask = drawComponent(P + 'Mask', 'Mask');
export const Use = drawComponent(P + 'Use', 'Use');
export const Symbol = drawComponent(P + 'Symbol', 'Symbol');
export const Pattern = drawComponent(P + 'Pattern', 'Pattern');
export const Marker = drawComponent(P + 'Marker', 'Marker');
export const ForeignObject = drawComponent(P + 'ForeignObject', 'ForeignObject');
export const Shape = G;

// Filter primitives: accepted, not drawn.
const unsupported = (name) => drawComponent(P + name, name);
export const Filter = unsupported('Filter');
export const FeBlend = unsupported('FeBlend');
export const FeColorMatrix = unsupported('FeColorMatrix');
export const FeComposite = unsupported('FeComposite');
export const FeDropShadow = unsupported('FeDropShadow');
export const FeFlood = unsupported('FeFlood');
export const FeGaussianBlur = unsupported('FeGaussianBlur');
export const FeMerge = unsupported('FeMerge');
export const FeMergeNode = unsupported('FeMergeNode');
export const FeOffset = unsupported('FeOffset');

// Svg props that size or place the view (the rest are presentation props of
// the drawing, inherited by its elements).
const VIEW_PROPS = {
  style: 1, children: 1, ref: 1, key: 1, width: 1, height: 1, viewBox: 1, preserveAspectRatio: 1, opacity: 1,
  onLayout: 1, onPress: 1, onPressIn: 1, onPressOut: 1, onLongPress: 1, pointerEvents: 1, testID: 1, nativeID: 1,
  accessible: 1, accessibilityLabel: 1, accessibilityRole: 1, 'aria-label': 1, focusable: 1, hitSlop: 1,
};

// Inside an <Svg>: a nested <Svg> is a drawing element (a viewport), not a view.
const InSvg = createContext(false);
const NestedSvg = drawComponent(P + 'Svg', 'Svg');

export function Svg(props) {
  if (useContext(InSvg)) return createElement(NestedSvg, props);
  const flat = flatten(props.style) || {};
  let width = props.width != null ? props.width : flat.width;
  let height = props.height != null ? props.height : flat.height;
  // react-native-svg: an Svg with neither size fills its parent.
  if (width == null && height == null) width = height = '100%';
  const root = {};
  for (const k in props) {
    if (VIEW_PROPS[k] !== 1 && typeof props[k] !== 'function') root[k] = props[k];
  }
  if (props.viewBox != null) root.viewBox = props.viewBox;
  if (props.preserveAspectRatio != null) root.preserveAspectRatio = props.preserveAspectRatio;
  root.width = width;
  root.height = height;
  const style = [{ overflow: 'hidden' }, props.style, { width: cssLength(width), height: cssLength(height) }];
  if (props.opacity != null) style.push({ opacity: +props.opacity });
  const out = { mode: 'svg', root, style, children: jsx(InSvg.Provider, { value: true, children: props.children }) };
  for (const k in props) {
    if (k === 'style' || k === 'children' || k === 'width' || k === 'height' || k === 'viewBox') continue;
    if (VIEW_PROPS[k] === 1 || typeof props[k] === 'function') out[k] = props[k];
  }
  return jsx('VectorView', out);
}
Svg.displayName = 'Svg';

function cssLength(v) {
  if (v == null) return undefined;
  if (typeof v === 'number') return v;
  const s = String(v).trim();
  if (s.endsWith('%')) return s;
  const n = parseFloat(s);
  return n === n ? n : undefined;
}

// --- numbers, lengths, transforms ---------------------------------------------

function numList(v) {
  if (v == null) return [];
  if (typeof v === 'number') return [v];
  if (Array.isArray(v)) return v.map(Number).filter((n) => n === n);
  const out = [];
  const re = /[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?/g;
  let m;
  while ((m = re.exec(String(v)))) out.push(+m[0]);
  return out;
}

// A length in user units; `ref` resolves percentages.
function len(v, ref, def) {
  if (v == null || v === '') return def;
  if (typeof v === 'number') return v;
  if (Array.isArray(v)) return len(v[0], ref, def);
  const s = String(v).trim();
  const n = parseFloat(s);
  if (n !== n) return def;
  if (s.endsWith('%')) return (n * ref) / 100;
  if (s.endsWith('em')) return n * 16;
  return n;
}

// A gradient coordinate: in bbox units percentages are fractions.
function coord(v, bbox, ref, def) {
  if (v == null || v === '') return def;
  if (typeof v === 'number') return v;
  const s = String(v).trim();
  const n = parseFloat(s);
  if (n !== n) return def;
  if (s.endsWith('%')) return bbox ? n / 100 : (n * ref) / 100;
  return n;
}

const I6 = [1, 0, 0, 1, 0, 0];
function mul(a, b) {
  return [
    a[0] * b[0] + a[2] * b[1],
    a[1] * b[0] + a[3] * b[1],
    a[0] * b[2] + a[2] * b[3],
    a[1] * b[2] + a[3] * b[3],
    a[0] * b[4] + a[2] * b[5] + a[4],
    a[1] * b[4] + a[3] * b[5] + a[5],
  ];
}
const DEG = Math.PI / 180;
const translate = (x, y) => [1, 0, 0, 1, x, y];
const scale = (x, y) => [x, 0, 0, y, 0, 0];
const rotate = (deg) => [Math.cos(deg * DEG), Math.sin(deg * DEG), -Math.sin(deg * DEG), Math.cos(deg * DEG), 0, 0];

/** Parses an SVG transform list ("translate(10 20) rotate(45 5 5)") into a matrix. */
export function parseTransformString(s) {
  let m = I6;
  const re = /(matrix|translate|scale|rotate|skewX|skewY)\s*\(([^)]*)\)/g;
  let t;
  while ((t = re.exec(s))) {
    const a = numList(t[2]);
    switch (t[1]) {
      case 'matrix':
        if (a.length === 6) m = mul(m, a);
        break;
      case 'translate':
        m = mul(m, translate(a[0] || 0, a[1] || 0));
        break;
      case 'scale':
        m = mul(m, scale(a[0] == null ? 1 : a[0], a[1] == null ? (a[0] == null ? 1 : a[0]) : a[1]));
        break;
      case 'rotate':
        if (a.length >= 3) m = mul(mul(mul(m, translate(a[1], a[2])), rotate(a[0])), translate(-a[1], -a[2]));
        else m = mul(m, rotate(a[0] || 0));
        break;
      case 'skewX':
        m = mul(m, [1, 0, Math.tan((a[0] || 0) * DEG), 1, 0, 0]);
        break;
      case 'skewY':
        m = mul(m, [1, Math.tan((a[0] || 0) * DEG), 0, 1, 0, 0]);
        break;
    }
  }
  return m;
}

function isIdentity(m) {
  return m[0] === 1 && m[1] === 0 && m[2] === 0 && m[3] === 1 && m[4] === 0 && m[5] === 0;
}

// react-native-svg's transform props (x / y on G and Use, translate, scale,
// rotation, origin, skew) then the `transform` prop.
function transformOf(p, out, translateXY) {
  let m = I6;
  const tx = (translateXY ? +(len(p.x, 0, 0) || 0) : 0) + +(p.translateX || 0);
  const ty = (translateXY ? +(len(p.y, 0, 0) || 0) : 0) + +(p.translateY || 0);
  let t = p.translate;
  if (t != null) {
    const a = typeof t === 'object' && !Array.isArray(t) ? [t.x, t.y] : numList(t);
    m = mul(m, translate(+(a[0] || 0), +(a[1] == null ? 0 : a[1])));
  }
  if (tx || ty) m = mul(m, translate(tx, ty));
  const o = p.origin != null ? numList(p.origin) : [];
  const ox = p.originX != null ? +p.originX : o[0] || 0;
  const oy = p.originY != null ? +p.originY : o.length > 1 ? o[1] : o[0] || 0;
  let sx = 1, sy = 1;
  if (p.scale != null) {
    const s = numList(p.scale);
    sx = s[0] == null ? 1 : s[0];
    sy = s.length > 1 ? s[1] : sx;
  }
  if (p.scaleX != null) sx = +p.scaleX;
  if (p.scaleY != null) sy = +p.scaleY;
  const rot = p.rotation != null ? +p.rotation : p.rotate != null && typeof p.rotate !== 'object' ? parseFloat(p.rotate) : 0;
  const kx = p.skewX != null ? +p.skewX : 0, ky = p.skewY != null ? +p.skewY : 0;
  if (rot || sx !== 1 || sy !== 1 || kx || ky) {
    m = mul(m, translate(ox, oy));
    if (rot) m = mul(m, rotate(rot));
    if (sx !== 1 || sy !== 1) m = mul(m, scale(sx, sy));
    if (kx || ky) m = mul(m, [1, Math.tan(ky * DEG), Math.tan(kx * DEG), 1, 0, 0]);
    m = mul(m, translate(-ox, -oy));
  }
  const tr = p.transform;
  if (typeof tr === 'string') m = mul(m, parseTransformString(tr));
  else if (Array.isArray(tr) && tr.length === 6 && typeof tr[0] === 'number') m = mul(m, tr);
  else if (Array.isArray(tr)) {
    out.transform = tr;  // RN-style (degrees), drawn natively about `origin`
    if (ox || oy) out.origin = [ox, oy];
  } else if (tr && typeof tr === 'object') {
    m = mul(m, transformOf(tr, {}, true) || I6);
  }
  if (!isIdentity(m)) out.m = m;
  return m;
}

// --- serialization -------------------------------------------------------------

// Presentation props passed through to native as they are (numbers, strings).
const PASS = [
  'fillOpacity', 'strokeOpacity', 'strokeLinecap', 'strokeLinejoin', 'strokeMiterlimit', 'strokeDashoffset',
  'fillRule', 'clipRule', 'fontFamily', 'fontWeight', 'fontStyle', 'textAnchor', 'display', 'visibility',
];
const NUM = { fillOpacity: 1, strokeOpacity: 1, strokeMiterlimit: 1, strokeDashoffset: 1 };

function camelStyle(p) {
  // react-native-svg also reads presentation props from `style`.
  const st = flatten(p.style);
  if (!st) return p;
  let out = null;
  for (const k in st) {
    if (k.startsWith('draw:') || p[k] !== undefined) continue;
    if (!out) out = Object.assign({}, p);
    out[k] = st[k];
  }
  return out || p;
}

function urlId(v) {
  const m = /^\s*url\(\s*['"]?#([^'")\s]+)['"]?\s*\)\s*(.*)$/.exec(v);
  return m ? { id: m[1], fallback: m[2] } : null;
}

function paintServer(v, ctx, out, name) {
  if (v == null) return;
  if (typeof v === 'string') {
    const u = v.indexOf('url(') >= 0 ? urlId(v) : null;
    if (u) {
      const ref = ctx.ids.get(u.id);
      const g = ref ? gradient(ref, ctx, 0) : null;
      if (g) {
        if (u.fallback) g.fallback = u.fallback;
        out[name] = g;
      } else {
        out[name] = u.fallback || 'none';
      }
      return;
    }
  }
  if (typeof v === 'object' && !Array.isArray(v)) return;  // (an Animated value is a style key)
  out[name] = v;
}

function presentation(p, ctx, out) {
  paintServer(p.fill, ctx, out, 'fill');
  paintServer(p.stroke, ctx, out, 'stroke');
  if (p.color != null) out.color = p.color;
  if (p.strokeWidth != null) out.strokeWidth = len(p.strokeWidth, ctx.diag, 1);
  if (p.opacity != null) out.opacity = +p.opacity;
  if (p.fontSize != null) out.fontSize = len(p.fontSize, 16, 16);
  if (p.letterSpacing != null) out.letterSpacing = len(p.letterSpacing, 0, 0);
  if (p.strokeDasharray != null) {
    const d = p.strokeDasharray === 'none' ? [] : numList(p.strokeDasharray);
    out.strokeDasharray = d;
  }
  for (const k of PASS) {
    const v = p[k];
    if (v != null && typeof v !== 'object') out[k] = NUM[k] === 1 ? +v : v;
  }
  const font = p.font;
  if (font && typeof font === 'object') {
    if (font.fontSize != null && out.fontSize == null) out.fontSize = len(font.fontSize, 16, 16);
    if (font.fontFamily != null && out.fontFamily == null) out.fontFamily = font.fontFamily;
    if (font.fontWeight != null && out.fontWeight == null) out.fontWeight = font.fontWeight;
    if (font.fontStyle != null && out.fontStyle == null) out.fontStyle = font.fontStyle;
  }
}

function collectIds(nodes, ids) {
  for (const n of nodes) {
    if (!(n instanceof DrawNode)) continue;
    if (n.props.id != null && !ids.has(String(n.props.id))) ids.set(String(n.props.id), n);
    collectIds(n.children, ids);
  }
}

function hrefOf(p) {
  const h = p.href != null ? p.href : p.xlinkHref != null ? p.xlinkHref : p['xlink:href'];
  return typeof h === 'string' && h[0] === '#' ? h.slice(1) : null;
}

function gradient(node, ctx, depth) {
  const n = node.name;
  if ((n !== 'LinearGradient' && n !== 'RadialGradient') || depth > 8) return null;
  // xlink:href: attributes and stops inherited from another gradient.
  const href = hrefOf(node.props);
  const base = href && ctx.ids.has(href) ? ctx.ids.get(href) : null;
  const pick = (k) => (node.props[k] != null ? node.props[k] : base ? base.props[k] : undefined);
  const user = pick('gradientUnits') === 'userSpaceOnUse';
  const out = { t: n === 'LinearGradient' ? 'linear' : 'radial' };
  if (!user) out.units = 'bbox';
  if (n === 'LinearGradient') {
    out.x1 = coord(pick('x1'), !user, ctx.vw, 0);
    out.y1 = coord(pick('y1'), !user, ctx.vh, 0);
    out.x2 = coord(pick('x2'), !user, ctx.vw, user ? ctx.vw : 1);
    out.y2 = coord(pick('y2'), !user, ctx.vh, 0);
  } else {
    out.cx = coord(pick('cx'), !user, ctx.vw, user ? ctx.vw / 2 : 0.5);
    out.cy = coord(pick('cy'), !user, ctx.vh, user ? ctx.vh / 2 : 0.5);
    out.r = coord(pick('r'), !user, ctx.diag, user ? ctx.diag / 2 : 0.5);
    out.fx = coord(pick('fx'), !user, ctx.vw, out.cx);
    out.fy = coord(pick('fy'), !user, ctx.vh, out.cy);
    const fr = coord(pick('fr'), !user, ctx.diag, 0);
    if (fr) out.fr = fr;
  }
  const gt = pick('gradientTransform');
  if (gt != null) {
    const m = typeof gt === 'string' ? parseTransformString(gt) : Array.isArray(gt) && gt.length === 6 ? gt : null;
    if (m && !isIdentity(m)) out.m = m;
    else if (Array.isArray(gt)) out.transform = gt;
  }
  const spread = pick('spreadMethod');
  if (spread) out.spread = spread;
  let stopNodes = visibleChildren(node).filter((c) => c instanceof DrawNode && c.name === 'Stop');
  if (!stopNodes.length && base) {
    const b = gradient(base, ctx, depth + 1);
    if (b) out.stops = b.stops;
  } else {
    out.stops = stopNodes.map((s) => {
      const p = camelStyle(s.props);
      const off = p.offset == null ? 0 : typeof p.offset === 'string' && p.offset.trim().endsWith('%') ? parseFloat(p.offset) / 100 : +p.offset;
      const color = p.stopColor != null ? p.stopColor : 'black';
      const op = p.stopOpacity != null ? +p.stopOpacity : 1;
      return [off === off ? off : 0, color, op];
    });
  }
  return out;
}

const SKIP_DRAW = {
  Defs: 1, LinearGradient: 1, RadialGradient: 1, Stop: 1, ClipPath: 1, Mask: 1, Symbol: 1, Pattern: 1, Marker: 1,
  Filter: 1, FeBlend: 1, FeColorMatrix: 1, FeComposite: 1, FeDropShadow: 1, FeFlood: 1, FeGaussianBlur: 1,
  FeMerge: 1, FeMergeNode: 1, FeOffset: 1, Title: 1, Desc: 1, Metadata: 1, Style: 1,
};

function elems(nodes, ctx) {
  const out = [];
  for (const n of nodes) {
    if (!(n instanceof DrawNode) || n.hidden) continue;
    const e = elem(n, ctx);
    if (e) out.push(e);
  }
  return out;
}

function viewBoxOf(v) {
  const a = numList(v);
  return a.length === 4 && a[2] > 0 && a[3] > 0 ? a : null;
}

function aspectOf(par, out) {
  const s = String(par || 'xMidYMid meet').trim().split(/\s+/);
  if (s[0] && s[0] !== 'xMidYMid') out.align = s[0];
  if (s[1] === 'slice') out.slice = true;
}

// The viewBox matrix of a nested <Svg> / <Symbol> drawn in a w x h viewport.
function viewBoxMatrix(vb, par, w, h) {
  const o = {};
  aspectOf(par, o);
  const sx = w / vb[2], sy = h / vb[3];
  if (o.align === 'none') return mul(scale(sx, sy), translate(-vb[0], -vb[1]));
  const s = o.slice ? Math.max(sx, sy) : Math.min(sx, sy);
  const al = o.align || 'xMidYMid';
  const ax = al.slice(1, 4) === 'Min' ? 0 : al.slice(1, 4) === 'Max' ? 1 : 0.5;
  const ay = al.slice(5, 8) === 'Min' ? 0 : al.slice(5, 8) === 'Max' ? 1 : 0.5;
  return mul(mul(translate((w - vb[2] * s) * ax, (h - vb[3] * s) * ay), scale(s, s)), translate(-vb[0], -vb[1]));
}

function clipOf(v, ctx) {
  const u = typeof v === 'string' ? urlId(v) : null;
  const ref = u && ctx.ids.get(u.id);
  if (!ref || (ref.name !== 'ClipPath' && ref.name !== 'Mask') || ctx.depth > 8) return null;
  ctx.depth++;
  const out = { c: elems(visibleChildren(ref), ctx) };
  ctx.depth--;
  return { ref, out };
}

function textElem(n, p, ctx, out) {
  out.t = 'text';
  const first = (v) => {
    const a = numList(v);
    return a.length ? a[0] : undefined;
  };
  if (p.x != null) out.x = len(Array.isArray(p.x) ? p.x[0] : first(p.x), ctx.vw, 0);
  if (p.y != null) out.y = len(Array.isArray(p.y) ? p.y[0] : first(p.y), ctx.vh, 0);
  if (p.dx != null) out.dx = first(p.dx);
  if (p.dy != null) out.dy = first(p.dy);
  // Own text, then runs: TSpans and the strings after them.
  let own = '';
  const runs = [];
  let seenSpan = false;
  const addSpan = (s) => {
    const r = { text: '' };
    const sp = camelStyle(s.props);
    presentation(sp, ctx, r);
    if (sp.x != null) r.x = len(first(sp.x), ctx.vw, 0);
    if (sp.y != null) r.y = len(first(sp.y), ctx.vh, 0);
    if (sp.dx != null) r.dx = first(sp.dx);
    if (sp.dy != null) r.dy = first(sp.dy);
    let t = '';
    for (const c of s.children) {
      if (c.hidden) continue;
      if (c instanceof DrawText) t += c.text;
      else if (c instanceof DrawNode) t += textOf(c);
    }
    r.text = t;
    runs.push(r);
  };
  for (const c of n.children) {
    if (c.hidden) continue;
    if (c instanceof DrawText) {
      if (!seenSpan) own += c.text;
      else runs.push({ text: c.text });
    } else if (c instanceof DrawNode && (c.name === 'TSpan' || c.name === 'TextPath')) {
      seenSpan = true;
      addSpan(c);
    }
  }
  if (p.children != null && typeof p.children !== 'object' && !n.children.length) own = String(p.children);
  // XML whitespace handling: runs of white space collapse, the ends trim.
  const collapse = (s) => s.replace(/\s+/g, ' ');
  own = collapse(own);
  for (const r of runs) r.text = collapse(r.text);
  if (!runs.length) own = own.trim();
  else {
    own = own.replace(/^ /, '');
    const last = runs[runs.length - 1];
    last.text = last.text.replace(/ $/, '');
  }
  out.text = own;
  if (runs.length) out.runs = runs;
}

function elem(n, ctx) {
  const name = n.name;
  if (SKIP_DRAW[name] === 1) return null;
  if (name === 'ForeignObject') {
    warnOnce('svg-foreignobject', '[jsui] react-native-svg: <ForeignObject> is not supported (not drawn)');
    return null;
  }
  const p = camelStyle(n.props);
  const out = {};
  const k = animKey(n, ctx.anim);
  if (k) out.k = k;
  presentation(p, ctx, out);
  const vw = ctx.vw, vh = ctx.vh;
  switch (name) {
    case 'Path':
      out.t = 'path';
      if (p.d != null) out.d = String(p.d);
      transformOf(p, out, false);
      break;
    case 'Rect':
      out.t = 'rect';
      out.x = len(p.x, vw, 0);
      out.y = len(p.y, vh, 0);
      if (p.width != null) out.width = len(p.width, vw, 0);
      if (p.height != null) out.height = len(p.height, vh, 0);
      if (p.rx != null) out.rx = len(p.rx, vw, 0);
      if (p.ry != null) out.ry = len(p.ry, vh, 0);
      transformOf(p, out, false);
      break;
    case 'Circle':
      out.t = 'circle';
      out.cx = len(p.cx, vw, 0);
      out.cy = len(p.cy, vh, 0);
      if (p.r != null) out.r = len(p.r, ctx.diag, 0);
      transformOf(p, out, false);
      break;
    case 'Ellipse':
      out.t = 'ellipse';
      out.cx = len(p.cx, vw, 0);
      out.cy = len(p.cy, vh, 0);
      if (p.rx != null) out.rx = len(p.rx, vw, 0);
      if (p.ry != null) out.ry = len(p.ry, vh, 0);
      transformOf(p, out, false);
      break;
    case 'Line':
      out.t = 'line';
      out.x1 = len(p.x1, vw, 0);
      out.y1 = len(p.y1, vh, 0);
      out.x2 = len(p.x2, vw, 0);
      out.y2 = len(p.y2, vh, 0);
      transformOf(p, out, false);
      break;
    case 'Polyline':
    case 'Polygon':
      out.t = 'poly';
      out.points = numList(p.points);
      if (name === 'Polygon') out.closed = true;
      transformOf(p, out, false);
      break;
    case 'Text':
    case 'TSpan':
    case 'TextPath':
      textElem(n, p, ctx, out);
      transformOf(p, out, false);
      break;
    case 'Image': {
      out.t = 'image';
      const src = p.href != null ? p.href : p.xlinkHref != null ? p.xlinkHref : p.source;
      const a = resolveAssetSource(src);
      if (a && a.uri) out.href = String(a.uri);
      out.x = len(p.x, vw, 0);
      out.y = len(p.y, vh, 0);
      if (p.width != null) out.width = len(p.width, vw, 0);
      if (p.height != null) out.height = len(p.height, vh, 0);
      const asp = {};
      aspectOf(p.preserveAspectRatio, asp);
      out.fit = asp.align === 'none' ? 'fill' : asp.slice ? 'cover' : 'contain';
      const al = asp.align || 'xMidYMid';
      out.alignX = al.slice(1, 4) === 'Min' ? 0 : al.slice(1, 4) === 'Max' ? 1 : 0.5;
      out.alignY = al.slice(5, 8) === 'Min' ? 0 : al.slice(5, 8) === 'Max' ? 1 : 0.5;
      transformOf(p, out, false);
      break;
    }
    case 'Use': {
      out.t = 'g';
      transformOf(p, out, true);
      const id = hrefOf(p);
      const ref = id != null ? ctx.ids.get(id) : null;
      if (!ref || ctx.depth > 8) return out;
      ctx.depth++;
      if (ref.name === 'Symbol') {
        const rp = ref.props;
        const vb = viewBoxOf(rp.viewBox);
        const g = { t: 'g', c: elems(visibleChildren(ref), ctx) };
        presentation(camelStyle(rp), ctx, g);
        if (vb) {
          const w = len(p.width, vw, vb[2]), h = len(p.height, vh, vb[3]);
          g.m = viewBoxMatrix(vb, rp.preserveAspectRatio, w, h);
        }
        out.c = [g];
      } else {
        const e = elem(ref, ctx);
        out.c = e ? [e] : [];
      }
      ctx.depth--;
      break;
    }
    case 'Svg': {
      // A nested <Svg>: a group placed at x / y, its viewBox into its size.
      out.t = 'g';
      const x = len(p.x, vw, 0), y = len(p.y, vh, 0);
      const vb = viewBoxOf(p.viewBox);
      let m = translate(x, y);
      if (vb) m = mul(m, viewBoxMatrix(vb, p.preserveAspectRatio, len(p.width, vw, vw), len(p.height, vh, vh)));
      if (!isIdentity(m)) out.m = m;
      out.c = elems(visibleChildren(n), ctx);
      break;
    }
    default:
      // G (and unknown elements: their children are drawn).
      out.t = 'g';
      transformOf(p, out, true);
      out.c = elems(visibleChildren(n), ctx);
      break;
  }
  if (p.clipPath != null) {
    const c = clipOf(p.clipPath, ctx);
    if (c && c.ref.name === 'ClipPath') {
      const cp = c.ref.props;
      if (cp.clipPathUnits === 'objectBoundingBox') c.out.units = 'bbox';
      const tm = {};
      transformOf(cp, tm, false);
      if (tm.m) c.out.m = tm.m;
      out.clip = c.out;
    }
  }
  if (p.mask != null) {
    const c = clipOf(p.mask, ctx);
    if (c && c.ref.name === 'Mask') {
      if (c.ref.props.maskContentUnits === 'objectBoundingBox') c.out.units = 'bbox';
      out.mask = c.out;
    }
  }
  return out;
}

registerSerializer('svg', (owner, anim) => {
  const rp = owner.reactProps.root || {};
  const p = camelStyle(rp);
  const vb = viewBoxOf(p.viewBox);
  const w = typeof p.width === 'number' ? p.width : parseFloat(p.width);
  const h = typeof p.height === 'number' ? p.height : parseFloat(p.height);
  const vw = vb ? vb[2] : w === w && !String(p.width).endsWith('%') ? w : 100;
  const vh = vb ? vb[3] : h === h && !String(p.height).endsWith('%') ? h : 100;
  const ctx = { ids: new Map(), anim, vw, vh, diag: Math.sqrt((vw * vw + vh * vh) / 2), depth: 0 };
  collectIds(owner.children, ctx.ids);
  const out = {};
  presentation(p, ctx, out);
  delete out.opacity;  // the view's own style opacity
  if (vb) {
    out.vb = vb;
    aspectOf(p.preserveAspectRatio, out);
  }
  out.c = elems(visibleChildren(owner), ctx);
  return out;
});

// --- SvgXml / SvgCss / SvgUri ---------------------------------------------------

const TAGS = {
  svg: 'Svg', g: G, path: Path, rect: Rect, circle: Circle, ellipse: Ellipse, line: Line, polyline: Polyline,
  polygon: Polygon, text: Text, tspan: TSpan, textPath: TextPath, image: Image, defs: Defs,
  linearGradient: LinearGradient, radialGradient: RadialGradient, stop: Stop, clipPath: ClipPath, mask: Mask,
  use: Use, symbol: Symbol, pattern: Pattern, marker: Marker, foreignObject: ForeignObject, a: G, switch: G,
};

const ENTITIES = { amp: '&', lt: '<', gt: '>', quot: '"', apos: "'", nbsp: ' ' };
function decode(s) {
  return s.replace(/&(#x[0-9a-fA-F]+|#\d+|\w+);/g, (m, e) => {
    if (e[0] === '#') return String.fromCodePoint(e[1] === 'x' ? parseInt(e.slice(2), 16) : parseInt(e.slice(1), 10));
    return ENTITIES[e] != null ? ENTITIES[e] : m;
  });
}

/** Parses SVG markup into {tag, attrs, children} (strings for text). Throws on malformed markup. */
export function parse(xml) {
  const src = String(xml);
  const root = { tag: '#root', attrs: {}, children: [] };
  const stack = [root];
  const re = /<!--[\s\S]*?-->|<!\[CDATA\[([\s\S]*?)\]\]>|<\?[\s\S]*?\?>|<!DOCTYPE[^>[]*(?:\[[\s\S]*?\])?\s*>|<\/\s*([\w:.-]+)\s*>|<([\w:.-]+)((?:\s+[\w:.-]+(?:\s*=\s*(?:"[^"]*"|'[^']*'|[^\s>]+))?)*)\s*(\/?)>|([^<]+)/g;
  let m;
  while ((m = re.exec(src))) {
    const top = stack[stack.length - 1];
    if (m[1] != null) top.children.push(m[1]);
    else if (m[2] != null) {
      if (stack.length > 1) stack.pop();
    } else if (m[3] != null) {
      const attrs = {};
      const ar = /([\w:.-]+)(?:\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+)))?/g;
      let a;
      while ((a = ar.exec(m[4]))) attrs[a[1]] = decode(a[2] != null ? a[2] : a[3] != null ? a[3] : a[4] != null ? a[4] : '');
      const el = { tag: m[3], attrs, children: [] };
      top.children.push(el);
      if (!m[5]) stack.push(el);
    } else if (m[6] != null) {
      top.children.push(decode(m[6]));
    }
  }
  const svg = findSvg(root);
  if (!svg) throw new Error('SvgXml: no <svg> element');
  return svg;
}

function findSvg(n) {
  for (const c of n.children) {
    if (typeof c === 'string') continue;
    if (c.tag === 'svg' || c.tag.endsWith(':svg')) return c;
    const f = findSvg(c);
    if (f) return f;
  }
  return null;
}

function camel(name) {
  if (name === 'xlink:href') return 'href';
  if (name === 'class') return 'className';
  const n = name.replace(/^xml:/, '').replace(/^xlink:/, '');
  return n.replace(/[-:]([a-z])/g, (_m, c) => c.toUpperCase());
}

function styleDecls(s, out) {
  for (const decl of String(s).split(';')) {
    const i = decl.indexOf(':');
    if (i < 0) continue;
    const k = decl.slice(0, i).trim();
    const v = decl.slice(i + 1).replace(/!important/, '').trim();
    if (k && v) out[camel(k)] = v;
  }
}

// SvgCss: rules of <style> elements with simple selectors (tag, .class,
// #id, tag.class, and lists of them), applied before inline styles.
function cssRules(n, rules) {
  for (const c of n.children) {
    if (typeof c === 'string') continue;
    if (c.tag === 'style') {
      const text = c.children.filter((x) => typeof x === 'string').join('').replace(/\/\*[\s\S]*?\*\//g, '');
      const re = /([^{}]+)\{([^}]*)\}/g;
      let m;
      while ((m = re.exec(text))) {
        const decls = {};
        styleDecls(m[2], decls);
        for (const sel of m[1].split(',')) {
          const s = sel.trim();
          if (/^[\w-]*(?:[.#][\w-]+)*$/.test(s) && s) rules.push({ sel: s, decls });
        }
      }
    } else {
      cssRules(c, rules);
    }
  }
}

function matches(sel, el) {
  const m = /^([\w-]*)((?:[.#][\w-]+)*)$/.exec(sel);
  if (!m) return false;
  if (m[1] && m[1] !== '*' && m[1] !== el.tag) return false;
  const classes = (el.attrs.class || '').split(/\s+/);
  const parts = m[2].match(/[.#][\w-]+/g) || [];
  for (const p of parts) {
    if (p[0] === '.' && classes.indexOf(p.slice(1)) < 0) return false;
    if (p[0] === '#' && el.attrs.id !== p.slice(1)) return false;
  }
  return true;
}

function toElement(el, rules, override, key) {
  const tag = el.tag.replace(/^svg:/, '');
  const C = TAGS[tag];
  if (!C || tag === 'style' || tag === 'title' || tag === 'desc' || tag === 'metadata') return null;
  const props = { key };
  if (rules) for (const r of rules) if (matches(r.sel, el)) Object.assign(props, r.decls);
  for (const a in el.attrs) {
    if (a === 'style') continue;
    if (a === 'xmlns' || a.startsWith('xmlns:') || a === 'class' || a === 'version') continue;
    props[camel(a)] = el.attrs[a];
  }
  if (el.attrs.style) styleDecls(el.attrs.style, props);
  if (override) Object.assign(props, override);
  const kids = [];
  let i = 0;
  for (const c of el.children) {
    if (typeof c === 'string') {
      if (tag === 'text' || tag === 'tspan' || tag === 'textPath') kids.push(c);
      continue;
    }
    const e = toElement(c, rules, null, i++);
    if (e) kids.push(e);
  }
  if (C === 'Svg') return createElement(Svg, props, ...kids);
  return createElement(C, props, ...kids);
}

/** Renders SVG markup. Props override the root <svg>'s attributes (width, height, color, ...). */
export function SvgXml(props) {
  const { xml, override, onError, __css, ...rest } = props;
  let tree = null;
  try {
    tree = xml ? parse(xml) : null;
  } catch (e) {
    if (onError) onError(e);
    else console.warn('[jsui] SvgXml: ' + (e && e.message));
  }
  if (!tree) return null;
  return toElement(tree, __css ? collectRules(tree) : null, Object.assign({}, override, rest), 0);
}
SvgXml.displayName = 'SvgXml';

function collectRules(tree) {
  const rules = [];
  cssRules(tree, rules);
  return rules;
}

/** SvgXml with the markup's <style> rules applied. */
export function SvgCss(props) {
  return jsx(SvgXml, Object.assign({}, props, { __css: true }));
}
SvgCss.displayName = 'SvgCss';
export const SvgFromXml = SvgXml;

function useFetched(uri, onError) {
  const [xml, setXml] = useState(null);
  useEffect(() => {
    if (!uri) return undefined;
    let live = true;
    fetch(uri)
      .then((r) => {
        if (!r.ok) throw new Error('SvgUri: ' + r.status + ' ' + uri);
        return r.text();
      })
      .then((t) => live && setXml(t))
      .catch((e) => {
        if (onError) onError(e);
        else console.warn('[jsui] ' + (e && e.message));
      });
    return () => {
      live = false;
    };
  }, [uri]);
  return xml;
}

/** Fetches SVG markup and renders it (null until it arrives). */
export function SvgUri(props) {
  const { uri, onError, onLoad, ...rest } = props;
  const xml = useFetched(uri, onError);
  useEffect(() => {
    if (xml && onLoad) onLoad();
  }, [xml]);
  return xml ? jsx(SvgXml, Object.assign({ xml }, rest, props.__css ? { __css: true } : null)) : null;
}
SvgUri.displayName = 'SvgUri';
export function SvgCssUri(props) {
  return jsx(SvgUri, Object.assign({}, props, { __css: true }));
}
export const SvgFromUri = SvgUri;

/** An SVG asset (require('./icon.svg') with the asset loader, or a {uri}). */
export function LocalSvg(props) {
  const { asset, ...rest } = props;
  const a = resolveAssetSource(asset);
  return jsx(SvgUri, Object.assign({ uri: a && a.uri }, rest));
}
export const WithLocalSvg = LocalSvg;

export const inlineStyles = (xml) => xml;
export const camelCase = camel;

export default Svg;
