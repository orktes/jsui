// GC-pause benchmark: showcase-style navigation under React (pure-JS host, as in react-commit.jsx).
// Every frame pushes a new screen (~2,000 host views, 200 stateful rows, closures and per-item data),
// keeps the 3 previous screens mounted (a navigation stack, screens memoized so only the one being
// hidden re-renders) and unmounts the oldest, so each frame
// allocates a full screen and turns another into cyclic garbage (fibers point at each other).
// 20 warm-up frames, then 300 measured frames. Prints per-frame percentiles.
// If the host defines __runPendingGC (qjs-gc build with QJS_GC_DEFERRED=1: JS_SetGCDeferred),
// it is called between frames and timed separately, as an embedder would after presenting a frame.
// With a global __gcDriven, the script only defines __gcFrame(f) and the host runs the frames.
// Build: bench/engines/build.sh  ->  bench/engines/out/gc-bench.js
import React, { useState, useCallback } from 'react';
import Reconciler from 'react-reconciler';

const log = typeof print === 'function' ? print : (s) => console.log(s);
const now = typeof performance !== 'undefined' && performance.now ? () => performance.now()
  : typeof preciseTime === 'function' ? () => preciseTime() * 1000 : () => Date.now();

let nodes = 0;
let prio = 32;
const hostConfig = {
  supportsMutation: true, isPrimaryRenderer: true, noTimeout: -1,
  createInstance(type, props) { nodes++; return { type, props, children: [] }; },
  createTextInstance(text) { return { text }; },
  appendInitialChild(p, c) { p.children.push(c); },
  appendChild(p, c) { p.children.push(c); },
  appendChildToContainer(p, c) { p.children.push(c); },
  insertBefore(p, c, b) { p.children.splice(p.children.indexOf(b), 0, c); },
  insertInContainerBefore(p, c, b) { p.children.splice(p.children.indexOf(b), 0, c); },
  removeChild(p, c) { p.children.splice(p.children.indexOf(c), 1); },
  removeChildFromContainer(p, c) { p.children.splice(p.children.indexOf(c), 1); },
  clearContainer(c) { c.children.length = 0; },
  finalizeInitialChildren() { return false; },
  commitUpdate(inst, type, oldProps, newProps) { inst.props = newProps; },
  commitTextUpdate(t, o, n) { t.text = n; },
  shouldSetTextContent() { return false; },
  getRootHostContext() { return {}; }, getChildHostContext(c) { return c; },
  getPublicInstance(i) { return i; }, prepareForCommit() { return null; }, resetAfterCommit() {},
  preparePortalMount() {},
  scheduleTimeout: (fn, ms) => (typeof setTimeout === 'function' ? setTimeout(fn, ms) : 0),
  cancelTimeout: (id) => (typeof clearTimeout === 'function' ? clearTimeout(id) : undefined),
  getCurrentUpdatePriority() { return prio; }, setCurrentUpdatePriority(p) { prio = p; },
  resolveUpdatePriority() { return prio; },
  getInstanceFromNode() { return null; }, beforeActiveInstanceBlur() {}, afterActiveInstanceBlur() {},
  prepareScopeUpdate() {}, getInstanceFromScope() { return null; }, detachDeletedInstance() {},
  maySuspendCommit() { return false; }, preloadInstance() { return true; }, startSuspendingCommit() {},
  suspendInstance() {}, waitForCommitToBeReady() { return null; }, resetFormInstance() {},
  shouldAttemptEagerTransition() { return false; }, trackSchedulerEvent() {}, resolveEventType() { return null; },
  resolveEventTimeStamp() { return -1.1; }, requestPostPaintCallback() {}, NotPendingTransition: null,
  HostTransitionContext: React.createContext(null),
};

const R = Reconciler(hostConfig);
const container = { children: [] };
const root = R.createContainer(container, 0, null, false, null, '', (e) => log('err ' + e), (e) => log('err ' + e), (e) => log('err ' + e), null);
const render = (el) => { R.updateContainerSync(el, root, null, null); R.flushSyncWork(); };

