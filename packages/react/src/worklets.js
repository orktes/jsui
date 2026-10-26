// react-native-worklets on JSUI (the CLI aliases the package here; see
// docs/ARCHITECTURE.md "Worklets").
//
// A worklet is a function with a 'worklet' directive (or a callback passed
// straight to runOnUI / scheduleOnUI / runOnRuntime / ...). The CLI's build
// transform (packages/cli/src/worklets.js, Software Mansion's Babel plugin)
// turns it into a function that also carries its code, a hash and the
// values it captured (__initData, __workletHash, __closure). Scheduling one
// serializes it with its arguments into a JSON "wire" text (pack below; the
// worklet runtime's half is src/js/worklets_prelude.cpp, keep the two in
// step) and hands it to __jsui_native.worklets: the runtime evaluates the
// code in a worklet runtime of its own (the UI runtime: a QuickJS context
// on JSUI's UI thread, run in the frame's tick; createWorkletRuntime: more
// of them) with the captured values copied over.
//
// What crosses (react-native-worklets' rules): primitives, plain objects and
// arrays (copied), worklets, Map / Set / RegExp / Error, ArrayBuffers and
// typed arrays (copied), synchronizables (shared), worklet runtimes, and any
// other function as a remote function (called back on this runtime through
// runOnJS / scheduleOnRN, never synchronously). JSUI adds Animated values:
// one that reaches a worklet is made native (as with useNativeDriver) and
// reads / writes the native node directly from the worklet (getValue(),
// setValue(v), the `value` property), so a UI-runtime worklet can drive an
// animation every frame with no JS-thread round trip.
//
// Without the native module (the host JS engine on the web, Node tests)
// everything runs on this runtime, as react-native-worklets does on the web:
// runOnUI on the next animation frame, runOnJS in a microtask.
//
// API and semantics follow react-native-worklets 0.13 (MIT, Copyright (c)
// 2016 Software Mansion <swmansion.com>; see docs/THIRD_PARTY_NOTICES.md).

import { native as bridge, queueMicrotask, requestFrame } from './native.js';

const g = globalThis;
const N = bridge.worklets && typeof bridge.worklets.schedule === 'function' ? bridge.worklets : null;

if (g.__RUNTIME_KIND === undefined) g.__RUNTIME_KIND = 1;
if (g._WORKLET === undefined) g._WORKLET = false;

/** The kinds of runtime (`globalThis.__RUNTIME_KIND`). */
export const RuntimeKind = Object.freeze({ ReactNative: 1, UI: 2, Worker: 3, 1: 'ReactNative', 2: 'UI', 3: 'Worker' });
/** The UI runtime's id (runOnRuntimeSyncWithId / scheduleOnRuntimeWithId). */
export const UIRuntimeId = 2;

const reportError = (e) => (typeof g.reportError === 'function' ? g.reportError(e) : console.error(e));

// ------------------------------------------------------------------ checks

/** True for a function the worklets transform marked (it carries __workletHash). */
export function isWorkletFunction(value) {
  return typeof value === 'function' && !!value.__workletHash;
}
export const getRuntimeKind = () => g.__RUNTIME_KIND;
export const isRNRuntime = () => g.__RUNTIME_KIND === 1;
export const isUIRuntime = () => g.__RUNTIME_KIND === 2;
export const isWorkerRuntime = () => g.__RUNTIME_KIND === 3;
export const isWorkletRuntime = () => g.__RUNTIME_KIND !== 1;
export const isSynchronizable = (v) => typeof v === 'object' && v !== null && !!v.__synchronizableRef;
/** Bundle Mode (react-native-worklets' Metro feature) does not exist here. */
export const isBundleModeEnabled = () => false;

function requireWorklet(fn, what) {
  if (!isWorkletFunction(fn)) {
    throw new Error(
      `[Worklets] The function passed to \`${what}\` is not a worklet. Mark it with a 'worklet' directive ` +
        '(the jsui CLI transforms worklets when it builds the bundle).',
    );
  }
}

