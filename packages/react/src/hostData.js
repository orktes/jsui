// Game data: shared host state and request / response calls
// (docs/ARCHITECTURE.md "Game data", include/jsui/jsui.h).
//
//   const inv = useHostState('inventory', { items: {} });   // jsui_set_state / jsui_patch_state
//   const gold = useHostStateSelector('player', (p) => p.gold, 0);
//   HostState.get(key) / subscribe(key, cb) / set(key, value) / patch(key, patch) / keys()
//   const res = await Host.call('shop.buy', { item }, { timeout: 5000 });   // on_call -> jsui_resolve_call
//   Host.handle('ui.confirm', async (args) => ...);                            // jsui_call_ui
//
// The values live in the runtime (they survive bundle reloads); this module
// caches each key's parsed value by the runtime's version number, so a key
// is parsed once per change no matter how many components read it, and
// keeps unchanged parts of the new value identical (===) to the old one, so
// React.memo / useMemo consumers skip work. Hosts without the native state
// bridge (react-native-web, plain Node) get an in-memory store with the same
// semantics.

import { useCallback, useEffect, useRef, useState, useSyncExternalStore } from 'react';
import { native, queueMicrotask } from './native.js';
import { reconciler, setUpdatePriority, DiscreteEventPriority } from './renderer.js';

const g = globalThis;
const DEV = typeof process === 'undefined' || !process.env || process.env.NODE_ENV !== 'production';

// --- storage: the native store, or a local fallback --------------------------

const nativeState = typeof native.getState === 'function' && typeof native.stateVersion === 'function';
const nativePatches = nativeState && typeof native.statePatches === 'function';
const local = new Map(); // key -> { version, json }
let localVersion = 1;

function readVersion(key) {
  if (nativeState) return native.stateVersion(key);
  const e = local.get(key);
  return e ? e.version : 0;
}

function readJson(key) {
  if (nativeState) return native.getState(key);
  const e = local.get(key);
  return e ? e.json : undefined;
}

function writeJson(key, json, patch) {
  if (nativeState) return patch ? native.patchState(key, json) : native.setState(key, json);
  if (json === undefined || (patch && json === 'null')) {
    local.delete(key);
    return 0;
  }
  const prev = local.get(key);
  let next = json;
  if (patch) next = JSON.stringify(mergePatch(prev ? JSON.parse(prev.json) : null, JSON.parse(json)));
  if (prev && prev.json === next) return prev.version;
  const version = localVersion++;
  local.set(key, { version, json: next });
  return version;
}

/** RFC 7386 JSON Merge Patch (returns the patched value; `target` is not modified). */
export function mergePatch(target, patch) {
  if (patch === null || typeof patch !== 'object' || Array.isArray(patch)) return patch;
  const out = target !== null && typeof target === 'object' && !Array.isArray(target) ? Object.assign({}, target) : {};
  for (const k of Object.keys(patch)) {
    const v = patch[k];
    if (v === null) delete out[k];
    else out[k] = mergePatch(out[k], v);
  }
  return out;
}

// --- parsed-value cache -------------------------------------------------------

const cache = new Map(); // key -> { version, value }

function isPlainObject(v) {
  return v !== null && typeof v === 'object' && !Array.isArray(v);
}

// Returns `next`, with every part deep-equal to the same part of `prev`
// replaced by prev's (so unchanged sub-trees keep their identity). `next` is
// freshly parsed and owned by us, so it is updated in place.
function share(prev, next) {
  if (prev === next) return prev;
  if (Array.isArray(prev) && Array.isArray(next)) {
    let same = prev.length === next.length;
    for (let i = 0; i < next.length; i++) {
      const v = i < prev.length ? share(prev[i], next[i]) : next[i];
      next[i] = v;
      if (same && v !== prev[i]) same = false;
    }
    return same ? prev : next;
  }
  if (isPlainObject(prev) && isPlainObject(next)) {
    const pk = Object.keys(prev);
    const nk = Object.keys(next);
    let same = pk.length === nk.length;
    for (let i = 0; i < nk.length; i++) {
      const k = nk[i];
      if (same && pk[i] !== k) same = false;
      if (!Object.prototype.hasOwnProperty.call(prev, k)) {
        same = false;
        continue;
      }
      const v = share(prev[k], next[k]);
      next[k] = v;
      if (same && v !== prev[k]) same = false;
    }
    return same ? prev : next;
  }
  return next;
}

// Values are shared between every reader: mutating one would silently
// diverge from the runtime's copy, so development builds freeze them.
function freeze(v) {
  if (v === null || typeof v !== 'object' || Object.isFrozen(v)) return v;
  Object.freeze(v);
  for (const k of Object.keys(v)) freeze(v[k]);
  return v;
}

function parse(key, json) {
  try {
    return JSON.parse(json);
  } catch (e) {
    if (g.console) console.error(`[jsui] HostState: value of "${key}" is not valid JSON: ${e && e.message}`);
    return undefined;
  }
}

