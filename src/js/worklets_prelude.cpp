// The JS half of a worklet runtime (js/worklets.h), evaluated once when the
// runtime is created, after the console piece of the bundle's prelude:
//
// * the wire format values cross runtimes in (unpack / pack; the bundle's
//   half is @orktes/jsui-react src/worklets.js, keep the two in step):
//   JSON where JSON can say it (null, booleans, strings, finite numbers,
//   arrays), every other value an object tagged by `$`:
//     u undefined | n {v: 'NaN'|'Infinity'|'-Infinity'|'-0'} | bi {v} bigint
//     o {v: {key: wire}} plain object | w {h, i?, c?, p?} worklet (hash,
//     init data {code, location, line} unless this runtime has it, closure,
//     other own properties) | f {id, n} a function of the bundle (a remote
//     function) | s {id, fx} synchronizable | av {t} an Animated value's
//     native tag | b {n} a react-native-worklets function | e {name,
//     message, stack} | ab {d} ArrayBuffer (base64) | tv {k, d} typed array
//     / DataView | m {k, v} Map | set {v} | re {s, f} RegExp | rt {id, name}
//     a worklet runtime | cu {u, v} a custom serializable (unpack worklet,
//     packed value);
// * timers (setTimeout / setInterval / setImmediate) and
//   requestAnimationFrame / requestAnimationFrameFinalizer on the runtime
//   clock, run by the native tick;
// * the react-native-worklets functions a worklet calls (scheduleOnRN,
//   runOnJS, scheduleOnUI, scheduleOnRuntime, createSynchronizable, ...): the
//   bundle's functions are sent as references to these (`b`).
//
// It returns the runtime's entry points: jobs(batch), runSync(job),
// tick(now, frame) -> next timer due.
//
// Worklet unpacking, the remote function and synchronizable shapes and the
// rAF queue follow react-native-worklets (MIT, Copyright (c) 2016
// Software Mansion <swmansion.com>; see docs/THIRD_PARTY_NOTICES.md).
namespace jsui {

// clang-format off
const char* workletsPrelude() {
  return R"JSUI((function (host) {
  'use strict';
  const G = globalThis;
  const define = (name, value) =>
    Object.defineProperty(G, name, { value, writable: true, configurable: true, enumerable: false });
  const RT_ID = host.runtimeId;
  const UI = 2;
  define('global', G);
  define('_WORKLET', true);
  define('__RUNTIME_KIND', host.kind);

  const report = (e, where) => host.reportError(e, '[Worklets] ' + (where || 'Uncaught '));

  // ---------------------------------------------------------------- base64
  const ABC = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
  const IDX = new Uint8Array(128);
  for (let i = 0; i < 64; i++) IDX[ABC.charCodeAt(i)] = i;
  function toB64(b) {
    let s = '';
    let i = 0;
    for (; i + 2 < b.length; i += 3) {
      const n = (b[i] << 16) | (b[i + 1] << 8) | b[i + 2];
      s += ABC[n >> 18] + ABC[(n >> 12) & 63] + ABC[(n >> 6) & 63] + ABC[n & 63];
    }
    if (i < b.length) {
      const n = (b[i] << 16) | ((i + 1 < b.length ? b[i + 1] : 0) << 8);
      s += ABC[n >> 18] + ABC[(n >> 12) & 63] + (i + 1 < b.length ? ABC[(n >> 6) & 63] : '=') + '=';
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
      const n = (IDX[s.charCodeAt(i)] << 18) | (IDX[s.charCodeAt(i + 1)] << 12) | (IDX[s.charCodeAt(i + 2)] << 6) | IDX[s.charCodeAt(i + 3)];
      if (o < len) out[o++] = n >> 16;
      if (o < len) out[o++] = (n >> 8) & 255;
      if (o < len) out[o++] = n & 255;
    }
    return out;
  }

  // ---------------------------------------------------------------- timers
  const timers = new Map();
  let timerSeq = 1;
  function addTimer(fn, ms, args, repeat) {
    if (typeof fn !== 'function') return 0;
    const id = timerSeq++;
    const d = Math.max(0, +ms || 0);
    const due = host.now() + d;
    timers.set(id, { fn, args, due, every: repeat ? Math.max(1, d) : -1 });
    host.timer(due);
    return id;
  }
  const clear = (id) => { timers.delete(id); };
  define('setTimeout', (fn, ms, ...args) => addTimer(fn, ms, args, false));
  define('setInterval', (fn, ms, ...args) => addTimer(fn, ms, args, true));
  define('setImmediate', (fn, ...args) => addTimer(fn, 0, args, false));
  define('clearTimeout', clear);
  define('clearInterval', clear);
  define('clearImmediate', clear);
  function runTimers(now) {
    if (!timers.size) return;
    const due = [];
    for (const [id, t] of timers) if (t.due <= now) due.push(t.due, id);
    for (let i = 0; i < due.length; i += 2) {
      // In due time, then creation order.
      for (let j = i + 2; j < due.length; j += 2)
        if (due[j] < due[i] || (due[j] === due[i] && due[j + 1] < due[i + 1])) {
          const a = due[i], b = due[i + 1];
          due[i] = due[j]; due[i + 1] = due[j + 1]; due[j] = a; due[j + 1] = b;
        }
      const id = due[i + 1];
      const t = timers.get(id);
      if (!t) continue;
      if (t.every < 0) timers.delete(id);
      else t.due = Math.max(t.due + t.every, now);
      try {
        t.fn(...t.args);
      } catch (e) {
        report(e);
      }
    }
  }
  function nextDue() {
    let m = Infinity;
    for (const t of timers.values()) if (t.due < m) m = t.due;
    return m;
  }

  // ------------------------------------------------- requestAnimationFrame
  let frames = [];  // id, callback, id, callback, ...
  let frameSeq = 1;
  let finalizers = [];
  define('requestAnimationFrame', (cb) => {
    const id = frameSeq++;
    frames.push(id, cb);
    host.wantFrame();
    return id;
  });
  define('cancelAnimationFrame', (id) => {
    for (let i = 0; i < frames.length; i += 2) if (frames[i] === id) frames[i + 1] = null;
  });
  define('requestAnimationFrameFinalizer', (cb) => {
    finalizers.push(cb);
    host.wantFrame();
  });
  function runFrame(ts) {
    const q = frames;
    frames = [];
    G.__frameTimestamp = ts;
    for (let i = 1; i < q.length; i += 2) {
      const cb = q[i];
      if (cb) {
        try {
          cb(ts);
        } catch (e) {
          report(e, where(cb));
        }
      }
    }
    const f = finalizers;
    finalizers = [];
    for (const cb of f) {
      try {
        cb();
      } catch (e) {
        report(e, where(cb));
      }
    }
    G.__frameTimestamp = undefined;
  }

)JSUI" R"JSUI(  // ------------------------------------------------------------------ wire
  const U = { $: 'u' };
  const codeCache = new Map();  // worklet hash -> { fn, init }
  const Registry = typeof FinalizationRegistry === 'function' ? FinalizationRegistry : null;
  // A remote function handle that is collected gives its reference back.
  const remoteRefs = Registry && new Registry((id) => host.toJS(JSON.stringify(['r', id, 1])));
  const syncRefs = Registry && new Registry((id) => host.syncRelease(id));

  function where(w) {
    const init = w && w.__initData;
    if (!init) return '';
    const name = (codeCache.get(w.__workletHash) || {}).name || 'anonymous';
    return 'in worklet `' + name + '`' + (init.location ? ' (' + init.location + (init.line ? ':' + init.line : '') + ')' : '') + ': ';
  }

  function unpackWorklet(w) {
    let entry = codeCache.get(w.h);
    if (!entry) {
      if (!w.i || typeof w.i.code !== 'string') throw new Error('[Worklets] Unknown worklet ' + w.h + ' (its code never reached this runtime).');
      // As react-native-worklets' valueUnpacker: evaluated once per hash.
      const fn = host.evalCode('(' + w.i.code + '\n)', w.i.location || 'worklet');
      // _recur (a recursive worklet calls itself through it) makes the
      // holder a cycle: only where the code uses it.
      entry = { fn, init: w.i, name: fn.name || 'anonymous', recur: w.i.code.includes('_recur') };
      codeCache.set(w.h, entry);
    }
    const closure = w.c ? w.c.map(unpack) : undefined;
    const holder = { __closure: closure, __workletHash: w.h, __initData: entry.init };
    if (w.p) for (const k in w.p) holder[k] = unpack(w.p[k]);
    const f = entry.fn.bind(holder);
    if (entry.recur) holder._recur = f;
    Object.defineProperties(f, {
      __workletHash: { value: w.h },
      __initData: { value: entry.init },
      __closure: { value: closure },
    });
    return f;
  }

  function remoteFunction(id, name) {
    const f = function () {
      throw new Error('[Worklets] Tried to synchronously call a non-worklet function `' + (name || 'anonymous') +
        '` on the ' + (RT_ID === UI ? 'UI' : 'worklet') + ' runtime. Use scheduleOnRN / runOnJS to call it on the RN runtime.');
    };
    f.__remoteFunction = true;
    f.__remoteId = id;
    f.__remoteName = name;
    if (remoteRefs) remoteRefs.register(f, id);
    return f;
  }

  function synchronizable(id, fixed) {
    const s = {
      __synchronizableRef: true,
      __syncId: id,
      getDirty: () => unpack(JSON.parse(host.syncGet(id, false))),
      getBlocking: () => unpack(JSON.parse(host.syncGet(id, true))),
      setBlocking(v) {
        if (typeof v !== 'function') return host.syncSet(id, JSON.stringify(pack(v)), true);
        const locked = host.syncLock(id);
        try {
          host.syncSet(id, JSON.stringify(pack(v(s.getBlocking()))), true);
        } finally {
          if (locked) host.syncUnlock(id);
        }
      },
      lock() {
        if (!host.syncLock(id)) throw new Error('[Worklets] Synchronizable lock timed out');
      },
      unlock: () => host.syncUnlock(id),
    };
    if (fixed) s.setDirty = (v) => host.syncSet(id, JSON.stringify(pack(v)), false);
    if (syncRefs) syncRefs.register(s, id);
    return s;
  }

  // An Animated value of the bundle (made native): reads and writes go to
  // the native node at once (the views follow in this frame).
  function animatedValue(tag) {
    return {
      __animatedTag: tag,
      setValue: (v) => host.animSet(tag, +v),
      setOffset: (v) => host.animOffset(tag, +v),
      getValue: () => host.animGet(tag),
      get value() { return host.animGet(tag); },
      set value(v) { host.animSet(tag, +v); },
    };
  }

  const runtimeObject = (id, name) => ({ __workletRuntime: true, runtimeId: id, name });

  function unpack(w) {
    if (w === null || typeof w !== 'object') return w;
    if (Array.isArray(w)) return w.map(unpack);
    switch (w.$) {
      case 'u': return undefined;
      case 'n': return w.v === '-0' ? -0 : Number(w.v);
      case 'bi': return BigInt(w.v);
      case 'o': {
        const o = {};
        for (const k in w.v) o[k] = unpack(w.v[k]);
        return o;
      }
      case 'w': return unpackWorklet(w);
      case 'f': return remoteFunction(w.id, w.n);
      case 's': return synchronizable(w.id, !!w.fx);
      case 'av': return animatedValue(w.t);
      case 'b': return builtins[w.n];
      case 'e': {
        const C = typeof G[w.name] === 'function' && (G[w.name] === Error || G[w.name].prototype instanceof Error) ? G[w.name] : Error;
        const e = new C(w.message);
        if (w.name) e.name = w.name;
        if (w.stack) e.stack = w.stack;
        return e;
      }
      case 'ab': return fromB64(w.d).buffer;
      case 'tv': {
        const b = fromB64(w.d);
        if (w.k === 'DataView') return new DataView(b.buffer);
        const C = G[w.k];
        return new C(b.buffer, 0, b.byteLength / C.BYTES_PER_ELEMENT);
      }
      case 'm': return new Map(w.k.map((k, i) => [unpack(k), unpack(w.v[i])]));
      case 'set': return new Set(w.v.map(unpack));
      case 're': return new RegExp(w.s, w.f);
      case 'rt': return runtimeObject(w.id, w.name);
      case 'cu': return unpack(w.u)(unpack(w.v));
    }
    return undefined;
  }

  // `toRuntime`: the value goes to another worklet runtime, not the bundle
  // (a remote function then takes a reference of its own).
  function pack(v, depth = 0, toRuntime = false) {
    if (depth > 30) throw new Error('[Worklets] Trying to convert a cyclic object to a serializable. This is not supported.');
    switch (typeof v) {
      case 'undefined': return U;
      case 'number': return Number.isFinite(v) && !Object.is(v, -0) ? v : { $: 'n', v: Object.is(v, -0) ? '-0' : String(v) };
      case 'string':
      case 'boolean': return v;
      case 'bigint': return { $: 'bi', v: String(v) };
      case 'symbol': return String(v);
      case 'function': {
        if (v.__remoteId) {
          if (toRuntime) host.toJS(JSON.stringify(['a', v.__remoteId]));
          return { $: 'f', id: v.__remoteId, n: v.__remoteName };
        }
        if (v.__workletHash) {
          return { $: 'w', h: v.__workletHash, i: v.__initData, c: v.__closure ? v.__closure.map((x) => pack(x, depth + 1, toRuntime)) : undefined };
        }
        if (v.__builtin) return { $: 'b', n: v.__builtin };
        console.warn('[Worklets] Cannot copy a function that is not a worklet (`' + (v.name || 'anonymous') + '`) out of a worklet runtime.');
        return U;
      }
    }
    if (v === null) return null;
    if (Array.isArray(v)) return v.map((x) => pack(x, depth + 1, toRuntime));
    if (v.__synchronizableRef && v.__syncId) {
      host.syncRetain(v.__syncId);
      return { $: 's', id: v.__syncId, fx: !!v.setDirty };
    }
    if (v.__animatedTag !== undefined) return { $: 'av', t: v.__animatedTag };
    if (v.__workletRuntime) return { $: 'rt', id: v.runtimeId, name: v.name };
    const proto = Object.getPrototypeOf(v);
    if (proto === Object.prototype || proto === null) {
      const o = {};
      for (const k of Object.keys(v)) o[k] = pack(v[k], depth + 1, toRuntime);
      return { $: 'o', v: o };
    }
    if (v instanceof Error) return { $: 'e', name: v.name, message: v.message, stack: v.stack };
    if (v instanceof ArrayBuffer) return { $: 'ab', d: toB64(new Uint8Array(v)) };
    if (ArrayBuffer.isView(v)) return { $: 'tv', k: v.constructor.name, d: toB64(new Uint8Array(v.buffer, v.byteOffset, v.byteLength)) };
    if (v instanceof Map) {
      const k = [], vals = [];
      for (const [a, b] of v) { k.push(pack(a, depth + 1, toRuntime)); vals.push(pack(b, depth + 1, toRuntime)); }
      return { $: 'm', k, v: vals };
    }
    if (v instanceof Set) return { $: 'set', v: [...v].map((x) => pack(x, depth + 1, toRuntime)) };
    if (v instanceof RegExp) return { $: 're', s: v.source, f: v.flags };
    console.warn('[Worklets] Cannot copy value of type `' + ((v.constructor && v.constructor.name) || 'unknown') + '`.');
    return U;
  }

  // ------------------------------------- react-native-worklets in a worklet
  const isWorkletFunction = (v) => typeof v === 'function' && !!v.__workletHash;
  const job = (w, args) => JSON.stringify([pack(w, 0, true), pack(args, 0, true), 0]);
  function scheduleOnRN(fun, ...args) {
    if (typeof fun !== 'function' || !(fun.__remoteId || isWorkletFunction(fun)))
      throw new TypeError('[Worklets] scheduleOnRN / runOnJS on a worklet runtime take a function of the RN runtime or a worklet.');
    host.toJS(JSON.stringify(['c', pack(fun), pack(args)]));
  }
  function scheduleOnRuntimeWithId(id, worklet, ...args) {
    if (!isWorkletFunction(worklet)) throw new TypeError('[Worklets] The function passed to `scheduleOnRuntime` is not a worklet.');
    if (id === RT_ID) queueMicrotask(() => worklet(...args));
    else host.post(id, job(worklet, args));
  }
  function createSynchronizable(initial, config) {
    const fixed = !!(config && config.fixedType);
    return synchronizable(host.syncCreate(JSON.stringify(pack(initial))), fixed);
  }
  const builtins = {
    scheduleOnRN,
    runOnJS: (fun) => (...args) => scheduleOnRN(fun, ...args),
    scheduleOnUI: (worklet, ...args) => scheduleOnRuntimeWithId(UI, worklet, ...args),
    runOnUI: (worklet) => (...args) => scheduleOnRuntimeWithId(UI, worklet, ...args),
    runOnUISync(worklet, ...args) {
      if (RT_ID !== UI) throw new Error('[Worklets] `runOnUISync` cannot be called from a worker runtime.');
      return worklet(...args);
    },
    executeOnUIRuntimeSync: (worklet) => (...args) => builtins.runOnUISync(worklet, ...args),
    runOnUIAsync(worklet, ...args) {
      if (RT_ID !== UI) return Promise.reject(new Error('[Worklets] `runOnUIAsync` cannot be called from a worker runtime.'));
      return Promise.resolve().then(() => worklet(...args));
    },
    scheduleOnRuntime: (rt, worklet, ...args) => scheduleOnRuntimeWithId(rt && rt.runtimeId, worklet, ...args),
    runOnRuntime: (rt, worklet) => (...args) => scheduleOnRuntimeWithId(rt && rt.runtimeId, worklet, ...args),
    scheduleOnRuntimeWithId,
    getRuntimeKind: () => G.__RUNTIME_KIND,
    isRNRuntime: () => false,
    isUIRuntime: () => G.__RUNTIME_KIND === UI,
    isWorkerRuntime: () => G.__RUNTIME_KIND === 3,
    isWorkletRuntime: () => true,
    isWorkletFunction,
    isSynchronizable: (v) => typeof v === 'object' && v !== null && !!v.__synchronizableRef,
    isShareable: () => false,
    isSerializableRef: () => false,
    createSerializable: (v) => v,
    makeShareable: (v) => v,
    makeShareableCloneRecursive: (v) => v,
    makeShareableCloneOnUIRecursive: (v) => v,
    createSynchronizable,
    callMicrotasks() {},
    getCurrentThreadId: () => (RT_ID === UI ? 'jsui-ui' : 'jsui-worklet-' + RT_ID),
  };
  for (const k in builtins) Object.defineProperty(builtins[k], '__builtin', { value: k });

  // ---------------------------------------------------------- entry points
  function run(j) {
    let w;
    try {
      w = unpack(j[0]);
      const r = w(...(j[1] === undefined ? [] : unpack(j[1])));
      if (j[2]) host.toJS(JSON.stringify(['p', j[2], 1, pack(r)]));
    } catch (e) {
      if (j[2]) host.toJS(JSON.stringify(['p', j[2], 0, pack(e)]));
      else report(e, where(w));
    }
  }
  return {
    jobs(batch) {
      for (const j of JSON.parse(batch)) run(j);
    },
    runSync(text) {
      const j = JSON.parse(text);
      let w;
      try {
        w = unpack(j[0]);
        return JSON.stringify({ ok: true, v: pack(w(...unpack(j[1]))) });
      } catch (e) {
        if (e instanceof Error && w && w.__initData && e.stack) e.stack += '\n    ' + where(w).slice(0, -2);
        return JSON.stringify({ ok: false, e: pack(e) });
      }
    },
    tick(now, frame) {
      runTimers(now);
      if (frame) runFrame(now);
      return nextDue();
    },
  };
})
)JSUI";
}
// clang-format on

}  // namespace jsui
