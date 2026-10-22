// Test bundle: the painter effects that could differ between the CPU raster
// and Skia on WebGL (rounded corners, blurred / rotated shadows, gradients,
// opacity / filter / blend layers, clips, text). Bridge only.
(function () {
  const N = globalThis.__jsui_native;
  const add = (style, text) => {
    const id = N.createNode(text ? 'Text' : 'View');
    N.setProps(id, { style: { position: 'absolute', ...style } });
    if (text) {
      const raw = N.createNode('RawText');
      N.setText(raw, text);
      N.appendChild(id, raw);
    }
    N.appendChild(N.rootId, id);
    return id;
  };
  add({ left: 0, top: 0, width: 480, height: 320, backgroundColor: '#202430' });
  add({ left: 16, top: 16, width: 120, height: 80, backgroundColor: '#3366ff', borderRadius: 16,
    boxShadow: '0 8px 16px rgba(0,0,0,0.6)' });
  add({ left: 170, top: 20, width: 100, height: 70, backgroundColor: '#ffcc00', borderRadius: 10,
    boxShadow: '4px 4px 12px rgba(255,64,64,0.8)', transform: [{ rotate: '12deg' }] });
  add({ left: 310, top: 16, width: 140, height: 80, borderWidth: 3, borderColor: '#fff', borderStyle: 'dashed',
    borderRadius: 12, boxShadow: 'inset 0 0 14px rgba(0,255,200,0.9)' });
  add({ left: 16, top: 120, width: 120, height: 70, borderRadius: 8,
    backgroundImage: 'linear-gradient(135deg, #ff0080, #7928ca 60%, #2af598)' });
  add({ left: 170, top: 120, width: 100, height: 70,
    backgroundImage: 'radial-gradient(circle closest-side, white, rgba(0,0,255,0.2))' });
  add({ left: 310, top: 120, width: 70, height: 60, backgroundColor: '#ff3366', opacity: 0.5, borderRadius: 30 });
  add({ left: 390, top: 120, width: 60, height: 60, backgroundColor: '#33ffcc', filter: 'blur(3px)' });
  add({ left: 350, top: 150, width: 80, height: 40, backgroundColor: '#00aaff', mixBlendMode: 'multiply' });
  add({ left: 16, top: 220, fontSize: 28, color: '#ffffff', fontWeight: '700' }, 'GPU vs CPU');
  add({ left: 220, top: 230, fontSize: 18, color: '#ffd0a0', textShadowColor: 'rgba(0,0,0,0.9)',
    textShadowOffset: { width: 1, height: 1 }, textShadowRadius: 2 }, 'Skia on WebGL');
})();
