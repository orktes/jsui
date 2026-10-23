// Frame data: typed arrays of numbers the game pushes every frame without
// JSON (docs/ARCHITECTURE.md "Frame data", include/jsui/jsui.h).
//
//   const count = useHostBufferSelector('enemies', (b) => b.length / 4, 0);  // re-renders when the count changes
//   const x = useHostBufferValue('enemies', i * 4);   // Animated node: the runtime moves the view, no JS per frame
//                                                      // (Animated.hostBuffer, animated/index.js)
//   const b = useHostBuffer('enemies');                // Float32Array, re-renders on every change
//   useHostBufferListener('enemies', (b) => ...);      // per change, no re-render
//   HostBuffer.get(key) / version(key) / subscribe(key, cb) / keys()
//
// A buffer is a typed array over the runtime's own memory (no copy, no
// parsing). The same object comes back until the buffer's length, type or
// storage changes, so read it through HostBuffer.get() (or the hooks) each
// time rather than keeping it: storage that moved is detached (length 0).
// Hosts without the native bridge (react-native-web, Node tests) get an
// in-memory store that HostBuffer.set() fills.

import { useCallback, useEffect, useRef, useState, useSyncExternalStore } from 'react';
import { native } from './native.js';
import { reconciler, setUpdatePriority, DiscreteEventPriority } from './renderer.js';

const g = globalThis;
// JsuiBufferType order.
const TYPES = [Float32Array, Float64Array, Int32Array, Uint32Array, Int16Array, Uint16Array, Int8Array, Uint8Array];

const nativeBuffers = typeof native.buffer === 'function' && typeof native.bufferVersion === 'function';
const local = new Map(); // key -> { version, view }
let localVersion = 1;
const cache = new Map(); // key -> { version, view, buffer, type, count }

function readVersion(key) {
  if (nativeBuffers) return native.bufferVersion(key);
  const e = local.get(key);
  return e ? e.version : 0;
}

function getView(key) {
  if (!nativeBuffers) {
    const e = local.get(key);
    return e ? e.view : undefined;
  }
  const version = native.bufferVersion(key);
  const e = cache.get(key);
  if (e && e.version === version) return e.view;
  const info = version ? native.buffer(key) : undefined;
  if (!info) {
    cache.delete(key);
    return undefined;
  }
  const buffer = info[0], type = info[1], count = info[2];
  const view =
    e && e.buffer === buffer && e.type === type && e.count === count ? e.view : new TYPES[type](buffer, 0, count);
  cache.set(key, { version: info[3], view, buffer, type, count });
  return view;
}

// --- subscriptions --------------------------------------------------------------

const listeners = new Map(); // key -> Set<cb>; '*' gets every key

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
  // Discrete priority, like shared state: hook updates render in this tick.
  const prev = setUpdatePriority(DiscreteEventPriority);
  try {
    reconciler.batchedUpdates(() => {
      for (const [cb, key] of work) {
        try {
          cb(getView(key), key);
        } catch (e) {
          reportListenerError(e);
        }
      }
    }, null);
  } finally {
    setUpdatePriority(prev);
  }
}

/** Called by the runtime once per tick with the buffers the embedder changed. */
function onHostBuffer(keys) {
  notify(Array.isArray(keys) ? keys.map(String) : [String(keys)]);
}

export const HostBuffer = {
  /** The typed array of `key` (undefined when unset). Read-only; don't keep it across frames. */
  get(key) {
    return getView(String(key));
  },
  /** Changes with every new contents of `key` (0: unset). */
  version(key) {
    return readVersion(String(key));
  },
  /** Calls `cb(view, key)` after each change of `key` ('*': every key). Returns an unsubscribe function. */
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
  /** Keys that currently have a buffer. */
  keys() {
    if (nativeBuffers && typeof native.bufferKeys === 'function') return native.bufferKeys();
    return Array.from(local.keys());
  },
  /**
   * Sets a buffer from JS (a typed array, copied; undefined removes it). Only
   * on hosts without the native bridge (react-native-web, tests): the JSUI
   * runtime's buffers are written by the game (jsui_set_buffer).
   */
  set(key, view) {
    key = String(key);
    if (nativeBuffers) throw new Error(`HostBuffer.set("${key}"): buffers are set by the host (jsui_set_buffer)`);
    if (view === undefined) local.delete(key);
    else local.set(key, { version: localVersion++, view: view.slice() });
    notify([key]);
  },
};

/** The typed array of `key` (undefined while unset); re-renders on every change. */
export function useHostBuffer(key) {
  key = String(key);
  const subscribe = useCallback((cb) => HostBuffer.subscribe(key, cb), [key]);
  const read = () => readVersion(key);
  useSyncExternalStore(subscribe, read, read);
  return getView(key);
}

/**
 * `selector(view of key)` (`fallback` while unset); re-renders only when the
 * result changes (Object.is), e.g. the number of entries.
 */
export function useHostBufferSelector(key, selector, fallback) {
  key = String(key);
  const subscribe = useCallback((cb) => HostBuffer.subscribe(key, cb), [key]);
  const memo = useRef(null);
  const read = () => {
    const version = readVersion(key);
    const m = memo.current;
    if (m && m.version === version && m.selector === selector) return m.output;
    const view = getView(key);
    const output = view === undefined ? fallback : selector(view);
    if (m && Object.is(m.output, output)) {
      m.version = version;
      m.selector = selector;
      return m.output;
    }
    memo.current = { version, selector, output };
    return output;
  };
  return useSyncExternalStore(subscribe, read, read);
}

/**
 * Calls `cb(view, key)` after each change of `key` while the component is
 * mounted (and once at mount when the buffer exists), without re-rendering:
 * for imperative updates (setNativeProps, Animated values, canvas-like drawing).
 */
export function useHostBufferListener(key, cb) {
  const [box] = useState(() => ({ current: cb }));
  box.current = cb;
  useEffect(() => {
    const k = String(key);
    const view = getView(k);
    if (view !== undefined) box.current(view, k);
    return HostBuffer.subscribe(k, (v, kk) => box.current(v, kk));
  }, [key, box]);
}

export function installHostBuffer() {
  g.__jsui_onHostBuffer = onHostBuffer;
}
