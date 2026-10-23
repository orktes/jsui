// Test bundle for host textures, game data, calls and scrolling, without
// React (main and worker mode): a 100x100 Image showing host texture 'tex'
// in the top-left corner, a 200px ScrollView of 40 pressable rows to its
// right (a click posts 'row' with the row's index), Host.handle('ui.add')
// for callUi, Host.call('host.greet') on the host message 'greet', and the
// host message 'buf' answering 'buf' with frame data buffer 'b'.
(function () {
  const N = globalThis.__jsui_native;
  const img = N.createNode('Image');
  N.setProps(img, { texture: 'tex', style: { position: 'absolute', left: 0, top: 0, width: 100, height: 100 } });
  N.appendChild(N.rootId, img);
  const sv = N.createNode('ScrollView');
  N.setProps(sv, { style: { position: 'absolute', left: 120, top: 0, width: 200, height: 200, backgroundColor: '#ffffff' } });
  N.appendChild(N.rootId, sv);
  const rows = [];
  for (let i = 0; i < 40; i++) {
    const r = N.createNode('View');
    N.setProps(r, { style: { height: 40, backgroundColor: i % 2 ? '#0000ff' : '#00ff00' }, __events: ['press'] });
    N.appendChild(sv, r);
    rows.push(r);
  }
  globalThis.__jsui_dispatchEvent = (e) => {
    const i = rows.indexOf(e.target);
    if (e.type === 'click' && i >= 0) {
      N.postMessage('row', JSON.stringify(i));
      return 1;
    }
    return 0;
  };
  globalThis.__jsui_onCall = (id, name, json) => {
    const a = JSON.parse(json);
    if (name === 'ui.add') N.callResult(id, true, JSON.stringify(a.a + a.b));
    else N.callResult(id, false, `no UI handler for ${name}`);
  };
  globalThis.__jsui_onHostMessage = (name, json) => {
    if (name === 'greet') {
      N.hostCall('host.greet', json, (ok, payload) => N.postMessage('greeted', JSON.stringify({ ok, payload })));
    } else if (name === 'buf') {
      const b = N.buffer('b');
      N.postMessage('buf', JSON.stringify(b ? Array.from(new Float32Array(b[0], 0, b[2])) : null));
    }
  };
})();
