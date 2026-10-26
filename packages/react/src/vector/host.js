// Vector drawing host layer: the shared base of react-native-svg (svg.js)
// and @shopify/react-native-skia (skia.js).
//
// <Svg> and <Canvas> render ONE native node, a VectorView
// (render/vector_view.h). Their drawing children (<Circle>, <Path>, <G>,
// <LinearGradient>, ...) are host elements of types "draw:<lib>.<Name>",
// but they are not native nodes: the renderer (renderer.js) keeps them as
// JS-side DrawNodes, linked to their owner VectorHost. Custom components,
// hooks, context and keys inside a Canvas / Svg work as anywhere else. A
// commit that changed any drawing element marks its owner dirty; at the end
// of the commit (resetAfterCommit -> flushVectors) each dirty owner
// serializes its tree (its library's serializer, registered below) into the
// VectorView's `commands` prop: one bridge call per Svg / Canvas per commit.
//
// Animated values as drawing props (`<Circle r={anim} />`, or
// Animated.createAnimatedComponent(Circle)) become paint-only style keys
// `draw:<k>.<prop>` of the owner's native node, k being the element's key:
// Animated writes them with patchStyle (JS driver) or natively
// (useNativeDriver), so they animate with no React render and no
// re-serialization; the VectorView paints them over the element's props.

import { useRef } from 'react';
import { jsx } from 'react/jsx-runtime';
import { HostInstance } from '../instances.js';
import { flatten } from '../StyleSheet.js';
import { warnOnce } from '../native.js';
import { AnimatedNode } from '../animated/nodes.js';
import { createAnimatedComponent } from '../animated/createAnimatedComponent.js';

export const DRAW_PREFIX = 'draw:';

// Host context of a VectorView's subtree (renderer.js getChildHostContext).
export const DRAW_CONTEXT = { inText: false, draw: true };

// mode -> (owner: VectorHost) => commands object
const serializers = Object.create(null);
export function registerSerializer(mode, fn) {
  serializers[mode] = fn;
}

const dirtyOwners = new Set();

/** The VectorHost a drawing node belongs to (null while detached). */
export function ownerOf(node) {
  let n = node;
  while (n && !(n instanceof VectorHost)) n = n.parent;
  return n || null;
}

function markDirty(node) {
  const o = ownerOf(node);
  if (o) dirtyOwners.add(o);
}

/** A drawing element: a host instance with no native node of its own. */
export class DrawNode {
  constructor(type, props) {
    this.type = type;
    const dot = type.indexOf('.');
    this.name = dot < 0 ? type.slice(DRAW_PREFIX.length) : type.slice(dot + 1);
    this.props = props;
    this.parent = null;
    this.children = [];
    this.hidden = false;
    this.live = null;  // Animated values written since the last render (JS driver)
  }

  // Animated (and refs that need a native node) target the owner's node.
  get id() {
    const o = ownerOf(this);
    return o ? o.id : undefined;
  }

  get _nativeTag() {
    return this.id;
  }

  update(props) {
    this.props = props;
    this.live = null;
    markDirty(this);
  }

  setHidden(hidden) {
    if (this.hidden === hidden) return;
    this.hidden = hidden;
    markDirty(this);
  }

  /** Animated (JS driver) / setNativeProps: `draw:<k>.<prop>` keys onto the owner. */
  patchStyle(partial) {
    const o = ownerOf(this);
    if (!this.live) this.live = {};
    Object.assign(this.live, partial);
    if (o) o.patchStyle(partial);
  }

  setNativeProps(props) {
    if (!props) return;
    if (props.style) this.patchStyle(flatten(props.style) || {});
    let rest = null;
    for (const k in props) if (k !== 'style') (rest || (rest = {}))[k] = props[k];
    if (rest) {
      this.props = Object.assign({}, this.props, rest);
      markDirty(this);
      const o = ownerOf(this);
      if (o) o.flush();
    }
  }

  measure(cb) {
    const o = ownerOf(this);
    if (o) o.measure(cb);
  }

  measureInWindow(cb) {
    const o = ownerOf(this);
    if (o) o.measureInWindow(cb);
  }

  focus() {}
  blur() {}
}
DrawNode.prototype.draw = true;

/** A string child of a drawing element (an SVG <Text>'s content). */
export class DrawText {
  constructor(text) {
    this.type = 'RawText';
    this.text = text;
    this.parent = null;
    this.hidden = false;
    this.props = null;
  }
  setText(text) {
    this.text = text;
    markDirty(this);
  }
  setHidden(hidden) {
    this.hidden = hidden;
    markDirty(this);
  }
}
DrawText.prototype.draw = true;

/**
 * The VectorView host instance. Its native props are the React props (style,
 * events, layout props) plus `commands`, rebuilt by flush().
 */
export class VectorHost extends HostInstance {
  constructor(id, type, props) {
    super(id, type, ownProps(props));
    this.reactProps = props;
    this.children = [];  // drawing children (DrawNodes)
    this.dead = false;
    dirtyOwners.add(this);
  }

  update(props) {
    this.reactProps = props;
    dirtyOwners.add(this);
  }

  setHidden(hidden) {
    if (this.hidden === hidden) return;
    this.hidden = hidden;
    dirtyOwners.add(this);
  }

  /** Serializes the drawing tree and sends it with the React props. */
  flush() {
    dirtyOwners.delete(this);
    if (this.dead) return;
    const rp = this.reactProps;
    const ser = serializers[rp.mode] || serializers.svg;
    const anim = {};
    const commands = ser ? ser(this, anim) : { c: [] };
    commands.mode = rp.mode || 'svg';
    let style = rp.style;
    for (const k in anim) {
      style = [rp.style, anim];
      break;
    }
    const props = ownProps(rp);
    props.commands = commands;
    props.style = style;
    this.replaceProps(props);
  }
}

