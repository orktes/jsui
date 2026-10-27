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
  // Dev builds with the JS debugger: the arguments themselves go to DevTools.
  const cdp = host.consoleApi;
  const console = {
    log: (...a) => { if (cdp) cdp('log', a); emit(INFO, format(a)); },
    info: (...a) => { if (cdp) cdp('info', a); emit(INFO, format(a)); },
    debug: (...a) => { if (cdp) cdp('debug', a); emit(DEBUG, format(a)); },
    warn: (...a) => { if (cdp) cdp('warning', a); emit(WARN, format(a)); },
    error: (...a) => { if (cdp) cdp('error', a); emit(ERROR, format(a)); },
    trace: (...a) => {
      if (cdp) cdp('trace', a);
      const st = (new Error().stack || '').split('\n').slice(1).join('\n');
      emit(INFO, 'Trace' + (a.length ? ': ' + format(a) : '') + '\n' + st);
    },
    dir: (v) => { if (cdp) cdp('dir', [v]); emit(INFO, inspect(v, 4, [])); },
    dirxml: (...a) => { if (cdp) cdp('dirxml', a); emit(INFO, format(a)); },
    table: (v) => { if (cdp) cdp('table', [v]); emit(INFO, inspect(v, 3, [])); },
    assert: (cond, ...a) => {
      if (cond) return;
      if (cdp) cdp('assert', a);
      emit(ERROR, 'Assertion failed' + (a.length ? ': ' + format(a) : ''));
    },
    group: (...a) => { if (cdp) cdp('startGroup', a); if (a.length) emit(INFO, format(a)); indent += '  '; },
    groupCollapsed: (...a) => { if (cdp) cdp('startGroupCollapsed', a); if (a.length) emit(INFO, format(a)); indent += '  '; },
    groupEnd: () => { if (cdp) cdp('endGroup', []); indent = indent.slice(2); },
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
  // Deeper stacks in dev builds (React frames are deep); QuickJS defaults to 10.
  if (host.devMode && typeof Error.stackTraceLimit === 'number') Error.stackTraceLimit = 50;

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
{"jsui:prelude/streams.js",
R"JSUI((function (host) {
  'use strict';
  const G = globalThis;
  const define = (obj, name, value) =>
    Object.defineProperty(obj, name, { value, writable: true, configurable: true, enumerable: false });
  const noop = () => {};
  // A promise nobody may wait on: its rejection is not "unhandled".
  const quiet = (p) => { p.catch(noop); return p; };

  // --------------------------------------------------------- ReadableStream
  // WHATWG streams, the parts apps use: default readers, async iteration,
  // tee, pipeTo / pipeThrough, from(). No byte streams (BYOB readers).
  class ReadableStreamDefaultController {
    constructor(s) { this._s = s; }
    get desiredSize() { return this._s._desiredSize(); }
    enqueue(chunk) {
      const s = this._s;
      if (s._closeRequested || s._state !== 'readable') throw new TypeError('ReadableStream: cannot enqueue into a closed stream');
      s._enqueue(chunk);
    }
    close() {
      const s = this._s;
      if (s._closeRequested || s._state !== 'readable') throw new TypeError('ReadableStream: already closed');
      s._closeRequested = true;
      if (!s._queue.length) s._close();
    }
    error(e) { this._s._error(e); }
  }

  class ReadableStreamDefaultReader {
    constructor(stream) {
      if (!(stream instanceof ReadableStream)) throw new TypeError('ReadableStreamDefaultReader: not a ReadableStream');
      if (stream._reader) throw new TypeError('ReadableStream is locked');
      stream._reader = this;
      this._s = stream;
      this._requests = [];
      this._newClosed();
      if (stream._state === 'closed') this._closedResolve();
      else if (stream._state === 'errored') this._closedReject(stream._storedError);
    }
    _newClosed() {
      this._closed = quiet(new Promise((res, rej) => { this._closedResolve = res; this._closedReject = rej; }));
    }
    get closed() { return this._closed; }
    read() {
      if (!this._s) return Promise.reject(new TypeError('Reader has been released'));
      return this._s._read(this);
    }
    cancel(reason) {
      if (!this._s) return Promise.reject(new TypeError('Reader has been released'));
      return this._s._cancel(reason);
    }
    releaseLock() {
      const s = this._s;
      if (!s) return;
      const e = new TypeError('Reader was released');
      for (const r of this._requests.splice(0)) r.reject(e);
      if (s._state === 'readable') this._closedReject(e);
      else { this._newClosed(); this._closedReject(e); }
      s._reader = null;
      this._s = null;
    }
  }

  class ReadableStream {
    constructor(source, strategy) {
      source = source || {};
      strategy = strategy || {};
      if (source.type !== undefined && source.type !== 'bytes') throw new RangeError('ReadableStream: unknown type ' + source.type);
      this._state = 'readable';
      this._queue = [];
      this._queueSize = 0;
      this._reader = null;
      this._storedError = undefined;
      this._disturbed = false;
      this._source = source;
      this._hwm = strategy.highWaterMark === undefined ? (source.type === 'bytes' ? 0 : 1) : +strategy.highWaterMark;
      this._size = typeof strategy.size === 'function' ? strategy.size : () => 1;
      this._started = false;
      this._pulling = false;
      this._pullAgain = false;
      this._closeRequested = false;
      const c = (this._controller = new ReadableStreamDefaultController(this));
      let start;
      try { start = source.start ? source.start.call(source, c) : undefined; } catch (e) { this._error(e); return; }
      Promise.resolve(start).then(() => { this._started = true; this._pullIfNeeded(); }, (e) => this._error(e));
    }
    get locked() { return !!this._reader; }
    getReader(opts) {
      if (opts && opts.mode !== undefined) throw new TypeError('ReadableStream: only default readers are supported');
      return new ReadableStreamDefaultReader(this);
    }
    cancel(reason) {
      if (this._reader) return Promise.reject(new TypeError('ReadableStream is locked'));
      return this._cancel(reason);
    }
    _cancel(reason) {
      this._disturbed = true;
      if (this._state === 'closed') return Promise.resolve();
      if (this._state === 'errored') return Promise.reject(this._storedError);
      this._queue = [];
      this._queueSize = 0;
      this._close();
      const src = this._source;
      return new Promise((res) => res(src.cancel ? src.cancel.call(src, reason) : undefined)).then(noop);
    }
    _desiredSize() {
      if (this._state === 'errored') return null;
      if (this._state === 'closed') return 0;
      return this._hwm - this._queueSize;
    }
    _enqueue(chunk) {
      const r = this._reader;
      if (r && r._requests.length) r._requests.shift().resolve({ value: chunk, done: false });
      else {
        let size = 1;
        try { size = +this._size(chunk); } catch (e) { this._error(e); throw e; }
        this._queue.push([chunk, size]);
        this._queueSize += size;
      }
      this._pullIfNeeded();
    }
    _close() {
      if (this._state !== 'readable') return;
      this._state = 'closed';
      const r = this._reader;
      if (!r) return;
      for (const q of r._requests.splice(0)) q.resolve({ value: undefined, done: true });
      r._closedResolve();
    }
    _error(e) {
      if (this._state !== 'readable') return;
      this._state = 'errored';
      this._storedError = e;
      this._queue = [];
      this._queueSize = 0;
      const r = this._reader;
      if (!r) return;
      for (const q of r._requests.splice(0)) q.reject(e);
      r._closedReject(e);
    }
    _pullIfNeeded() {
      if (!this._started || this._state !== 'readable' || this._closeRequested) return;
      const r = this._reader;
      if (!(r && r._requests.length) && this._desiredSize() <= 0) return;
      if (this._pulling) { this._pullAgain = true; return; }
      const src = this._source;
      if (!src.pull) return;
      this._pulling = true;
      new Promise((res) => res(src.pull.call(src, this._controller))).then(() => {
        this._pulling = false;
        if (this._pullAgain) { this._pullAgain = false; this._pullIfNeeded(); }
      }, (e) => this._error(e));
    }
    _read(reader) {
      this._disturbed = true;
      if (this._queue.length) {
        const [value, size] = this._queue.shift();
        this._queueSize -= size;
        if (this._closeRequested && !this._queue.length) this._close();
        else this._pullIfNeeded();
        return Promise.resolve({ value, done: false });
      }
      if (this._state === 'closed') return Promise.resolve({ value: undefined, done: true });
      if (this._state === 'errored') return Promise.reject(this._storedError);
      return new Promise((resolve, reject) => {
        reader._requests.push({ resolve, reject });
        this._pullIfNeeded();
      });
    }
    tee() {
      const reader = this.getReader();
      let c1, c2, canceled = 0, reading = false;
      const reasons = [];
      let cancelDone;
      const cancelPromise = new Promise((res) => { cancelDone = res; });
      const pull = () => {
        if (reading) return;
        reading = true;
        reader.read().then(({ value, done }) => {
          reading = false;
          if (done) {
            if (!(canceled & 1)) c1.close();
            if (!(canceled & 2)) c2.close();
            return;
          }
          if (!(canceled & 1)) c1.enqueue(value);
          if (!(canceled & 2)) c2.enqueue(value);
        }, (e) => { c1.error(e); c2.error(e); });
      };
      const cancel = (bit) => (reason) => {
        canceled |= bit;
        reasons[bit - 1] = reason;
        if (canceled === 3) cancelDone(reader.cancel(reasons));
        return cancelPromise;
      };
      const a = new ReadableStream({ start: (c) => { c1 = c; }, pull, cancel: cancel(1) });
      const b = new ReadableStream({ start: (c) => { c2 = c; }, pull, cancel: cancel(2) });
      return [a, b];
    }
    pipeTo(dest, opts) {
      opts = opts || {};
      if (this._reader) return Promise.reject(new TypeError('ReadableStream is locked'));
      if (!dest || typeof dest.getWriter !== 'function') return Promise.reject(new TypeError('pipeTo: not a WritableStream'));
      const reader = this.getReader(), writer = dest.getWriter();
      const signal = opts.signal;
      let aborted = null;
      const onAbort = () => { aborted = signal.reason; reader.cancel(aborted).catch(noop); };
      if (signal) {
        if (signal.aborted) onAbort();
        else signal.addEventListener('abort', onAbort);
      }
      const finish = () => {
        if (signal) signal.removeEventListener('abort', onAbort);
        reader.releaseLock();
        writer.releaseLock();
      };
      const loop = () => reader.read().then(({ value, done }) => {
        if (aborted !== null) throw aborted;
        if (done) return opts.preventClose ? undefined : writer.close();
        return writer.write(value).then(loop);
      });
      return loop().then(finish, (e) => {
        const tail = [];
        if (!opts.preventAbort) tail.push(writer.abort(e).catch(noop));
        if (!opts.preventCancel) tail.push(reader.cancel(e).catch(noop));
        return Promise.all(tail).then(() => { finish(); throw e; });
      });
    }
    pipeThrough(pair, opts) {
      if (!pair || !pair.writable || !pair.readable) throw new TypeError('pipeThrough: expected { writable, readable }');
      quiet(this.pipeTo(pair.writable, opts));
      return pair.readable;
    }
    values(opts) {
      const reader = this.getReader();
      const preventCancel = !!(opts && opts.preventCancel);
      return {
        next: () => reader.read().then((r) => { if (r.done) reader.releaseLock(); return r; }),
        return: (value) => {
          const p = preventCancel ? Promise.resolve() : reader.cancel(value);
          return p.then(() => { reader.releaseLock(); return { value, done: true }; });
        },
        [Symbol.asyncIterator]() { return this; },
      };
    }
    [Symbol.asyncIterator](opts) { return this.values(opts); }
    static from(src) {
      if (src instanceof ReadableStream) return src;
      const it = src[Symbol.asyncIterator] ? src[Symbol.asyncIterator]() : src[Symbol.iterator]();
      return new ReadableStream({
        pull: (c) => Promise.resolve(it.next()).then((r) => { if (r.done) c.close(); else c.enqueue(r.value); }),
        cancel: (reason) => (typeof it.return === 'function' ? Promise.resolve(it.return(reason)).then(noop) : undefined),
      }, { highWaterMark: 0 });
    }
    get [Symbol.toStringTag]() { return 'ReadableStream'; }
  }

)JSUI"
R"JSUI(  // --------------------------------------------------------- WritableStream
  // Writes run one at a time in order; `ready` is always resolved (no
  // backpressure beyond that).
  class WritableStreamDefaultWriter {
    constructor(stream) {
      if (stream._writer) throw new TypeError('WritableStream is locked');
      stream._writer = this;
      this._s = stream;
    }
    get closed() { return this._s ? this._s._closed : Promise.reject(new TypeError('Writer has been released')); }
    get ready() { return Promise.resolve(); }
    get desiredSize() { return this._s && this._s._state === 'writable' ? 1 : 0; }
    write(chunk) { return this._s ? this._s._write(chunk) : Promise.reject(new TypeError('Writer has been released')); }
    close() { return this._s ? this._s._closeStream() : Promise.reject(new TypeError('Writer has been released')); }
    abort(reason) { return this._s ? this._s._abort(reason) : Promise.reject(new TypeError('Writer has been released')); }
    releaseLock() { if (this._s) { this._s._writer = null; this._s = null; } }
  }

  class WritableStream {
    constructor(sink) {
      sink = sink || {};
      this._sink = sink;
      this._state = 'writable';
      this._writer = null;
      this._storedError = undefined;
      this._closed = quiet(new Promise((res, rej) => { this._closedResolve = res; this._closedReject = rej; }));
      const ctl = (this._ctl = new AbortController());
      const controller = { error: (e) => this._fail(e), signal: ctl.signal };
      this._controller = controller;
      this._chain = quiet(new Promise((res) => res(sink.start ? sink.start.call(sink, controller) : undefined)).catch((e) => { this._fail(e); throw e; }));
    }
    get locked() { return !!this._writer; }
    getWriter() { return new WritableStreamDefaultWriter(this); }
    close() { return this._writer ? Promise.reject(new TypeError('WritableStream is locked')) : this._closeStream(); }
    abort(reason) { return this._writer ? Promise.reject(new TypeError('WritableStream is locked')) : this._abort(reason); }
    _fail(e) {
      if (this._state === 'errored' || this._state === 'closed') return;
      this._state = 'errored';
      this._storedError = e;
      this._closedReject(e);
    }
    _then(fn) {
      const p = this._chain.then(() => {
        if (this._state === 'errored') throw this._storedError;
        return fn();
      });
      this._chain = quiet(p.catch((e) => { this._fail(e); throw e; }));
      return p;
    }
    _write(chunk) {
      if (this._state !== 'writable') return Promise.reject(this._storedError || new TypeError('WritableStream is closed'));
      const sink = this._sink;
      return this._then(() => (sink.write ? sink.write.call(sink, chunk, this._controller) : undefined)).then(noop);
    }
    _closeStream() {
      if (this._state !== 'writable') return Promise.reject(this._storedError || new TypeError('WritableStream is closed'));
      this._state = 'closing';
      const sink = this._sink;
      return this._then(() => (sink.close ? sink.close.call(sink) : undefined)).then(() => {
        this._state = 'closed';
        this._closedResolve();
      });
    }
    _abort(reason) {
      if (this._state === 'closed' || this._state === 'errored') return Promise.resolve();
      this._ctl.abort(reason);
      const sink = this._sink;
      this._fail(reason);
      return new Promise((res) => res(sink.abort ? sink.abort.call(sink, reason) : undefined)).then(noop);
    }
    get [Symbol.toStringTag]() { return 'WritableStream'; }
  }

  // -------------------------------------------------------- TransformStream
  class TransformStream {
    constructor(transformer) {
      transformer = transformer || {};
      let rc;
      this.readable = new ReadableStream({
        start: (c) => { rc = c; },
        cancel: (reason) => {
          this.writable._fail(reason);
          return transformer.cancel ? transformer.cancel.call(transformer, reason) : undefined;
        },
      });
      const tc = {
        enqueue: (chunk) => rc.enqueue(chunk),
        error: (e) => { rc.error(e); this.writable._fail(e); },
        terminate: () => { try { rc.close(); } catch (e) { /* closed */ } this.writable._fail(new TypeError('TransformStream terminated')); },
        get desiredSize() { return rc.desiredSize; },
      };
      const started = new Promise((res) => res(transformer.start ? transformer.start.call(transformer, tc) : undefined));
      this.writable = new WritableStream({
        start: () => started,
        write: (chunk) => (transformer.transform ? transformer.transform.call(transformer, chunk, tc) : tc.enqueue(chunk)),
        close: () => Promise.resolve(transformer.flush ? transformer.flush.call(transformer, tc) : undefined).then(() => {
          try { rc.close(); } catch (e) { /* already closed */ }
        }),
        abort: (reason) => rc.error(reason),
      });
    }
    get [Symbol.toStringTag]() { return 'TransformStream'; }
  }

  class TextDecoderStream extends TransformStream {
    constructor(label, opts) {
      const d = new TextDecoder(label, opts);
      super({
        transform: (chunk, c) => { const s = d.decode(chunk, { stream: true }); if (s) c.enqueue(s); },
        flush: (c) => { const s = d.decode(); if (s) c.enqueue(s); },
      });
      this.encoding = d.encoding;
      this.fatal = d.fatal;
      this.ignoreBOM = d.ignoreBOM;
    }
  }
  class TextEncoderStream extends TransformStream {
    constructor() {
      const e = new TextEncoder();
      super({ transform: (chunk, c) => { const s = String(chunk); if (s) c.enqueue(e.encode(s)); } });
      this.encoding = 'utf-8';
    }
  }

  class CountQueuingStrategy {
    constructor(init) { this.highWaterMark = init.highWaterMark; }
    size() { return 1; }
  }
  class ByteLengthQueuingStrategy {
    constructor(init) { this.highWaterMark = init.highWaterMark; }
    size(chunk) { return chunk.byteLength; }
  }

  define(G, 'ReadableStream', ReadableStream);
  define(G, 'ReadableStreamDefaultReader', ReadableStreamDefaultReader);
  define(G, 'ReadableStreamDefaultController', ReadableStreamDefaultController);
  define(G, 'WritableStream', WritableStream);
  define(G, 'WritableStreamDefaultWriter', WritableStreamDefaultWriter);
  define(G, 'TransformStream', TransformStream);
  define(G, 'TextDecoderStream', TextDecoderStream);
  define(G, 'TextEncoderStream', TextEncoderStream);
  define(G, 'CountQueuingStrategy', CountQueuingStrategy);
  define(G, 'ByteLengthQueuingStrategy', ByteLengthQueuingStrategy);
})
)JSUI"
},
{"jsui:prelude/blob.js",
R"JSUI((function (host) {
  'use strict';
  const G = globalThis;
  const define = (obj, name, value) =>
    Object.defineProperty(obj, name, { value, writable: true, configurable: true, enumerable: false });
  const encoder = new TextEncoder();

  // ------------------------------------------------------------ Blob / File
  // In-memory: a Blob holds its bytes (`_bytes`), so everything about it is
  // synchronous inside the runtime; the async methods are the web's API.
  const viewOf = (part) => {
    if (part instanceof Blob) return part._bytes;
    if (part instanceof ArrayBuffer) return new Uint8Array(part);
    if (ArrayBuffer.isView(part)) return new Uint8Array(part.buffer, part.byteOffset, part.byteLength);
    return encoder.encode(String(part));
  };
  const concat = (views) => {
    let n = 0;
    for (const v of views) n += v.byteLength;
    const out = new Uint8Array(n);
    let o = 0;
    for (const v of views) { out.set(v, o); o += v.byteLength; }
    return out;
  };
  class Blob {
    constructor(parts, opts) {
      if (parts !== undefined && (parts === null || typeof parts[Symbol.iterator] !== 'function'))
        throw new TypeError("Blob: the first argument must be iterable");
      const views = [];
      if (parts) for (const p of parts) views.push(viewOf(p));
      this._bytes = concat(views);
      const type = opts && opts.type !== undefined ? String(opts.type) : '';
      this._type = /^[\x20-\x7e]*$/.test(type) ? type.toLowerCase() : '';
    }
    get size() { return this._bytes.byteLength; }
    get type() { return this._type; }
    slice(start, end, type) {
      const n = this.size;
      const clamp = (v, d) => (v === undefined ? d : v < 0 ? Math.max(n + v, 0) : Math.min(v, n));
      const s = clamp(start, 0), e = clamp(end, n);
      const b = new Blob([], { type: type === undefined ? '' : type });
      b._bytes = this._bytes.slice(s, Math.max(s, e));
      return b;
    }
    arrayBuffer() { return Promise.resolve(this._bytes.slice().buffer); }
    bytes() { return Promise.resolve(this._bytes.slice()); }
    text() { return Promise.resolve(new TextDecoder().decode(this._bytes)); }
    stream() {
      const bytes = this._bytes;
      let off = 0;
      return new ReadableStream({
        pull(c) {
          if (off >= bytes.byteLength) { c.close(); return; }
          const end = Math.min(off + 65536, bytes.byteLength);
          c.enqueue(bytes.slice(off, end));
          off = end;
        },
      }, { highWaterMark: 0 });
    }
    get [Symbol.toStringTag]() { return 'Blob'; }
  }
  class File extends Blob {
    constructor(parts, name, opts) {
      if (arguments.length < 2) throw new TypeError('File: a name is required');
      super(parts, opts);
      this._name = String(name);
      this._lastModified = opts && opts.lastModified !== undefined ? +opts.lastModified : Date.now();
    }
    get name() { return this._name; }
    get lastModified() { return this._lastModified; }
    get webkitRelativePath() { return ''; }
    get [Symbol.toStringTag]() { return 'File'; }
  }

  // --------------------------------------------------------------- FormData
  // Entries are strings or Files (a Blob value becomes a File named "blob").
  const entryValue = (value, filename) => {
    if (value instanceof Blob) {
      if (!(value instanceof File) || filename !== undefined) {
        const f = new File([], filename !== undefined ? filename : value instanceof File ? value.name : 'blob', {
          type: value.type,
          lastModified: value instanceof File ? value.lastModified : undefined,
        });
        f._bytes = value._bytes;
        return f;
      }
      return value;
    }
    return String(value);
  };
  class FormData {
    constructor(form) {
      if (form !== undefined) throw new TypeError('FormData: there are no <form> elements in JSUI');
      this._entries = [];
    }
    append(name, value, filename) { this._entries.push([String(name), entryValue(value, filename)]); }
    set(name, value, filename) {
      name = String(name);
      const v = entryValue(value, filename);
      const i = this._entries.findIndex((e) => e[0] === name);
      if (i < 0) { this._entries.push([name, v]); return; }
      this._entries[i] = [name, v];
      this._entries = this._entries.filter((e, j) => j <= i || e[0] !== name);
    }
    get(name) { const e = this._entries.find((x) => x[0] === String(name)); return e ? e[1] : null; }
    getAll(name) { return this._entries.filter((x) => x[0] === String(name)).map((x) => x[1]); }
    has(name) { return this._entries.some((x) => x[0] === String(name)); }
    delete(name) { name = String(name); this._entries = this._entries.filter((x) => x[0] !== name); }
    forEach(fn, thisArg) { for (const [k, v] of this._entries.slice()) fn.call(thisArg, v, k, this); }
    *entries() { for (const e of this._entries.slice()) yield [e[0], e[1]]; }
    *keys() { for (const e of this._entries.slice()) yield e[0]; }
    *values() { for (const e of this._entries.slice()) yield e[1]; }
    [Symbol.iterator]() { return this.entries(); }
    get [Symbol.toStringTag]() { return 'FormData'; }
  }

  // multipart/form-data: serialize (fetch bodies) and parse (res.formData()).
  const escapeName = (s) => s.replace(/\n/g, '%0A').replace(/\r/g, '%0D').replace(/"/g, '%22');
  function encodeMultipart(fd) {
    let boundary = '----JSUIFormBoundary';
    for (let i = 0; i < 16; i++) boundary += 'abcdefghijklmnopqrstuvwxyz0123456789'[(Math.random() * 36) | 0];
    const parts = [];
    for (const [name, value] of fd._entries) {
      let head = '--' + boundary + '\r\nContent-Disposition: form-data; name="' + escapeName(name) + '"';
      if (typeof value === 'string') {
        parts.push(encoder.encode(head + '\r\n\r\n' + value.replace(/\r\n|\r|\n/g, '\r\n') + '\r\n'));
      } else {
        head += '; filename="' + escapeName(value.name) + '"\r\nContent-Type: ' + (value.type || 'application/octet-stream');
        parts.push(encoder.encode(head + '\r\n\r\n'), value._bytes, encoder.encode('\r\n'));
      }
    }
    parts.push(encoder.encode('--' + boundary + '--\r\n'));
    return [concat(parts), 'multipart/form-data; boundary=' + boundary];
  }

  const indexOf = (hay, needle, from) => {
    const n = needle.length, last = hay.length - n;
    outer: for (let i = from; i <= last; i++) {
      for (let j = 0; j < n; j++) if (hay[i + j] !== needle[j]) continue outer;
      return i;
    }
    return -1;
  };
  const param = (header, key) => {
    const m = new RegExp('(?:^|;)\\s*' + key + '\\s*=\\s*(?:"((?:[^"\\\\]|\\\\.)*)"|([^;\\s]*))', 'i').exec(header);
    return m ? (m[1] !== undefined ? m[1].replace(/\\(.)/g, '$1') : m[2]) : null;
  };
  // -> FormData; throws TypeError on malformed input.
  function parseFormData(bytes, contentType) {
    const type = (contentType || '').toLowerCase();
    const fd = new FormData();
    if (type.startsWith('application/x-www-form-urlencoded')) {
      for (const [k, v] of new URLSearchParams(new TextDecoder().decode(bytes))) fd.append(k, v);
      return fd;
    }
    if (!type.startsWith('multipart/form-data')) throw new TypeError('formData(): unsupported content type ' + (contentType || '(none)'));
    const boundary = param(contentType, 'boundary');
    if (!boundary) throw new TypeError('formData(): multipart body without a boundary');
    const delim = encoder.encode('--' + boundary);
    const crlf2 = encoder.encode('\r\n\r\n');
    const dec = new TextDecoder();
    let pos = indexOf(bytes, delim, 0);
    if (pos < 0) throw new TypeError('formData(): malformed multipart body');
    for (;;) {
      pos += delim.length;
      if (bytes[pos] === 45 && bytes[pos + 1] === 45) return fd;  // "--": the end
      if (bytes[pos] === 13 && bytes[pos + 1] === 10) pos += 2;
      const headEnd = indexOf(bytes, crlf2, pos);
      if (headEnd < 0) throw new TypeError('formData(): malformed multipart part');
      const headers = {};
      for (const line of dec.decode(bytes.subarray(pos, headEnd)).split('\r\n')) {
        const c = line.indexOf(':');
        if (c > 0) headers[line.slice(0, c).trim().toLowerCase()] = line.slice(c + 1).trim();
      }
      const bodyStart = headEnd + 4;
      const next = indexOf(bytes, delim, bodyStart);
      if (next < 0) throw new TypeError('formData(): unterminated multipart body');
      const body = bytes.subarray(bodyStart, next - 2);  // minus the CRLF before the delimiter
      const disp = headers['content-disposition'] || '';
      const name = param(disp, 'name');
      if (name !== null) {
        const filename = param(disp, 'filename');
        if (filename !== null) fd.append(name, new File([body], filename, { type: headers['content-type'] || '' }));
        else fd.append(name, dec.decode(body));
      }
      pos = next;
    }
  }

  define(G, 'Blob', Blob);
  define(G, 'File', File);
  define(G, 'FormData', FormData);
  define(G, '__jsui_multipart', { encode: encodeMultipart, parse: parseFormData, concat });
})
)JSUI"
},
{"jsui:prelude/fetch.js",
R"JSUI((function (host) {
  'use strict';
  const G = globalThis;
  const define = (obj, name, value) =>
    Object.defineProperty(obj, name, { value, writable: true, configurable: true, enumerable: false });
  const MP = G.__jsui_multipart;

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
  // A body is bytes (Uint8Array) or a ReadableStream of Uint8Arrays.
  const encoder = new TextEncoder();
  const decode = (b) => new TextDecoder().decode(b);
  // Returns [bytes | stream | null, contentType | null].
  function extractBody(body) {
    if (body === undefined || body === null) return [null, null];
    if (typeof body === 'string') return [encoder.encode(body), 'text/plain;charset=UTF-8'];
    if (body instanceof URLSearchParams) return [encoder.encode(body.toString()), 'application/x-www-form-urlencoded;charset=UTF-8'];
    if (body instanceof ArrayBuffer) return [new Uint8Array(body.slice(0)), null];
    if (ArrayBuffer.isView(body)) return [new Uint8Array(body.buffer.slice(body.byteOffset, body.byteOffset + body.byteLength)), null];
    if (body instanceof Blob) return [body._bytes.slice(), body.type || null];
    if (body instanceof FormData) return MP.encode(body);
    if (body instanceof ReadableStream) {
      if (body.locked || body._disturbed) throw new TypeError('The body stream is locked or already read');
      return [body, null];
    }
    return [encoder.encode(String(body)), 'text/plain;charset=UTF-8'];
  }
  // All of a stream's bytes.
  function readAll(stream) {
    const reader = stream.getReader();
    const chunks = [];
    const step = () => reader.read().then(({ value, done }) => {
      if (done) return MP.concat(chunks);
      if (!(value instanceof Uint8Array)) {
        if (ArrayBuffer.isView(value)) value = new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
        else if (value instanceof ArrayBuffer) value = new Uint8Array(value);
        else throw new TypeError('Body stream chunks must be Uint8Arrays');
      }
      chunks.push(value);
      return step();
    });
    return step();
  }
  const bytesStream = (bytes) => new ReadableStream({
    start(c) { if (bytes.byteLength) c.enqueue(bytes); c.close(); },
  });

  class Body {
    _initBody(src) {
      this._bytes = src instanceof ReadableStream ? null : src;
      this._stream = src instanceof ReadableStream ? src : null;
      this._used = false;
    }
    get body() {
      if (!this._stream && this._bytes) {
        this._stream = bytesStream(this._bytes);
        this._bytes = null;
      }
      return this._stream;
    }
    get bodyUsed() { return this._used || !!(this._stream && this._stream._disturbed); }
    _consume() {
      if (this.bodyUsed) return Promise.reject(new TypeError('Body has already been consumed'));
      if (this._stream && this._stream.locked) return Promise.reject(new TypeError('Body stream is locked'));
      this._used = true;
      if (this._stream) return readAll(this._stream);
      return Promise.resolve(this._bytes || new Uint8Array(0));
    }
    // A clone of the body (tee for streams).
    _cloneBody() {
      if (this.bodyUsed) throw new TypeError('Body has already been consumed');
      if (!this._stream) return this._bytes;
      const [a, b] = this._stream.tee();
      this._stream = a;
      return b;
    }
    arrayBuffer() {
      return this._consume().then((b) => (b.byteOffset === 0 && b.byteLength === b.buffer.byteLength ? b.buffer : b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength)));
    }
    bytes() { return this._consume().then((b) => new Uint8Array(b)); }
    text() { return this._consume().then(decode); }
    json() { return this.text().then((t) => JSON.parse(t)); }
    blob() {
      return this._consume().then((b) => {
        const out = new Blob([], { type: this.headers.get('content-type') || '' });
        out._bytes = b;
        return out;
      });
    }
    formData() { return this._consume().then((b) => MP.parse(b, this.headers.get('content-type'))); }
  }

  // ---------------------------------------------------------------- Request
  const REDIRECT = { follow: 0, error: 1, manual: 2 };
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
      this.redirect = init.redirect || (src ? src.redirect : 'follow');
      if (!(this.redirect in REDIRECT)) throw new TypeError("Request: redirect must be 'follow', 'error' or 'manual'");
      this.credentials = init.credentials || 'same-origin';
      this.mode = init.mode || 'cors';
      this.cache = init.cache || 'default';
      this.keepalive = !!init.keepalive;
      this.duplex = init.duplex || 'half';
      this._timeout = typeof init.timeout === 'number' ? init.timeout : src ? src._timeout : 0;  // JSUI extension (ms)
      let body = null;
      if (init.body !== undefined && init.body !== null) {
        if (this.method === 'GET' || this.method === 'HEAD') throw new TypeError('Request with GET/HEAD method cannot have body');
        const [b, type] = extractBody(init.body);
        body = b;
        if (type && !this.headers.has('content-type')) this.headers.set('content-type', type);
      } else if (src) {
        body = src._cloneBody();
        src._used = true;  // a Request made from another takes its body
      }
      this._initBody(body);
    }
    clone() {
      const r = new Request(this.url, { method: this.method, headers: this.headers, signal: this.signal, redirect: this.redirect, timeout: this._timeout });
      r._initBody(this._cloneBody());
      return r;
    }
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
      const [src, type] = extractBody(body);
      if (type && !this.headers.has('content-type')) this.headers.set('content-type', type);
      this._initBody(src);
    }
    get status() { return this._status; }
    get ok() { return this._status >= 200 && this._status < 300; }
    clone() {
      const r = new Response(null, { status: 200, statusText: this.statusText, headers: this.headers });
      r._status = this._status; r.url = this.url; r.redirected = this.redirected; r.type = this.type;
      r._initBody(this._cloneBody());
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

  // ------------------------------------------------------------------ fetch
  // The body streams in: fetch() resolves at the response headers, res.body
  // gets each piece as the host delivers it, and text() / json() / ... read
  // it to the end. Aborting before the headers rejects fetch(); after them it
  // errors the body.
  function fetch(input, init) {
    let req;
    try { req = new Request(input, init); } catch (e) { return Promise.reject(e); }
    const signal = req.signal;
    if (signal && signal.aborted) return Promise.reject(signal.reason);
    let body;
    try { body = req._bytes ? Promise.resolve(req._bytes) : req._stream ? req._consume() : Promise.resolve(null); } catch (e) { return Promise.reject(e); }
    return body.then((bytes) => new Promise((resolve, reject) => {
      if (signal && signal.aborted) { reject(signal.reason); return; }
      let id = 0, state = 0;  // 0: waiting for the head, 1: body streaming, 2: done
      let ctl = null;
      const stop = () => {
        state = 2;
        if (signal) signal.removeEventListener('abort', onAbort);
      };
      const onAbort = () => {
        if (state === 2) return;
        const was = state;
        stop();
        host.cancel(id);
        if (was === 0) reject(signal.reason);
        else ctl.error(signal.reason);
      };
      const pairs = [];
      req.headers.forEach((v, k) => pairs.push([k, v]));
      try {
        id = host.fetch(req.url, req.method, pairs, bytes || undefined, req._timeout, REDIRECT[req.redirect],
          (kind, a, b, c, d, e) => {
            if (state === 2) return;
            if (kind === 0) {  // head: status, statusText, url, headers, error
              if (a === 0) {
                stop();
                reject(new TypeError('fetch failed: ' + (e || 'network error') + ' (' + req.url + ')'));
                return;
              }
              state = 1;
              const stream = new ReadableStream({
                start(c2) { ctl = c2; },
                cancel() { if (state !== 2) { stop(); host.cancel(id); } },
              }, { highWaterMark: 0 });
              const r = new Response(null, { status: 200, statusText: b, headers: d });
              r._status = a;
              r.url = c;
              r.redirected = c !== req.url;
              r.type = 'basic';
              r._initBody(req.method === 'HEAD' || a === 204 || a === 304 ? null : stream);
              resolve(r);
            } else if (kind === 1) {  // a body piece
              if (ctl) ctl.enqueue(new Uint8Array(a));
            } else {  // end: '' or the error
              stop();
              if (a) ctl.error(new TypeError('fetch: the response body failed: ' + a + ' (' + req.url + ')'));
              else ctl.close();
            }
          });
      } catch (e) { reject(e); return; }
      if (signal) signal.addEventListener('abort', onAbort);
    }));
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

const PreludePiece* preludePieces(size_t* count) {
  *count = sizeof(kPrelude) / sizeof(kPrelude[0]);
  return kPrelude;
}

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
