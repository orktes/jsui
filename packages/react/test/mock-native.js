// Mock __jsui_native bridge for Node tests: records the node tree and every
// bridge call. Import this before @orktes/jsui-react.

const nodes = new Map();
const calls = [];
let nextId = 2;
const ROOT = 1;
nodes.set(ROOT, { id: ROOT, type: 'Root', props: {}, children: [], parent: null, text: '' });

function node(id) {
  const n = nodes.get(id);
  if (!n) throw new Error('mock: unknown node ' + id);
  return n;
}

function detach(n) {
  if (n.parent != null) {
    const p = node(n.parent);
    const i = p.children.indexOf(n.id);
    if (i >= 0) p.children.splice(i, 1);
    n.parent = null;
  }
}

const TYPES = new Set(['View', 'Text', 'RawText', 'Image', 'ScrollView', 'TextInput']);

export const mock = {
  nodes,
  calls,
  focused: null,
  layouts: new Map(),
  messages: [],
  reset() {
    calls.length = 0;
  },
  count(name) {
    return calls.filter((c) => c[0] === name).length;
  },
  root: () => node(ROOT),
  /** Serializes the subtree as nested arrays: [type, props, ...children]. */
  tree(id = ROOT) {
    const n = node(id);
    if (n.type === 'RawText') return n.text;
    return [n.type, n.props, ...n.children.map((c) => mock.tree(c))];
  },
  /** Finds the first node matching pred (depth-first). */
  find(pred, id = ROOT) {
    const n = node(id);
    if (pred(n)) return n;
    for (const c of n.children) {
      const r = mock.find(pred, c);
      if (r) return r;
    }
    return null;
  },
  findAll(pred, id = ROOT, out = []) {
    const n = node(id);
    if (pred(n)) out.push(n);
    for (const c of n.children) mock.findAll(pred, c, out);
    return out;
  },
  text(id) {
    const n = node(id);
    if (n.type === 'RawText') return n.text;
    return n.children.map((c) => mock.text(c)).join('');
  },
  dispatch(ev) {
    return globalThis.__jsui_dispatchEvent(Object.assign({ timestamp: Date.now() }, ev));
  },
};

globalThis.__jsui_native = {
  rootId: ROOT,
  viewport: { width: 800, height: 600, scale: 2 },
  createNode(type) {
    if (!TYPES.has(type)) throw new TypeError(`unknown element type '${type}'`);
    const id = nextId++;
    nodes.set(id, { id, type, props: {}, children: [], parent: null, text: '' });
    calls.push(['createNode', type, id]);
    return id;
  },
  setProps(id, props) {
    // Mirror the native bridge: values are copied (JSON-like), functions dropped.
    node(id).props = JSON.parse(JSON.stringify(props));
    calls.push(['setProps', id, props]);
  },
  patchStyle(id, partial) {
    const n = node(id);
    n.props = Object.assign({}, n.props, { style: Object.assign({}, n.props.style, JSON.parse(JSON.stringify(partial))) });
    calls.push(['patchStyle', id, partial]);
  },
  setText(id, text) {
    node(id).text = String(text);
    calls.push(['setText', id, text]);
  },
  appendChild(p, c) {
    const child = node(c);
    detach(child);
    node(p).children.push(c);
    child.parent = p;
    calls.push(['appendChild', p, c]);
  },
  insertBefore(p, c, before) {
    const child = node(c);
    detach(child);
    const kids = node(p).children;
    const i = kids.indexOf(before);
    kids.splice(i < 0 ? kids.length : i, 0, c);
    child.parent = p;
    calls.push(['insertBefore', p, c, before]);
  },
  removeChild(p, c) {
    const child = node(c);
    if (child.parent === p) detach(child);
    calls.push(['removeChild', p, c]);
  },
  destroyNode(id) {
    const rec = (i) => {
      const n = nodes.get(i);
      if (!n) return;
      for (const c of n.children) rec(c);
      nodes.delete(i);
    };
    rec(id);
    calls.push(['destroyNode', id]);
  },
  measure(id) {
    return mock.layouts.get(id) || { x: 0, y: 0, width: 100, height: 50, pageX: 10, pageY: 20 };
  },
  focus(id) {
    mock.focused = id;
    calls.push(['focus', id]);
  },
  blur(id) {
    if (mock.focused === id) mock.focused = null;
    calls.push(['blur', id]);
  },
  scrollTo(id, x, y, animated) {
    calls.push(['scrollTo', id, x, y, animated]);
  },
  flashScrollIndicators(id) {
    calls.push(['flashScrollIndicators', id]);
  },
  postMessage(name, json) {
    mock.messages.push([name, json]);
  },
  loadFont(family, url, weight, italic) {
    calls.push(['loadFont', family, url, weight, italic]);
    return Promise.resolve(true);
  },
  // Shared state (src/host_state.cpp semantics: a runtime-wide version per change).
  getState(key) {
    const e = hostState.get(key);
    return e ? e.json : undefined;
  },
  stateVersion(key) {
    const e = hostState.get(key);
    return e ? e.version : 0;
  },
  stateKeys() {
    return Array.from(hostState.keys());
  },
  setState(key, json) {
    if (json === undefined) {
      if (hostState.delete(key)) mock.uiState.push([key, undefined]);
      return 0;
    }
    JSON.parse(json); // SyntaxError like the native bridge
    const prev = hostState.get(key);
    if (!prev || prev.json !== json) {
      hostState.set(key, { json, version: stateVersion++ });
      mock.uiState.push([key, json]);
    }
    return hostState.get(key).version;
  },
  patchState(key, json) {
    const prev = hostState.get(key);
    const next = JSON.stringify(applyPatch(prev ? JSON.parse(prev.json) : null, JSON.parse(json)));
    return __jsui_native.setState(key, next);
  },
  // Calls (Host.call -> on_call; the test answers with mock.resolveCall / rejectCall).
  hostCall(name, json, settle) {
    if (!mock.callsEnabled) return 0;
    const id = nextCallId++;
    mock.hostCalls.set(id, { name, json, settle });
    if (mock.onCall) mock.onCall(id, name, json);
    return id;
  },
  cancelCall(id) {
    if (!mock.hostCalls.delete(id)) return false;
    mock.cancelled.push(id);
    return true;
  },
  callResult(id, ok, payload) {
    mock.uiResults.push([id, ok, payload]);
  },
};

