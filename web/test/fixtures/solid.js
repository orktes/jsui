// Test bundle: one 100px band. The dev-reload test serves different colours
// under this name; the default is teal.
(function () {
  const N = globalThis.__jsui_native;
  const id = N.createNode('View');
  N.setProps(id, { style: { width: 2000, height: 100, backgroundColor: '#008080' } });
  N.appendChild(N.rootId, id);
})();
