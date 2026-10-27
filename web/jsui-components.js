// Native components drawn by page JS (include/jsui/jsui.h "Native
// components"): the browser host's side of jsui_register_component. The
// page registers a class by name; the bundle uses it with
// requireNativeComponent(name) like any React Native native component; the
// class draws into the view with Skia through a small canvas API, every
// frame it needs to, without going through React:
//
//   jsui.registerComponent('Radar', {
//     clocked: true,                       // repaint every frame
//     draw(ctx, view) {                    // view: { id, width, height, pixelRatio, time, state,
//                                          //   emit(event, data), invalidate() }
//       const r = Math.min(view.width, view.height) / 2;
//       ctx.circle(r, r, r - 1, { color: '#3f6', style: 'stroke', width: 2 });
//       const a = ctx.number('sweep', view.time / 1000);  // a prop (Animated ok)
//       ctx.arc(0, 0, 2 * r, 2 * r, a * 360, 40, true, { gradient: { type: 'sweep', ... } });
//     },
//     create(view) { return { contacts: [] }; },   // optional: per-view state (view.state)
//     props(state, props, view) {},                 // optional: React changed its props
//     command(state, name, args, view) {},          // optional: ref commands
//     destroy(state, view) {},                      // optional
//   });
//   view.invalidate()  /  view.emit('contactPress', { id })   // (jsui.invalidateComponent /
//                                                              //  emitComponentEvent by view id)
//
// Colors: '#rgb', '#rgba', '#rrggbb', '#rrggbbaa', 'rgb(...)', 'rgba(...)',
// 'transparent', or a number 0xAARRGGBB. Paint: { color, style: 'fill' |
// 'stroke' | 'fillAndStroke', width, cap: 'butt' | 'round' | 'square',
// join: 'miter' | 'round' | 'bevel', blend: 'plus' | 'multiply' | 'screen' |
// 'src' | 'dstOut' | 'dstIn', blur, dash: [on, off, ...], dashPhase,
// antialias, gradient: { type: 'linear' | 'radial' | 'sweep', x0, y0, x1,
// y1, r, start, end, colors: [...], stops: [...] } }. Font: { family, size,
// weight, italic, align: 'left' | 'center' | 'right' }.

const STYLE = { fill: 0, stroke: 1, fillAndStroke: 2 };
const CAP = { butt: 0, round: 1, square: 2 };
const JOIN = { miter: 0, round: 1, bevel: 2 };
const BLEND = { srcOver: 0, normal: 0, plus: 1, multiply: 2, screen: 3, src: 4, dstOut: 5, dstIn: 6 };
const GRADIENT = { linear: 1, radial: 2, sweep: 3 };
const ALIGN = { left: 0, center: 1, right: 2 };
const NAMED = { transparent: 0, black: 0xff000000, white: 0xffffffff, red: 0xffff0000, green: 0xff008000,
  lime: 0xff00ff00, blue: 0xff0000ff, yellow: 0xffffff00, cyan: 0xff00ffff, magenta: 0xffff00ff };

const colorCache = new Map();

/** A color (see the top of this file) as 0xAARRGGBB, unsigned. */
export function toArgb(c, fallback = 0xff000000) {
  if (typeof c === 'number') return c >>> 0;
  if (typeof c !== 'string') return fallback >>> 0;
  let v = colorCache.get(c);
  if (v !== undefined) return v;
  const s = c.trim().toLowerCase();
  v = fallback >>> 0;
  if (s[0] === '#') {
    let h = s.slice(1);
    if (h.length === 3 || h.length === 4) h = [...h].map((x) => x + x).join('');
    if (h.length === 6) h += 'ff';
    if (h.length === 8 && /^[0-9a-f]+$/.test(h)) {
      const n = parseInt(h, 16) >>> 0;
      v = (((n & 0xff) << 24) | (n >>> 8)) >>> 0;  // RRGGBBAA -> AARRGGBB
    }
  } else if (s.startsWith('rgb')) {
    const p = s.slice(s.indexOf('(') + 1, s.lastIndexOf(')')).split(/[\s,/]+/).filter(Boolean).map(parseFloat);
    if (p.length >= 3) {
      const a = p.length > 3 ? Math.round(Math.max(0, Math.min(1, p[3])) * 255) : 255;
      const b = (x) => Math.max(0, Math.min(255, Math.round(x)));
      v = ((a << 24) | (b(p[0]) << 16) | (b(p[1]) << 8) | b(p[2])) >>> 0;
    }
  } else if (NAMED[s] !== undefined) {
    v = NAMED[s] >>> 0;
  }
  colorCache.set(c, v);
  return v;
}