// -------------------------------------------------------------- wire format

const U = { $: 'u' };
const builtinNames = new Map();  // react-native-worklets function -> name (filled at the end)
const builtinsByName = new Map();
const remotes = new Map();  // remote function id -> { fn, refs }
const remoteIds = new WeakMap();
let nextRemote = 1;
const animatedByTag = new Map();  // native tag -> WeakRef(Animated value) (values coming back)
const customSerializables = [];  // { name, determine, pack, unpack }
const runtimes = new Map();  // id -> runtime object
// Worklet code each runtime has (hash -> sent): only the first job carries it.
const sentCode = new Map();
const WORKLET_KEYS = new Set(['__closure', '__workletHash', '__initData', '__pluginVersion', '__stackDetails']);
const Weak = typeof g.WeakRef === 'function' ? g.WeakRef : null;

const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
function toB64(b) {
  let s = '';
  let i = 0;
  for (; i + 2 < b.length; i += 3) {
    const n = (b[i] << 16) | (b[i + 1] << 8) | b[i + 2];
    s += B64[n >> 18] + B64[(n >> 12) & 63] + B64[(n >> 6) & 63] + B64[n & 63];
  }
  if (i < b.length) {
    const n = (b[i] << 16) | ((i + 1 < b.length ? b[i + 1] : 0) << 8);
    s += B64[n >> 18] + B64[(n >> 12) & 63] + (i + 1 < b.length ? B64[(n >> 6) & 63] : '=') + '=';
  }
  return s;
}
function fromB64(s) {
  let len = (s.length / 4) * 3;
  if (s.endsWith('==')) len -= 2;
  else if (s.endsWith('=')) len -= 1;
  const out = new Uint8Array(len);
  let o = 0;
  for (let i = 0; i < s.length; i += 4) {
    const n = (B64.indexOf(s[i]) << 18) | (B64.indexOf(s[i + 1]) << 12) | ((B64.indexOf(s[i + 2]) & 63) << 6) | (B64.indexOf(s[i + 3]) & 63);
    if (o < len) out[o++] = n >> 16;
    if (o < len) out[o++] = (n >> 8) & 255;
    if (o < len) out[o++] = n & 255;
  }
  return out;
}

const isAnimatedValue = (v) =>
  typeof v.__makeNative === 'function' && typeof v.__getNativeTag === 'function' && typeof v.setOffset === 'function' && !('x' in v);

function codeSent(rid) {
  let s = sentCode.get(rid);
  if (!s) sentCode.set(rid, (s = new Set()));
  return s;
}

/**
 * The wire form of `v` for runtime `rid` (0: any runtime, e.g. a
 * synchronizable's value: worklet code always included).
 */
