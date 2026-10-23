// Test bundle: a panel with children (so its opacity needs a layer) that the
// host fades and moves with 'move' messages ({ opacity, x, y }), the way an
// Animated opacity / translate frame does (patchStyle), over a static
// backdrop (a background and a gradient strip). Bridge only.
(function () {
  const N = globalThis.__jsui_native;
  const node = (type, parent, style, text) => {
    const id = N.createNode(type);
    N.setProps(id, { style });
    if (text) {
      const raw = N.createNode('RawText');
      N.setText(raw, text);
      N.appendChild(id, raw);
    }
    N.appendChild(parent, id);
    return id;
  };
  node('View', N.rootId, { position: 'absolute', left: 0, top: 0, width: 400, height: 300, backgroundColor: '#1b2030' });
  const strip = node('View', N.rootId, { position: 'absolute', left: 20, top: 200, width: 360, height: 60, borderRadius: 12,
    backgroundImage: 'linear-gradient(90deg, #ff0080, #2af598)' });
  const panel = node('View', N.rootId, { position: 'absolute', left: 40, top: 30, width: 260, height: 150,
    backgroundColor: '#3355cc', borderRadius: 18, borderWidth: 2, borderColor: '#ffffff',
    boxShadow: '0 6px 14px rgba(0,0,0,0.6)', opacity: 0.5 });
  node('Text', panel, { position: 'absolute', left: 16, top: 12, fontSize: 22, color: '#ffffff', fontWeight: '700' }, 'Composited');
  node('View', panel, { position: 'absolute', left: 16, top: 56, width: 90, height: 70, borderRadius: 35,
    backgroundColor: '#ffcc00' });
  const child = node('View', panel, { position: 'absolute', left: 120, top: 60, width: 120, height: 60, borderRadius: 8,
    backgroundColor: '#ff3366', transform: [{ rotate: '8deg' }] });
  // 'move': the panel; 'child': a child's own opacity / rotation / colour
  // (the panel's layer content changes); 'backdrop': the strip's style.
  globalThis.__jsui_onHostMessage = function (name, json) {
    const m = JSON.parse(json);
    if (name === 'move') N.patchStyle(panel, { opacity: m.opacity, transform: [{ translateX: m.x }, { translateY: m.y }] });
    if (name === 'child') N.patchStyle(child, m);
    if (name === 'backdrop') N.patchStyle(strip, m);
  };
})();
