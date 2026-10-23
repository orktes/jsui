// Host textures: pictures the host makes itself (a decoded video, a render
// target, a webcam) shown inside the UI (include/jsui/jsui.h "Host
// textures", docs/ARCHITECTURE.md). JSUI never sees the pixels in JS: the
// host keeps the texture current and the runtime repaints the views showing
// it, with no JS per frame.
//
//   <HostTexture name="trailer" resizeMode="contain" style={{ width: 640, aspectRatio: 16 / 9 }} />
//   const size = useHostTexture('trailer');     // { width, height } once the host set it, else null
//   HostTextures.get(name) / keys() / subscribe(name, cb)
//
// HostTexture is an Image whose picture is the host texture: it lays out and
// paints like one (intrinsic size = the texture's px / `scale`, resizeMode,
// borderRadius clipping, tintColor, opacity, transforms). Hosts without the
// native bridge (react-native-web, Node tests) get an in-memory store that
// HostTextures.set() fills, and an empty view.

import { createElement as h, useCallback, useSyncExternalStore } from 'react';
import { flatten } from './StyleSheet.js';
import { native } from './native.js';
import { reconciler, setUpdatePriority, DiscreteEventPriority } from './renderer.js';

const g = globalThis;
const nativeTextures = typeof native.texture === 'function';
const local = new Map(); // name -> { width, height, version }
let localVersion = 1;
const cache = new Map(); // name -> { version, info }

function read(name) {
  if (!nativeTextures) {
    const e = local.get(name);
    return e ? e : null;
  }
  const t = native.texture(name);
  if (!t) {
    cache.delete(name);
    return null;
  }
  const e = cache.get(name);
  if (e && e.version === t[2]) return e.info;
  // The same object until the texture changes (useSyncExternalStore).
  const info = { width: t[0], height: t[1] };
  cache.set(name, { version: t[2], info });
  return info;
}

const listeners = new Map(); // name -> Set<cb>; '*' gets every name

function reportListenerError(e) {
  if (typeof g.__jsui_reportError === 'function') g.__jsui_reportError(e);
  else if (typeof g.reportError === 'function') g.reportError(e);
  else setTimeout(() => {
    throw e;
  }, 0);
}

function notify(names) {
  const any = listeners.get('*');
  const work = [];
  for (const name of names) {
    const set = listeners.get(name);
    if (set && set.size) for (const cb of Array.from(set)) work.push([cb, name]);
    if (any && any.size) for (const cb of Array.from(any)) work.push([cb, name]);
  }
  if (!work.length) return;
  const prev = setUpdatePriority(DiscreteEventPriority);
  try {
    reconciler.batchedUpdates(() => {
      for (const [cb, name] of work) {
        try {
          cb(read(name), name);
        } catch (e) {
          reportListenerError(e);
        }
      }
    }, null);
  } finally {
    setUpdatePriority(prev);
  }
}

/** Called by the runtime once per tick with the textures set, resized or removed. */
function onHostTexture(names) {
  notify(Array.isArray(names) ? names.map(String) : [String(names)]);
}

export const HostTextures = {
  /** { width, height } (px) of texture `name`, or null while the host has not set it. */
  get(name) {
    return read(String(name));
  },
  /** Names of the textures the host set. */
  keys() {
    if (nativeTextures && typeof native.textureKeys === 'function') return native.textureKeys();
    return Array.from(local.keys());
  },
  /** Calls `cb(info, name)` when `name` is set, resized or removed ('*': every name). Returns an unsubscribe function. */
  subscribe(name, cb) {
    name = String(name);
    let set = listeners.get(name);
    if (!set) listeners.set(name, (set = new Set()));
    set.add(cb);
    return () => {
      set.delete(cb);
      if (!set.size && listeners.get(name) === set) listeners.delete(name);
    };
  },
  /**
   * Hosts without the native bridge only (react-native-web, tests): records
   * a texture's size ({ width, height }; null removes it). The JSUI
   * runtime's textures are set by the host (jsui_set_texture).
   */
  set(name, info) {
    name = String(name);
    if (nativeTextures) throw new Error(`HostTextures.set("${name}"): textures are set by the host (jsui_set_texture)`);
    if (info == null) local.delete(name);
    else local.set(name, { width: info.width, height: info.height, version: localVersion++ });
    notify([name]);
  },
};

/** { width, height } of host texture `name` once set (null before); re-renders when it changes size. */
export function useHostTexture(name) {
  name = String(name);
  const subscribe = useCallback((cb) => HostTextures.subscribe(name, cb), [name]);
  const get = () => read(name);
  return useSyncExternalStore(subscribe, get, get);
}

/**
 * Shows host texture `name`. Props: Image's layout and paint props (style,
 * resizeMode, tintColor) plus `scale` (texture px per layout px, default 1).
 */
export function HostTexture(props) {
  const { name, style, resizeMode, tintColor, ...rest } = props;
  let s = flatten(style);
  if (resizeMode && (!s || s.resizeMode === undefined)) s = Object.assign({}, s, { resizeMode });
  if (tintColor && (!s || s.tintColor === undefined)) s = Object.assign({}, s, { tintColor });
  rest.style = s;
  rest.texture = name == null ? '' : String(name);
  if (resizeMode) rest.resizeMode = resizeMode;
  return h('Image', rest);
}
HostTexture.displayName = 'HostTexture';

export function installHostTexture() {
  g.__jsui_onHostTexture = onHostTexture;
}