function getValue(key) {
  const version = readVersion(key);
  const entry = cache.get(key);
  if (entry && entry.version === version) return entry.value;
  if (!version) {
    cache.delete(key);
    return undefined;
  }
  let value;
  // The runtime keeps the merge patches since recent versions: applying the
  // few small patches to the cached value avoids serializing and re-parsing
  // the whole document (a big inventory changing one slot per frame).
  const patches = entry && entry.value !== undefined && nativePatches ? native.statePatches(key, entry.version) : undefined;
  if (patches) {
    value = entry.value;
    for (let i = 0; i < patches.length; i++) value = mergePatch(value, parse(key, patches[i]));
  } else {
    const json = readJson(key);
    value = json === undefined ? undefined : parse(key, json);
  }
  if (entry) value = share(entry.value, value);
  if (DEV) freeze(value);
  cache.set(key, { version, value });
  return value;
}

// --- subscriptions --------------------------------------------------------------

const listeners = new Map(); // key -> Set<cb>; '*' gets every key

function batched(fn) {
  // Sync priority, like host messages: the update renders in the microtask
  // checkpoint right after this native -> JS call, in the same tick.
  const prev = setUpdatePriority(DiscreteEventPriority);
  try {
    return reconciler.batchedUpdates(fn, null);
  } finally {
    setUpdatePriority(prev);
  }
}

function reportListenerError(e) {
  if (typeof g.__jsui_reportError === 'function') g.__jsui_reportError(e);
  else if (typeof g.reportError === 'function') g.reportError(e);
  else setTimeout(() => {
    throw e;
  }, 0);
}

function notify(keys) {
  const any = listeners.get('*');
  const work = [];
  for (const key of keys) {
    const set = listeners.get(key);
    if (set && set.size) for (const cb of Array.from(set)) work.push([cb, key]);
    if (any && any.size) for (const cb of Array.from(any)) work.push([cb, key]);
  }
  if (!work.length) return;
  batched(() => {
    for (const [cb, key] of work) {
      try {
        cb(getValue(key), key);
      } catch (e) {
        reportListenerError(e);
      }
    }
  });
}

/** Called by the runtime once per tick with the keys the embedder changed. */
function onHostState(keys) {
  notify(Array.isArray(keys) ? keys.map(String) : [String(keys)]);
}

function toJson(what, key, value) {
  if (value === undefined) return undefined;
  const json = JSON.stringify(value);
  if (json === undefined) throw new TypeError(`HostState.${what}("${key}"): value is not JSON-serializable`);
  return json;
}

export const HostState = {
  /** Current value of `key` (undefined when unset). Parsed once per change; treat it as immutable. */
  get(key) {
    return getValue(String(key));
  },
  /** Calls `cb(value, key)` after each change of `key` ('*': every key). Returns an unsubscribe function. */
  subscribe(key, cb) {
    key = String(key);
    let set = listeners.get(key);
    if (!set) listeners.set(key, (set = new Set()));
    set.add(cb);
    return () => {
      set.delete(cb);
      if (!set.size && listeners.get(key) === set) listeners.delete(key);
    };
  },
  /** Sets a UI-owned key (undefined removes it). The embedder is told through JsuiConfig.on_state. */
  set(key, value) {
    key = String(key);
    const before = readVersion(key);
    if (writeJson(key, toJson('set', key, value), false) !== before) notify([key]);
  },
  /** Applies an RFC 7386 merge patch to `key` (null members delete). */
  patch(key, patch) {
    key = String(key);
    const before = readVersion(key);
    if (writeJson(key, toJson('patch', key, patch === undefined ? null : patch), true) !== before) notify([key]);
  },
  /** Keys that currently have a value. */
  keys() {
    if (nativeState && typeof native.stateKeys === 'function') return native.stateKeys();
    return Array.from(local.keys());
  },
};

/**
 * The value of shared host state `key` (jsui_set_state / jsui_patch_state /
 * HostState.set), or `fallback` while it is unset. Re-renders when `key`
 * changes, and only then.
 */
export function useHostState(key, fallback) {
  key = String(key);
  const subscribe = useCallback((cb) => HostState.subscribe(key, cb), [key]);
  const read = () => getValue(key);
  const value = useSyncExternalStore(subscribe, read, read);
  return value === undefined ? fallback : value;
}

/**
 * `selector(value of key)`; re-renders only when the selected result changes
 * (Object.is). Unchanged parts of a value keep their identity across updates,
 * so selecting a sub-object is fine: `useHostStateSelector('player', (p) => p.stats)`.
 * The selector receives `fallback` while the key is unset.
 */
export function useHostStateSelector(key, selector, fallback) {
  key = String(key);
  const subscribe = useCallback((cb) => HostState.subscribe(key, cb), [key]);
  const memo = useRef(null);
  const read = () => {
    let v = getValue(key);
    if (v === undefined) v = fallback;
    const m = memo.current;
    if (m && m.input === v && m.selector === selector) return m.output;
    const output = selector(v);
    if (m && Object.is(m.output, output)) {
      m.input = v;
      m.selector = selector;
      return m.output;
    }
    memo.current = { input: v, selector, output };
    return output;
  };
  return useSyncExternalStore(subscribe, read, read);
}

