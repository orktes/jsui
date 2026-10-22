// The JS half of the web-style globals: console (with %s formatting),
// reportError, structuredClone, process/navigator/crypto, Event/EventTarget,
// AbortController, MessageChannel, setImmediate, window/self/location,
// TextEncoder/TextDecoder, URL/URLSearchParams, Headers/Request/Response and
// fetch. Each piece is a function expression called with the native
// __jsui_host object (host_api.cpp). Pieces are split into adjacent string
// literals below 16 KB each (MSVC limit).
#include <cstring>
#include <string>

#include "js/engine_impl.h"
#include "runtime.h"

namespace jsui {

namespace {

struct PreludePiece {
  const char* name;
  const char* source;
};

// clang-format off
const PreludePiece kPrelude[] = {
{"jsui:prelude/console.js",
R"JSUI((function (host) {
  'use strict';
  const G = globalThis;
  const define = (obj, name, value) =>
    Object.defineProperty(obj, name, { value, writable: true, configurable: true, enumerable: false });

  // ---------------------------------------------------------------- inspect
  const MAX_ITEMS = 100;
  const MAX_STRING = 10000;

  function quote(s) {
    return "'" + s.replace(/\\/g, '\\\\').replace(/'/g, "\\'").replace(/\n/g, '\\n') + "'";
  }

  function errorString(e) {
    let head;
    try { head = String(e); } catch (_) { head = 'Error'; }
    const stack = typeof e.stack === 'string' ? e.stack.replace(/\s+$/, '') : '';
    let s = stack ? head + '\n' + stack : head;
    if (e.cause !== undefined) s += '\n  [cause]: ' + inspect(e.cause, 2, []);
    return s;
  }

  function keyString(k) {
    return /^[A-Za-z_$][\w$]*$/.test(k) ? k : quote(k);
  }

  function inspect(v, depth, seen) {
    switch (typeof v) {
      case 'string': return quote(v.length > MAX_STRING ? v.slice(0, MAX_STRING) + '...' : v);
      case 'number': return Object.is(v, -0) ? '-0' : String(v);
      case 'bigint': return v + 'n';
      case 'boolean': case 'undefined': return String(v);
      case 'symbol': return v.toString();
      case 'function': return '[Function: ' + (v.name || '(anonymous)') + ']';
    }
    if (v === null) return 'null';
    if (seen.includes(v)) return '[Circular]';
    if (v instanceof Error) return errorString(v);
    if (v instanceof Date) return isNaN(v) ? 'Invalid Date' : v.toISOString();
    if (v instanceof RegExp) return String(v);
    if (typeof v.then === 'function' && v instanceof Promise) return 'Promise {}';
    if (depth < 0) return Array.isArray(v) ? '[Array]' : '[Object]';
    seen = seen.concat([v]);
    const parts = [];
    let open = '{', close = '}', prefix = '';
    if (Array.isArray(v) || ArrayBuffer.isView(v)) {
      open = '['; close = ']';
      if (!Array.isArray(v)) prefix = (v.constructor && v.constructor.name || 'TypedArray') + '(' + v.length + ') ';
      const n = Math.min(v.length, MAX_ITEMS);
      for (let i = 0; i < n; i++) parts.push(inspect(v[i], depth - 1, seen));
      if (v.length > n) parts.push('... ' + (v.length - n) + ' more items');
    } else if (v instanceof Map) {
      prefix = 'Map(' + v.size + ') ';
      let i = 0;
      for (const [k, x] of v) {
        if (i++ >= MAX_ITEMS) { parts.push('...'); break; }
        parts.push(inspect(k, depth - 1, seen) + ' => ' + inspect(x, depth - 1, seen));
      }
    } else if (v instanceof Set) {
      prefix = 'Set(' + v.size + ') ';
      let i = 0;
      for (const x of v) {
        if (i++ >= MAX_ITEMS) { parts.push('...'); break; }
        parts.push(inspect(x, depth - 1, seen));
      }
    } else if (v instanceof ArrayBuffer) {
      return 'ArrayBuffer { byteLength: ' + v.byteLength + ' }';
    } else {
      const ctor = v.constructor;
      if (ctor && ctor !== Object && typeof ctor.name === 'string' && ctor.name) prefix = ctor.name + ' ';
      let keys;
      try { keys = Object.keys(v); } catch (_) { keys = []; }
      const n = Math.min(keys.length, MAX_ITEMS);
      for (let i = 0; i < n; i++) {
        let x;
        try { x = v[keys[i]]; } catch (e) { x = '[Getter threw]'; }
        parts.push(keyString(keys[i]) + ': ' + inspect(x, depth - 1, seen));
      }
      if (keys.length > n) parts.push('... ' + (keys.length - n) + ' more keys');
    }
    if (!parts.length) return prefix + open + close;
    const one = prefix + open + ' ' + parts.join(', ') + ' ' + close;
    return one;
  }

  function str(v) { return typeof v === 'string' ? v : inspect(v, 4, []); }

  // console.log formatting: %s %d %i %f %o %O %j %c %%
  function format(args) {
    if (!args.length) return '';
    let out, i;
    if (typeof args[0] === 'string' && args.length > 1 && args[0].indexOf('%') >= 0) {
      const f = args[0];
      out = '';
      i = 1;
      for (let k = 0; k < f.length; k++) {
        const c = f.charCodeAt(k);
        if (c === 37 /* % */ && k + 1 < f.length) {
          const n = f[k + 1];
          if (n === '%') { out += '%'; k++; continue; }
          if ('sdifoOjc'.indexOf(n) >= 0) {
            k++;
            if (n === 'c') { i++; continue; }  // CSS: ignored
            if (i >= args.length) { out += '%' + n; continue; }
            const a = args[i++];
            switch (n) {
              case 's': out += typeof a === 'string' ? a : (typeof a === 'object' && a !== null && !(a instanceof Error) ? inspect(a, 1, []) : (a instanceof Error ? errorString(a) : String(a))); break;
              case 'd': case 'i':
                if (typeof a === 'bigint') out += a + 'n';
                else if (typeof a === 'symbol') out += 'NaN';
                else { const x = Number(a); out += String(n === 'i' ? Math.trunc(x) : x); }
                break;
              case 'f': out += typeof a === 'symbol' ? 'NaN' : String(parseFloat(a)); break;
              case 'j': try { out += JSON.stringify(a); } catch (_) { out += '[Circular]'; } break;
              default: out += inspect(a, n === 'o' ? 4 : 2, []);
            }
            continue;
          }
        }
        out += f[k];
      }
    } else {
      out = str(args[0]);
      i = 1;
    }
    for (; i < args.length; i++) out += ' ' + str(args[i]);
    return out;
  }

  // ---------------------------------------------------------------- console
  const DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3;
  let indent = '';
  const counts = new Map();
  const timers = new Map();
  const emit = (level, text) => host.log(level, indent ? indent + text.replace(/\n/g, '\n' + indent) : text);
  const console = {
    log: (...a) => emit(INFO, format(a)),
    info: (...a) => emit(INFO, format(a)),
    debug: (...a) => emit(DEBUG, format(a)),
    warn: (...a) => emit(WARN, format(a)),
    error: (...a) => emit(ERROR, format(a)),
    trace: (...a) => {
      const st = (new Error().stack || '').split('\n').slice(1).join('\n');
      emit(INFO, 'Trace' + (a.length ? ': ' + format(a) : '') + '\n' + st);
    },
    dir: (v) => emit(INFO, inspect(v, 4, [])),
    dirxml: (...a) => emit(INFO, format(a)),
    table: (v) => emit(INFO, inspect(v, 3, [])),
    assert: (cond, ...a) => { if (!cond) emit(ERROR, 'Assertion failed' + (a.length ? ': ' + format(a) : '')); },
    group: (...a) => { if (a.length) emit(INFO, format(a)); indent += '  '; },
    groupCollapsed: (...a) => { if (a.length) emit(INFO, format(a)); indent += '  '; },
    groupEnd: () => { indent = indent.slice(2); },
    count: (label = 'default') => { const n = (counts.get(label) || 0) + 1; counts.set(label, n); emit(INFO, label + ': ' + n); },
    countReset: (label = 'default') => { counts.delete(label); },
    time: (label = 'default') => { timers.set(label, performance.now()); },
    timeLog: (label = 'default', ...a) => {
      if (!timers.has(label)) return emit(WARN, "Timer '" + label + "' does not exist");
      emit(INFO, label + ': ' + (performance.now() - timers.get(label)).toFixed(3) + 'ms' + (a.length ? ' ' + format(a) : ''));
    },
    timeEnd: (label = 'default') => {
      if (!timers.has(label)) return emit(WARN, "Timer '" + label + "' does not exist");
      emit(INFO, label + ': ' + (performance.now() - timers.get(label)).toFixed(3) + 'ms');
      timers.delete(label);
    },
    timeStamp: () => {},
    profile: () => {},
    profileEnd: () => {},
    clear: () => {},
  };
  define(G, 'console', console);

  // reportError(e): what React 19 calls for uncaught render errors.
  define(G, 'reportError', function reportError(e) { host.reportError(e, 'Uncaught '); });

  // ------------------------------------------------------- structuredClone
  function clone(v, seen) {
    if (typeof v !== 'object' || v === null) {
      if (typeof v === 'function' || typeof v === 'symbol')
        throw new (G.DOMException || Error)('value could not be cloned', 'DataCloneError');
      return v;
    }
    if (seen.has(v)) return seen.get(v);
    let out;
    if (v instanceof Date) out = new Date(v.getTime());
    else if (v instanceof RegExp) out = new RegExp(v.source, v.flags);
    else if (v instanceof ArrayBuffer) out = v.slice(0);
    else if (ArrayBuffer.isView(v)) out = new v.constructor(v.buffer.slice(v.byteOffset, v.byteOffset + v.byteLength));
    else if (v instanceof Map) { out = new Map(); seen.set(v, out); for (const [k, x] of v) out.set(clone(k, seen), clone(x, seen)); return out; }
    else if (v instanceof Set) { out = new Set(); seen.set(v, out); for (const x of v) out.add(clone(x, seen)); return out; }
    else if (v instanceof Error) { out = new v.constructor(v.message); if (v.stack) out.stack = v.stack; }
    else if (Array.isArray(v)) { out = []; seen.set(v, out); for (let i = 0; i < v.length; i++) out[i] = clone(v[i], seen); return out; }
    else { out = {}; seen.set(v, out); for (const k of Object.keys(v)) out[k] = clone(v[k], seen); return out; }
    seen.set(v, out);
    return out;
  }
  if (typeof G.structuredClone !== 'function') define(G, 'structuredClone', (v) => clone(v, new Map()));

  // ------------------------------------------------------ process/navigator
  if (typeof G.process === 'undefined') {
    define(G, 'process', {
      env: { NODE_ENV: host.devMode ? 'development' : 'production' },
      nextTick: (fn, ...args) => queueMicrotask(() => fn(...args)),
      platform: 'jsui',
      browser: true,
    });
  }
  if (typeof G.navigator === 'undefined') {
    define(G, 'navigator', { userAgent: 'JSUI/0.1', language: 'en-US', languages: ['en-US'], onLine: true, hardwareConcurrency: 1 });
  }
  if (typeof G.crypto === 'undefined') {
    // Not cryptographically secure: ids / keys for UI code only.
    define(G, 'crypto', {
      getRandomValues(a) {
        if (!ArrayBuffer.isView(a)) throw new TypeError('getRandomValues: expected an integer TypedArray');
        const bytes = new Uint8Array(a.buffer, a.byteOffset, a.byteLength);
        for (let i = 0; i < bytes.length; i++) bytes[i] = (Math.random() * 256) | 0;
        return a;
      },
      randomUUID() {
        const b = this.getRandomValues(new Uint8Array(16));
        b[6] = (b[6] & 0x0f) | 0x40; b[8] = (b[8] & 0x3f) | 0x80;
        const h = Array.from(b, (x) => (x + 0x100).toString(16).slice(1)).join('');
        return h.slice(0, 8) + '-' + h.slice(8, 12) + '-' + h.slice(12, 16) + '-' + h.slice(16, 20) + '-' + h.slice(20);
      },
    });
  }
  define(G, '__jsui_inspect', (v) => inspect(v, 4, []));
})
)JSUI"
},
{"jsui:prelude/events.js",
R"JSUI((function (host) {
  'use strict';
  const G = globalThis;
  const define = (obj, name, value) =>
    Object.defineProperty(obj, name, { value, writable: true, configurable: true, enumerable: false });

  // ---------------------------------------------------- Event / EventTarget
  class Event {
    constructor(type, init) {
      init = init || {};
      this.type = String(type);
      this.bubbles = !!init.bubbles;
      this.cancelable = !!init.cancelable;
      this.composed = !!init.composed;
      this.defaultPrevented = false;
      this.target = null;
      this.currentTarget = null;
      this.timeStamp = performance.now();
      this.isTrusted = false;
      this._stop = false;
    }
    preventDefault() { if (this.cancelable) this.defaultPrevented = true; }
    stopPropagation() {}
    stopImmediatePropagation() { this._stop = true; }
  }
  class CustomEvent extends Event {
    constructor(type, init) { super(type, init); this.detail = init && init.detail !== undefined ? init.detail : null; }
  }

  const LISTENERS = Symbol('listeners');
  class EventTarget {
    constructor() { define(this, LISTENERS, new Map()); }
    addEventListener(type, fn, opts) {
      if (!fn) return;
      if (!this[LISTENERS]) define(this, LISTENERS, new Map());
      const once = typeof opts === 'object' && opts !== null && !!opts.once;
      const signal = typeof opts === 'object' && opts !== null ? opts.signal : undefined;
      if (signal && signal.aborted) return;
      let list = this[LISTENERS].get(type);
      if (!list) this[LISTENERS].set(type, (list = []));
      if (list.some((l) => l.fn === fn)) return;
      const entry = { fn, once };
      list.push(entry);
      if (signal) signal.addEventListener('abort', () => this.removeEventListener(type, fn));
    }
    removeEventListener(type, fn) {
      const list = this[LISTENERS] && this[LISTENERS].get(type);
      if (!list) return;
      const i = list.findIndex((l) => l.fn === fn);
      if (i >= 0) list.splice(i, 1);
    }
    dispatchEvent(ev) {
      try { ev.target = this; ev.currentTarget = this; } catch (_) {}
      const handlerProp = this['on' + ev.type];
      const list = (this[LISTENERS] && this[LISTENERS].get(ev.type)) || [];
      const run = (fn) => {
        try {
          if (typeof fn === 'function') fn.call(this, ev);
          else if (fn && typeof fn.handleEvent === 'function') fn.handleEvent(ev);
        } catch (e) {
          G.reportError(e);
        }
      };
      if (typeof handlerProp === 'function') run(handlerProp);
      for (const l of list.slice()) {
        if (ev._stop) break;
        if (l.once) this.removeEventListener(ev.type, l.fn);
        run(l.fn);
      }
      return !ev.defaultPrevented;
    }
  }
  define(G, 'Event', Event);
  define(G, 'CustomEvent', CustomEvent);
  define(G, 'EventTarget', EventTarget);

  // ------------------------------------------------- AbortController/Signal
  const DOMEx = G.DOMException;
  const abortError = (msg) => (DOMEx ? new DOMEx(msg || 'This operation was aborted', 'AbortError') : Object.assign(new Error(msg || 'aborted'), { name: 'AbortError' }));
  class AbortSignal extends EventTarget {
    constructor() { super(); this.aborted = false; this.reason = undefined; this.onabort = null; }
    throwIfAborted() { if (this.aborted) throw this.reason; }
    _abort(reason) {
      if (this.aborted) return;
      this.aborted = true;
      this.reason = reason === undefined ? abortError() : reason;
      this.dispatchEvent(new Event('abort'));
    }
    static abort(reason) { const s = new AbortSignal(); s._abort(reason); return s; }
    static timeout(ms) {
      const s = new AbortSignal();
      setTimeout(() => s._abort(DOMEx ? new DOMEx('signal timed out', 'TimeoutError') : new Error('TimeoutError')), ms);
      return s;
    }
    static any(signals) {
      const s = new AbortSignal();
      for (const x of signals) {
        if (x.aborted) { s._abort(x.reason); break; }
        x.addEventListener('abort', () => s._abort(x.reason));
      }
      return s;
    }
  }
  class AbortController {
    constructor() { this.signal = new AbortSignal(); }
    abort(reason) { this.signal._abort(reason); }
  }
  define(G, 'AbortSignal', AbortSignal);
  define(G, 'AbortController', AbortController);
  define(G, '__jsui_abortError', abortError);

  // ---------------------------------------------- MessageChannel / Immediate
  // Built on the timer queue: a posted message is a zero-delay macrotask,
  // which is what React's scheduler expects from MessageChannel.
  const nativeSetTimeout = G.setTimeout, nativeClearTimeout = G.clearTimeout;
  class MessagePort extends EventTarget {
    constructor() { super(); this.onmessage = null; this.onmessageerror = null; this._other = null; this._closed = false; }
    postMessage(data) {
      const other = this._other;
      if (!other || this._closed) return;
      nativeSetTimeout(() => {
        if (other._closed) return;
        const ev = new Event('message');
        ev.data = data;
        ev.ports = [];
        other.dispatchEvent(ev);
      }, 0);
    }
    start() {}
    close() { this._closed = true; if (this._other) this._other._closed = true; }
  }
  class MessageChannel {
    constructor() {
      this.port1 = new MessagePort();
      this.port2 = new MessagePort();
      this.port1._other = this.port2;
      this.port2._other = this.port1;
    }
  }
  define(G, 'MessagePort', MessagePort);
  define(G, 'MessageChannel', MessageChannel);
  define(G, 'setImmediate', (fn, ...args) => nativeSetTimeout(fn, 0, ...args));
  define(G, 'clearImmediate', (id) => nativeClearTimeout(id));

  // ------------------------------------------------------- window / location
  define(G, 'window', G);
  define(G, 'self', G);
  const winTarget = new EventTarget();
  define(G, 'addEventListener', winTarget.addEventListener.bind(winTarget));
  define(G, 'removeEventListener', winTarget.removeEventListener.bind(winTarget));
  define(G, 'dispatchEvent', winTarget.dispatchEvent.bind(winTarget));
  define(G, 'devicePixelRatio', 1);
})
)JSUI"
},
{"jsui:prelude/url.js",
R"JSUI((function (host) {
  'use strict';
  const G = globalThis;
  const define = (obj, name, value) =>
    Object.defineProperty(obj, name, { value, writable: true, configurable: true, enumerable: false });

  // -------------------------------------------------- TextEncoder/Decoder
  class TextEncoder {
    get encoding() { return 'utf-8'; }
    encode(s) { return host.utf8Encode(s === undefined ? '' : String(s)); }
    encodeInto(s, dest) {
      const bytes = host.utf8Encode(String(s));
      // Only write whole code points.
      let n = Math.min(bytes.length, dest.length);
      while (n > 0 && n < bytes.length && (bytes[n] & 0xc0) === 0x80) n--;
      dest.set(bytes.subarray(0, n));
      let read = 0;
      for (let i = 0; i < n; i++) if ((bytes[i] & 0xc0) !== 0x80) read += bytes[i] >= 0xf0 ? 2 : 1;
      return { read, written: n };
    }
  }
  const LABELS = ['utf-8', 'utf8', 'unicode-1-1-utf-8'];
  class TextDecoder {
    constructor(label, opts) {
      label = label === undefined ? 'utf-8' : String(label).trim().toLowerCase();
      if (LABELS.indexOf(label) < 0) throw new RangeError("TextDecoder: unsupported encoding '" + label + "' (only utf-8)");
      this._fatal = !!(opts && opts.fatal);
      this._ignoreBOM = !!(opts && opts.ignoreBOM);
      this._pending = null;
      this._mid = false;
    }
    get encoding() { return 'utf-8'; }
    get fatal() { return this._fatal; }
    get ignoreBOM() { return this._ignoreBOM; }
    decode(input, opts) {
      const stream = !!(opts && opts.stream);
      let bytes;
      if (input === undefined || input === null) bytes = new Uint8Array(0);
      else if (input instanceof ArrayBuffer) bytes = new Uint8Array(input);
      else if (ArrayBuffer.isView(input)) bytes = new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
      else throw new TypeError('TextDecoder.decode: expected an ArrayBuffer or ArrayBufferView');
      if (this._pending) {
        const joined = new Uint8Array(this._pending.length + bytes.length);
        joined.set(this._pending);
        joined.set(bytes, this._pending.length);
        bytes = joined;
        this._pending = null;
      }
      if (stream) {
        // Hold back an incomplete trailing sequence for the next call.
        let i = bytes.length - 1, back = 0;
        while (i >= 0 && back < 3 && (bytes[i] & 0xc0) === 0x80) { i--; back++; }
        if (i >= 0) {
          const b = bytes[i];
          const need = b >= 0xf0 ? 4 : b >= 0xe0 ? 3 : b >= 0xc0 ? 2 : 1;
          if (need > back + 1) { this._pending = bytes.slice(i); bytes = bytes.subarray(0, i); }
        }
      }
      // A BOM is only stripped at the start of a stream.
      const stripBOM = !this._ignoreBOM && !this._mid;
      const out = host.utf8Decode(bytes, this._fatal, !stripBOM);
      this._mid = stream && (this._mid || bytes.length > 0);
      return out;
    }
  }
  define(G, 'TextEncoder', TextEncoder);
  define(G, 'TextDecoder', TextDecoder);

  // ------------------------------------------------------- URLSearchParams
  const enc = (s) => encodeURIComponent(s).replace(/%20/g, '+').replace(/[!'()~]/g, (c) => '%' + c.charCodeAt(0).toString(16).toUpperCase());
  const dec = (s) => { try { return decodeURIComponent(s.replace(/\+/g, ' ')); } catch (_) { return s; } };
  class URLSearchParams {
    constructor(init) {
      this._list = [];
      this._url = null;
      if (init === undefined || init === null) return;
      if (typeof init === 'object') {
        if (typeof init[Symbol.iterator] === 'function') {
          for (const pair of init) {
            const p = Array.from(pair);
            if (p.length !== 2) throw new TypeError('URLSearchParams: each pair needs exactly two items');
            this._list.push([String(p[0]), String(p[1])]);
          }
        } else {
          for (const k of Object.keys(init)) this._list.push([k, String(init[k])]);
        }
      } else {
        this._parse(String(init));
      }
    }
    _parse(s) {
      this._list = [];
      if (s[0] === '?') s = s.slice(1);
      for (const part of s.split('&')) {
        if (!part) continue;
        const eq = part.indexOf('=');
        this._list.push(eq < 0 ? [dec(part), ''] : [dec(part.slice(0, eq)), dec(part.slice(eq + 1))]);
      }
    }
    _update() { if (this._url) { const s = this.toString(); this._url._search = s ? '?' + s : ''; } }
    get size() { return this._list.length; }
    append(k, v) { this._list.push([String(k), String(v)]); this._update(); }
    delete(k, v) { k = String(k); this._list = this._list.filter((p) => p[0] !== k || (v !== undefined && p[1] !== String(v))); this._update(); }
    get(k) { k = String(k); const p = this._list.find((x) => x[0] === k); return p ? p[1] : null; }
    getAll(k) { k = String(k); return this._list.filter((x) => x[0] === k).map((x) => x[1]); }
    has(k, v) { k = String(k); return this._list.some((x) => x[0] === k && (v === undefined || x[1] === String(v))); }
    set(k, v) {
      k = String(k); v = String(v);
      const i = this._list.findIndex((x) => x[0] === k);
      if (i < 0) this._list.push([k, v]);
      else { this._list[i][1] = v; this._list = this._list.filter((x, j) => j <= i || x[0] !== k); }
      this._update();
    }
    sort() { this._list.sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0)); this._update(); }
    forEach(fn, thisArg) { for (const [k, v] of this._list) fn.call(thisArg, v, k, this); }
    keys() { return this._list.map((p) => p[0])[Symbol.iterator](); }
    values() { return this._list.map((p) => p[1])[Symbol.iterator](); }
    entries() { return this._list.map((p) => [p[0], p[1]])[Symbol.iterator](); }
    [Symbol.iterator]() { return this.entries(); }
    toString() { return this._list.map((p) => enc(p[0]) + '=' + enc(p[1])).join('&'); }
    get [Symbol.toStringTag]() { return 'URLSearchParams'; }
  }
  define(G, 'URLSearchParams', URLSearchParams);

  // ------------------------------------------------------------------ URL
  // A practical subset of WHATWG URL: hierarchical schemes (http, https,
  // file, ws, ...) with authority, plus opaque ones (data:, blob:, mailto:).
  const DEFAULT_PORTS = { 'http:': '80', 'https:': '443', 'ws:': '80', 'wss:': '443', 'ftp:': '21' };
  const SPECIAL = { 'http:': 1, 'https:': 1, 'ws:': 1, 'wss:': 1, 'ftp:': 1, 'file:': 1 };
  const SCHEME_RE = /^([a-zA-Z][a-zA-Z0-9+.-]*):/;

  function removeDots(path) {
    const out = [];
    const segs = path.split('/');
    for (let i = 0; i < segs.length; i++) {
      const s = segs[i];
      const last = i === segs.length - 1;
      if (s === '..' || s.toLowerCase() === '%2e%2e') { if (out.length > 1) out.pop(); if (last) out.push(''); }
      else if (s === '.' || s.toLowerCase() === '%2e') { if (last) out.push(''); }
      else out.push(s);
    }
    let r = out.join('/');
    if (path[0] === '/' && r[0] !== '/') r = '/' + r;
    return r;
  }
  const encPath = (p) => p.replace(/[ "<>`{}]/g, (c) => '%' + c.charCodeAt(0).toString(16).toUpperCase());

  function parseAbsolute(u, s) {
    const m = SCHEME_RE.exec(s);
    u._protocol = m[1].toLowerCase() + ':';
    let rest = s.slice(m[0].length);
    const hashAt = rest.indexOf('#');
    u._hash = hashAt >= 0 && rest.length > hashAt + 1 ? rest.slice(hashAt) : '';
    if (hashAt >= 0) rest = rest.slice(0, hashAt);
    const special = SPECIAL[u._protocol];
    if (special) rest = rest.replace(/\\/g, '/');
    const qAt = rest.indexOf('?');
    u._search = qAt >= 0 && rest.length > qAt + 1 ? rest.slice(qAt) : '';
    if (qAt >= 0) rest = rest.slice(0, qAt);
    u._username = u._password = u._hostname = u._port = '';
    if (rest.startsWith('//') || (special && u._protocol !== 'file:')) {
      // file:///path has an empty host; special schemes tolerate extra slashes.
      rest = special && u._protocol !== 'file:' ? rest.replace(/^\/*/, '') : rest.slice(2);
      const slash = rest.indexOf('/');
      let auth = slash >= 0 ? rest.slice(0, slash) : rest;
      rest = slash >= 0 ? rest.slice(slash) : '';
      const at = auth.lastIndexOf('@');
      if (at >= 0) {
        const cred = auth.slice(0, at);
        auth = auth.slice(at + 1);
        const c = cred.indexOf(':');
        u._username = c >= 0 ? cred.slice(0, c) : cred;
        u._password = c >= 0 ? cred.slice(c + 1) : '';
      }
      const pm = /^(\[[^\]]*\]|[^:]*)(?::(\d*))?$/.exec(auth);
      if (!pm) throw new TypeError('Invalid URL: ' + s);
      u._hostname = pm[1].toLowerCase();
      u._port = pm[2] || '';
      if (u._port && DEFAULT_PORTS[u._protocol] === u._port) u._port = '';
      if (special && u._protocol !== 'file:' && !u._hostname) throw new TypeError('Invalid URL: ' + s);
      u._pathname = special ? removeDots(rest || '/') : rest;
      u._opaque = false;
    } else if (rest[0] === '/' || special) {
      u._pathname = removeDots(rest || '/');
      u._opaque = false;
    } else {
      u._pathname = rest;
      u._opaque = true;
    }
    u._pathname = encPath(u._pathname);
  }

  class URL {
    constructor(url, base) {
      url = String(url).trim();
      if (SCHEME_RE.test(url) && !(base !== undefined && /^[a-zA-Z]:[\\/]/.test(url))) {
        parseAbsolute(this, url);
      } else {
        if (base === undefined) throw new TypeError('Invalid URL: ' + url);
        const b = base instanceof URL ? base : new URL(String(base));
        if (b._opaque) throw new TypeError('Invalid URL: ' + url + ' (base is opaque)');
        this._protocol = b._protocol;
        this._username = b._username;
        this._password = b._password;
        this._hostname = b._hostname;
        this._port = b._port;
        this._opaque = false;
        let r = url;
        if (SPECIAL[b._protocol]) r = r.replace(/\\/g, '/');
        const hashAt = r.indexOf('#');
        const hash = hashAt >= 0 && r.length > hashAt + 1 ? r.slice(hashAt) : '';
        if (hashAt >= 0) r = r.slice(0, hashAt);
        if (r.startsWith('//')) { parseAbsolute(this, b._protocol + r); this._hash = hash; }
        else {
          const qAt = r.indexOf('?');
          const search = qAt >= 0 ? (r.length > qAt + 1 ? r.slice(qAt) : '') : null;
          const path = qAt >= 0 ? r.slice(0, qAt) : r;
          if (path === '') {
            this._pathname = b._pathname;
            this._search = search === null ? b._search : search;
          } else if (path[0] === '/') {
            this._pathname = encPath(removeDots(path));
            this._search = search || '';
          } else {
            const dir = b._pathname.slice(0, b._pathname.lastIndexOf('/') + 1);
            this._pathname = encPath(removeDots(dir + path));
            this._search = search || '';
          }
          this._hash = hash;
        }
      }
      this._params = new URLSearchParams(this._search);
      this._params._url = this;
    }
    static canParse(url, base) { try { new URL(url, base); return true; } catch (_) { return false; } }
    static parse(url, base) { try { return new URL(url, base); } catch (_) { return null; } }
    get protocol() { return this._protocol; }
    set protocol(v) { v = String(v).replace(/:.*$/, ''); if (/^[a-zA-Z][a-zA-Z0-9+.-]*$/.test(v)) this._protocol = v.toLowerCase() + ':'; }
    get username() { return this._username; }
    set username(v) { this._username = String(v); }
    get password() { return this._password; }
    set password(v) { this._password = String(v); }
    get hostname() { return this._hostname; }
    set hostname(v) { this._hostname = String(v).toLowerCase(); }
    get port() { return this._port; }
    set port(v) { v = String(v); this._port = DEFAULT_PORTS[this._protocol] === v ? '' : v.replace(/\D.*$/, ''); }
    get host() { return this._hostname + (this._port ? ':' + this._port : ''); }
    set host(v) { const m = /^(\[[^\]]*\]|[^:]*)(?::(\d*))?$/.exec(String(v)); if (m) { this.hostname = m[1]; this.port = m[2] || ''; } }
    get origin() {
)JSUI"
R"JSUI(      if (this._protocol === 'blob:') { try { return new URL(this._pathname).origin; } catch (_) { return 'null'; } }
      return SPECIAL[this._protocol] && this._protocol !== 'file:' ? this._protocol + '//' + this.host : 'null';
    }
    get pathname() { return this._pathname; }
    set pathname(v) { if (!this._opaque) { v = String(v); this._pathname = encPath(removeDots(v[0] === '/' ? v : '/' + v)); } }
    get search() { return this._search; }
    set search(v) { v = String(v); this._search = v && v !== '?' ? (v[0] === '?' ? v : '?' + v) : ''; this._params._parse(this._search); }
    get searchParams() { return this._params; }
    get hash() { return this._hash; }
    set hash(v) { v = String(v); this._hash = v && v !== '#' ? (v[0] === '#' ? v : '#' + v) : ''; }
    get href() {
      if (this._opaque) return this._protocol + this._pathname + this._search + this._hash;
      const cred = this._username ? this._username + (this._password ? ':' + this._password : '') + '@' : '';
      const auth = SPECIAL[this._protocol] || this._hostname ? '//' + cred + this.host : '';
      return this._protocol + auth + this._pathname + this._search + this._hash;
    }
    set href(v) { const u = new URL(v); Object.assign(this, u); this._params._url = this; }
    toString() { return this.href; }
    toJSON() { return this.href; }
    get [Symbol.toStringTag]() { return 'URL'; }
  }
  define(G, 'URL', URL);

  // location mirrors the bundle URL (read-only; reload() reloads the bundle).
  Object.defineProperty(G, 'location', {
    configurable: true,
    enumerable: false,
    get() {
      let u = null;
      try { u = new URL(host.baseUrl() || 'file:///'); } catch (_) {
        try { u = new URL('file:///' + host.baseUrl().replace(/^\/+/, '')); } catch (__) {}
      }
      const href = u ? u.href : '';
      return {
        href, origin: u ? u.origin : 'null', protocol: u ? u.protocol : '', host: u ? u.host : '',
        hostname: u ? u.hostname : '', port: u ? u.port : '', pathname: u ? u.pathname : '',
        search: u ? u.search : '', hash: u ? u.hash : '',
        reload() { if (G.__jsui_native) G.__jsui_native.reload(); },
        toString() { return href; },
      };
    },
  });
})
)JSUI"
},
{"jsui:prelude/fetch.js",
R"JSUI((function (host) {
  'use strict';
  const G = globalThis;
  const define = (obj, name, value) =>
    Object.defineProperty(obj, name, { value, writable: true, configurable: true, enumerable: false });

  // ---------------------------------------------------------------- Headers
  const normName = (n) => {
    n = String(n);
    if (!/^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/.test(n)) throw new TypeError("Invalid header name '" + n + "'");
    return n.toLowerCase();
  };
  const normValue = (v) => String(v).replace(/^[\t\n\r ]+|[\t\n\r ]+$/g, '');
  class Headers {
    constructor(init) {
      this._map = new Map();  // lower-case name -> [values]
      if (init === undefined || init === null) return;
      if (init instanceof Headers) init.forEach((v, k) => this.append(k, v));
      else if (typeof init[Symbol.iterator] === 'function') {
        for (const pair of init) {
          const p = Array.from(pair);
          if (p.length !== 2) throw new TypeError('Headers: each header needs a name and a value');
          this.append(p[0], p[1]);
        }
      } else if (typeof init === 'object') for (const k of Object.keys(init)) this.append(k, init[k]);
    }
    append(k, v) { k = normName(k); const l = this._map.get(k); if (l) l.push(normValue(v)); else this._map.set(k, [normValue(v)]); }
    set(k, v) { this._map.set(normName(k), [normValue(v)]); }
    get(k) { const l = this._map.get(normName(k)); return l ? l.join(', ') : null; }
    getSetCookie() { return (this._map.get('set-cookie') || []).slice(); }
    has(k) { return this._map.has(normName(k)); }
    delete(k) { this._map.delete(normName(k)); }
    forEach(fn, thisArg) { for (const [k, v] of this.entries()) fn.call(thisArg, v, k, this); }
    *entries() { for (const k of [...this._map.keys()].sort()) yield [k, this._map.get(k).join(', ')]; }
    *keys() { for (const [k] of this.entries()) yield k; }
    *values() { for (const [, v] of this.entries()) yield v; }
    [Symbol.iterator]() { return this.entries(); }
    get [Symbol.toStringTag]() { return 'Headers'; }
  }

  // ------------------------------------------------------------------- Body
  const encoder = new TextEncoder();
  const decoder = () => new TextDecoder();
  // Returns [bytes (Uint8Array|null), contentType|null].
  function extractBody(body) {
    if (body === undefined || body === null) return [null, null];
    if (typeof body === 'string') return [encoder.encode(body), 'text/plain;charset=UTF-8'];
    if (body instanceof URLSearchParams) return [encoder.encode(body.toString()), 'application/x-www-form-urlencoded;charset=UTF-8'];
    if (body instanceof ArrayBuffer) return [new Uint8Array(body.slice(0)), null];
    if (ArrayBuffer.isView(body)) return [new Uint8Array(body.buffer.slice(body.byteOffset, body.byteOffset + body.byteLength)), null];
    return [encoder.encode(String(body)), 'text/plain;charset=UTF-8'];
  }

  class Body {
    _initBody(bytes) { this._bytes = bytes; this._used = false; }
    get bodyUsed() { return this._used; }
    get body() { return null; }  // no ReadableStream support
    _consume() {
      if (this._used) return Promise.reject(new TypeError('Body has already been consumed'));
      this._used = true;
      return Promise.resolve(this._bytes || new Uint8Array(0));
    }
    arrayBuffer() {
      return this._consume().then((b) => (b.byteOffset === 0 && b.byteLength === b.buffer.byteLength ? b.buffer : b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength)));
    }
    bytes() { return this._consume().then((b) => new Uint8Array(b)); }
    text() { return this._consume().then((b) => decoder().decode(b)); }
    json() { return this.text().then((t) => JSON.parse(t)); }
    blob() { return Promise.reject(new TypeError('Blob is not supported in JSUI; use arrayBuffer()')); }
    formData() { return Promise.reject(new TypeError('FormData is not supported in JSUI')); }
  }

  // ---------------------------------------------------------------- Request
  class Request extends Body {
    constructor(input, init) {
      super();
      init = init || {};
      let src = null;
      if (input instanceof Request) src = input;
      this.url = src ? src.url : host.resolveUrl(String(input));
      this.method = String(init.method || (src ? src.method : 'GET')).toUpperCase();
      this.headers = new Headers(init.headers || (src ? src.headers : undefined));
      this.signal = init.signal || (src ? src.signal : null) || null;
      this.redirect = init.redirect || 'follow';
      this.credentials = init.credentials || 'same-origin';
      this.mode = init.mode || 'cors';
      this.cache = init.cache || 'default';
      this.keepalive = !!init.keepalive;
      this._timeout = typeof init.timeout === 'number' ? init.timeout : 0;  // JSUI extension (ms)
      let bytes = null;
      if (init.body !== undefined && init.body !== null) {
        if (this.method === 'GET' || this.method === 'HEAD') throw new TypeError('Request with GET/HEAD method cannot have body');
        const [b, type] = extractBody(init.body);
        bytes = b;
        if (type && !this.headers.has('content-type')) this.headers.set('content-type', type);
      } else if (src) bytes = src._bytes;
      this._initBody(bytes);
    }
    clone() { if (this._used) throw new TypeError('Request body already used'); return new Request(this); }
  }

  // --------------------------------------------------------------- Response
  class Response extends Body {
    constructor(body, init) {
      super();
      init = init || {};
      const status = init.status === undefined ? 200 : init.status | 0;
      if (status < 200 || status > 599) throw new RangeError('Response status must be in 200..599, got ' + status);
      this._status = status;
      this.statusText = init.statusText === undefined ? '' : String(init.statusText);
      this.headers = new Headers(init.headers);
      this.url = '';
      this.redirected = false;
      this.type = 'default';
      const [bytes, type] = extractBody(body);
      if (type && !this.headers.has('content-type')) this.headers.set('content-type', type);
      this._initBody(bytes);
    }
    get status() { return this._status; }
    get ok() { return this._status >= 200 && this._status < 300; }
    clone() {
      if (this._used) throw new TypeError('Response body already used');
      const r = new Response(null, { status: 200, statusText: this.statusText, headers: this.headers });
      r._status = this._status; r.url = this.url; r.redirected = this.redirected; r.type = this.type;
      r._initBody(this._bytes);
      return r;
    }
    static json(data, init) {
      const h = new Headers(init && init.headers);
      if (!h.has('content-type')) h.set('content-type', 'application/json');
      return new Response(JSON.stringify(data), Object.assign({}, init, { headers: h }));
    }
    static error() { const r = new Response(null, { status: 200 }); r._status = 0; r.type = 'error'; return r; }
    static redirect(url, status) {
      const r = new Response(null, { status: 200, headers: { location: String(url) } });
      r._status = status || 302;
      return r;
    }
  }
  // Native completion -> Response. Status 0 means a network error.
  function makeResponse(status, statusText, url, headerPairs, buffer, requestUrl) {
    const r = new Response(null, { status: 200, statusText, headers: headerPairs });
    r._status = status;
    r.url = url;
    r.redirected = url !== requestUrl;
    r.type = 'basic';
    r._initBody(new Uint8Array(buffer));
    return r;
  }

  // ------------------------------------------------------------------ fetch
  function fetch(input, init) {
    return new Promise((resolve, reject) => {
      let req;
      try { req = new Request(input, init); } catch (e) { reject(e); return; }
      const signal = req.signal;
      if (signal && signal.aborted) { reject(signal.reason); return; }
      let id = 0, done = false;
      const onAbort = () => {
        if (done) return;
        done = true;
        host.cancel(id);
        reject(signal.reason);
      };
      const pairs = [];
      req.headers.forEach((v, k) => pairs.push([k, v]));
      try {
        id = host.fetch(req.url, req.method, pairs, req._bytes ? req._bytes : undefined, req._timeout,
          (status, statusText, url, headers, buffer, error) => {
            if (done) return;
            done = true;
            if (signal) signal.removeEventListener('abort', onAbort);
            if (status === 0) {
              const e = new TypeError('fetch failed: ' + (error || 'network error') + ' (' + req.url + ')');
              reject(e);
              return;
            }
            resolve(makeResponse(status, statusText, url, headers, buffer, req.url));
          });
      } catch (e) { reject(e); return; }
      if (signal) signal.addEventListener('abort', onAbort);
    });
  }

  define(G, 'Headers', Headers);
  define(G, 'Request', Request);
  define(G, 'Response', Response);
  define(G, 'fetch', fetch);
})
)JSUI"
},
};
// clang-format on

}  // namespace

bool runPrelude(JsEngine::Impl& impl, JSValue host) {
  JSContext* ctx = impl.ctx;
  bool ok = true;
  for (auto& piece : kPrelude) {
    JSValue fn = JS_Eval(ctx, piece.source, std::strlen(piece.source), piece.name, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(fn)) {
      impl.reportException();
      ok = false;
      continue;
    }
    JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 1, &host);
    if (JS_IsException(r)) {
      impl.reportException();
      ok = false;
    }
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, fn);
  }
  return ok;
}

}  // namespace jsui
