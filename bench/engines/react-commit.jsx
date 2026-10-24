// Engine-neutral React commit benchmark (QuickJS-ng / Hermes / JavaScriptCore / V8).
// Same scenarios as packages/react/bench/commit-bench.jsx, but with a pure-JS
// host config (no native bridge) so every engine runs identical code.
// Build: bench/engines/build.sh  ->  bench/engines/out/react-commit.js
import React, { useState } from 'react';
import Reconciler from 'react-reconciler';

const log = typeof print === 'function' ? print : (s) => console.log(s);
const now = typeof performance !== 'undefined' && performance.now ? () => performance.now()
  : typeof preciseTime === 'function' ? () => preciseTime() * 1000 : () => Date.now();

let nodes = 0;
let updates = 0;
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
  commitUpdate(inst, type, oldProps, newProps) { updates++; inst.props = newProps; },
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

const t0 = now();
const R = Reconciler(hostConfig);
const container = { children: [] };
const root = R.createContainer(container, 0, null, false, null, '', (e) => log('err ' + e), (e) => log('err ' + e), (e) => log('err ' + e), null);

const N = 500;
let setTick;
const cellStyle = { width: 30, height: 20, margin: 1 };
function Cell({ hue }) { return <view style={[cellStyle, { backgroundColor: `hsl(${hue}, 60%, 50%)` }]} />; }
const MemoCell = React.memo(Cell);
function App() {
  const [state, set] = useState({ tick: 0, mode: 'none', memo: false });
  setTick = set;
  const cells = [];
  const C = state.memo ? MemoCell : Cell;
  for (let i = 0; i < N; i++) {
    const changed = state.mode === 'all' || (state.mode === 'tenth' && i % 10 === 0);
    const hue = changed ? (i * 7 + state.tick * 13) % 360 : (i * 7) % 360;
    cells.push(<C key={i} hue={hue} />);
  }
  return <view>{cells}</view>;
}
const render = (el) => { R.updateContainerSync(el, root, null, null); R.flushSyncWork(); };
log(`[bench] init (React + reconciler setup): ${(now() - t0).toFixed(3)} ms`);

function time(label, it, fn) {
  for (let k = 0; k < 3; k++) fn(k); // warm-up
  const s = now();
  const u0 = updates;
  for (let k = 0; k < it; k++) fn(k);
  log(`[bench] ${label}: ${((now() - s) / it).toFixed(3)} ms/commit, ${(updates - u0) / it} host updates/commit`);
}
time('mount 500 views', 20, () => { render(null); render(<App />); });
let tick = 1;
for (const memo of [false, true]) {
  R.flushSyncFromReconciler(() => setTick({ tick: tick++, mode: 'none', memo }));
  for (const mode of ['none', 'tenth', 'all']) {
    time(`update ${memo ? 'memo' : 'plain'} (${mode})`, 100, () => R.flushSyncFromReconciler(() => setTick({ tick: tick++, mode, memo })));
  }
}
log(`[bench] total: ${(now() - t0).toFixed(1)} ms, host nodes created ${nodes}`);
