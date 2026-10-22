// Test bundle that throws while loading.
(function () {
  const N = globalThis.__jsui_native;
  const id = N.createNode('View');
  N.setProps(id, { style: { width: 100, height: 100, backgroundColor: '#00ff00' } });
  N.appendChild(N.rootId, id);
  throw new Error('fixture exploded');
})();