const ROWS = 200, PER_ROW = 9, KEEP = 3, WARM = 20, FRAMES = 300; // 200 rows x (1 + 9) = 2,000 views
const rowStyle = { flexDirection: 'row', height: 24 };
function Item({ data, onPress }) {
  return <view style={{ width: 20 + data.w, opacity: data.on ? 1 : 0.5 }} onPress={() => onPress(data.id)} label={data.label} />;
}
function Row({ screen, r }) {
  const [sel, setSel] = useState(-1);
  const onPress = useCallback((id) => setSel(id), []);
  const items = [];
  for (let i = 0; i < PER_ROW; i++) {
    const data = { id: i, w: (screen * 3 + r + i) % 40, on: i !== sel, label: `s${screen} r${r} i${i}`, tags: [r, i, screen] };
    items.push(<Item key={i} data={data} onPress={onPress} />);
  }
  return <view style={rowStyle} onLayout={(e) => e && sel}>{items}</view>;
}
// Per-screen app state with reference cycles (rows point back at the model, listeners close over it),
// like a store or navigation state. React clears a deleted fiber's own pointers, so without this all of
// an unmounted screen is freed by refcounting alone; this part needs QuickJS's cycle collector.
function makeModel(id) {
  const model = { id, rows: [], listeners: [] };
  for (let r = 0; r < ROWS; r++) {
    const row = { r, parent: model, cells: new Array(PER_ROW).fill(r) };
    row.self = row;
    model.rows.push(row);
    model.listeners.push(() => model.rows[r].cells.length + model.id);
  }
  return model;
}
const Screen = React.memo(function Screen({ id, active }) {
  const [state] = useState(() => ({ opened: id, history: new Array(16).fill(id), model: makeModel(id) }));
  const rows = [];
  for (let r = 0; r < ROWS; r++) rows.push(<Row key={r} screen={id} r={r} />);
  return <view hidden={!active} title={`screen ${state.opened}`}>{rows}</view>;
});
const Stack = ({ top }) => {
  const screens = [];
  for (let s = Math.max(0, top - KEEP); s <= top; s++) screens.push(<Screen key={s} id={s} active={s === top} />);
  return <view>{screens}</view>;
};

// Driven by a host (gc-driver.c): it calls __gcFrame(f) per frame and collects between calls.
if (typeof __gcDriven !== 'undefined') globalThis.__gcFrame = (f) => render(<Stack top={f} />);
else run();
function run() {
const pendingGC = typeof __runPendingGC === 'function' ? __runPendingGC : null;
const frames = [], gcs = [];
const t0 = now();
for (let f = 0; f < WARM + FRAMES; f++) {
  const s = now();
  render(<Stack top={f} />);
  const e = now();
  if (f >= WARM) frames.push(e - s);
  if (pendingGC) { const g0 = now(); pendingGC(); if (f >= WARM) gcs.push(now() - g0); }
}
const total = now() - t0;
const pct = (a, p) => { const b = a.slice().sort((x, y) => x - y); return b[Math.min(b.length - 1, Math.floor(p / 100 * b.length))]; };
const sum = (a) => a.reduce((x, y) => x + y, 0);
const st = (a) => `p50=${pct(a, 50).toFixed(2)} p95=${pct(a, 95).toFixed(2)} p99=${pct(a, 99).toFixed(2)} max=${Math.max(...a).toFixed(2)} mean=${(sum(a) / a.length).toFixed(2)}`;
log(`[gc] frame ms: ${st(frames)}`);
log(`[gc] worst frames: ${frames.map((t, i) => [t, i]).sort((a, b) => b[0] - a[0]).slice(0, 5).map(([t, i]) => `#${i}:${t.toFixed(1)}`).join(' ')}`);
if (pendingGC) log(`[gc] between-frame GC ms: ${st(gcs)} total=${sum(gcs).toFixed(1)}`);
log(`[gc] total ${total.toFixed(1)} ms for ${WARM + FRAMES} frames, host nodes created ${nodes}, mounted at end ${container.children.length ? container.children[0].children.length : 0} screens`);
}