// Struct layouts (wasm32): JsuiPaint 48 bytes, JsuiGradient 44, JsuiFont 20,
// JsuiDrawInfo 24 (time_ms a double at 16).
const PAINT = 0, GRAD = 48, FONT = 96, COLORS = 128, STOPS = 384, DASH = 640, STR = 768;
const SCRATCH = 2048;
const MAX_STOPS = 64, MAX_DASH = 32;

/** The canvas handed to draw(): Skia through the jsui_canvas_* C API. Valid during draw only. */
class ComponentCanvas {
  constructor(M) {
    this.M = M;
    this.c = 0;  // JsuiCanvas* during draw
    this.mem = M._malloc(SCRATCH);
    this.paths = new Map();  // SVG path data -> JsuiPath*
  }

  free() {
    for (const p of this.paths.values()) this.M._jsui_path_destroy(p);
    this.paths.clear();
    this.M._free(this.mem);
    this.mem = 0;
  }

  // ---- strings in scratch memory (or a temporary allocation)
  _str(s, at = STR, end = STR + 512) {
    const M = this.M;
    const n = M.lengthBytesUTF8(s) + 1;
    if (at + n <= end) {
      M.stringToUTF8(s, this.mem + at, n);
      return [this.mem + at, 0];
    }
    const p = M._malloc(n);
    M.stringToUTF8(s, p, n);
    return [p, p];
  }
  _withStr(s, fn) {
    const [p, owned] = this._str(String(s));
    try { return fn(p); } finally { if (owned) this.M._free(owned); }
  }

  // Writes `paint` into the scratch JsuiPaint; returns its address.
  _paint(paint) {
    const M = this.M, base = this.mem;
    const v = new DataView(M.HEAPU8.buffer, base, SCRATCH);
    const p = paint || {};
    v.setUint32(PAINT + 0, toArgb(p.color, 0xff000000), true);
    v.setInt32(PAINT + 4, STYLE[p.style] ?? 0, true);
    v.setFloat32(PAINT + 8, p.width ?? p.strokeWidth ?? 1, true);
    v.setInt32(PAINT + 12, CAP[p.cap] ?? 0, true);
    v.setInt32(PAINT + 16, JOIN[p.join] ?? 0, true);
    v.setInt32(PAINT + 20, BLEND[p.blend] ?? 0, true);
    v.setInt32(PAINT + 24, p.antialias === false ? 0 : 1, true);
    v.setFloat32(PAINT + 28, p.blur ?? 0, true);
    const dash = Array.isArray(p.dash) ? p.dash.slice(0, MAX_DASH) : null;
    if (dash && dash.length >= 2) {
      for (let i = 0; i < dash.length; i++) v.setFloat32(DASH + 4 * i, dash[i], true);
      v.setUint32(PAINT + 32, base + DASH, true);
      v.setInt32(PAINT + 36, dash.length & ~1, true);
    } else {
      v.setUint32(PAINT + 32, 0, true);
      v.setInt32(PAINT + 36, 0, true);
    }
    v.setFloat32(PAINT + 40, p.dashPhase ?? 0, true);
    const g = p.gradient;
    if (g && Array.isArray(g.colors) && g.colors.length >= 2) {
      const n = Math.min(g.colors.length, MAX_STOPS);
      v.setInt32(GRAD + 0, GRADIENT[g.type] ?? 1, true);
      v.setFloat32(GRAD + 4, g.x0 ?? 0, true);
      v.setFloat32(GRAD + 8, g.y0 ?? 0, true);
      v.setFloat32(GRAD + 12, g.x1 ?? 0, true);
      v.setFloat32(GRAD + 16, g.y1 ?? 0, true);
      v.setFloat32(GRAD + 20, g.r ?? 0, true);
      v.setFloat32(GRAD + 24, g.start ?? 0, true);
      v.setFloat32(GRAD + 28, g.end ?? 360, true);
      for (let i = 0; i < n; i++) v.setUint32(COLORS + 4 * i, toArgb(g.colors[i]), true);
      const stops = Array.isArray(g.stops) && g.stops.length >= n;
      if (stops) for (let i = 0; i < n; i++) v.setFloat32(STOPS + 4 * i, g.stops[i], true);
      v.setUint32(GRAD + 32, base + COLORS, true);
      v.setUint32(GRAD + 36, stops ? base + STOPS : 0, true);
      v.setInt32(GRAD + 40, n, true);
      v.setUint32(PAINT + 44, base + GRAD, true);
      if (p.color === undefined) v.setUint32(PAINT + 0, 0xff000000, true);
    } else {
      v.setUint32(PAINT + 44, 0, true);
    }
    return base + PAINT;
  }

