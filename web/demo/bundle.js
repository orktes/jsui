// Hand-written JSUI bundle (no React): drives the native bridge directly.
// Used as the default bundle of web/index.html; a React app built with
// `npx jsui build` is loaded the same way via ?bundle=<url>.
(function () {
  const N = globalThis.__jsui_native;
  const root = N.rootId;

  function el(type, style, props) {
    const id = N.createNode(type);
    N.setProps(id, Object.assign({ style }, props || {}));
    return id;
  }
  function text(str, style) {
    const t = el('Text', style);
    const raw = N.createNode('RawText');
    N.setText(raw, str);
    N.appendChild(t, raw);
    return t;
  }

  const screen = el('View', {
    flex: 1, backgroundColor: '#0d1117', padding: 32, gap: 24, flexDirection: 'column',
  });
  N.appendChild(root, screen);

  // Top bar: health / mana / xp bars.
  const bar = el('View', { flexDirection: 'row', gap: 16, alignItems: 'center' });
  N.appendChild(screen, bar);
  const meters = [
    ['#e5484d', 0.72], ['#3e63dd', 0.45], ['#f5d90a', 0.9],
  ].map(([color, v]) => {
    const track = el('View', { width: 220, height: 18, backgroundColor: '#21262d', borderRadius: 9, overflow: 'hidden' });
    const fill = el('View', { width: 220 * v, height: 18, backgroundColor: color, borderRadius: 9 });
    N.appendChild(track, fill);
    N.appendChild(bar, track);
    return { fill, color, v };
  });

  // Menu: focusable buttons.
  const menu = el('View', { flexDirection: 'column', gap: 12, width: 320, marginTop: 24 });
  N.appendChild(screen, menu);
  const labels = ['Continue', 'New Game', 'Settings', 'Quit'];
  const buttons = labels.map((label, i) => {
    const b = el('View', {
      height: 52, borderRadius: 10, backgroundColor: i === 0 ? '#238636' : '#30363d',
      justifyContent: 'center', paddingHorizontal: 20,
    }, { focusable: true, __events: ['press', 'focus', 'blur', 'hoverIn', 'hoverOut'] });
    N.appendChild(b, text(label, { color: '#f0f6fc', fontSize: 20, fontWeight: '600' }));
    N.appendChild(menu, b);
    return b;
  });

  // Animated swatch row: proves the frame loop runs (timers -> setProps -> repaint).
  const row = el('View', { flexDirection: 'row', gap: 8, marginTop: 24 });
  N.appendChild(screen, row);
  const swatches = [];
  for (let i = 0; i < 12; i++) {
    const s = el('View', { width: 32, height: 32, borderRadius: 6, backgroundColor: '#30363d' });
    N.appendChild(row, s);
    swatches.push(s);
  }
  let tick = 0;
  function animate() {
    tick++;
    swatches.forEach((s, i) => {
      const on = (tick + i) % 12 < 4;
      N.setProps(s, { style: { width: 32, height: 32, borderRadius: 6, backgroundColor: on ? '#58a6ff' : '#30363d' } });
    });
    setTimeout(animate, 120);
  }
  animate();

  // Host -> UI: {hp: 0..1} updates the first meter.
  globalThis.__jsui_onHostMessage = function (name, json) {
    const data = JSON.parse(json);
    if (name === 'hud' && typeof data.hp === 'number') {
      const m = meters[0];
      N.setProps(m.fill, { style: { width: 220 * Math.max(0, Math.min(1, data.hp)), height: 18, backgroundColor: m.color, borderRadius: 9 } });
    }
  };

  // UI -> host: report presses.
  globalThis.__jsui_dispatchEvent = function (ev) {
    const i = buttons.indexOf(ev.target);
    if (ev.type === 'click' && i >= 0) {
      if (N.postMessage) N.postMessage('menu', JSON.stringify({ item: labels[i] }));
      return true;
    }
    return false;
  };
})();
