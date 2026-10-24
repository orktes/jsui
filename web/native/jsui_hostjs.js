// The JS half of the host JS engine (src/js/engine_hostjs.cpp), linked into
// jsui.js as a --pre-js: with createJsui({ js: 'host' }) (or the headless
// CLI's `js: 'host'`) the bundle runs on the JS engine that loaded the
// module (V8 / JavaScriptCore / SpiderMonkey) instead of QuickJS in WASM.
// See docs/ARCHITECTURE.md "Host JS engine".
//
// Per runtime an Engine holds what QuickJsEngine keeps natively: timers,
// requestAnimationFrame callbacks and the microtask queue (all on the
// runtime clock that jsui_tick drives), the pending Host.call settle
// functions and fetch / loadFont callbacks, and the entry depth: every
// native -> JS call runs through call(), and when the outermost one returns
// the microtasks queued meanwhile run (drain), then queued onLayout events,
// like JsEngine::Impl::drain. __jsui_native is implemented here over the
// jsui_hjs_* exports.
//
// The bundle and the prelude (prelude.cpp's sources, the same QuickJS runs)
// see the runtime's globals, not the page's: they run inside a function
// whose parameters shadow every global the runtime defines (globalThis /
// window / self, setTimeout, requestAnimationFrame, queueMicrotask,
// performance, console, fetch, MessageChannel, __jsui_native, ...); the
// global object they see (`G`) holds those plus the ECMAScript built-ins.
// Native promises (async functions, fetch().then) settle on the host's own
// microtask checkpoint, i.e. once the host's current task (a frame, an
// input event) returns, not inside the native call that resolved them.