  // Writes `font` into the scratch JsuiFont (its family string at STR + 512).
  _font(font, owned) {
    const M = this.M, base = this.mem;
    const f = font || {};
    let fam = 0;
    if (f.family) {
      const [p, o] = this._str(String(f.family), STR + 512, SCRATCH);
      fam = p;
      if (o) owned.push(o);
    }
    const v = new DataView(M.HEAPU8.buffer, base, SCRATCH);
    v.setUint32(FONT + 0, fam, true);
    v.setFloat32(FONT + 4, f.size ?? 14, true);
    v.setInt32(FONT + 8, f.weight ?? 400, true);
    v.setInt32(FONT + 12, f.italic ? 1 : 0, true);
    v.setInt32(FONT + 16, ALIGN[f.align] ?? 0, true);
    return base + FONT;
  }

  // ---- props (the view's current props; Animated ones at their current value)
  /** A prop's value (parsed), or `fallback` when missing. */
  prop(name, fallback) {
    const p = this._withStr(name, (n) => this.M._jsui_prop_json(this.c, n));
    return p ? JSON.parse(this.M.UTF8ToString(p)) : fallback;
  }
  number(name, fallback = 0) { return this._withStr(name, (n) => this.M._jsui_prop_number(this.c, n, fallback)); }
  color(name, fallback = 0xff000000) {
    return this._withStr(name, (n) => this.M._jsui_prop_color(this.c, n, toArgb(fallback))) >>> 0;
  }

  // ---- state
  save() { this.M._jsui_canvas_save(this.c); }
  saveLayer(alpha) { this.M._jsui_canvas_save_layer(this.c, alpha); }
  restore() { this.M._jsui_canvas_restore(this.c); }
  translate(x, y) { this.M._jsui_canvas_translate(this.c, x, y); }
  scale(x, y = x) { this.M._jsui_canvas_scale(this.c, x, y); }
  rotate(degrees) { this.M._jsui_canvas_rotate(this.c, degrees); }
  clipRect(x, y, w, h) { this.M._jsui_canvas_clip_rect(this.c, x, y, w, h); }
  clipRRect(x, y, w, h, r) { this.M._jsui_canvas_clip_rrect(this.c, x, y, w, h, r); }
  clipPath(d) { const p = this._path(d); if (p) this.M._jsui_canvas_clip_path(this.c, p); }

  // ---- shapes
  clear(color) { this.M._jsui_canvas_clear(this.c, toArgb(color, 0)); }
  rect(x, y, w, h, paint) { this.M._jsui_canvas_draw_rect(this.c, x, y, w, h, this._paint(paint)); }
  rrect(x, y, w, h, r, paint) { this.M._jsui_canvas_draw_rrect(this.c, x, y, w, h, r, this._paint(paint)); }
  circle(cx, cy, r, paint) { this.M._jsui_canvas_draw_circle(this.c, cx, cy, r, this._paint(paint)); }
  oval(x, y, w, h, paint) { this.M._jsui_canvas_draw_oval(this.c, x, y, w, h, this._paint(paint)); }
  line(x0, y0, x1, y1, paint) { this.M._jsui_canvas_draw_line(this.c, x0, y0, x1, y1, this._paint(paint)); }
  /** An arc of the oval (x, y, w, h): `start` and `sweep` in degrees, clockwise from +x. */
  arc(x, y, w, h, start, sweep, useCenter, paint) {
    this.M._jsui_canvas_draw_arc(this.c, x, y, w, h, start, sweep, useCenter ? 1 : 0, this._paint(paint));
  }
  /** SVG path data ("M0 0 L10 10 Z"); parsed once and cached. */
  path(d, paint) { const p = this._path(d); if (p) this.M._jsui_canvas_draw_path(this.c, p, this._paint(paint)); }
  /** A polyline through [x0, y0, x1, y1, ...]. */
  polyline(points, closed, paint) {
    const M = this.M, n = points.length >> 1;
    if (n < 2) return;
    const p = M._jsui_path_create();
    M._jsui_path_move_to(p, points[0], points[1]);
    for (let i = 1; i < n; i++) M._jsui_path_line_to(p, points[2 * i], points[2 * i + 1]);
    if (closed) M._jsui_path_close(p);
    M._jsui_canvas_draw_path(this.c, p, this._paint(paint));
    M._jsui_path_destroy(p);
  }
  /** Text at baseline y; returns its width. */
  text(str, x, y, font, paint) {
    const owned = [];
    try {
      const f = this._font(font, owned);
      const pp = this._paint(paint);
      return this._withStr(str, (s) => this.M._jsui_canvas_draw_text(this.c, s, x, y, f, pp));
    } finally { for (const o of owned) this.M._free(o); }
  }
  measureText(str, font) {
    const owned = [];
    try {
      const f = this._font(font, owned);
      return this._withStr(str, (s) => this.M._jsui_canvas_measure_text(this.c, s, f));
    } finally { for (const o of owned) this.M._free(o); }
  }
  /** An image URL (like Image's uri); false while it loads (the view repaints when it arrives). */
  image(uri, x, y, w, h, alpha = 1) {
    return !!this._withStr(uri, (u) => this.M._jsui_canvas_draw_image(this.c, u, x, y, w, h, alpha));
  }
  /** A host texture (setTexture name); new frames repaint the view. */
  texture(name, x, y, w, h, alpha = 1) {
    return !!this._withStr(name, (k) => this.M._jsui_canvas_draw_texture(this.c, k, x, y, w, h, alpha));
  }

