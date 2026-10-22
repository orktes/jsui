// Perf bundle: a 1080p HUD-like tree (~600 nodes: panels, rows, cells, bars)
// with one value changing every frame through a timer, to measure
// tick / paint / upload cost of the web host in steady state.
(function () {
  const N = globalThis.__jsui_native;
  let count = 0;
  const el = (style, parent) => {
    const id = N.createNode('View');
    N.setProps(id, { style });
    N.appendChild(parent, id);
    count++;
    return id;
  };
  const screen = el({ width: 1920, height: 1080, padding: 24, backgroundColor: '#0b0f14' }, N.rootId);
  for (let p = 0; p < 6; p++) {
    const panel = el({ width: 600, height: 160, padding: 8, margin: 4, backgroundColor: '#161b22' }, screen);
    for (let r = 0; r < 8; r++) {
      const row = el({ height: 16, margin: 1, backgroundColor: '#21262d' }, panel);
      for (let c = 0; c < 10; c++) el({ width: 40, height: 4, margin: 1, backgroundColor: c % 2 ? '#30363d' : '#3e63dd' }, row);
    }
  }
  const bar = el({ width: 1, height: 12, backgroundColor: '#e5484d' }, screen);
  let t = 0;
  (function step() {
    t = (t + 7) % 800;
    N.setProps(bar, { style: { width: 100 + t, height: 12, backgroundColor: '#e5484d' } });
    setTimeout(step, 0);
  })();
  globalThis.__nodeCount = count;
})();