var jsuiHostJs = (function () {
  'use strict';
  const realGlobal = globalThis;
  const engines = new Map();  // engine id (its C++ address) -> Engine
  const encoder = new TextEncoder();
  const decoder = new TextDecoder();
  // The ECMAScript built-ins the bundle's global object has (QuickJS's set;
  // no WebAssembly, no DOM).
  const BUILTINS = ['Object', 'Function', 'Array', 'Number', 'parseFloat', 'parseInt', 'Boolean', 'String', 'Symbol',
    'Date', 'Promise', 'RegExp', 'Error', 'AggregateError', 'EvalError', 'RangeError', 'ReferenceError', 'SyntaxError',
    'TypeError', 'URIError', 'JSON', 'Math', 'Intl', 'ArrayBuffer', 'SharedArrayBuffer', 'Atomics', 'Uint8Array',
    'Int8Array', 'Uint16Array', 'Int16Array', 'Uint32Array', 'Int32Array', 'Float32Array', 'Float64Array',
    'Float16Array', 'Uint8ClampedArray', 'BigUint64Array', 'BigInt64Array', 'DataView', 'Map', 'BigInt', 'Set',
    'WeakMap', 'WeakSet', 'Proxy', 'Reflect', 'FinalizationRegistry', 'WeakRef', 'decodeURI', 'decodeURIComponent',
    'encodeURI', 'encodeURIComponent', 'escape', 'unescape', 'eval', 'isFinite', 'isNaN', 'Iterator',
    'DOMException'];
  // Globals read as plain identifiers that a script defines after it starts
  // (react-refresh installs the devtools hook React reads, the HMR runtime
  // its client): live bindings that follow G's property.
  const LIVE = ['__REACT_DEVTOOLS_GLOBAL_HOOK__', '__jsui_hmr'];
  const RESERVED = new Set(('break case catch class const continue debugger default delete do else enum export ' +
    'extends false finally for function if import in instanceof new null return super switch this throw true try ' +
    'typeof var void while with yield let static implements interface package private protected public await ' +
    'arguments eval undefined NaN Infinity').split(' ').concat(LIVE));
  const IDENT = /^[A-Za-z_$][\w$]*$/;
  // Bytes per element of each JsuiBufferType.
  const ELEMENT_SIZES = [4, 8, 4, 4, 2, 2, 1, 1];
  // The host's own global names (window's prototype chain included), once.
  let hostNames = null;
  function hostGlobalNames() {
    if (hostNames) return hostNames;
    const s = new Set();
    for (let o = realGlobal; o && o !== Object.prototype; o = Object.getPrototypeOf(o))
      for (const n of Object.getOwnPropertyNames(o)) s.add(n);
    return (hostNames = [...s]);
  }
  const define = (obj, name, value) =>
    Object.defineProperty(obj, name, { value, writable: true, configurable: true, enumerable: false });

  // ---------------------------------------------------- exports, memory
  let X = null;   // the jsui_hjs_* exports, without the jsui_hjs_ prefix
  let scratch = 0;  // jsui_hjs_scratch(): double num[8], int32 len at +64
  function exportsOf() {
    if (X) return X;
    const raw = (typeof wasmExports !== 'undefined' && wasmExports) || Module['asm'] || {};
    X = {};
    for (const k of Object.keys(raw)) if (k.startsWith('jsui_hjs_')) X[k.slice(9)] = raw[k];
    for (const k of Object.keys(Module)) if (k.startsWith('_jsui_hjs_') && !X[k.slice(10)]) X[k.slice(10)] = Module[k];
    scratch = X.scratch();
    return X;
  }
  const malloc = (n) => Module['_malloc'](n);
  const free = (p) => Module['_free'](p);
  const num = (i) => HEAPF64[(scratch >> 3) + i];

  // UTF-8 text in WASM memory -> string.
  function str(p, len) {
    if (!len) return '';
    const h = HEAPU8;
    if (len < 64) {
      let s = '';
      for (let i = 0; i < len; i++) {
        const c = h[p + i];
        if (c >= 0x80) return decoder.decode(h.subarray(p, p + len));
        s += String.fromCharCode(c);
      }
      return s;
    }
    return decoder.decode(h.subarray(p, p + len));
  }
  // A string an export returned (its length is in the scratch block); null for 0.
  const out = (p) => (p ? str(p, HEAP32[(scratch + 64) >> 2]) : null);

  // Strings -> WASM memory: a scratch arena the next reset() reuses. Each
  // bridge call resets it, converts its arguments (user toString / toJSON
  // code may run then, and may call the bridge), then put()s them; the
  // native side copies them before it can call back into JS.
  let arena = 0, arenaCap = 0, arenaOff = 0;
  const retired = [];
  let L = 0;  // byte length of the last put()
  function reset() {
    arenaOff = 0;
    while (retired.length) free(retired.pop());
  }
  function put(s) {
    const n = s.length;
    const need = n * 3 + 1;
    if (arenaOff + need > arenaCap) {
      // A new block (strings already put stay valid until reset()).
      if (arena) retired.push(arena);
      arenaCap = Math.max(65536, need, arenaCap * 2);
      arena = malloc(arenaCap);
      arenaOff = 0;
    }
    const p = arena + arenaOff, h = HEAPU8;
    let i = 0;
    for (; i < n; i++) {
      const c = s.charCodeAt(i);
      if (c >= 0x80) break;
      h[p + i] = c;
    }
    if (i < n) i += encoder.encodeInto(i ? s.slice(i) : s, h.subarray(p + i, p + need)).written;
    h[p + i] = 0;
    L = i;
    arenaOff += i + 1;
    return p;
  }
  // Bytes -> a malloc'ed copy the caller frees.
  function putBytes(u8) {
    const p = malloc(Math.max(1, u8.length));
    HEAPU8.set(u8, p);
    return p;
  }

  // JSON for the native side: what toValue (engine.cpp) makes of a value.
  // JSON.stringify agrees on plain data; values it rejects (cycles, BigInt)
  // go through the same conversion QuickJS does (functions dropped, depth 32).
  function json(v) {
    if (v === undefined) return '';
    try {
      const j = JSON.stringify(v);
      return j === undefined ? '' : j;
    } catch (_) {
      return JSON.stringify(plain(v, 0)) || '';
    }
  }
  function plain(v, depth) {
    if (depth > 32) return null;
    switch (typeof v) {
      case 'boolean': case 'number': case 'string': return v;
      case 'object': break;
      default: return null;
    }
    if (v === null) return null;
    if (Array.isArray(v)) return Array.from(v, (e) => plain(e, depth + 1));
    const o = {};
    for (const k of Object.keys(v)) {
      let e;
      try { e = v[k]; } catch (_) { continue; }
      if (e !== undefined && typeof e !== 'function') o[k] = plain(e, depth + 1);
    }
    return o;
  }

  // "Name: message\n<stack>", like formatError (engine.cpp). V8 stacks
  // already start with the message line.
  function formatError(err) {
    try {
      if (err !== null && typeof err === 'object' && (err instanceof Error || typeof err.stack === 'string')) {
        let head;
        try { head = String(err); } catch (_) { head = 'Error'; }
        const st = typeof err.stack === 'string' ? err.stack.replace(/\s+$/, '') : '';
        if (st.startsWith(head)) return st;
        return st ? head + '\n' + st : head;
      }
      if (err !== null && typeof err === 'object') {
        try {
          const j = JSON.stringify(err);
          if (j !== undefined) return j;
        } catch (_) {}
      }
      return String(err);
    } catch (_) {
      return 'Error';
    }
  }

  const numArg = (v) => (typeof v === 'number' ? v : NaN);
  const toInt = (v) => v | 0;
  const toId = (v) => {
    const d = +v;
    return d >= 1 && d < 9007199254740992 ? Math.floor(d) : 0;
  };
  // QuickJS's JS_ToCString: String(), '' when that throws.
  const text = (v) => {
    try { return String(v); } catch (_) { return ''; }
  };

  // ------------------------------------------- unhandled rejections
  // Reported to the runtime whose script the reason's stack names (else, for
  // a reason without a stack, the runtime whose JS ran last).
  let lastActive = null;
  let rejectionsHooked = false;
  function ownerOf(reason) {
    const stack = reason !== null && typeof reason === 'object' && typeof reason.stack === 'string' ? reason.stack : null;
    for (const eng of engines.values()) {
      if (stack && eng.scripts.some((n) => stack.includes(n))) return eng;
    }
    if (!stack && lastActive && !lastActive.dead) return lastActive;
    return null;
  }
  function hookRejections() {
    if (rejectionsHooked) return;
    rejectionsHooked = true;
    const report = (eng, reason) => eng.reportError(reason, 'Unhandled promise rejection: ');
    if (typeof realGlobal.addEventListener === 'function' && typeof realGlobal.process === 'undefined') {
      realGlobal.addEventListener('unhandledrejection', (ev) => {
        const eng = ownerOf(ev.reason);
        if (!eng) return;
        ev.preventDefault();
        report(eng, ev.reason);
      });
    } else if (typeof realGlobal.process === 'object' && typeof realGlobal.process.on === 'function') {
      // Node: rejections that are not a runtime's keep Node's default (an
      // uncaught exception) unless the process listens for them itself.
      // Other listeners also see the runtimes' rejections (Node has no
      // preventDefault).
      const proc = realGlobal.process;
      proc.on('unhandledRejection', (reason) => {
        const eng = ownerOf(reason);
        if (eng) report(eng, reason);
        else if (proc.listenerCount('unhandledRejection') === 1) proc.nextTick(() => { throw reason; });
      });
    }
  }

  // ---------------------------------------------------------- Engine
  class Engine {
    constructor(e, rootId, devMode, lottie) {
      exportsOf();
      this.e = e;
      this.dead = false;
      this.depth = 0;
      this.draining = false;
      this.micro = [];
      this.flushQueued = false;
      this.timers = new Map();
      this.nextTimer = 1;
      this.timerSeq = 0;
      this.earliestDue = Infinity;
      this.rafs = [];
      this.rafsRunning = [];
      this.nextRaf = 1;
      this.layoutQueue = [];
      this.hostCalls = new Map();  // Host.call id -> settle(ok, payload)
      this.loads = new Map();      // fetch request id -> callback
      this.fonts = new Map();      // loadFont request id -> [resolve, reject]
      this.cells = new Map();      // live global name -> {value, sinks}
      this.buffers = new Map();    // frame data key -> {version, type, count, buffer} (N.buffer)
      this.scripts = [];           // names of the scripts evaluated (stack attribution)
      this.devMode = devMode;
      const G = (this.G = {});
      for (const n of BUILTINS) if (Object.prototype.hasOwnProperty.call(realGlobal, n)) define(G, n, realGlobal[n]);
      define(G, 'globalThis', G);
      define(G, 'setTimeout', (fn, ms, ...args) => this.setTimer(fn, ms, args, false));
      define(G, 'setInterval', (fn, ms, ...args) => this.setTimer(fn, ms, args, true));
      define(G, 'clearTimeout', (id) => this.clearTimer(id));
      define(G, 'clearInterval', (id) => this.clearTimer(id));
      define(G, 'requestAnimationFrame', (fn) => this.requestAnimationFrame(fn));
      define(G, 'cancelAnimationFrame', (id) => this.cancelAnimationFrame(id));
      define(G, 'queueMicrotask', (fn) => this.queueMicrotask(fn));
      // performance.now() is the runtime clock (the embedder's tick time).
      const now = X.now;
      define(G, 'performance', { now: () => now(e), timeOrigin: 0 });
      // Better than the prelude's fallbacks (real randomness, every type).
      if (realGlobal.crypto && typeof realGlobal.crypto.getRandomValues === 'function') define(G, 'crypto', realGlobal.crypto);
      if (typeof realGlobal.structuredClone === 'function') define(G, 'structuredClone', realGlobal.structuredClone);
      G.__jsui_native = this.bridge(rootId, devMode, lottie);
      this.host = this.hostApi();
      hookRejections();
    }

    // -------------------------------------------- entries and drains
    enter() {
      this.depth++;
    }

    leave() {
      if (--this.depth === 0 && !this.draining) this.drain();
    }

    // Every native -> JS call: exceptions are reported, and the outermost
    // one drains the microtasks it queued.
    call(fn, thisArg, args) {
      this.depth++;
      lastActive = this;
      let r;
      try {
        r = fn.apply(thisArg, args);
      } catch (err) {
        this.reportError(err, '');
      }
      this.leave();
      return r;
    }

    drain() {
      this.draining = true;
      this.depth++;
      let jobs = 0;
      for (let round = 0; round < 64; round++) {
        jobs += this.runMicrotasks();
        if (!this.layoutQueue.length) break;
        const list = this.layoutQueue;
        this.layoutQueue = [];
        const fn = this.G.__jsui_onLayout;
        for (const l of list) {
          if (typeof fn !== 'function') break;
          try {
            fn(l[0], { x: l[1], y: l[2], width: l[3], height: l[4] });
          } catch (err) {
            this.reportError(err, '');
          }
        }
      }
      this.depth--;
      this.draining = false;
      if (jobs && !this.dead) X.count_jobs(this.e, jobs);
    }

    runMicrotasks() {
      let n = 0;
      while (this.micro.length && n < 10000000) {
        const batch = this.micro;
        this.micro = [];
        for (let i = 0; i < batch.length; i++) {
          n++;
          try {
            batch[i]();
          } catch (err) {
            this.reportError(err, '');
          }
        }
      }
      return n;
    }

    queueMicrotask(fn) {
      if (typeof fn !== 'function') throw new TypeError('queueMicrotask: callback is not a function');
      this.micro.push(fn);
      // Queued outside any native call (a native promise's continuation, a
      // host task): drained on the host's microtask checkpoint.
      if (this.depth === 0 && !this.flushQueued) {
        this.flushQueued = true;
        Promise.resolve().then(() => {
          this.flushQueued = false;
          if (!this.dead && this.depth === 0) {
            this.depth++;
            lastActive = this;
            this.leave();
          }
        });
      }
    }

    runJobs() {
      if (this.depth === 0) {
        this.depth++;
        this.leave();
      }
    }

    reportError(err, prefix) {
      if (this.dead) return;
      const msg = prefix + formatError(err);
      reset();
      X.report_error(this.e, put(msg), L);
    }

    // ------------------------------------------------ timers / rAF
    now() {
      return X.now(this.e);
    }

    setTimer(fn, ms, args, repeat) {
      if (typeof fn !== 'function') return 0;  // string code is not supported
      ms = +ms;
      if (!(ms >= 0) || !Number.isFinite(ms)) ms = 0;
      ms = Math.min(ms, 2147483647);
      const id = this.nextTimer++;
      if (this.nextTimer > 0x7fffffff) this.nextTimer = 1;
      const due = this.now() + ms;
      this.timers.set(id, { due, interval: repeat ? ms : -1, seq: this.timerSeq++, fn, args });
      if (due < this.earliestDue) this.earliestDue = due;
      return id;
    }

    clearTimer(id) {
      if (typeof id === 'number') this.timers.delete(id | 0);
    }

    // Due timers in due-time then FIFO order; zero-delay timers queued by
    // callbacks run in later passes of the same tick (at most 8).
    runTimers(now) {
      for (let pass = 0; pass < 8 && this.earliestDue <= now; pass++) {
        const due = [];
        let later = Infinity;
        for (const [id, t] of this.timers) {
          if (t.due <= now) due.push([t.due, t.seq, id]);
          else if (t.due < later) later = t.due;
        }
        this.earliestDue = later;
        due.sort((a, b) => a[0] - b[0] || a[1] - b[1]);
        for (const d of due) {
          const t = this.timers.get(d[2]);
          if (!t || t.seq !== d[1]) continue;  // cleared meanwhile
          if (t.interval >= 0) {
            t.due = now + Math.max(1, t.interval);
            t.seq = this.timerSeq++;
            if (t.due < this.earliestDue) this.earliestDue = t.due;
          } else {
            this.timers.delete(d[2]);
          }
          this.call(t.fn, undefined, t.args);
          if (this.dead) return;
        }
      }
    }

    requestAnimationFrame(fn) {
      if (typeof fn !== 'function') throw new TypeError('requestAnimationFrame: callback is not a function');
      const id = this.nextRaf++;
      if (this.nextRaf > 0x7fffffff) this.nextRaf = 1;
      this.rafs.push({ id, fn });
      return id;
    }

    cancelAnimationFrame(id) {
      id = toInt(id);
      if (id <= 0) return;
      for (const list of [this.rafs, this.rafsRunning]) {
        for (const r of list) {
          if (r.id === id) {
            r.id = 0;
            r.fn = null;
          }
        }
      }
    }

    // Callbacks registered while running go to the next frame.
    runAnimationFrame(now) {
      if (!this.rafs.length) return;
      this.rafsRunning = this.rafs;
      this.rafs = [];
      for (const r of this.rafsRunning) {
        if (!r.id) continue;
        const fn = r.fn;
        r.fn = null;
        r.id = 0;
        this.call(fn, undefined, [now]);
        if (this.dead) return;
      }
      this.rafsRunning = [];
    }

    hasPendingWork(now) {
      return this.earliestDue <= now || this.rafs.length > 0 || this.micro.length > 0;
    }

    // ------------------------------------------------- native -> JS
    dispatchEvent(evJson) {
      const fn = this.G.__jsui_dispatchEvent;
      if (typeof fn !== 'function') return 0;
      const r = this.call(fn, undefined, [JSON.parse(evJson)]);
      if (typeof r === 'boolean') return r ? 3 : 0;
      return typeof r === 'number' ? r | 0 : 0;
    }

    hostMessage(name, data) {
      const fn = this.G.__jsui_onHostMessage;
      if (typeof fn === 'function') this.call(fn, undefined, [name, data]);
    }

    // Delivered right away outside JS; queued until it unwinds otherwise.
    layoutEvent(id, x, y, w, h) {
      this.layoutQueue.push([id, x, y, w, h]);
      if (this.depth === 0) {
        this.depth++;
        this.leave();
      }
    }

    callGlobal(name, argsJson) {
      const fn = this.G[name];
      if (typeof fn !== 'function') return false;
      this.call(fn, undefined, JSON.parse(argsJson));
      return true;
    }

    settleCall(id, ok, payload) {
      const fn = this.hostCalls.get(id);
      this.hostCalls.delete(id);
      if (fn) this.call(fn, undefined, [ok, payload]);
    }

    // JS has copies of the buffers (see N.buffer): the next read copies again.
    releaseBuffer(key) {
      this.buffers.delete(key);
    }

    loadDone(id, status, statusText, url, headers, body, error) {
      const cb = this.loads.get(id);
      if (!cb) return;
      this.loads.delete(id);
      this.call(cb, undefined, [status, statusText, url, JSON.parse(headers), body, error]);
    }

    fontDone(id, ok, error) {
      const p = this.fonts.get(id);
      if (!p) return;
      this.fonts.delete(id);
      if (ok) this.call(p[0], undefined, [true]);
      else this.call(p[1], undefined, [new Error(error)]);
    }

    // --------------------------------------------- scripts and scope
    // The global names a script sees as parameters: G's own properties, and
    // every other global of the host (window's DOM, Node's process /
    // setImmediate, WebAssembly, ...) as undefined, so that feature checks
    // (React's scheduler prefers setImmediate) see what QuickJS has.
    shadowNames() {
      const names = new Set(Object.getOwnPropertyNames(this.G));
      for (const n of hostGlobalNames()) names.add(n);
      return [...names].filter((n) => IDENT.test(n) && !RESERVED.has(n));
    }

    live(name, sink) {
      let cell = this.cells.get(name);
      if (!cell) {
        cell = { value: this.G[name], sinks: [] };
        this.cells.set(name, cell);
        Object.defineProperty(this.G, name, {
          get: () => cell.value,
          set: (v) => {
            cell.value = v;
            for (const s of cell.sinks) s(v);
          },
          configurable: true,
          enumerable: false,
        });
      }
      cell.sinks.push(sink);
      return cell.value;
    }

    // Evaluates `source` as a script of the runtime's global scope and
    // returns what it returns (`prelude`: the source is an expression).
    // The wrapper sits on the source's first line, so line numbers match.
    run(source, name, prelude) {
      const names = this.shadowNames();
      let body = source;
      let directive = '';
      const m = /^\s*(['"])use strict\1;?/.exec(body);
      if (m) {
        directive = '"use strict";';
        body = body.slice(m[0].length);
      }
      let lives = '';
      for (const n of LIVE) lives += 'var ' + n + ' = __jsui_live(' + JSON.stringify(n) + ', function (v) { ' + n + ' = v; });';
      const src = '(function (__jsui_live' + (names.length ? ', ' + names.join(', ') : '') + ') {' + directive + lives +
        (prelude ? 'return ' : '') + body + '\n})\n//# sourceURL=' + String(name).replace(/\s/g, '%20');
      const fn = (0, eval)(src);
      const args = [(n, sink) => this.live(n, sink)];
      for (const n of names) args.push(this.G[n]);
      if (name && !this.scripts.includes(name)) this.scripts.push(name);
      return fn.apply(this.G, args);
    }

    prelude(name, source) {
      this.call(() => this.run(source, name, true)(this.host), undefined, []);
    }

    evalBundle(source, name) {
      let ok = true;
      this.depth++;
      lastActive = this;
      try {
        this.run(source, name, false);
      } catch (err) {
        ok = false;
        this.reportError(err, '');
      }
      this.leave();
      return ok;
    }

    // ------------------------------------------ __jsui_host (prelude)
    hostApi() {
      const e = this.e;
      const decoders = new Map();
      const D = this;
      return {
        devMode: this.devMode,
        log(level, msg) {
          if (D.dead) return;
          msg = text(msg);
          reset();
          X.log(e, toInt(level), put(msg), L);
        },
        reportError(err, prefix) {
          D.reportError(err, typeof prefix === 'string' ? prefix : '');
        },
        utf8Encode(s) {
          return encoder.encode(s === undefined ? 'undefined' : text(s));
        },
        utf8Decode(bytes, fatal, ignoreBOM) {
          const u8 = bytesOf(bytes);
          if (!u8) throw new TypeError('TextDecoder.decode: expected a BufferSource');
          const key = (fatal ? 1 : 0) | (ignoreBOM ? 2 : 0);
          let d = decoders.get(key);
          if (!d) decoders.set(key, (d = new TextDecoder('utf-8', { fatal: !!fatal, ignoreBOM: !!ignoreBOM })));
          try {
            return d.decode(u8);
          } catch (_) {
            throw new TypeError('The encoded data was not valid utf-8');
          }
        },
        resolveUrl(url, base) {
          if (D.dead) return '';
          url = text(url);
          const hasBase = typeof base === 'string';
          reset();
          const u = put(url), ul = L;
          const b = hasBase ? put(base) : 0, bl = hasBase ? L : 0;
          return out(X.resolve_url(e, u, ul, b, bl, hasBase ? 1 : 0));
        },
        baseUrl() {
          return D.dead ? '' : out(X.base_url(e));
        },
        fetch(url, method, headers, body, timeout, cb) {
          if (typeof cb !== 'function') throw new TypeError('host.fetch: bad arguments');
          if (D.dead) return 0;
          url = text(url);
          method = text(method);
          const pairs = Array.isArray(headers) ? JSON.stringify(headers.map((p) => [text(p[0]), text(p[1])])) : '[]';
          const bytes = bytesOf(body);
          if (!bytes) throw new TypeError('fetch: unsupported body type');
          const bp = bytes.length ? putBytes(bytes) : 0;
          reset();
          const u = put(url), ul = L, m = put(method), ml = L, h = put(pairs), hl = L;
          const id = X.fetch(e, u, ul, m, ml, h, hl, bp, bytes.length, typeof timeout === 'number' ? timeout | 0 : 0);
          if (bp) free(bp);
          D.loads.set(id, cb);
          return id;
        },
        cancel(id) {
          const d = +id;
          if (!(d > 0) || D.dead) return;
          X.cancel(e, d);
          D.loads.delete(d);
        },
      };
    }

    // ------------------------------------------------- __jsui_native
    bridge(rootId, devMode, lottie) {
      const e = this.e, D = this;
      const putStr = (s) => {
        reset();
        return put(s);
      };
      const N = {
        rootId,
        textEditing: true,
        lottie,
        devMode,

        // ---- node tree (@orktes/jsui-react's host config). No updateProps /
        // getProps: the package builds and diffs native props in JS then
        // (instances.js), and only changed props cross, as JSON.
        createNode(type, rootTag) {
          if (D.dead) return 0;
          const t = arguments.length > 0 ? text(type) : '';
          const id = X.create_node(e, putStr(t), L, typeof rootTag === 'number' ? rootTag : 0);
          if (!id) throw new TypeError("unknown element type '" + t + "'");
          return id;
        },
        setProps(id, props) {
          if (D.dead || arguments.length < 2) return;
          const j = json(props);
          X.set_props(e, numArg(id), putStr(j), L);
        },
        patchStyle(id, partial) {
          if (D.dead || arguments.length < 2 || partial === null || typeof partial !== 'object') return;
          // undefined members remove the key, like null (JSON drops them).
          let p = partial;
          for (const k in partial) {
            if (partial[k] === undefined) {
              p = Object.assign({}, partial);
              for (const k2 of Object.keys(p)) if (p[k2] === undefined) p[k2] = null;
              break;
            }
          }
          const j = json(p);
          X.patch_style(e, numArg(id), putStr(j), L);
        },
        setText(id, s) {
          if (D.dead || arguments.length < 2) return;
          X.set_text(e, numArg(id), putStr(text(s)), L);
        },
        appendChild(p, c) {
          if (!D.dead && arguments.length >= 2) X.append_child(e, numArg(p), numArg(c));
        },
        insertBefore(p, c, b) {
          if (!D.dead && arguments.length >= 2) X.insert_before(e, numArg(p), numArg(c), numArg(b));
        },
        removeChild(p, c) {
          if (!D.dead && arguments.length >= 2) X.remove_child(e, numArg(p), numArg(c));
        },
        destroyNode(id) {
          if (!D.dead && arguments.length >= 1) X.destroy_node(e, numArg(id));
        },
        configureLayoutAnimation(cfg) {
          if (D.dead) return;
          if (cfg === null || typeof cfg !== 'object') return void X.configure_layout_animation(e, 0, 0);
          const j = json(cfg);
          X.configure_layout_animation(e, putStr(j), L);
        },
        measure(id) {
          if (D.dead || !X.measure(e, numArg(id))) return null;
          return { x: num(0), y: num(1), width: num(2), height: num(3), pageX: num(4), pageY: num(5) };
        },
        viewport(rootTag) {
          if (D.dead) return { width: 0, height: 0, scale: 1 };
          X.viewport(e, typeof rootTag === 'number' ? rootTag : 0);
          return { width: num(0), height: num(1), scale: num(2) };
        },
        surfaceApp(rootTag) {
          if (D.dead || typeof rootTag !== 'number') return null;
          const p = X.surface_app(e, rootTag);
          return p ? JSON.parse(out(p)) : null;
        },
        focus(id) {
          if (!D.dead) X.focus(e, numArg(id), 0);
        },
        blur(id) {
          if (!D.dead) X.focus(e, numArg(id), 1);
        },
        scrollTo(id, x, y, animated) {
          if (D.dead) return;
          X.scroll_to(e, numArg(id), arguments.length > 1 ? +x : 0, arguments.length > 2 ? +y : 0,
            arguments.length > 3 && animated ? 1 : 0);
        },
        flashScrollIndicators(id) {
          if (!D.dead) X.flash_scroll_indicators(e, numArg(id));
        },
        setTextInputSelection(id, start, end) {
          if (D.dead) return;
          const s = arguments.length > 1 ? +start : 0;
          const en = arguments.length > 2 && end !== undefined && end !== null ? +end : s;
          X.set_text_input_selection(e, numArg(id), s, en);
        },
        getClipboard() {
          return D.dead ? '' : out(X.get_clipboard(e));
        },
        setClipboard(t) {
          if (D.dead) return;
          X.set_clipboard(e, putStr(arguments.length > 0 ? text(t) : ''), L);
        },
        postMessage(name, data) {
          if (D.dead) return;
          const n = arguments.length > 0 ? text(name) : '';
          let j = 'null';
          if (arguments.length > 1) {
            if (typeof data === 'string') j = data;
            else if (data !== undefined) {
              const s = JSON.stringify(data);
              if (typeof s === 'string') j = s;
            }
          }
          reset();
          const np = put(n), nl = L;
          X.post_message(e, np, nl, put(j), L);
        },

        // ---- Lottie / shaders
        lottieData(src) {
          if (typeof src !== 'string') throw new TypeError('lottieData: expected a JSON string');
          return D.dead ? '' : out(X.lottie_data(e, putStr(src), L));
        },
        lottieCommand(id, cmd, a, b) {
          if (D.dead || arguments.length < 2) return false;
          return !!X.lottie_command(e, numArg(id), putStr(text(cmd)), L, numArg(a), numArg(b));
        },
        shaderInfo(sksl) {
          if (typeof sksl !== 'string') throw new TypeError('shaderInfo: expected an SkSL string');
          if (D.dead) return { error: 'the runtime was destroyed' };
          return JSON.parse(out(X.shader_info(e, putStr(sksl), L, 0, 0, 0)));
        },
        shaderPreload(sksl, images) {
          if (typeof sksl !== 'string') throw new TypeError('shaderPreload: expected an SkSL string');
          if (D.dead) return { error: 'the runtime was destroyed' };
          const ij = images !== null && typeof images === 'object' ? json(images) : '';
          reset();
          const s = put(sksl), sl = L;
          const ip = ij ? put(ij) : 0;
          return JSON.parse(out(X.shader_info(e, s, sl, 1, ip, ij ? L : 0)));
        },
        shadersWarming() {
          return D.dead ? 0 : X.shaders_warming(e);
        },

        // ---- fonts
        loadFont(family, url, weight, italic) {
          const fam = typeof family === 'string' ? family : '';
          const u = arguments.length > 1 ? text(url) : '';
          const [w, it] = fontHints(weight, italic);
          if (!u) return Promise.reject(new Error('loadFont(family, url): url is required'));
          return new Promise((resolve, reject) => {
            if (D.dead) return void reject(new Error('loadFont: the runtime was destroyed'));
            reset();
            const fp = put(fam), fl = L;
            const id = X.load_font(e, fp, fl, put(u), L, w, it);
            D.fonts.set(id, [resolve, reject]);
          });
        },
        loadFontData(family, bytes, weight, italic) {
          const fam = typeof family === 'string' ? family : '';
          const u8 = arguments.length < 2 || typeof bytes === 'string' ? null : bytesOf(bytes);
          if (!u8 || !u8.length) throw new TypeError('loadFontData(family, bytes): bytes must be an ArrayBuffer or typed array');
          const [w, it] = fontHints(weight, italic);
          if (D.dead) return false;
          const bp = putBytes(u8);
          const err = X.load_font_data(e, putStr(fam), L, bp, u8.length, w, it);
          const why = out(err);
          free(bp);
          if (err) throw new TypeError('loadFontData: ' + why);
          return true;
        },
        unloadFont(family) {
          if (D.dead || arguments.length < 1) return false;
          return !!X.unload_font(e, putStr(text(family)), L);
        },
        getFontFamilies() {
          return D.dead ? [] : JSON.parse(out(X.get_font_families(e)));
        },
        setFontFallbacks(families) {
          if (D.dead) return;
          const list = [];
          const add = (csv) => {
            for (const f of csv.split(',')) {
              const t = f.replace(/^[ \t"']+|[ \t"']+$/g, '');
              if (t) list.push(t);
            }
          };
          if (Array.isArray(families)) {
            for (const f of families) if (typeof f === 'string') add(f);
          } else if (typeof families === 'string') add(families);
          X.set_font_fallbacks(e, putStr(JSON.stringify(list)), L);
        },
        setDefaultFontFamily(family) {
          if (!D.dead && typeof family === 'string') X.set_default_font_family(e, putStr(family), L);
        },

        // ---- dev
        reload() {
          if (!D.dead) X.reload(e);
        },
        devError(t) {
          if (D.dead) return;
          if (arguments.length < 1 || t === null || t === undefined) X.dev_error(e, 0, 0, 1);
          else X.dev_error(e, putStr(text(t)), L, 0);
        },
        // Runs a script in the runtime's global scope (hot updates). The
        // host engine cannot return a script's completion value: undefined.
        evalScript(source, name) {
          if (D.dead || arguments.length < 1) return undefined;
          D.run(text(source), arguments.length > 1 ? text(name) : '<eval>', false);
          return undefined;
        },

        animated: animatedBridge(D),
      };
      hostDataBridge(D, N);
      audioBridge(D, N);
      return N;
    }
  }

  // QuickJS bytesOf: undefined / null -> empty, string -> UTF-8, ArrayBuffer,
  // typed array, DataView; anything else -> null.
  function bytesOf(v) {
    if (v === undefined || v === null) return new Uint8Array(0);
    if (typeof v === 'string') return encoder.encode(v);
    if (v instanceof ArrayBuffer) return new Uint8Array(v);
    if (ArrayBuffer.isView(v)) return new Uint8Array(v.buffer, v.byteOffset, v.byteLength);
    return null;
  }

  // Font face hints: weight <= 0 and italic < 0 take the file's own.
  function fontHints(weight, italic) {
    const w = typeof weight === 'number' ? weight | 0 : 0;
    const i = typeof italic === 'boolean' ? (italic ? 1 : 0) : typeof italic === 'number' ? italic | 0 : -1;
    return [w, i];
  }

  // __jsui_native.animated (bridge_animated.cpp).
  function animatedBridge(D) {
    const e = D.e;
    const fail = (p) => {
      if (p) throw new TypeError(out(p));
    };
    return {
      createNode(tag, config) {
        if (D.dead) return;
        const j = arguments.length > 1 ? json(config) : '';
        reset();
        fail(X.a_create_node(e, toInt(tag), j ? put(j) : 0, j ? L : 0));
      },
      connect(a, b) { if (!D.dead) X.a_link(e, 0, toInt(a), toInt(b)); },
      disconnect(a, b) { if (!D.dead) X.a_link(e, 1, toInt(a), toInt(b)); },
      connectToView(a, view) { if (!D.dead) X.a_link(e, 2, toInt(a), toInt(view) >>> 0); },
      disconnectFromView(a, view) { if (!D.dead) X.a_link(e, 3, toInt(a), toInt(view) >>> 0); },
      dropNode(tag) { if (!D.dead) X.a_node_op(e, 0, toInt(tag)); },
      flattenOffset(tag) { if (!D.dead) X.a_node_op(e, 1, toInt(tag)); },
      extractOffset(tag) { if (!D.dead) X.a_node_op(e, 2, toInt(tag)); },
      startListening(tag) { if (!D.dead) X.a_node_op(e, 3, toInt(tag)); },
      stopListening(tag) { if (!D.dead) X.a_node_op(e, 4, toInt(tag)); },
      setValue(tag, v) { if (!D.dead) X.a_set_value(e, toInt(tag), +v, 0); },
      setOffset(tag, v) { if (!D.dead) X.a_set_value(e, toInt(tag), +v, 1); },
      getValue(tag) { return D.dead ? 0 : X.a_get_value(e, toInt(tag)); },
      startAnimation(id, tag, config) {
        if (D.dead) return;
        const j = arguments.length > 2 ? json(config) : '';
        reset();
        fail(X.a_start_animation(e, toInt(id), toInt(tag), j ? put(j) : 0, j ? L : 0));
      },
      // -> {velocity} if it was running, else null.
      stopAnimation(id) {
        if (D.dead) return null;
        const v = X.a_stop_animation(e, toInt(id));
        return v === v ? { velocity: v } : null;
      },
      addEventMapping(view, type, path, tag) {
        if (D.dead || arguments.length < 4) return;
        const t = text(type);
        const keys = JSON.stringify(Array.isArray(path) ? path.map((k) => (typeof k === 'string' ? k : '')) : []);
        reset();
        const tp = put(t), tl = L;
        X.a_event_mapping(e, toInt(view) >>> 0, tp, tl, put(keys), L, toInt(tag), 0);
      },
      removeEventMapping(view, type, tag) {
        if (D.dead || arguments.length < 3) return;
        X.a_event_mapping(e, toInt(view) >>> 0, putStr(text(type)), L, 0, 0, toInt(tag), 1);
      },
    };
    function putStr(s) {
      reset();
      return put(s);
    }
  }

  // Shared state, frame data, host textures and calls (bridge_host.cpp).
  function hostDataBridge(D, N) {
    const e = D.e;
    const keyArg = (k) => {
      reset();
      return put(text(k));
    };
    N.getState = function (key) {
      if (D.dead || arguments.length < 1) return undefined;
      const p = X.get_state(e, keyArg(key), L);
      return p ? out(p) : undefined;
    };
    N.stateVersion = function (key) {
      return D.dead || arguments.length < 1 ? 0 : X.state_version(e, keyArg(key), L);
    };
    N.stateKeys = () => (D.dead ? [] : JSON.parse(out(X.state_keys(e))));
    N.statePatches = function (key, since) {
      if (D.dead || arguments.length < 2) return undefined;
      const s = +since;
      const k = text(key);
      const p = X.state_patches(e, putFresh(k), L, s);
      return p ? JSON.parse(out(p)) : undefined;
    };
    const write = (what, patch) => function (key, value) {
      if (arguments.length < 1) throw new TypeError(what + '(key, json): key is required');
      if (D.dead) return 0;
      const k = text(key);
      const remove = arguments.length < 2 || value === undefined;
      const j = remove ? '' : text(value);
      reset();
      const kp = put(k), kl = L;
      const jp = remove ? 0 : put(j), jl = remove ? 0 : L;
      const v = X.write_state(e, kp, kl, jp, jl, remove ? 1 : 0, patch);
      if (v < 0) throw new SyntaxError(what + ': invalid JSON (' + out(X.last_string()) + ')');
      return v;
    };
    N.setState = write('setState', 0);
    N.patchState = write('patchState', 1);
    // [ArrayBuffer, type, count, version]: a copy of the buffer's elements
    // per version (an ArrayBuffer cannot alias a range of WASM memory), the
    // same object while the version stays.
    N.buffer = function (key) {
      if (D.dead || arguments.length < 1) return undefined;
      const k = text(key);
      if (!X.buffer(e, putFresh(k), L)) {
        D.buffers.delete(k);
        return undefined;
      }
      const addr = num(0), type = num(1), count = num(2), version = num(3);
      let c = D.buffers.get(k);
      if (!c || c.version !== version || c.type !== type || c.count !== count) {
        c = { version, type, count, buffer: HEAPU8.slice(addr, addr + count * ELEMENT_SIZES[type]).buffer };
        D.buffers.set(k, c);
      }
      return [c.buffer, type, count, version];
    };
    N.bufferVersion = function (key) {
      return D.dead || arguments.length < 1 ? 0 : X.buffer_version(e, keyArg(key), L);
    };
    N.bufferKeys = () => (D.dead ? [] : JSON.parse(out(X.buffer_keys(e))));
    N.texture = function (key) {
      if (D.dead || arguments.length < 1 || !X.texture(e, keyArg(key), L)) return undefined;
      return [num(0), num(1), num(2)];
    };
    N.textureKeys = () => (D.dead ? [] : JSON.parse(out(X.texture_keys(e))));
    N.hostCall = function (name, args, settle) {
      if (arguments.length < 3 || typeof settle !== 'function') throw new TypeError('hostCall(name, json, settle)');
      if (D.dead) return 0;
      const n = text(name);
      const j = args === undefined ? 'null' : text(args);
      const id = X.new_call_id(e);
      if (!id) return 0;
      // Registered before on_call runs: the embedder may answer from inside it.
      D.hostCalls.set(id, settle);
      reset();
      const np = put(n), nl = L;
      if (!X.host_call(e, id, np, nl, put(j), L)) {
        D.hostCalls.delete(id);
        return 0;
      }
      return id;
    };
    N.cancelCall = function (id) {
      const d = arguments.length > 0 ? toId(id) : 0;
      if (!d || D.dead) return false;
      D.hostCalls.delete(d);
      return !!X.cancel_call(e, d);
    };
    N.callResult = function (id, ok, payload) {
      const d = arguments.length > 0 ? toId(id) : 0;
      if (!d || D.dead) return;
      const k = arguments.length > 1 && !!ok;
      const p = arguments.length > 2 && payload !== undefined ? text(payload) : k ? 'null' : '';
      X.call_result(e, d, k ? 1 : 0, putFresh(p), L);
    };
    function putFresh(s) {
      reset();
      return put(s);
    }
  }

  // UI audio (bridge_audio.cpp).
  function audioBridge(D, N) {
    const e = D.e;
    const n = (v, fallback) => {
      if (v === undefined) return fallback;
      let d;
      try { d = Number(v); } catch (_) { return fallback; }
      return d === d ? d : fallback;
    };
    const s = (v) => (v === undefined || v === null ? '' : text(v));
    N.audioPlay = function (url, bus, volume, pitch, pan, loop, fadeMs, offsetS) {
      const u = s(url);
      if (!u) throw new TypeError('audioPlay: no sound');
      if (D.dead) return 0;
      const b = s(bus);
      reset();
      const up = put(u), ul = L;
      const bp = put(b), bl = L;
      return X.audio_play(e, up, ul, bp, bl, n(volume, 1), n(pitch, 1), n(pan, 0), arguments.length > 5 && loop ? 1 : 0,
        n(fadeMs, 0), n(offsetS, 0));
    };
    N.audioVoice = function (op, voice, volume, pitch, pan, fadeMs) {
      if (!D.dead) X.audio_voice(e, Math.trunc(n(op, -1)), n(voice, 0), n(volume, 1), n(pitch, 1), n(pan, 0), n(fadeMs, 0));
    };
    N.audioBus = function (bus, volume, muted, fadeMs) {
      const b = s(bus);
      if (!b || D.dead) return;
      reset();
      X.audio_bus(e, put(b), L, n(volume, 1), arguments.length > 2 && muted ? 1 : 0, n(fadeMs, 0));
    };
    N.audioStopAll = function (bus, fadeMs) {
      if (D.dead) return;
      const b = s(bus);
      reset();
      X.audio_stop_all(e, put(b), L, n(fadeMs, 0));
    };
    ['audioPreload', 'audioUnload', 'audioResolve'].forEach((name, magic) => {
      N[name] = function (url) {
        const u = s(url);
        if (!u || D.dead) return undefined;
        reset();
        return out(X.audio_sound(e, put(u), L, magic));
      };
    });
  }

  return {
    str,
    create(e, rootId, devMode, lottie) {
      engines.set(e, new Engine(e, rootId, devMode, lottie));
    },
    prelude(e, name, source) {
      engines.get(e).prelude(name, source);
    },
    destroy(e) {
      const eng = engines.get(e);
      if (!eng) return;
      eng.dead = true;
      eng.timers.clear();
      eng.rafs = [];
      eng.micro = [];
      eng.hostCalls.clear();
      eng.loads.clear();
      eng.fonts.clear();
      engines.delete(e);
      if (lastActive === eng) lastActive = null;
    },
    engine: (e) => engines.get(e),
  };
})();