function pack(v, rid, depth) {
  if (depth > 30) throw new Error('[Worklets] Trying to convert a cyclic object to a serializable. This is not supported.');
  switch (typeof v) {
    case 'undefined':
      return U;
    case 'number':
      return Number.isFinite(v) && !Object.is(v, -0) ? v : { $: 'n', v: Object.is(v, -0) ? '-0' : String(v) };
    case 'string':
    case 'boolean':
      return v;
    case 'bigint':
      return { $: 'bi', v: String(v) };
    case 'symbol':
      return String(v);
    case 'function': {
      if (isWorkletFunction(v)) return packWorklet(v, rid, depth);
      const b = builtinNames.get(v);
      if (b) return { $: 'b', n: b };
      // Any other function: a remote function (called back here).
      let id = remoteIds.get(v);
      let e = id && remotes.get(id);
      if (!e) {
        id = nextRemote++;
        e = { fn: v, refs: 0 };
        remotes.set(id, e);
        remoteIds.set(v, id);
      }
      e.refs++;
      return v.name ? { $: 'f', id, n: v.name } : { $: 'f', id };
    }
  }
  if (v === null) return null;
  if (Array.isArray(v)) return v.map((x) => pack(x, rid, depth + 1));
  if (v.__synchronizableRef && v.__syncId) {
    N.syncRetain(v.__syncId);
    return { $: 's', id: v.__syncId, fx: !!v.setDirty };
  }
  if (v.__workletRuntime) return { $: 'rt', id: v.runtimeId, name: v.name };
  if (isAnimatedValue(v)) {
    v.__makeNative();
    let t;
    try {
      t = v.__getNativeTag();
    } catch (e) {
      console.warn('[Worklets] An Animated value can only reach a worklet where the native Animated driver exists.');
      return U;
    }
    if (Weak) animatedByTag.set(t, new Weak(v));
    return { $: 'av', t };
  }
  const proto = Object.getPrototypeOf(v);
  if (proto === Object.prototype || proto === null) return { $: 'o', v: packProps(v, rid, depth) };
  if (v instanceof Error) return { $: 'e', name: v.name, message: v.message, stack: v.stack };
  if (v instanceof ArrayBuffer) return { $: 'ab', d: toB64(new Uint8Array(v)) };
  if (ArrayBuffer.isView(v)) return { $: 'tv', k: v.constructor.name, d: toB64(new Uint8Array(v.buffer, v.byteOffset, v.byteLength)) };
  if (v instanceof Map) {
    const k = [];
    const vals = [];
    for (const [a, b] of v) {
      k.push(pack(a, rid, depth + 1));
      vals.push(pack(b, rid, depth + 1));
    }
    return { $: 'm', k, v: vals };
  }
  if (v instanceof Set) return { $: 'set', v: [...v].map((x) => pack(x, rid, depth + 1)) };
  if (v instanceof RegExp) return { $: 're', s: v.source, f: v.flags };
  for (const c of customSerializables) {
    if (c.determine(v)) return { $: 'cu', u: pack(c.unpack, rid, depth + 1), v: pack(c.pack(v), rid, depth + 1) };
  }
  // Animated.ValueXY and other objects made of Animated values: their own
  // properties (x, y: native values).
  if (typeof v.__makeNative === 'function' && v.x && v.y) return { $: 'o', v: { x: pack(v.x, rid, depth + 1), y: pack(v.y, rid, depth + 1) } };
  console.warn(`[Worklets] Cannot copy value of type \`${(v.constructor && v.constructor.name) || 'unknown'}\`.`);
  return U;
}

function packProps(v, rid, depth) {
  const o = {};
  for (const k of Object.keys(v)) o[k] = pack(v[k], rid, depth + 1);
  return o;
}

function packWorklet(v, rid, depth) {
  // An Error (react-native-worklets keeps it for dev stack traces): it
  // cannot be copied, and is not needed once the worklet leaves.
  if (v.__stackDetails) delete v.__stackDetails;
  const h = v.__workletHash;
  const w = { $: 'w', h };
  const sent = rid ? codeSent(rid) : null;
  if (!sent || !sent.has(h)) {
    w.i = v.__initData;
    if (sent) sent.add(h);
  }
  if (v.__closure) w.c = v.__closure.map((x) => pack(x, rid, depth + 1));
  let p = null;
  for (const k of Object.keys(v)) {
    if (WORKLET_KEYS.has(k)) continue;
    (p || (p = {}))[k] = pack(v[k], rid, depth + 1);
  }
  if (p) w.p = p;
  return w;
}

// Worklets coming back from a worklet runtime (a worklet passed to
// scheduleOnRN, a value returned by one) are evaluated here once per hash,
// like react-native-worklets' valueUnpacker.
const codeCache = new Map();
function unpackWorklet(w) {
  let fn = codeCache.get(w.h);
  if (!fn) {
    if (!w.i || typeof w.i.code !== 'string') throw new Error('[Worklets] A worklet without code reached the RN runtime.');
    fn = (0, eval)('(' + w.i.code + '\n)');
    codeCache.set(w.h, fn);
  }
  const closure = w.c ? w.c.map(unpack) : undefined;
  const holder = { __closure: closure, __workletHash: w.h, __initData: w.i };
  if (w.p) for (const k in w.p) holder[k] = unpack(w.p[k]);
  const f = fn.bind(holder);
  holder._recur = f;
  f.__workletHash = w.h;
  f.__initData = w.i;
  if (closure) f.__closure = closure;
  return f;
}

