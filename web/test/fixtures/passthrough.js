// Test bundle for passThrough without React: a button (press -> 'press'
// message) in the top-left corner of a painted, but inert, panel.
(function () {
  const N = globalThis.__jsui_native;
  const panel = N.createNode('View');
  N.setProps(panel, { style: { position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, backgroundColor: '#204060' } });
  N.appendChild(N.rootId, panel);
  const btn = N.createNode('View');
  N.setProps(btn, { style: { width: 100, height: 50, backgroundColor: '#ff0000' }, __events: ['press'] });
  N.appendChild(panel, btn);
  globalThis.__jsui_dispatchEvent = (e) => {
    if (e.type === 'click' && e.target === btn) N.postMessage('press', '{}');
    return 0;
  };
})();
