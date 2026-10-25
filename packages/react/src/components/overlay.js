// Overlay: content drawn in the surface's overlay layer, above everything
// and outside the clips of its ancestors, positioned natively against an
// anchor every frame (src/core/overlay.h).

import { createElement as h, useLayoutEffect, useMemo, useState } from 'react';
import { findNodeHandle } from '../apis.js';

// Native id of a ref / instance / id; -1 while a ref is not attached yet
// (native keeps the overlay hidden until it is).
function nodeId(target) {
  if (target == null) return 0;
  if (typeof target === 'number') return target;
  const id = findNodeHandle(target);
  return typeof id === 'number' ? id : -1;
}

function isRef(v) {
  return v != null && v !== 'parent' && v !== 'viewport' && typeof v !== 'number';
}

/**
 * Renders its children in the overlay layer: above every other view of the
 * surface (zIndex of the rest does not matter) and not clipped by
 * overflow: 'hidden' or ScrollView ancestors, yet still a child of where it
 * is rendered for everything else: hover / press chains (a popout inside a
 * `group` keeps the group hovered while the pointer is on it), group state,
 * context, event bubbling and Tab order, its parent's transform (a scaled
 * stage scales it too) and opacity.
 *
 * The runtime places it every frame, without JS, so it follows scrolling
 * and animated anchors:
 * - `anchor`: 'parent' (default, the view it is rendered in), a ref or a
 *   node id.
 * - `placement`: 'top' | 'bottom' (default) | 'left' | 'right' | 'center' |
 *   'auto' (the side with room), optionally '-start' / '-end' to align to
 *   the anchor's edge instead of centring ('bottom-start').
 * - `offset`: gap from the anchor (px); `crossOffset`: shift along it.
 * - `flip` (default true): use the opposite side when it does not fit.
 * - `collision`: 'shift' (default: slide along the anchor to stay inside the
 *   boundary) or 'none'; `collisionPadding`: keep this far from its edges.
 * - `boundary`: a ref / id whose box replaces the viewport as the boundary.
 * - `matchWidth`: true (the anchor's width) or 'min' (at least it).
 * - `hideWhenClipped` (default true): hide while the anchor is scrolled /
 *   clipped out of view.
 * Other props go to the overlay's View (style, pointerEvents, ...). Put an
 * <OverlayArrow> inside to get an arrow on the edge facing the anchor.
 */
export function Overlay(props) {
  const {
    anchor = 'parent',
    placement = 'bottom',
    offset,
    crossOffset,
    flip,
    collision,
    collisionPadding,
    boundary,
    matchWidth,
    hideWhenClipped,
    children,
  } = props;
  // Refs resolve after commit: their ids come back as state.
  const [ids, setIds] = useState(() => ({ anchor: isRef(anchor) ? -1 : 0, boundary: isRef(boundary) ? -1 : 0 }));
  useLayoutEffect(() => {
    const a = isRef(anchor) ? nodeId(anchor) : 0;
    const b = isRef(boundary) ? nodeId(boundary) : 0;
    if (a !== ids.anchor || b !== ids.boundary) setIds({ anchor: a, boundary: b });
  });
  const anchorId = typeof anchor === 'number' ? anchor : isRef(anchor) ? ids.anchor : 0;
  const boundaryId = typeof boundary === 'number' ? boundary : isRef(boundary) ? ids.boundary : 0;
  const overlay = useMemo(() => {
    const o = { placement };
    if (anchorId) o.anchor = anchorId;
    if (boundaryId) o.boundary = boundaryId;
    if (offset != null) o.offset = offset;
    if (crossOffset != null) o.crossOffset = crossOffset;
    if (flip === false) o.flip = false;
    if (collision != null) o.collision = collision;
    if (collisionPadding != null) o.collisionPadding = collisionPadding;
    if (matchWidth) o.matchWidth = matchWidth === 'min' ? 'min' : true;
    if (hideWhenClipped === false) o.hideWhenClipped = false;
    return o;
  }, [placement, anchorId, boundaryId, offset, crossOffset, flip, collision, collisionPadding, matchWidth, hideWhenClipped]);
  const rest = Object.assign({}, props);
  for (const k of ['anchor', 'placement', 'offset', 'crossOffset', 'flip', 'collision', 'collisionPadding', 'boundary', 'matchWidth', 'hideWhenClipped', 'children'])
    delete rest[k];
  rest.overlay = overlay;
  return h('View', rest, children);
}
Overlay.displayName = 'Overlay';

const arrowBase = { position: 'absolute', left: 0, top: 0 };

/**
 * A view inside an <Overlay> that the runtime moves to the overlay's edge
 * facing the anchor, centred on the anchor as far as the overlay allows
 * (after flips and shifts). Half of it sticks out: a rotated square drawn
 * before the overlay's body makes a pointer. Style it with its size and
 * looks; its position is native.
 */
export function OverlayArrow(props) {
  const rest = Object.assign({}, props);
  rest.overlayArrow = true;
  rest.style = [arrowBase, props.style];
  if (rest.pointerEvents === undefined) rest.pointerEvents = 'none';
  return h('View', rest);
}
OverlayArrow.displayName = 'OverlayArrow';
Overlay.Arrow = OverlayArrow;