function unpack(w) {
  if (w === null || typeof w !== 'object') return w;
  if (Array.isArray(w)) return w.map(unpack);
  switch (w.$) {
    case 'u':
      return undefined;
    case 'n':
      return w.v === '-0' ? -0 : Number(w.v);
    case 'bi':
      return BigInt(w.v);
    case 'o': {
      const o = {};
      for (const k in w.v) o[k] = unpack(w.v[k]);
      return o;
    }
    case 'w':
      return unpackWorklet(w);
    case 'f': {
      const e = remotes.get(w.id);
      return e ? e.fn : () => console.warn('[Worklets] A remote function was called after it was released.');
    }
    case 's':
      return makeSynchronizable(w.id, !!w.fx);
    case 'av': {
      const ref = animatedByTag.get(w.t);
      return ref ? ref.deref() : undefined;
    }
    case 'b':
      return builtinsByName.get(w.n);
    case 'e': {
      const C = typeof g[w.name] === 'function' && (g[w.name] === Error || g[w.name].prototype instanceof Error) ? g[w.name] : Error;
      const e = new C(w.message);
      if (w.name) e.name = w.name;
      if (w.stack) e.stack = w.stack;
      return e;
    }
    case 'ab':
      return fromB64(w.d).buffer;
    case 'tv': {
      const b = fromB64(w.d);
      if (w.k === 'DataView') return new DataView(b.buffer);
      const C = g[w.k];
      return new C(b.buffer, 0, b.byteLength / C.BYTES_PER_ELEMENT);
    }
    case 'm':
      return new Map(w.k.map((k, i) => [unpack(k), unpack(w.v[i])]));
    case 'set':
      return new Set(w.v.map(unpack));
    case 're':
      return new RegExp(w.s, w.f);
    case 'rt':
      return runtimeObject(w.id, w.name);
  }
  return undefined;
}

// What the worklet runtimes send back, once per tick:
// ['c', fn, args] call, ['r', id, n] / ['a', id] release / retain a remote
// function, ['p', promiseId, ok, value] settle a runOn*Async promise.
const promises = new Map();
let nextPromise = 1;
function onWorklets(json) {
  for (const op of JSON.parse(json)) {
    try {
      switch (op[0]) {
        case 'c': {
          const fn = unpack(op[1]);
          if (typeof fn === 'function') fn(...unpack(op[2]));
          break;
        }
        case 'r': {
          const e = remotes.get(op[1]);
          if (e && (e.refs -= op[2]) <= 0) {
            remotes.delete(op[1]);
            remoteIds.delete(e.fn);
          }
          break;
        }
        case 'a': {
          const e = remotes.get(op[1]);
          if (e) e.refs++;
          break;
        }
        case 'p': {
          const p = promises.get(op[1]);
          promises.delete(op[1]);
          if (p) (op[2] ? p.resolve : p.reject)(unpack(op[3]));
          break;
        }
      }
    } catch (e) {
      reportError(e);
    }
  }
}
if (N) g.__jsui_onWorklets = onWorklets;

// ------------------------------------------------------------- scheduling

function job(rid, worklet, args, promiseId) {
  return JSON.stringify([pack(worklet, rid, 0), pack(args, rid, 0), promiseId || 0]);
}

