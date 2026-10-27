// Unit tests of web/jsui-components.js (native components drawn by page JS)
// against a mock WASM module: the class lifecycle the module's callbacks
// drive, the structs the canvas writes (JsuiPaint / JsuiGradient /
// JsuiFont), colors, prop readers. No browser or WASM build needed.
//
//   node --test web/test/components.test.mjs
import assert from 'node:assert/strict';
import test from 'node:test';
import { ComponentSet, toArgb } from '../jsui-components.js';

function mockModule() {
  const mem = new ArrayBuffer(1 << 20);
  let top = 64;
  const calls = [];
  const u8 = new Uint8Array(mem);
  const str = (p) => { let e = p; while (u8[e]) e++; return new TextDecoder().decode(u8.subarray(p, e)); };
  const M = {
    HEAPU8: u8,
    calls,
    _malloc: (n) => { const p = top; top += (n + 7) & ~7; return p; },
    _free: () => {},
    lengthBytesUTF8: (s) => new TextEncoder().encode(s).length,
    stringToUTF8: (s, p, n) => { const b = new TextEncoder().encode(s); u8.set(b.subarray(0, n - 1), p); u8[p + Math.min(b.length, n - 1)] = 0; },
    UTF8ToString: str,
    props: { range: 120, tint: '#0f0' },
    _jsui_web_register_component: (rt, host, cls, name, flags) => { calls.push(['register', str(name), cls, flags]); return 1; },
    _jsui_unregister_component: (rt, name) => { calls.push(['unregister', str(name)]); return 1; },
    _jsui_prop_json: (c, n) => {
      const v = M.props[str(n)];
      if (v === undefined) return 0;
      const p = M._malloc(256);
      M.stringToUTF8(JSON.stringify(v), p, 256);
      return p;
    },
    _jsui_prop_number: (c, n, d) => (typeof M.props[str(n)] === 'number' ? M.props[str(n)] : d),
    _jsui_canvas_draw_rect: (c, x, y, w, h, p) => calls.push(['rect', c, x, y, w, h, paintAt(p)]),
    _jsui_canvas_draw_text: (c, s, x, y, f, p) => { calls.push(['text', str(s), x, y, fontAt(f)]); return 10; },
    _jsui_canvas_save: (c) => calls.push(['save', c]),
    _jsui_canvas_restore: (c) => calls.push(['restore', c]),
  };
  function paintAt(p) {
    const v = new DataView(mem, p, 48);
    const out = { color: v.getUint32(0, true), style: v.getInt32(4, true), width: v.getFloat32(8, true),
      cap: v.getInt32(12, true), blend: v.getInt32(20, true), aa: v.getInt32(24, true), blur: v.getFloat32(28, true),
      dashCount: v.getInt32(36, true) };
    const dash = v.getUint32(32, true);
    if (dash) out.dash = Array.from(new Float32Array(mem, dash, out.dashCount));
    const g = v.getUint32(44, true);
    if (g) {
      const gv = new DataView(mem, g, 44);
      const n = gv.getInt32(40, true);
      out.gradient = { type: gv.getInt32(0, true), x0: gv.getFloat32(4, true), end: gv.getFloat32(28, true),
        colors: Array.from(new Uint32Array(mem, gv.getUint32(32, true), n)), stops: gv.getUint32(36, true) };
    }
    return out;
  }
  function fontAt(p) {
    const v = new DataView(mem, p, 20);
    const fam = v.getUint32(0, true);
    return { family: fam ? str(fam) : null, size: v.getFloat32(4, true), weight: v.getInt32(8, true), align: v.getInt32(16, true) };
  }
  M.drawInfo = (view, w, h, pr, time) => {
    const p = M._malloc(24);
    const v = new DataView(mem, p, 24);
    v.setInt32(0, view, true);
    v.setFloat32(4, w, true);
    v.setFloat32(8, h, true);
    v.setFloat32(12, pr, true);
    v.setFloat64(16, time, true);
    return p;
  };
  return M;
}

function mockHost(M) {
  return {
    module: M, rt: 1, hostId: 7,
    _withStrings(strs, fn) {
      const ptrs = strs.map((s) => {
        if (s == null) return 0;
        const p = M._malloc(M.lengthBytesUTF8(s) + 1);
        M.stringToUTF8(s, p, M.lengthBytesUTF8(s) + 1);
        return p;
      });
      return fn(...ptrs);
    },
  };
}

