import { mock } from './mock-native.js';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createElement as h, useRef } from 'react';
import { View, Overlay, OverlayArrow, render } from '../src/index.js';

function find(pred) {
  for (const [id, n] of mock.nodes) if (pred(n)) return [id, n];
  return [0, null];
}

test('Overlay sends its placement as the overlay prop of a View', () => {
  render(
    h(View, { style: { width: 40, height: 20 } },
      h(Overlay, { placement: 'top-start', offset: 6, flip: false, collision: 'none', matchWidth: 'min', style: { padding: 4 }, pointerEvents: 'none' },
        h(View, { style: { width: 10, height: 10 } }))),
  );
  const [, n] = find((x) => x.props && x.props.overlay);
  assert.ok(n, 'an overlay node');
  assert.equal(n.type, 'View');
  assert.deepEqual(n.props.overlay, { placement: 'top-start', offset: 6, flip: false, collision: 'none', matchWidth: 'min' });
  assert.equal(n.props.style.padding, 4);
  assert.equal(n.props.style.pointerEvents, 'none');
  assert.equal(n.props.anchor, undefined);
  assert.equal(n.children.length, 1);
  render(null);
});

test('Overlay resolves a ref anchor and boundary to node ids after commit', () => {
  function App() {
    const anchor = useRef(null);
    const box = useRef(null);
    return h(View, null,
      h(View, { ref: box, style: { width: 300, height: 200 } }),
      h(View, { ref: anchor, style: { width: 20, height: 20 } }),
      h(Overlay, { anchor, boundary: box, placement: 'right' }, h(View, null)));
  }
  render(h(App));
  const [, n] = find((x) => x.props && x.props.overlay);
  const root = mock.nodes.get(mock.root().children[0]);
  const [boxId, anchorId] = root.children;
  assert.equal(n.props.overlay.anchor, anchorId);
  assert.equal(n.props.overlay.boundary, boxId);
  assert.equal(n.props.overlay.placement, 'right');
  render(null);
});

test('Overlay takes a node id anchor as is', () => {
  render(h(Overlay, { anchor: 42 }, h(View, null)));
  const [, n] = find((x) => x.props && x.props.overlay);
  assert.deepEqual(n.props.overlay, { placement: 'bottom', anchor: 42 });
  render(null);
});

test('OverlayArrow is an absolutely positioned, non-hit View marked for native placement', () => {
  render(h(Overlay, null, h(OverlayArrow, { style: { width: 12, height: 12, backgroundColor: 'red' } })));
  const [, a] = find((x) => x.props && x.props.overlayArrow);
  assert.ok(a);
  assert.equal(a.props.overlayArrow, true);
  assert.deepEqual(a.props.style, { position: 'absolute', left: 0, top: 0, width: 12, height: 12, backgroundColor: 'red', pointerEvents: 'none' });
  assert.equal(Overlay.Arrow, OverlayArrow);
  render(null);
});