// What of the VectorView's React props goes to native as is.
function ownProps(props) {
  const out = {};
  for (const k in props) if (k !== 'children' && k !== 'root' && k !== 'mode') out[k] = props[k];
  return out;
}

/** End of a commit: dirty owners send their new commands. */
export function flushVectors() {
  if (dirtyOwners.size === 0) return;
  const list = Array.from(dirtyOwners);
  dirtyOwners.clear();
  for (const o of list) o.flush();
}

/** Forgets an owner whose node React deleted. */
export function forgetOwner(inst) {
  if (inst instanceof VectorHost) {
    inst.dead = true;
    dirtyOwners.delete(inst);
  }
}

// --- tree edits (renderer.js) ------------------------------------------------

export function drawInsert(parent, child, before) {
  if (!parent.children) {
    warnOnce('draw-parent', `[jsui] <${child.type}> must be inside <Svg> or <Canvas>`);
    return;
  }
  const list = parent.children;
  const i = list.indexOf(child);
  if (i >= 0) list.splice(i, 1);
  const j = before ? list.indexOf(before) : -1;
  list.splice(j < 0 ? list.length : j, 0, child);
  child.parent = parent;
  markDirty(parent);
}

export function drawRemove(parent, child) {
  markDirty(parent);
  const list = parent.children;
  const i = list ? list.indexOf(child) : -1;
  if (i >= 0) list.splice(i, 1);
  child.parent = null;
}

// --- serialization helpers ---------------------------------------------------

/**
 * The element's Animated style keys (draw:<k>.<prop>, with the values Animated
 * wrote since the render over them) merged into `anim`; returns k (0: none).
 */
export function animKey(node, anim) {
  const st = flatten(node.props.style);
  let k = 0;
  if (st) {
    for (const key in st) {
      if (key.charCodeAt(0) !== 100 || !key.startsWith(DRAW_PREFIX)) continue;
      anim[key] = st[key];
      if (!k) k = parseInt(key.slice(DRAW_PREFIX.length), 10) || 0;
    }
  }
  const live = node.live;
  if (live) for (const key in live) anim[key] = live[key];
  return k;
}

/** Visible children (React's Suspense hides them). */
export function visibleChildren(node) {
  const out = [];
  for (const c of node.children) if (!c.hidden) out.push(c);
  return out;
}

/** The concatenated text of an element's string children. */
export function textOf(node) {
  let s = '';
  for (const c of node.children) if (c instanceof DrawText && !c.hidden) s += c.text;
  return s;
}

// --- drawing components ------------------------------------------------------

let nextKey = 1;

// Props that are never animated drawing values.
const NOT_ANIMATED = { children: 1, style: 1, ref: 1, key: 1 };

function hasNested(v, depth) {
  if (v instanceof AnimatedNode) return true;
  if (depth > 3 || v === null || typeof v !== 'object') return false;
  if (Array.isArray(v)) {
    for (let i = 0; i < v.length; i++) if (hasNested(v[i], depth + 1)) return true;
    return false;
  }
  const proto = Object.getPrototypeOf(v);
  if (proto !== Object.prototype && proto !== null) return false;
  for (const k in v) if (hasNested(v[k], depth + 1)) return true;
  return false;
}

function styleKeysFor(key) {
  return new Proxy(
    {},
    {
      get: (_t, prop) => (typeof prop === 'string' && NOT_ANIMATED[prop] !== 1 ? DRAW_PREFIX + key + '.' + prop : undefined),
    },
  );
}

/**
 * A drawing component over host type `type` that accepts Animated values in
 * its props: top-level ones (and `transform` arrays holding them) become
 * draw:<k>.<prop> style keys driven without React renders; Animated values
 * nested deeper (shader uniforms, ...) re-render the element on each change
 * (JS driver only).
 */
export function drawComponent(type, displayName, normalize) {
  let AnimatedHost = null;
  function DrawComponent(props) {
    const keyRef = useRef(0);
    let anim = null;
    let nested = false;
    for (const p in props) {
      if (NOT_ANIMATED[p] === 1) continue;
      const v = props[p];
      if (v instanceof AnimatedNode || (p === 'transform' && Array.isArray(v) && hasNested(v, 0))) {
        if (!keyRef.current) keyRef.current = nextKey++;
        (anim || (anim = {}))[DRAW_PREFIX + keyRef.current + '.' + p] = v;
      } else if (v !== null && typeof v === 'object' && !nested && hasNested(v, 0)) {
        nested = true;
      }
    }
    let out = props;
    // Skia's `style` is the paint style ('fill' / 'stroke'): it travels as
    // `paintStyle`, so `style` can carry the Animated keys.
    const paintStyle = props.style != null && typeof props.style !== 'object';
    if (anim || nested || paintStyle) {
      out = {};
      for (const p in props) if (!anim || anim[DRAW_PREFIX + keyRef.current + '.' + p] === undefined) out[p] = props[p];
      if (paintStyle) {
        out.paintStyle = props.style;
        out.style = anim || undefined;
      } else if (anim) out.style = props.style ? [props.style, anim] : anim;
      if ((anim || nested) && !AnimatedHost) AnimatedHost = createAnimatedComponent(type);
    }
    if (normalize) out = normalize(out);
    return jsx(anim || nested ? AnimatedHost : type, out);
  }
  DrawComponent.displayName = displayName;
  // Animated.createAnimatedComponent(Circle): its animated props become this
  // instance's draw:<k>.<prop> style keys (one key per mounted component).
  DrawComponent.__animatedStyleProps = (sub) => sub.__drawKeys || (sub.__drawKeys = styleKeysFor(nextKey++));
  return DrawComponent;
}
