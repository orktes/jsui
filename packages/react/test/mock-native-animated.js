// Adds a recording __jsui_native.animated (the native Animated driver's
// bridge) to the mock bridge. Values are stored so getValue / setValue
// round-trip; animations only run when the test ends them through
// __jsui_onAnimatedEnd, like the runtime would. Import before @jsui/react.
import { mock } from './mock-native.js';

const nodes = new Map();  // tag -> {config, value, offset, parents, view}
const animations = new Map();  // id -> {tag, config}
const calls = [];

export const animated = {
  nodes,
  animations,
  calls,
  reset() {
    calls.length = 0;
  },
  count(name) {
    return calls.filter((c) => c[0] === name).length;
  },
  of(name) {
    return calls.filter((c) => c[0] === name).map((c) => c.slice(1));
  },
  /** Simulates the runtime finishing animation `id` with the value at `value`. */
  end(id, finished, value) {
    const a = animations.get(id);
    animations.delete(id);
    if (a && value !== undefined) nodes.get(a.tag).value = value;
    globalThis.__jsui_onAnimatedEnd(id, finished, value !== undefined ? value : a ? nodes.get(a.tag).value : 0);
  },
  /** Simulates a native frame moving value node `tag` (listeners hear it). */
  frame(tag, value) {
    nodes.get(tag).value = value;
    globalThis.__jsui_onAnimatedValueUpdate(tag, value);
  },
  stopVelocity: 0,
};

const record = (name, fn) =>
  function (...args) {
    calls.push([name, ...JSON.parse(JSON.stringify(args))]);
    return fn ? fn(...args) : undefined;
  };

globalThis.__jsui_native.animated = {
  createNode: record('createNode', (tag, config) => {
    if (!config || typeof config.type !== 'string') throw new TypeError('bad config');
    nodes.set(tag, { config: JSON.parse(JSON.stringify(config)), value: config.value || 0, offset: config.offset || 0, parents: [], view: null });
  }),
  connect: record('connect', (p, c) => nodes.get(c) && nodes.get(c).parents.push(p)),
  disconnect: record('disconnect'),
  connectToView: record('connectToView', (tag, view) => (nodes.get(tag).view = view)),
  disconnectFromView: record('disconnectFromView', (tag) => nodes.get(tag) && (nodes.get(tag).view = null)),
  dropNode: record('dropNode', (tag) => nodes.delete(tag)),
  setValue: record('setValue', (tag, v) => {
    nodes.get(tag).value = v;
    for (const [id, a] of animations) if (a.tag === tag) animations.delete(id);
  }),
  setOffset: record('setOffset', (tag, o) => (nodes.get(tag).offset = o)),
  flattenOffset: record('flattenOffset', (tag) => {
    const n = nodes.get(tag);
    n.value += n.offset;
    n.offset = 0;
  }),
  extractOffset: record('extractOffset', (tag) => {
    const n = nodes.get(tag);
    n.offset += n.value;
    n.value = 0;
  }),
  getValue: (tag) => (nodes.get(tag) ? nodes.get(tag).value : 0),
  startAnimation: record('startAnimation', (id, tag, config) => animations.set(id, { tag, config })),
  stopAnimation: record('stopAnimation', (id) => {
    if (!animations.has(id)) return null;
    animations.delete(id);
    return { velocity: animated.stopVelocity };
  }),
  addEventMapping: record('addEventMapping'),
  removeEventMapping: record('removeEventMapping'),
  startListening: record('startListening'),
  stopListening: record('stopListening'),
};

export { mock };