// Without the native module: one queue, run on the next animation frame
// (react-native-worklets on the web).
let fallbackQueue = [];
function fallbackSchedule(worklet, args, resolve, reject) {
  fallbackQueue.push([worklet, args, resolve, reject]);
  if (fallbackQueue.length === 1) {
    queueMicrotask(() => {
      const queue = fallbackQueue;
      fallbackQueue = [];
      requestFrame(() => {
        for (const [w, a, res, rej] of queue) {
          try {
            const r = w(...a);
            if (res) res(r);
          } catch (e) {
            if (rej) rej(e);
            else reportError(e);
          }
        }
      });
    });
  }
}

function scheduleOn(rid, worklet, args, resolve, reject) {
  if (!N) return fallbackSchedule(worklet, args, resolve, reject);
  let pid = 0;
  if (resolve) {
    pid = nextPromise++;
    promises.set(pid, { resolve, reject });
  }
  N.schedule(rid, job(rid, worklet, args, pid));
}

function runSyncOn(rid, worklet, args) {
  if (!N) return worklet(...args);
  const r = N.runSync(rid, job(rid, worklet, args, 0));
  if (r == null) {
    sentCode.delete(rid);  // the job may not have run: send the code again
    throw new Error('[Worklets] The worklet runtime did not answer in time (the UI thread is busy or not ticking).');
  }
  const o = JSON.parse(r);
  if (!o.ok) throw unpack(o.e);
  return unpack(o.v);
}

/** Runs a worklet on the UI runtime in its next frame (asynchronously, no result). */
export function scheduleOnUI(worklet, ...args) {
  requireWorklet(worklet, 'scheduleOnUI');
  scheduleOn(UIRuntimeId, worklet, args);
}

/** @deprecated react-native-worklets: use scheduleOnUI. Returns a function that schedules `worklet` with its arguments. */
export function runOnUI(worklet) {
  requireWorklet(worklet, 'runOnUI');
  return (...args) => scheduleOn(UIRuntimeId, worklet, args);
}

/** Runs a worklet on the UI runtime and resolves with what it returns. */
export function runOnUIAsync(worklet, ...args) {
  requireWorklet(worklet, 'runOnUIAsync');
  return new Promise((resolve, reject) => scheduleOn(UIRuntimeId, worklet, args, resolve, reject));
}

/**
 * Runs a worklet on the UI runtime now and returns its result (throws what
 * it throws). With JS on its own thread this waits for the UI thread (up to
 * Runtime::kProxyTimeoutMs).
 */
export function runOnUISync(worklet, ...args) {
  requireWorklet(worklet, 'runOnUISync');
  return runSyncOn(UIRuntimeId, worklet, args);
}

export function executeOnUIRuntimeSync(worklet) {
  return (...args) => runOnUISync(worklet, ...args);
}

/** Calls `fun` on this (the RN) runtime in a microtask. In a worklet: back on the RN runtime. */
export function scheduleOnRN(fun, ...args) {
  queueMicrotask(args.length ? () => fun(...args) : fun);
}

/** @deprecated react-native-worklets: use scheduleOnRN. */
export function runOnJS(fun) {
  return (...args) => scheduleOnRN(fun, ...args);
}

function runtimeObject(id, name) {
  let rt = runtimes.get(id);
  if (!rt) {
    rt = Object.freeze({ __workletRuntime: true, name: name || 'anonymous', runtimeId: id, toString: () => `[WorkletRuntime ${name}]` });
    runtimes.set(id, rt);
  }
  return rt;
}

/**
 * A worklet runtime of its own (`createWorkletRuntime(name, initializer)`
 * or `createWorkletRuntime({ name, initializer })`). On JSUI it runs on the
 * UI thread's tick like the UI runtime (separate heap, own timers and
 * requestAnimationFrame), not on a thread of its own.
 */
export function createWorkletRuntime(nameOrConfig, initializer) {
  const config = typeof nameOrConfig === 'object' && nameOrConfig !== null ? nameOrConfig : {};
  const name = typeof nameOrConfig === 'string' ? nameOrConfig : config.name || 'anonymous';
  const init = typeof nameOrConfig === 'string' || nameOrConfig === undefined ? initializer : config.initializer;
  if (init && !isWorkletFunction(init)) throw new Error('[Worklets] The initializer passed to `createWorkletRuntime` is not a worklet.');
  const id = N ? N.createRuntime(name) : 3 + runtimes.size;
  const rt = runtimeObject(id, name);
  if (init) scheduleOn(id, init, []);
  return rt;
}