// --- calls: UI -> host -------------------------------------------------------------

function callError(name, message, code) {
  const e = new Error(message || 'rejected');
  e.name = 'HostCallError';
  e.call = name;
  if (code) e.code = code;
  return e;
}

/**
 * Calls the embedder (JsuiConfig.on_call) and resolves with the JSON value it
 * passes to jsui_resolve_call, or rejects with its jsui_reject_call message.
 * Options: `timeout` (ms; rejects with code 'timeout' and tells the host the
 * call was cancelled) and `signal` (an AbortSignal; rejects with its reason).
 */
export function call(name, args, options) {
  name = String(name);
  return new Promise((resolve, reject) => {
    if (typeof native.hostCall !== 'function') {
      reject(callError(name, `Host.call("${name}"): this host has no call bridge`, 'unavailable'));
      return;
    }
    let json;
    try {
      json = args === undefined ? 'null' : JSON.stringify(args);
      if (json === undefined) throw new TypeError(`Host.call("${name}"): arguments are not JSON-serializable`);
    } catch (e) {
      reject(e);
      return;
    }
    const signal = options && options.signal;
    if (signal && signal.aborted) {
      reject(signal.reason !== undefined ? signal.reason : callError(name, 'aborted', 'aborted'));
      return;
    }
    let done = false;
    let timer = 0;
    let id = 0;
    let onAbort = null;
    const finish = () => {
      done = true;
      if (timer) clearTimeout(timer);
      if (onAbort) signal.removeEventListener('abort', onAbort);
    };
    const settle = (ok, payload) => {
      if (done) return;
      finish();
      if (!ok) {
        reject(callError(name, payload));
        return;
      }
      let value;
      try {
        value = payload === '' || payload === undefined ? undefined : JSON.parse(payload);
      } catch (e) {
        reject(e);
        return;
      }
      resolve(value);
    };
    const cancel = (error) => {
      if (done) return;
      finish();
      if (id && typeof native.cancelCall === 'function') native.cancelCall(id);
      reject(error);
    };
    id = native.hostCall(name, json, settle);
    if (done) return; // answered from inside on_call
    if (!id) {
      settle(false, `Host.call("${name}"): the host does not take calls (JsuiConfig.on_call is not set)`);
      return;
    }
    const timeout = options && +options.timeout;
    if (timeout > 0) {
      timer = setTimeout(() => cancel(callError(name, `Host.call("${name}") timed out after ${timeout} ms`, 'timeout')), timeout);
    }
    if (signal) {
      onAbort = () => cancel(signal.reason !== undefined ? signal.reason : callError(name, 'aborted', 'aborted'));
      signal.addEventListener('abort', onAbort);
    }
  });
}

// --- calls: host -> UI -------------------------------------------------------------

const handlers = new Map(); // name -> fn

/**
 * Handles jsui_call_ui(name, args): `fn(args)` may return a value or a
 * promise; its JSON result (or the error message) goes back to the embedder.
 * One handler per name (a later one replaces it). Returns an unregister function.
 */
export function handle(name, fn) {
  name = String(name);
  if (typeof fn !== 'function') throw new TypeError(`Host.handle("${name}", fn): fn must be a function`);
  handlers.set(name, fn);
  return () => {
    if (handlers.get(name) === fn) handlers.delete(name);
  };
}

/** Registers a Host.handle handler while the component is mounted (always calls the latest `fn`). */
export function useHostHandler(name, fn) {
  const [box] = useState(() => ({ current: fn }));
  box.current = fn;
  useEffect(() => handle(name, (args) => box.current(args)), [name, box]);
}

function errorMessage(e) {
  if (e && typeof e === 'object' && typeof e.message === 'string') return e.message;
  return String(e);
}

function onCall(id, name, json) {
  const fail = (e) => native.callResult(id, false, errorMessage(e));
  const fn = handlers.get(name);
  if (!fn) {
    fail(`no UI handler for "${name}" (Host.handle)`);
    return;
  }
  let result;
  try {
    const args = json === undefined || json === '' ? undefined : JSON.parse(json);
    result = batched(() => fn(args));
  } catch (e) {
    fail(e);
    return;
  }
  const done = (v) => {
    let out;
    try {
      out = v === undefined ? 'null' : JSON.stringify(v);
      if (out === undefined) throw new TypeError(`handler result for "${name}" is not JSON-serializable`);
    } catch (e) {
      fail(e);
      return;
    }
    native.callResult(id, true, out);
  };
  // A plain result settles on the runtime's microtask queue (in this tick on
  // every JS engine); a promise when it resolves.
  if (result !== null && (typeof result === 'object' || typeof result === 'function') && typeof result.then === 'function')
    Promise.resolve(result).then(done, fail);
  else queueMicrotask(() => done(result));
}

export function installHostData() {
  g.__jsui_onHostState = onHostState;
  g.__jsui_onCall = onCall;
}