  _path(d) {
    if (typeof d !== 'string') return 0;
    let p = this.paths.get(d);
    if (p === undefined) {
      p = this._withStr(d, (s) => this.M._jsui_path_from_svg(s)) || 0;
      if (this.paths.size > 256) {  // a bounded cache
        for (const old of this.paths.values()) this.M._jsui_path_destroy(old);
        this.paths.clear();
      }
      this.paths.set(d, p);
    }
    return p;
  }
}

/** The host's registered classes (JsuiHost.registerComponent). */
export class ComponentSet {
  constructor(host) {
    this.host = host;
    this.classes = new Map();  // cls id -> { name, impl, views: Map(view -> info) }
    this.byName = new Map();   // name -> cls id
    this.nextId = 1;
    this.canvas = null;
  }

  register(name, impl) {
    if (typeof impl === 'function') impl = { draw: impl };
    if (!impl || typeof impl.draw !== 'function') throw new TypeError('registerComponent: needs a draw(ctx, view) function');
    const host = this.host, M = host.module;
    this.unregister(name);
    const id = this.nextId++;
    this.classes.set(id, { name, impl, views: new Map() });
    this.byName.set(name, id);
    const flags = impl.clocked ? 1 : 0;
    const r = host._withStrings([String(name)], (n) => M._jsui_web_register_component(host.rt, host.hostId, id, n, flags));
    return r === 1;
  }

  unregister(name) {
    const id = this.byName.get(name);
    if (id === undefined) return false;
    const host = this.host;
    host._withStrings([String(name)], (n) => host.module._jsui_unregister_component(host.rt, n));
    this.byName.delete(name);
    this.classes.delete(id);
    return true;
  }

  // jsui_web.cpp's onComponent.
  call(cls, kind, view, canvas, info, a, b) {
    const c = this.classes.get(cls);
    if (!c) return;
    const impl = c.impl;
    try {
      if (kind === 0) {
        const host = this.host;
        const v = { id: view, width: 0, height: 0, pixelRatio: 1, time: 0, state: undefined,
          emit: (event, data) => host.emitComponentEvent(view, event, data),
          invalidate: () => host.invalidateComponent(view) };
        c.views.set(view, v);
        if (impl.create) v.state = impl.create(v);
      } else if (kind === 1) {
        const v = c.views.get(view);
        c.views.delete(view);
        if (v && impl.destroy) impl.destroy(v.state, v);
      } else if (kind === 2) {
        const v = c.views.get(view);
        if (v && impl.props) impl.props(v.state, JSON.parse(a), v);
      } else if (kind === 3) {
        const v = c.views.get(view);
        if (!v) return;
        const M = this.host.module;
        const d = new DataView(M.HEAPU8.buffer, info, 24);
        v.width = d.getFloat32(4, true);
        v.height = d.getFloat32(8, true);
        v.pixelRatio = d.getFloat32(12, true);
        v.time = d.getFloat64(16, true);
        const ctx = this.canvas || (this.canvas = new ComponentCanvas(M));
        ctx.c = canvas;
        try { impl.draw(ctx, v); } finally { ctx.c = 0; }
      } else if (kind === 4) {
        const v = c.views.get(view);
        if (v && impl.command) impl.command(v.state, a, b ? JSON.parse(b) : [], v);
      }
    } catch (e) {
      console.error(`[jsui] component '${c.name}':`, e);
    }
  }

  destroy() {
    this.canvas?.free();
    this.canvas = null;
    this.classes.clear();
    this.byName.clear();
  }
}