const runtimeId = (rt) => {
  if (!rt || typeof rt.runtimeId !== 'number') throw new Error('[Worklets] Expected a worklet runtime (createWorkletRuntime).');
  return rt.runtimeId;
};

export function scheduleOnRuntime(workletRuntime, worklet, ...args) {
  requireWorklet(worklet, 'scheduleOnRuntime');
  scheduleOn(runtimeId(workletRuntime), worklet, args);
}
export function scheduleOnRuntimeWithId(id, worklet, ...args) {
  requireWorklet(worklet, 'scheduleOnRuntimeWithId');
  scheduleOn(id, worklet, args);
}
/** @deprecated react-native-worklets: use scheduleOnRuntime. */
export function runOnRuntime(workletRuntime, worklet) {
  requireWorklet(worklet, 'runOnRuntime');
  const id = runtimeId(workletRuntime);
  return (...args) => scheduleOn(id, worklet, args);
}
export function runOnRuntimeSync(workletRuntime, worklet, ...args) {
  requireWorklet(worklet, 'runOnRuntimeSync');
  return runSyncOn(runtimeId(workletRuntime), worklet, args);
}
export function runOnRuntimeSyncWithId(id, worklet, ...args) {
  requireWorklet(worklet, 'runOnRuntimeSyncWithId');
  return runSyncOn(id, worklet, args);
}
export function runOnRuntimeAsync(workletRuntime, worklet, ...args) {
  requireWorklet(worklet, 'runOnRuntimeAsync');
  const id = runtimeId(workletRuntime);
  return new Promise((resolve, reject) => scheduleOn(id, worklet, args, resolve, reject));
}
export function runOnRuntimeAsyncWithId(id, worklet, ...args) {
  requireWorklet(worklet, 'runOnRuntimeAsyncWithId');
  return new Promise((resolve, reject) => scheduleOn(id, worklet, args, resolve, reject));
}

// ---------------------------------------------------------------- memory

/**
 * react-native-worklets returns a reference to a native copy here; on JSUI
 * values are copied when a job is sent, so this returns `value` itself
 * (pass it to a worklet as before).
 */
export function createSerializable(value) {
  return value;
}
export const makeShareable = createSerializable;
export const makeShareableCloneRecursive = createSerializable;
export const makeShareableCloneOnUIRecursive = createSerializable;
export const isSerializableRef = () => false;
export const isShareableRef = isSerializableRef;
export const isShareable = () => false;
const noCache = Object.freeze({ get: () => undefined, set: () => {} });
export const serializableMappingCache = noCache;
export const shareableMappingCache = noCache;

/**
 * Lets values of a class cross runtimes: `determine(value)` says whether
 * the registration handles it, `pack(value)` turns it into something
 * serializable, `unpack(packed)` (run on the receiving runtime) rebuilds it.
 * All three must be worklets.
 */
export function registerCustomSerializable({ name, determine, pack: packFn, unpack: unpackFn }) {
  for (const f of [determine, packFn, unpackFn])
    if (!isWorkletFunction(f)) throw new Error('[Worklets] registerCustomSerializable takes worklets (determine, pack, unpack).');
  if (customSerializables.some((c) => c.name === name)) return;
  customSerializables.push({ name, determine, pack: packFn, unpack: unpackFn });
}

/** react-native-worklets' Shareables need its Bundle Mode, which JSUI has no equivalent of. */
export function createShareable() {
  throw new Error('[Worklets] createShareable is not supported on JSUI (it needs Bundle Mode).');
}

const syncRegistry = N && typeof g.FinalizationRegistry === 'function' ? new g.FinalizationRegistry((id) => N.syncRelease(id)) : null;