const hostState = new Map();
let stateVersion = 1;
let nextCallId = 1;
function applyPatch(target, patch) {
  if (patch === null || typeof patch !== 'object' || Array.isArray(patch)) return patch;
  const out = target && typeof target === 'object' && !Array.isArray(target) ? Object.assign({}, target) : {};
  for (const k of Object.keys(patch)) {
    if (patch[k] === null) delete out[k];
    else out[k] = applyPatch(out[k], patch[k]);
  }
  return out;
}
Object.assign(mock, {
  uiState: [],
  hostCalls: new Map(),
  cancelled: [],
  uiResults: [],
  callsEnabled: true,
  onCall: null,
  /** Embedder side: jsui_set_state (json undefined removes). Delivered by mock.tickState(). */
  setHostState(key, json) {
    const prev = hostState.get(key);
    if (json === undefined) hostState.delete(key);
    else if (prev && prev.json === json) return;
    else hostState.set(key, { json, version: stateVersion++ });
    mock.dirtyState.add(key);
  },
  dirtyState: new Set(),
  /** The start of jsui_tick: hands the changed keys to JS. */
  tickState() {
    const keys = Array.from(mock.dirtyState);
    mock.dirtyState.clear();
    if (keys.length) globalThis.__jsui_onHostState(keys);
  },
  resolveCall(id, json) {
    const c = mock.hostCalls.get(id);
    if (!c) return -1;
    mock.hostCalls.delete(id);
    c.settle(true, json);
    return 0;
  },
  rejectCall(id, message) {
    const c = mock.hostCalls.get(id);
    if (!c) return -1;
    mock.hostCalls.delete(id);
    c.settle(false, message);
    return 0;
  },
});

// Simulated frame clock for Animated: requestAnimationFrame callbacks run
// when the test calls mock.frame(ms).
let rafQueue = [];
let time = 0;
globalThis.requestAnimationFrame = (cb) => {
  rafQueue.push(cb);
  return rafQueue.length;
};
globalThis.cancelAnimationFrame = () => {};
const realNow = performance.now.bind(performance);
let useFake = true;
Object.defineProperty(globalThis, 'performance', { value: { now: () => (useFake ? time : realNow()) }, configurable: true });
mock.frame = (ms = 16) => {
  time += ms;
  const q = rafQueue;
  rafQueue = [];
  for (const cb of q) cb(time);
};
mock.advance = (ms) => {
  for (let t = 0; t < ms; t += 16) mock.frame(16);
};
mock.now = () => time;
mock.realTime = (on) => {
  useFake = !on;
};
