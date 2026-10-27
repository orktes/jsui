// The site's native components (/features/native): classes the page
// registers with createJsui({ components: './components.js' }) and the app
// shows with requireNativeComponent(name). In worker mode the worker imports
// this module, so it imports nothing and touches no DOM. Each draws with
// Skia through web/jsui-components.js' canvas, outside React: a game would
// read its own state here (units, map, damage numbers) every frame.

const PALETTE = ['#f2c35b', '#4fd1ff', '#b26bff', '#4ade80', '#ff9a3c', '#ff4d6a'];
const NAMES = ['Wraith', 'Lancer', 'Seraph', 'Warden', 'Vanguard', 'Specter', 'Nomad', 'Hydra', 'Corsair'];

const alpha = (hex, a) => hex + Math.round(Math.max(0, Math.min(1, a)) * 255).toString(16).padStart(2, '0');

// Contacts drifting on slow orbits: the "game state" the radar reads.
function contacts(n, seed) {
  const out = [];
  let s = seed;
  const rnd = () => ((s = (s * 16807) % 2147483647) / 2147483647);
  for (let i = 0; i < n; i++) {
    out.push({ name: NAMES[i % NAMES.length], hue: i % PALETTE.length, r: 0.25 + 0.65 * rnd(), a: rnd() * Math.PI * 2,
      speed: (rnd() - 0.5) * 0.25, wobble: rnd() * 6 });
  }
  return out;
}

function place(c, t, half) {
  const a = c.a + c.speed * t;
  const r = (half - 10) * (c.r + 0.05 * Math.sin(t * 0.7 + c.wobble));
  return [half + Math.cos(a) * r, half + Math.sin(a) * r];
}

export const Radar = {
  clocked: true,  // the sweep moves: repaint every frame
  create() { return { list: contacts(7, 7), selected: -1, t: 0 }; },
  props(state, props) {
    if (props.count != null && props.count !== state.list.length) {
      state.list = contacts(Math.max(1, Math.min(24, props.count | 0)), 7);
      state.selected = -1;
    }
  },
  // ref commands (codegenNativeCommands): select(x, y) in view px.
  command(state, name, args, view) {
    if (name !== 'select') return;
    const [px, py] = args, half = Math.min(view.width, view.height) / 2;
    let best = -1, bestD = 18 * 18;
    state.list.forEach((c, i) => {
      const [x, y] = place(c, state.t, half);
      const d = (x - px) ** 2 + (y - py) ** 2;
      if (d < bestD) { bestD = d; best = i; }
    });
    state.selected = best;
    // Answers with an event: the view's onSelect prop gets it.
    view.emit('select', best < 0 ? { index: -1 } : { index: best, name: state.list[best].name, hue: PALETTE[state.list[best].hue] });
  },
  draw(ctx, view) {
    const state = view.state, t = (state.t = view.time / 1000);
    const size = Math.min(view.width, view.height), half = size / 2, rr = half - 8;
    const accent = ctx.color('accent', '#4fd1ff');
    const hex = '#' + (accent & 0xffffff).toString(16).padStart(6, '0');
    const flash = ctx.number('flash', 0);

    ctx.circle(half, half, half - 1, { gradient: { type: 'radial', x0: half, y0: half, r: half, colors: ['#122036f0', '#060a14f0'] } });
    for (let i = 1; i <= 3; i++) ctx.circle(half, half, (rr * i) / 3, { color: alpha(hex, 0.2), style: 'stroke', width: 1 });
    ctx.line(half, 8, half, size - 8, { color: alpha(hex, 0.16), style: 'stroke', width: 1 });
    ctx.line(8, half, size - 8, half, { color: alpha(hex, 0.16), style: 'stroke', width: 1 });

    const sweep = (t * 90) % 360;
    ctx.save();
    ctx.translate(half, half);
    ctx.rotate(sweep);
    ctx.arc(-rr, -rr, 2 * rr, 2 * rr, -60, 60, true, { gradient: { type: 'sweep', start: 300, end: 360, colors: [alpha(hex, 0), alpha(hex, 0.45)] } });
    ctx.line(0, 0, rr, 0, { color: alpha(hex, 0.9), style: 'stroke', width: 1.5, cap: 'round' });
    ctx.restore();

    state.list.forEach((c, i) => {
      const [x, y] = place(c, t, half);
      const ang = (Math.atan2(y - half, x - half) * 180) / Math.PI;
      const behind = (sweep - ang + 720) % 360;
      const lit = 0.3 + 0.7 * Math.exp(-behind / 90);
      const col = PALETTE[c.hue];
      ctx.circle(x, y, 6, { color: alpha(col, 0.35 * lit), blur: 3 });
      ctx.circle(x, y, 3, { color: alpha(col, lit) });
      if (i === state.selected) {
        const pulse = 0.5 + 0.5 * Math.sin(t * 6);
        ctx.circle(x, y, 8 + 2 * pulse + 8 * flash, { color: alpha(col, 0.9 - 0.5 * flash), style: 'stroke', width: 1.5 });
        ctx.text(c.name.toUpperCase(), x + 12, y + 4, { size: 10, weight: 700 }, { color: alpha(col, 0.95) });
      }
    });

    ctx.circle(half, half, half - 1, { color: alpha(hex, 0.35 + 0.65 * flash), style: 'stroke', width: 1.5 + 2 * flash });
    ctx.text(`${state.list.length} CONTACTS`, half, size - 16, { size: 10, weight: 700, align: 'center' }, { color: alpha(hex, 0.8) });
  },
};

