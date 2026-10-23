// Test bundle for surfaces (createSurface / jsui_surface_create) without
// React: a minimal AppRegistry stand-in. Every surface gets a box filling it,
// colored by its props (`color`; `size`: a fixed size for auto-sized
// surfaces); the main surface shows a teal band. Pointer presses post
// `press` with the surface's root tag.
(function () {
  const N = globalThis.__jsui_native;
  const main = N.createNode('View');
  N.setProps(main, { style: { width: 2000, height: 100, backgroundColor: '#008080' } });
  N.appendChild(N.rootId, main);
  const boxes = {};      // rootTag -> node id
  const tagOf = {};      // node id -> rootTag
  globalThis.__jsui_runSurfaces = (list) => {
    for (const [tag, appKey, propsJson] of list) run(tag, appKey, propsJson);
  };
  const run = (tag, appKey, propsJson) => {
    const props = JSON.parse(propsJson);
    let box = boxes[tag];
    if (!box) {
      box = N.createNode('View', tag);
      N.appendChild(tag, box);
      boxes[tag] = box;
      tagOf[box] = tag;
    }
    const style = props.size ? { width: props.size[0], height: props.size[1] } : { flex: 1 };
    N.setProps(box, { style: { ...style, backgroundColor: props.color }, __events: ['pointerdown'] });
    N.postMessage('run', JSON.stringify({ tag, appKey, color: props.color }));
  };
  globalThis.__jsui_stopSurface = (tag) => {
    N.removeChild(tag, boxes[tag]);
    N.destroyNode(boxes[tag]);
    delete boxes[tag];
    N.postMessage('stop', JSON.stringify(tag));
  };
  globalThis.__jsui_dispatchEvent = (e) => {
    if (e.type === 'pointerdown' && tagOf[e.target]) N.postMessage('press', JSON.stringify(tagOf[e.target]));
    return 0;
  };
})();