test('colors: hex, rgb(a), names, numbers', () => {
  assert.equal(toArgb('#f80'), 0xffff8800);
  assert.equal(toArgb('#ff880080'), 0x80ff8800);
  assert.equal(toArgb('#0f08'), 0x8800ff00);
  assert.equal(toArgb('rgba(255, 0, 0, 0.5)'), 0x80ff0000);
  assert.equal(toArgb('rgb(0 0 255)'), 0xff0000ff);
  assert.equal(toArgb('transparent'), 0);
  assert.equal(toArgb(0x12345678), 0x12345678);
  assert.equal(toArgb('nonsense', 0xff000000), 0xff000000);
});

test('lifecycle: register, create / props / draw / command / destroy, unregister', () => {
  const M = mockModule();
  const set = new ComponentSet(mockHost(M));
  const seen = [];
  assert.throws(() => set.register('Bad', {}), /draw/);
  assert.equal(set.register('Radar', {
    clocked: true,
    create: (v) => { seen.push(['create', v.id]); return { n: 0 }; },
    props: (s, p, v) => seen.push(['props', p, v.id]),
    command: (s, name, args) => seen.push(['command', name, args]),
    destroy: (s, v) => seen.push(['destroy', s.n, v.id]),
    draw(ctx, v) {
      v.state.n++;
      seen.push(['draw', v.width, v.height, v.pixelRatio, v.time, ctx.number('range'), ctx.prop('tint'), ctx.prop('none', 'x')]);
      ctx.rect(1, 2, 3, 4, { color: '#f00', style: 'stroke', width: 2, cap: 'round', blend: 'plus', blur: 3, dash: [4, 2, 1] });
      ctx.rect(0, 0, 1, 1, { gradient: { type: 'sweep', x0: 5, colors: ['#f00', '#00f'] } });
      ctx.text('hi', 5, 6, { family: 'Inter', size: 20, weight: 700, align: 'center' }, { color: 0xffffffff });
    },
  }), true);
  const cls = M.calls[0][2];
  assert.deepEqual(M.calls[0], ['register', 'Radar', cls, 1]);
  set.call(cls, 0, 42, 0, 0, null, null);
  set.call(cls, 2, 42, 0, 0, '{"range":120}', null);
  set.call(cls, 3, 42, 999, M.drawInfo(42, 100, 50, 2, 1234.5), null, null);
  set.call(cls, 4, 42, 0, 0, 'ping', '[1,"a"]');
  set.call(cls, 1, 42, 0, 0, null, null);
  set.call(cls, 3, 42, 999, M.drawInfo(42, 1, 1, 1, 0), null, null);  // destroyed: ignored
  assert.deepEqual(seen, [
    ['create', 42],
    ['props', { range: 120 }, 42],
    ['draw', 100, 50, 2, 1234.5, 120, '#0f0', 'x'],
    ['command', 'ping', [1, 'a']],
    ['destroy', 1, 42],
  ]);
  const [r1, r2] = M.calls.filter((c) => c[0] === 'rect');
  assert.equal(r1[1], 999);
  assert.deepEqual(r1[6], { color: 0xffff0000, style: 1, width: 2, cap: 1, blend: 1, aa: 1, blur: 3, dashCount: 2, dash: [4, 2] });
  assert.equal(r2[6].gradient.type, 3);
  assert.equal(r2[6].gradient.x0, 5);
  assert.equal(r2[6].gradient.end, 360);
  assert.deepEqual(r2[6].gradient.colors, [0xffff0000, 0xff0000ff]);
  assert.equal(r2[6].gradient.stops, 0);
  assert.equal(r2[6].color, 0xff000000);
  const t = M.calls.find((c) => c[0] === 'text');
  assert.deepEqual(t.slice(1), ['hi', 5, 6, { family: 'Inter', size: 20, weight: 700, align: 1 }]);
  assert.equal(set.unregister('Radar'), true);
  assert.equal(set.unregister('Radar'), false);
  assert.deepEqual(M.calls.at(-1), ['unregister', 'Radar']);
});

test('a draw that throws is logged, not fatal; a bare function is a draw', () => {
  const M = mockModule();
  const set = new ComponentSet(mockHost(M));
  set.register('F', () => { throw new Error('boom'); });
  const cls = M.calls[0][2];
  set.call(cls, 0, 1, 0, 0, null, null);
  const err = console.error;
  let logged = 0;
  console.error = () => logged++;
  try { set.call(cls, 3, 1, 5, M.drawInfo(1, 1, 1, 1, 0), null, null); } finally { console.error = err; }
  assert.equal(logged, 1);
});