// A gauge with no clock: it repaints only when a prop changes, Animated ones
// included (`value` runs on the native driver, so no React render per frame).
export const Gauge = {
  draw(ctx, view) {
    const w = view.width, h = view.height, cx = w / 2, cy = h * 0.62, r = Math.min(w / 2, h * 0.62) - 10;
    const v = Math.max(0, Math.min(1, ctx.number('value', 0)));
    const from = 150, span = 240;
    // Rotated so the dial starts at 0 degrees: a sweep gradient's angles stay in 0..360.
    ctx.save();
    ctx.translate(cx, cy);
    ctx.rotate(from);
    ctx.arc(-r, -r, 2 * r, 2 * r, 0, span, false, { color: '#ffffff14', style: 'stroke', width: 12, cap: 'round' });
    ctx.arc(-r, -r, 2 * r, 2 * r, 0, Math.max(0.5, span * v), false, { style: 'stroke', width: 12, cap: 'round',
      gradient: { type: 'sweep', start: 0, end: span, colors: ['#4ade80', '#f2c35b', '#ff4d6a'] } });
    ctx.restore();
    for (let i = 0; i <= 12; i++) {
      const a = ((from + (span * i) / 12) * Math.PI) / 180, long = i % 3 === 0;
      const r0 = r - (long ? 22 : 18), r1 = r - 12;
      ctx.line(cx + Math.cos(a) * r0, cy + Math.sin(a) * r0, cx + Math.cos(a) * r1, cy + Math.sin(a) * r1,
        { color: long ? '#ffffffa0' : '#ffffff50', style: 'stroke', width: long ? 2 : 1 });
    }
    const na = ((from + span * v) * Math.PI) / 180;
    ctx.line(cx, cy, cx + Math.cos(na) * (r - 26), cy + Math.sin(na) * (r - 26), { color: '#ff4d6a', style: 'stroke', width: 3, cap: 'round' });
    ctx.circle(cx, cy, 7, { color: '#e8ecf6' });
    ctx.text(String(Math.round(v * 9000)), cx, cy + r * 0.55, { size: 26, weight: 800, align: 'center' }, { color: '#e8ecf6' });
    ctx.text(ctx.prop('label', 'RPM'), cx, cy + r * 0.55 + 18, { size: 10, weight: 700, align: 'center' }, { color: '#8b93ad' });
  },
};