function makeSynchronizable(id, fixed) {
  const get = (blocking) => unpack(JSON.parse(N.syncGet(id, blocking)));
  const set = (v, blocking) => N.syncSet(id, JSON.stringify(pack(v, 0, 0)), blocking);
  const s = {
    __synchronizableRef: true,
    __syncId: id,
    getDirty: () => get(false),
    getBlocking: () => get(true),
    setBlocking(v) {
      if (typeof v !== 'function') return set(v, true);
      const locked = N.syncLock(id);
      try {
        set(v(get(true)), true);
      } finally {
        if (locked) N.syncUnlock(id);
      }
    },
    lock() {
      if (!N.syncLock(id)) throw new Error('[Worklets] Synchronizable lock timed out');
    },
    unlock: () => N.syncUnlock(id),
  };
  if (fixed) s.setDirty = (v) => set(v, false);
  if (syncRegistry) syncRegistry.register(s, id);
  return s;
}

// Without the native module: a plain object (one runtime, nothing to lock).
function localSynchronizable(initial, fixed) {
  let value = initial;
  const s = {
    __synchronizableRef: true,
    getDirty: () => value,
    getBlocking: () => value,
    setBlocking: (v) => {
      value = typeof v === 'function' ? v(value) : v;
    },
    lock() {},
    unlock() {},
  };
  if (fixed) s.setDirty = (v) => (value = v);
  return s;
}

/**
 * A value shared by every runtime, read and written synchronously from any
 * of them (getDirty / getBlocking / setBlocking / lock / unlock; setDirty
 * with `{ fixedType: true }`). Values are copied in and out.
 */
export function createSynchronizable(initialValue, config) {
  const fixed = !!(config && config.fixedType);
  if (fixed && typeof initialValue !== 'number' && typeof initialValue !== 'boolean')
    throw new Error('[Worklets] `fixedType` requires a number or boolean initial value.');
  if (!N) return localSynchronizable(initialValue, fixed);
  return makeSynchronizable(N.syncCreate(JSON.stringify(pack(initialValue, 0, 0))), fixed);
}

// ------------------------------------------------------------------- misc

/** react-native-worklets drains its microtask queue here; QuickJS does on its own. */
export function callMicrotasks() {}
export function getCurrentThreadId() {
  return 'jsui-js';
}
export const getStaticFeatureFlag = () => false;
export const getDynamicFeatureFlag = () => false;
export const setDynamicFeatureFlag = () => {};
export const toggleSlowAnimationsOnUIRuntime = () => {};
export const getUIRuntimeHolder = () => ({});
export const getUISchedulerHolder = () => ({});
/** The native module's presence (null without it: everything runs on this runtime). */
export const WorkletsModule = N;

// The functions a worklet may call: captured in a closure, they are sent as
// references to the worklet runtime's own versions (worklets_prelude.cpp).
for (const [name, fn] of Object.entries({
  scheduleOnRN,
  runOnJS,
  scheduleOnUI,
  runOnUI,
  runOnUISync,
  executeOnUIRuntimeSync,
  runOnUIAsync,
  scheduleOnRuntime,
  runOnRuntime,
  scheduleOnRuntimeWithId,
  getRuntimeKind,
  isRNRuntime,
  isUIRuntime,
  isWorkerRuntime,
  isWorkletRuntime,
  isWorkletFunction,
  isSynchronizable,
  isShareable,
  isSerializableRef,
  createSerializable,
  makeShareable,
  makeShareableCloneRecursive,
  makeShareableCloneOnUIRecursive,
  createSynchronizable,
  callMicrotasks,
  getCurrentThreadId,
})) {
  if (!builtinNames.has(fn)) builtinNames.set(fn, name);
  builtinsByName.set(name, fn);
}

/** For tests: the wire codec and the bookkeeping behind it. */
export const __internal = { pack, unpack, onWorklets, remotes, sentCode };
