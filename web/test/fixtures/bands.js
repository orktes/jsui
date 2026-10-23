// Test bundle: four 50px horizontal bands (red, green, blue, 50% red) in a
// 320px wide column, plus hooks the browser test drives. Uses only the bridge
// and the most basic styles so it works without @orktes/jsui-react.
(function () {
  const N = globalThis.__jsui_native;
  const setBand = (id, color) =>
    N.setProps(id, { style: { width: 320, height: 50, backgroundColor: color }, __events: ['press'] });
  const bands = ['#ff0000', '#00ff00', '#0000ff', '#ff000080'].map((color) => {
    const id = N.createNode('View');
    setBand(id, color);
    N.appendChild(N.rootId, id);
    return id;
  });

  // Host -> UI: 'paint' recolours band 0.
  globalThis.__jsui_onHostMessage = function (name, json) {
    if (name === 'paint') setBand(bands[0], JSON.parse(json).color);
  };
  // Events: a click on band 1 turns it white and tells the host.
  globalThis.__jsui_dispatchEvent = function (ev) {
    if (ev.type === 'click' && ev.target === bands[1]) {
      setBand(bands[1], '#ffffff');
      if (N.postMessage) N.postMessage('clicked', JSON.stringify({ band: 1 }));
      return true;
    }
    return false;
  };

  // UI -> host on startup (engine's postMessage; absent in the bare skeleton).
  if (N.postMessage) N.postMessage('ready', JSON.stringify({ bands: bands.length, text: 'héllo ✓ 日本' }));

  // Timer-driven change: band 2 turns yellow after 200 ms.
  setTimeout(() => setBand(bands[2], '#ffff00'), 200);
})();
