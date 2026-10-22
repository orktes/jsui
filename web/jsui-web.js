// JSUI browser host: runs the WASM build of the JSUI runtime on a <canvas>.
//
//   import { createJsui } from './jsui-web.js';
//   const jsui = await createJsui({ canvas, bundleUrl: './bundle.js' });
//   jsui.on('message', (name, data) => ...);
//   jsui.postMessage('hud', { hp: 42 });
//
// Renderers. The default ('auto') draws with Skia Ganesh straight into the
// canvas' WebGL2 (or WebGL 1) context (jsui_render_gl): no CPU raster and no
// upload. It needs the module built with the GPU backend (WEBGL=1
// scripts/build-wasm.sh -> web/dist-webgl/jsui.js), which createJsui() loads
// by default when the browser has WebGL, falling back to the CPU-only module
// (web/dist/jsui.js) when that one is not deployed.
//
// 'auto' uses the CPU path instead when the browser only offers WebGL through
// a software rasteriser (failIfMajorPerformanceCaveat, e.g. Chrome's SwiftShader
// fallback without a usable GPU): there Skia's full GPU repaints cost more
// than the CPU raster's incremental ones. renderer 'gpu' uses it anyway.
//
// It falls back to the CPU path when there is no WebGL, when the module has
// no GPU backend, when the GL / Skia context cannot be created or a GPU frame
// fails, and when the WebGL context is lost and not restored within
// `contextRestoreTimeout` ms (the canvas element is then replaced by a fresh
// clone, since a canvas with a dead WebGL context cannot get another context;
// listen for 'renderer' to pick up the new `jsui.canvas`).
//
// CPU path (renderer 'cpu', or the fallback): per frame jsui_tick -> if the
// UI changed, jsui_render_rgba_incremental repaints only the damaged pixels of
// a buffer in the WASM heap -> only the damaged rows / rect are uploaded as a
// WebGL texture straight from the heap (the runtime's premultiplied RGBA is
// exactly what a premultipliedAlpha:true context composites, so there is no
// CPU conversion and no copy besides the GPU upload). A Canvas2D blitter
// ('2d', or no WebGL) un-premultiplies just the damaged rect into an
// ImageData and puts that rect.
//
// One WASM module instance is shared by all runtimes on the page (one per
// module URL); each createJsui() call owns an independent JsuiRuntime.

/** @typedef {{
 *   canvas: HTMLCanvasElement,
 *   bundleUrl?: string,          // URL of the JS bundle (resolved against the page)
 *   bundleSource?: string,       // or the bundle source itself
 *   devMode?: boolean,           // dev overlay + live reload
 *   devPoll?: boolean,           // host-side polling of /__jsui/version. Default: only for
 *                                // https:// bundles; the runtime itself polls http:// origins
 *                                // (through this module's fetch loader) when devMode is set.
 *   pixelRatio?: number,         // default: window.devicePixelRatio (tracked live)
 *   moduleUrl?: string|URL,      // default: ./dist-webgl/jsui.js next to this file when WebGL is
 *                                // available and the renderer may use the GPU, else (or when
 *                                // that fails to load) ./dist/jsui.js
 *   renderer?: 'auto'|'gpu'|'cpu'|'webgl'|'2d',
 *                                // 'auto' (default): Skia on WebGL unless WebGL is software-only,
 *                                // falling back to the CPU raster. 'gpu': Skia on any WebGL. 'cpu': force the CPU raster, blitted with WebGL
 *                                // (or Canvas2D without WebGL). 'webgl' / '2d': CPU raster with
 *                                // that blitter only.
 *   contextRestoreTimeout?: number, // ms to wait for a lost WebGL context to come back before
 *                                // falling back to the CPU on a fresh canvas (default 3000)
 *   autoFocus?: boolean,         // focus the keyboard sink on start (default true)
 *   gamepads?: boolean,          // poll the Gamepad API (default true)
 *   autoStart?: boolean,         // start the frame loop immediately (default true)
 *   trapTab?: boolean,           // keep Tab inside the UI even if unhandled (default true;
 *                                // false lets an unhandled Tab move page focus on)
 * }} JsuiOptions */

const modules = new Map();  // module URL -> Promise<Module>

function loadModule(moduleUrl) {
  const key = String(moduleUrl);
  let p = modules.get(key);
  if (!p) {
    p = import(/* @vite-ignore */ key).then((ns) =>
      ns.default({ locateFile: (path) => new URL(path, key).href }));
    p.catch(() => modules.delete(key));
    modules.set(key, p);
  }
  return p;
}

let nextHostId = 1;
const modsOf = (e) => (e.shiftKey ? 1 : 0) | (e.ctrlKey ? 2 : 0) | (e.altKey ? 4 : 0) | (e.metaKey ? 8 : 0);
// Keys whose browser default (focus move, caret move inside the keyboard sink)
// we suppress when the UI consumed them. Printable keys are never suppressed:
// their text must still reach the UI through the `input` event.
const NAV_KEYS = new Set(['Tab', 'Enter', 'Escape', 'Backspace', 'Delete', 'ArrowUp', 'ArrowDown', 'ArrowLeft',
  'ArrowRight', 'Home', 'End', 'PageUp', 'PageDown', 'F1', 'F2', 'F3', 'F4', 'F5', 'F6', 'F7', 'F8', 'F9', 'F10',
  'F11', 'F12']);
const GAMEPAD_BUTTONS = 17;
const GAMEPAD_AXES = 4;
const AXIS_EPSILON = 1 / 256;

/**
 * Creates a JSUI runtime on `canvas` and starts its frame loop.
 * @param {JsuiOptions} opts
 */
export async function createJsui(opts) {
  const canvas = opts.canvas;
  if (!canvas) throw new Error('createJsui: canvas is required');
  return new JsuiHost(await loadModuleFor(opts), canvas, opts);
}

const CPU_MODULE = './dist/jsui.js';
const GPU_MODULE = './dist-webgl/jsui.js';

// The GPU module when the renderer may use it and WebGL exists, else the
// CPU-only one; each falls back to the other if it is not deployed (both can
// run the CPU path).
async function loadModuleFor(opts) {
  if (opts.moduleUrl) return loadModule(opts.moduleUrl);
  const mode = opts.renderer ?? 'auto';
  const gpu = mode === 'gpu' ? webglAvailable() : mode === 'auto' && webglAvailable(true);
  const [first, second] = gpu ? [GPU_MODULE, CPU_MODULE] : [CPU_MODULE, GPU_MODULE];
  try {
    return await loadModule(new URL(first, import.meta.url));
  } catch (e) {
    try { return await loadModule(new URL(second, import.meta.url)); } catch { throw e; }
  }
}

const webglProbe = {};
/** True if this browser can create a WebGL (2 or 1) context; `hardware`: one without a major
 * performance caveat, i.e. not a software rasteriser such as SwiftShader the browser fell back
 * to (probed once each, on a throwaway canvas). */
export function webglAvailable(hardware = false) {
  const key = hardware ? 'hw' : 'any';
  if (webglProbe[key] !== undefined) return webglProbe[key];
  try {
    const attrs = { failIfMajorPerformanceCaveat: hardware };
    const c = document.createElement('canvas');
    const gl = c.getContext('webgl2', attrs) || c.getContext('webgl', attrs);
    webglProbe[key] = !!gl;
    gl?.getExtension('WEBGL_lose_context')?.loseContext();  // free it now, not at GC
  } catch {
    webglProbe[key] = false;
  }
  return webglProbe[key];
}

class JsuiHost {
  constructor(M, canvas, opts) {
    this.module = M;
    this.canvas = canvas;
    this.opts = opts;
    this.hostId = nextHostId++;
    this.listeners = new Map();
    // renders: frames painted. blits / blitPixels: CPU path uploads and the
    // pixels they covered (lastBlit: the last uploaded rect, device px).
    this.stats = { frames: 0, renders: 0, tickMs: 0, renderMs: 0, blitMs: 0, blits: 0, blitPixels: 0, lastBlit: null };
    this._lastError = null;
    this._cursor = '';
    this._raf = 0;
    this._destroyed = false;
    this._abort = new AbortController();
    this._pressedKeys = new Map();  // code -> key, released on blur
    this._mouseButtons = 0;
    this._pads = [];
    this._fixedRatio = opts.pixelRatio > 0 ? opts.pixelRatio : 0;
    this._trapTab = opts.trapTab !== false;
    this._handlers = new Map();  // Host.call name -> fn (handle())
    this._uiCalls = new Map();   // callUi id -> {resolve, reject}

    (M.__jsuiHosts ??= new Map()).set(this.hostId, {
      onMessage: (name, json) => {
        let data;
        try { data = JSON.parse(json); } catch { data = json; }
        this._emit('message', name, data);
      },
      onState: (key, json) => {
        let value;
        try { value = json == null ? undefined : JSON.parse(json); } catch { value = json; }
        this._emit('state', key, value);
      },
      onCall: (id, name, json) => this._onCall(id, name, json),
      onCallCancel: (id) => this._emit('callCancel', id),
      onCallResult: (id, ok, payload) => {
        const p = this._uiCalls.get(id);
        if (!p) return;
        this._uiCalls.delete(id);
        if (!ok) {
          const e = new Error(payload);
          e.name = 'UiCallError';
          p.reject(e);
          return;
        }
        try { p.resolve(JSON.parse(payload)); } catch (e) { p.reject(e); }
      },
      onLog: (level, text) => {
        if (!this._emit('log', level, text)) {
          (level >= 3 ? console.error : level === 2 ? console.warn : level === 0 ? console.debug : console.log)(
            '[jsui] ' + text);
        }
      },
    });

    // Small reusable scratch area for short strings (key names, codes).
    this._scratch = M._malloc(512);
    this._dirty = M._malloc(32);  // JsuiRect filled by jsui_render_rgba_incremental / JsuiGLTarget
    this._buf = 0;
    this._bufSize = 0;

    this._measure();
    const pageUrl = typeof location !== 'undefined' ? location.href : undefined;
    this._bundleUrl = opts.bundleUrl ? new URL(opts.bundleUrl, pageUrl).href : null;
    // Lives as long as the runtime (JsuiConfig keeps the pointer).
    this._baseUrlPtr = this._bundleUrl ? this._newString(this._bundleUrl) : 0;
    this.rt = M._jsui_web_create(this.hostId, this.width, this.height, this.pixelRatio, opts.devMode ? 1 : 0,
      this._baseUrlPtr);
    if (!this.rt) throw new Error('jsui_web_create failed');

    this._initBlitter(opts.renderer ?? 'auto');
    this._installInput();
    this._installCanvasInput();
    this._installResize();
    if (opts.gamepads !== false) this._installGamepads();

    if (opts.bundleSource != null) this.loadSource(opts.bundleSource, this._bundleUrl ?? 'bundle.js');
    else if (this._bundleUrl) this.loadUrl(this._bundleUrl);
    const runtimePolls = this._bundleUrl?.startsWith('http://');
    if (opts.devMode && (opts.devPoll ?? !runtimePolls) && this._bundleUrl) this._startDevPoll();
    if (opts.autoStart !== false) this.start();
  }

  // ------------------------------------------------------------- public API

  /** Subscribe: 'message' (name, data), 'state' (key, value: HostState.set in the UI),
   * 'call' (id, name, args: Host.call without a handle() handler), 'callCancel' (id),
   * 'error' (message), 'log' (level, text), 'frame' (stats). */
  on(type, cb) {
    let set = this.listeners.get(type);
    if (!set) this.listeners.set(type, (set = new Set()));
    set.add(cb);
    return () => this.off(type, cb);
  }

  off(type, cb) { this.listeners.get(type)?.delete(cb); }

  /** Send data to the UI (`jsui.onMessage(name, cb)` / useHostMessage in JS). */
  postMessage(name, data) {
    this._withStrings([String(name), JSON.stringify(data === undefined ? null : data)], (n, j) =>
      this.module._jsui_post_message(this.rt, n, j));
  }

  // ------------------------------------------------- game data (jsui.h)

  /** Shared state: the UI reads it with useHostState(key) / HostState.get(key).
   * `value` undefined removes the key. Returns true if the value changed. */
  setState(key, value) {
    const json = value === undefined ? null : JSON.stringify(value);
    return this._withStrings([String(key), json], (k, j) => this.module._jsui_set_state(this.rt, k, j)) === 1;
  }

  /** RFC 7386 merge patch of a state key (null members delete). Returns true if the value changed. */
  patchState(key, patch) {
    return this._withStrings([String(key), JSON.stringify(patch === undefined ? null : patch)],
      (k, j) => this.module._jsui_patch_state(this.rt, k, j)) === 1;
  }

  /** Current value of a state key (also keys the UI set with HostState.set), or undefined. */
  getState(key) {
    const p = this._withStrings([String(key)], (k) => this.module._jsui_get_state(this.rt, k));
    return p ? JSON.parse(this.module.UTF8ToString(p)) : undefined;
  }

  /** Answers the UI's `Host.call(name, args)`: `fn(args)` returns a value or a
   * promise (a throw / rejection rejects the call). Calls without a handler
   * are emitted as 'call' (id, name, args) for resolveCall / rejectCall, or
   * rejected when nobody listens. Returns an unregister function. */
  handle(name, fn) {
    this._handlers.set(String(name), fn);
    return () => { if (this._handlers.get(String(name)) === fn) this._handlers.delete(String(name)); };
  }

  resolveCall(id, value) {
    if (this._destroyed) return -1;
    return this._withStrings([JSON.stringify(value === undefined ? null : value)],
      (j) => this.module._jsui_web_resolve_call(this.rt, id, j));
  }

  rejectCall(id, message) {
    if (this._destroyed) return -1;
    return this._withStrings([String(message ?? 'rejected')], (m) => this.module._jsui_web_reject_call(this.rt, id, m));
  }

  /** Calls a handler the UI registered with Host.handle(name, fn); resolves with its result. */
  callUi(name, args) {
    return new Promise((resolve, reject) => {
      const json = JSON.stringify(args === undefined ? null : args);
      const id = this._withStrings([String(name), json], (n, j) => this.module._jsui_web_call_ui(this.rt, this.hostId, n, j));
      this._uiCalls.set(id, { resolve, reject });
    });
  }

  _onCall(id, name, json) {
    let args;
    try { args = JSON.parse(json); } catch { args = undefined; }
    const fn = this._handlers.get(name);
    if (!fn) {
      if (!this._emit('call', id, name, args)) this.rejectCall(id, `no host handler for "${name}"`);
      return;
    }
    // Settled asynchronously (never re-entering the runtime from inside on_call).
    Promise.resolve().then(() => fn(args)).then(
      (v) => this.resolveCall(id, v),
      (e) => this.rejectCall(id, e && e.message !== undefined ? e.message : String(e)));
  }

  loadUrl(url) {
    const abs = new URL(url, typeof location !== 'undefined' ? location.href : undefined).href;
    this._withStrings([abs], (u) => this.module._jsui_load_bundle_url(this.rt, u));
  }

  loadSource(source, name = 'bundle.js') {
    const M = this.module;
    const len = M.lengthBytesUTF8(source);
    const src = M._malloc(len + 1);
    M.stringToUTF8(source, src, len + 1);
    const r = this._withStrings([name], (n) => M._jsui_load_bundle_source(this.rt, src, len, n));
    M._free(src);
    return r === 0;
  }

  reload() { this.module._jsui_reload(this.rt); }

  /** Register a font from bytes (ArrayBuffer / typed array). */
  registerFont(family, bytes, weight = 400, italic = false) {
    const M = this.module;
    const data = ArrayBuffer.isView(bytes) ? new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength)
      : new Uint8Array(bytes);
    const p = M._malloc(data.length);
    M.HEAPU8.set(data, p);
    const r = this._withStrings([family], (f) => M._jsui_register_font(this.rt, f, p, data.length, weight, italic ? 1 : 0));
    M._free(p);
    return r === 0;
  }

  get lastError() { return this._lastError; }

  /** Resolves after a frame with no resource loads in flight (bundle, images,
   * fonts, fetch) and nothing left to paint: the UI is on screen. */
  whenIdle() {
    return new Promise((resolve) => (this._idleWaiters ??= []).push(resolve));
  }

  start() {
    if (this._raf || this._destroyed) return;
    const loop = (t) => {
      this._raf = requestAnimationFrame(loop);
      this.frame(t);
    };
    this._raf = requestAnimationFrame(loop);
  }

  stop() {
    if (this._raf) cancelAnimationFrame(this._raf);
    this._raf = 0;
  }

  /** Runs one frame (tick + paint if needed). Called by the rAF loop; usable for manual stepping. */
  frame(now = performance.now()) {
    if (this._destroyed) return;
    const M = this.module, rt = this.rt, st = this.stats;
    if (this._padsActive) this._pollGamepads();
    const t0 = performance.now();
    M._jsui_tick(rt, now);
    const t1 = performance.now();
    st.tickMs = ema(st.tickMs, t1 - t0);
    st.frames++;
    if ((M._jsui_needs_redraw(rt) || this._sizeChanged) && this.pixelWidth > 0 && this.pixelHeight > 0) {
      const resized = this._sizeChanged;
      this._sizeChanged = false;
      let t2, t3;
      if (this._gpu) {
        // Skia draws straight into the canvas' WebGL framebuffer (full repaint).
        if (this._renderGpu() === false) this._switchToCpu('GPU frame failed', false);
        t2 = t3 = performance.now();
      } else {
        this._ensureBuffer();
        // The buffer keeps the previous frame, so only damaged pixels are repainted.
        const changed = M._jsui_render_rgba_incremental(rt, this._buf, this.pixelWidth, this.pixelHeight,
          this.pixelWidth * 4, this._dirty);
        t2 = performance.now();
        if (changed || resized) {
          const d = this._dirty >> 2, H = M.HEAP32;
          this._blit(resized ? null : { x: H[d], y: H[d + 1], width: H[d + 2], height: H[d + 3] });
        }
        t3 = performance.now();
      }
      st.renderMs = ema(st.renderMs, t2 - t1);
      st.blitMs = ema(st.blitMs, t3 - t2);
      st.renders++;
    }
    this._checkError();
    this._updateCursor();
    if (this.listeners.get('frame')?.size) this._emit('frame', st);
    if (this._idleWaiters?.length && !M._jsui_pending_loads(rt) && !M._jsui_needs_redraw(rt)) {
      const w = this._idleWaiters;
      this._idleWaiters = [];
      w.forEach((r) => r());
    }
  }

  /** The last rendered frame as premultiplied RGBA (copy), for tests and screenshots. */
  readPixels() {
    if (this._gpu) return this._readGpuPixels();
    if (!this._buf) return null;
    return { width: this.pixelWidth, height: this.pixelHeight,
      data: this.module.HEAPU8.slice(this._buf, this._buf + this._bufSize) };
  }

  destroy() {
    if (this._destroyed) return;
    this.stop();
    this._destroyed = true;
    this._abort.abort();
    this._blitAbort?.abort();
    this._canvasAbort?.abort();
    clearTimeout(this._restoreTimer);
    this._resizeObserver?.disconnect();
    this._sink?.remove();
    const M = this.module;
    M._jsui_destroy(this.rt);
    this.rt = 0;
    if (this._gpu) {
      M.GL.makeContextCurrent(this._glHandle);
      M._jsui_gpu_destroy(this._gpu);
      M.GL.makeContextCurrent(0);
      M.GL.deleteContext(this._glHandle);
      this._gpu = this._glHandle = 0;
    }
    M.__jsuiHosts.delete(this.hostId);
    if (this._buf) M._free(this._buf);
    if (this._baseUrlPtr) M._free(this._baseUrlPtr);
    M._free(this._scratch);
    M._free(this._dirty);
    this._buf = this._scratch = this._baseUrlPtr = this._dirty = 0;
    if (this._gl && this._tex) {
      this._gl.deleteTexture(this._tex);
      this._gl.deleteBuffer(this._vb);
      this._gl.deleteProgram(this._prog);
    }
  }

  // ---------------------------------------------------------------- helpers

  _emit(type, ...args) {
    const set = this.listeners.get(type);
    if (!set || set.size === 0) return false;
    for (const cb of set) {
      try { cb(...args); } catch (e) { console.error(e); }
    }
    return true;
  }

  _newString(s) {
    const M = this.module;
    const n = M.lengthBytesUTF8(s) + 1;
    const p = M._malloc(n);
    M.stringToUTF8(s, p, n);
    return p;
  }

  // Calls fn with C strings; short strings use the scratch area (no malloc).
  _withStrings(strs, fn) {
    const M = this.module;
    const ptrs = [], owned = [];
    let off = 0;
    for (const s of strs) {
      if (s == null) { ptrs.push(0); continue; }
      const n = M.lengthBytesUTF8(s) + 1;
      if (off + n <= 512) {
        M.stringToUTF8(s, this._scratch + off, n);
        ptrs.push(this._scratch + off);
        off += n;
      } else {
        const p = this._newString(s);
        ptrs.push(p);
        owned.push(p);
      }
    }
    try { return fn(...ptrs); } finally { for (const p of owned) M._free(p); }
  }

  _checkError() {
    const p = this.module._jsui_last_error(this.rt);
    const msg = p ? this.module.UTF8ToString(p) : null;
    if (msg !== this._lastError) {
      this._lastError = msg;
      if (msg) this._emit('error', msg);
    }
  }

  _updateCursor() {
    const p = this.module._jsui_get_cursor(this.rt);
    const c = p ? this.module.UTF8ToString(p) : 'default';
    if (c !== this._cursor) {
      this._cursor = c;
      this.canvas.style.cursor = c;
    }
  }

  // ------------------------------------------------------------ size / DPR

  _ratio() { return this._fixedRatio || (typeof devicePixelRatio === 'number' ? devicePixelRatio : 1) || 1; }

  // Logical size = the canvas' CSS box; device size = logical * ratio, which
  // is also the backing store size, so frames are blitted 1:1.
  _measure(entry) {
    const c = this.canvas;
    let cssW, cssH, devW, devH;
    if (entry) {
      const box = entry.contentBoxSize?.[0];
      cssW = box ? box.inlineSize : entry.contentRect.width;
      cssH = box ? box.blockSize : entry.contentRect.height;
      // Exact device-pixel size when the browser reports it (avoids blurry
      // 1px-off backing stores at fractional DPRs). Ignored when it disagrees
      // with CSS size * DPR by more than rounding (DPR emulation reports CSS px).
      const dbox = !this._fixedRatio && entry.devicePixelContentBoxSize?.[0];
      const r = this._ratio();
      if (dbox && Math.abs(dbox.inlineSize - cssW * r) <= 1 && Math.abs(dbox.blockSize - cssH * r) <= 1) {
        devW = dbox.inlineSize;
        devH = dbox.blockSize;
      }
    } else {
      cssW = c.clientWidth || c.width;
      cssH = c.clientHeight || c.height;
    }
    const w = Math.max(1, Math.round(cssW)), h = Math.max(1, Math.round(cssH));
    const ratio = this._ratio();
    devW = devW || Math.round(w * ratio);
    devH = devH || Math.round(h * ratio);
    const pr = devW / w;  // exact ratio of the backing store
    if (w === this.width && h === this.height && devW === this.pixelWidth && devH === this.pixelHeight) return false;
    this.width = w;
    this.height = h;
    this.pixelRatio = pr;
    this.pixelWidth = devW;
    this.pixelHeight = devH;
    c.width = devW;
    c.height = devH;
    this._sizeChanged = true;
    return true;
  }

  _installResize() {
    const apply = (entry) => {
      if (this._measure(entry) && this.rt) {
        this.module._jsui_set_viewport(this.rt, this.width, this.height, this.pixelRatio);
        this._rect = null;
      }
    };
    if (typeof ResizeObserver !== 'undefined') {
      this._resizeObserver = new ResizeObserver((entries) => apply(entries[entries.length - 1]));
      this._observeCanvas();
    }
    // DPR changes (zoom, moving between monitors) without a CSS size change.
    if (!this._fixedRatio && typeof matchMedia === 'function') {
      const watch = () => {
        const mq = matchMedia(`(resolution: ${this._ratio()}dppx)`);
        mq.addEventListener('change', () => { apply(); watch(); }, { once: true, signal: this._abort.signal });
      };
      watch();
    }
    addEventListener('scroll', () => { this._rect = null; }, { passive: true, capture: true, signal: this._abort.signal });
    addEventListener('resize', () => { this._rect = null; }, { passive: true, signal: this._abort.signal });
  }

  _observeCanvas() {
    const ro = this._resizeObserver;
    if (!ro) return;
    try {
      ro.observe(this.canvas, { box: 'device-pixel-content-box' });
    } catch {
      ro.observe(this.canvas);
    }
  }

  _ensureBuffer() {
    const size = this.pixelWidth * this.pixelHeight * 4;
    if (size === this._bufSize && this._buf) return;
    const M = this.module;
    if (this._buf) M._free(this._buf);
    this._buf = M._malloc(size);
    this._bufSize = size;
    this._texW = this._texH = 0;
    this._imageData = null;
  }

  // ---------------------------------------------------------------- blitting

  _initBlitter(mode) {
    this._blitAbort?.abort();
    this._blitAbort = new AbortController();
    if (mode === 'auto' || mode === 'gpu') {
      if (this._initGpu(mode === 'auto')) return;
      mode = 'cpu';  // CPU raster + blit on the same canvas
    }
    if (mode !== '2d') {
      // A context the GPU attempt created is reused (getContext returns it).
      const attrs = { alpha: true, premultipliedAlpha: true, antialias: false, depth: false, stencil: false,
        preserveDrawingBuffer: false, powerPreference: 'low-power' };
      let gl = this._gl;
      try { gl ??= this.canvas.getContext('webgl2', attrs) || this.canvas.getContext('webgl', attrs); } catch {}
      if (gl && !gl.isContextLost()) {
        this._setupGL(gl);
        const signal = this._blitAbort.signal;
        this.canvas.addEventListener('webglcontextlost', (e) => {
          e.preventDefault();
          this._glLost = true;
          this._armRestoreTimer('2d');
        }, { signal });
        this.canvas.addEventListener('webglcontextrestored', () => {
          clearTimeout(this._restoreTimer);
          this._glLost = false;
          this._setupGL(gl);
          this._texW = this._texH = 0;
          this._sizeChanged = true;  // repaint into the new context
        }, { signal });
        return;
      }
      this._gl = null;
      if (mode === 'webgl') throw new Error('createJsui: WebGL unavailable');
    }
    this._ctx2d = this.canvas.getContext('2d');
    if (!this._ctx2d) throw new Error('createJsui: no 2D context');
    this.renderer = '2d';
  }

  // Skia Ganesh on the canvas' WebGL context. The context is created here and
  // registered with emscripten's GL layer, which the GPU module's Skia calls
  // into. False when the module has no GPU backend, WebGL is unavailable or
  // the Skia GPU context cannot be created. `hardwareOnly` ('auto'): also when
  // the browser only offers WebGL with a major performance caveat (a software
  // rasteriser such as SwiftShader, where Skia's full GPU repaints cost more
  // than the CPU raster's incremental ones); the CPU path then gets a context
  // on the same canvas (the failed request creates none).
  _initGpu(hardwareOnly) {
    const M = this.module;
    if (!M.GL || !M._jsui_gpu_create_gl) return false;
    // Stencil: Ganesh uses it for some paths / clips. Premultiplied output
    // like the CPU blit.
    const attrs = { alpha: true, premultipliedAlpha: true, antialias: false, depth: false, stencil: true,
      preserveDrawingBuffer: false, powerPreference: 'high-performance', failIfMajorPerformanceCaveat: !!hardwareOnly };
    let gl = null;
    try { gl = this.canvas.getContext('webgl2', attrs) || this.canvas.getContext('webgl', attrs); } catch {}
    if (!gl || gl.isContextLost()) return false;
    const version = typeof WebGL2RenderingContext !== 'undefined' && gl instanceof WebGL2RenderingContext ? 2 : 1;
    this._glAttrs = { ...attrs, majorVersion: version, minorVersion: 0, enableExtensionsByDefault: true };
    this._gl = gl;  // a CPU fallback blits through the same context
    const handle = M.GL.registerContext(gl, this._glAttrs);
    if (!handle) return false;
    M.GL.makeContextCurrent(handle);
    const gpu = M._jsui_gpu_create_gl(0, 0);
    if (!gpu) {
      M.GL.makeContextCurrent(0);
      M.GL.deleteContext(handle);
      resetGLState(gl);
      this._log(2, 'Skia GPU context creation failed: using the CPU renderer');
      return false;
    }
    this._gpu = gpu;
    this._glHandle = handle;
    this._stencilBits = gl.getContextAttributes()?.stencil ? 8 : 0;
    this.renderer = version === 2 ? 'gpu-webgl2' : 'gpu-webgl';
    const signal = this._blitAbort.signal;
    this.canvas.addEventListener('webglcontextlost', (e) => {
      e.preventDefault();
      this._glLost = true;
      if (this._gpu) M._jsui_gpu_context_lost(this._gpu);  // drop GPU objects without GL calls
      this._armRestoreTimer('cpu');
    }, { signal });
    this.canvas.addEventListener('webglcontextrestored', () => {
      clearTimeout(this._restoreTimer);
      // Extension objects of the lost context are dead: register afresh. The
      // GPU context rebuilds itself on the next render.
      M.GL.deleteContext(this._glHandle);
      this._glHandle = M.GL.registerContext(gl, this._glAttrs);
      this._glLost = false;
      this._sizeChanged = true;  // repaint into the new context
    }, { signal });
    return true;
  }

  // A lost context that does not come back within contextRestoreTimeout:
  // continue on a fresh canvas with the CPU renderer ('cpu': WebGL blit if a
  // new context works, '2d': Canvas2D).
  _armRestoreTimer(mode) {
    clearTimeout(this._restoreTimer);
    const ms = this.opts.contextRestoreTimeout ?? 3000;
    if (!(ms >= 0 && ms < Infinity)) return;
    this._restoreTimer = setTimeout(() => {
      if (this._glLost && !this._destroyed) this._switchToCpu('WebGL context lost and not restored', true, mode);
    }, ms);
  }

  // Drops the GPU renderer (or a dead WebGL blitter) and continues with the
  // CPU raster. `freshCanvas`: the canvas' context is gone for good, so the
  // canvas is replaced by a clone (same id / class / style / place in the DOM).
  _switchToCpu(reason, freshCanvas, mode = 'cpu') {
    const M = this.module;
    clearTimeout(this._restoreTimer);
    if (this._gpu) {
      if (this._glHandle) M.GL.makeContextCurrent(this._glHandle);
      M._jsui_gpu_destroy(this._gpu);
      M.GL.makeContextCurrent(0);
      if (this._glHandle) M.GL.deleteContext(this._glHandle);
      this._gpu = this._glHandle = 0;
    }
    if (freshCanvas) {
      const old = this.canvas;
      const c = old.cloneNode(false);
      old.replaceWith(c);
      this.canvas = c;
      this._gl = null;
      this._glLost = false;
      this._tex = this._prog = this._vb = null;
      this._resizeObserver?.unobserve(old);
      this._observeCanvas();
      this._installCanvasInput();
      c.width = this.pixelWidth;
      c.height = this.pixelHeight;
      this._rect = null;
    } else if (this._gl) {
      resetGLState(this._gl);
    }
    this._texW = this._texH = 0;
    this._imageData = null;
    this._initBlitter(mode);
    this._sizeChanged = true;  // full repaint + upload on the next frame
    this._log(2, `${reason}: continuing with the CPU renderer (${this.renderer})`);
    this._emit('renderer', this.renderer, reason);
  }

  _log(level, text) {
    if (!this._emit('log', level, text)) (level >= 2 ? console.warn : console.log)('[jsui] ' + text);
  }

  // Draws a GPU frame: true when drawn, false when the GPU path failed (the
  // caller falls back to the CPU), undefined while the context is lost.
  _renderGpu() {
    if (this._glLost || !this._glHandle) return undefined;
    const M = this.module;
    M.GL.makeContextCurrent(this._glHandle);
    // JsuiGLTarget {fbo, texture, format, width, height, samples, stencil_bits, flags}
    const t = this._dirty >> 2;
    M.HEAP32.fill(0, t, t + 8);
    M.HEAP32[t + 3] = this.pixelWidth;
    M.HEAP32[t + 4] = this.pixelHeight;
    M.HEAP32[t + 6] = this._stencilBits;
    if (M._jsui_render_gl_target(this.rt, this._gpu, this._dirty) === 0) return true;
    return this._gl.isContextLost() ? undefined : false;
  }

  // Renders a frame and reads it back from the drawing buffer (in the same
  // task, so preserveDrawingBuffer is not needed). Premultiplied RGBA, top row first.
  _readGpuPixels() {
    const gl = this._gl, w = this.pixelWidth, h = this.pixelHeight;
    if (!this._renderGpu()) return null;
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    if (typeof WebGL2RenderingContext !== 'undefined' && gl instanceof WebGL2RenderingContext) {
      gl.bindBuffer(gl.PIXEL_PACK_BUFFER, null);
    }
    gl.pixelStorei(gl.PACK_ALIGNMENT, 4);
    const px = new Uint8Array(w * h * 4);
    gl.readPixels(0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, px);
    const row = w * 4, tmp = new Uint8Array(row);
    for (let y = 0; y < h >> 1; y++) {  // GL rows are bottom-up
      const a = y * row, b = (h - 1 - y) * row;
      tmp.set(px.subarray(a, a + row));
      px.copyWithin(a, b, b + row);
      px.set(tmp, b);
    }
    return { width: w, height: h, data: px };
  }

  _setupGL(gl) {
    const vs = 'attribute vec2 p; varying vec2 uv; void main() { uv = vec2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);' +
      ' gl_Position = vec4(p, 0.0, 1.0); }';
    const fs = 'precision mediump float; varying vec2 uv; uniform sampler2D t; void main() { gl_FragColor = texture2D(t, uv); }';
    const sh = (type, src) => {
      const s = gl.createShader(type);
      gl.shaderSource(s, src);
      gl.compileShader(s);
      if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(s));
      return s;
    };
    const prog = gl.createProgram();
    gl.attachShader(prog, sh(gl.VERTEX_SHADER, vs));
    gl.attachShader(prog, sh(gl.FRAGMENT_SHADER, fs));
    gl.linkProgram(prog);
    if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(prog));
    gl.useProgram(prog);
    const vb = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, vb);
    // One triangle covering the viewport.
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 3, -1, -1, 3]), gl.STATIC_DRAW);
    const loc = gl.getAttribLocation(prog, 'p');
    gl.enableVertexAttribArray(loc);
    gl.vertexAttribPointer(loc, 2, gl.FLOAT, false, 0, 0);
    const tex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    // Pixels are already premultiplied; don't let the browser touch them.
    gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
    gl.disable(gl.BLEND);
    this._gl = gl;
    this._gl2 = typeof WebGL2RenderingContext !== 'undefined' && gl instanceof WebGL2RenderingContext;
    this._prog = prog;
    this._tex = tex;
    this._vb = vb;
    this.renderer = this._gl2 ? 'webgl2' : 'webgl';
  }

  // Uploads the damaged part of the frame (`rect` in device px; null: all of
  // it) and presents. WebGL2 uploads exactly the rect (UNPACK_ROW_LENGTH),
  // WebGL 1 the full-width rows it spans, Canvas2D un-premultiplies and puts
  // just the rect.
  _blit(rect) {
    const w = this.pixelWidth, h = this.pixelHeight;
    // Re-read HEAPU8 every time: memory growth replaces the ArrayBuffer.
    const heap = this.module.HEAPU8.buffer;
    const gl = this._gl;
    if (gl && this._glLost) return;
    const full = !rect || (gl ? this._texW !== w || this._texH !== h
      : !this._imageData || this._imageData.width !== w || this._imageData.height !== h);
    let x = 0, y = 0, rw = w, rh = h;
    if (!full) {  // clamp (the runtime reports rects inside the buffer; be defensive)
      x = Math.max(0, rect.x);
      y = Math.max(0, rect.y);
      rw = Math.min(w, rect.x + rect.width) - x;
      rh = Math.min(h, rect.y + rect.height) - y;
      if (rw <= 0 || rh <= 0) return;
    }
    if (gl) {
      if (full) {
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array(heap, this._buf, w * h * 4));
        this._texW = w;
        this._texH = h;
      } else if (this._gl2) {
        gl.pixelStorei(gl.UNPACK_ROW_LENGTH, w);
        gl.texSubImage2D(gl.TEXTURE_2D, 0, x, y, rw, rh, gl.RGBA, gl.UNSIGNED_BYTE,
          new Uint8Array(heap, this._buf + (y * w + x) * 4, ((rh - 1) * w + rw) * 4));
        gl.pixelStorei(gl.UNPACK_ROW_LENGTH, 0);
      } else {
        x = 0;
        rw = w;
        gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, y, w, rh, gl.RGBA, gl.UNSIGNED_BYTE,
          new Uint8Array(heap, this._buf + y * w * 4, rh * w * 4));
      }
      gl.viewport(0, 0, w, h);
      gl.drawArrays(gl.TRIANGLES, 0, 3);  // the drawing buffer is not preserved: present all of it
    } else {
      // Canvas2D wants straight alpha.
      if (full) this._imageData = new ImageData(w, h);
      unpremultiply(new Uint8Array(heap, this._buf, w * h * 4), this._imageData.data, w, x, y, rw, rh);
      this._ctx2d.putImageData(this._imageData, 0, 0, x, y, rw, rh);
    }
    const st = this.stats;
    st.blits++;
    st.blitPixels += rw * rh;
    st.lastBlit = { x, y, width: rw, height: rh };
  }

  // ------------------------------------------------------------------ input

  _point(e) {
    const r = this._rect ??= this.canvas.getBoundingClientRect();
    // Map CSS px of the element box to logical px (equal unless the canvas is CSS-transformed).
    const sx = r.width ? this.width / r.width : 1, sy = r.height ? this.height / r.height : 1;
    return [(e.clientX - r.left) * sx, (e.clientY - r.top) * sy];
  }

  // Pointer / wheel listeners on the canvas (again on a replacement canvas
  // after a CPU fallback, see _switchToCpu).
  _installCanvasInput() {
    this._canvasAbort?.abort();
    this._canvasAbort = new AbortController();
    const M = this.module, c = this.canvas, signal = this._canvasAbort.signal;
    const opt = { signal };
    const active = { signal, passive: false };
    const focusSink = () => this.focus();
    c.style.touchAction = 'none';
    c.style.userSelect = 'none';
    c.style.webkitUserSelect = 'none';
    c.style.outline = 'none';

    // The DOM event time (same clock as the rAF timestamps given to
    // jsui_tick) stamps the event: gesture velocities see the real spacing
    // of the events that arrive between two frames.
    const stamp = (e) => M._jsui_set_event_time?.(this.rt, e.timeStamp);
    c.addEventListener('pointerdown', (e) => {
      stamp(e);
      const [x, y] = this._point(e);
      let used;
      if (e.pointerType === 'mouse') {
        this._mouseButtons |= 1 << e.button;
        used = M._jsui_send_mouse_button(this.rt, x, y, mouseButton(e.button), 1, modsOf(e));
      } else {
        used = M._jsui_send_touch(this.rt, e.pointerId, 0, x, y);
      }
      try { c.setPointerCapture(e.pointerId); } catch {}
      // Not for touch: focusing a textarea pops up the virtual keyboard.
      if (e.pointerType !== 'touch') focusSink();
      if (used) e.preventDefault();
      this._updateCursor();
    }, active);
    c.addEventListener('pointermove', (e) => {
      stamp(e);
      const [x, y] = this._point(e);
      if (e.pointerType === 'mouse') M._jsui_send_mouse_move(this.rt, x, y);
      else M._jsui_send_touch(this.rt, e.pointerId, 1, x, y);
    }, opt);
    const up = (e, cancel) => {
      stamp(e);
      const [x, y] = this._point(e);
      let used;
      if (e.pointerType === 'mouse') {
        if (!(this._mouseButtons & (1 << e.button))) return;
        this._mouseButtons &= ~(1 << e.button);
        used = M._jsui_send_mouse_button(this.rt, x, y, mouseButton(e.button), 0, modsOf(e));
      } else {
        used = M._jsui_send_touch(this.rt, e.pointerId, cancel ? 3 : 2, x, y);
      }
      if (used) e.preventDefault();
      this._updateCursor();
    };
    c.addEventListener('pointerup', (e) => up(e, false), active);
    c.addEventListener('pointercancel', (e) => up(e, true), active);
    c.addEventListener('pointerleave', (e) => {
      if (e.pointerType === 'mouse' && !this._mouseButtons) M._jsui_send_mouse_leave(this.rt);
    }, opt);
    c.addEventListener('wheel', (e) => {
      const [x, y] = this._point(e);
      // Normalise to px like browsers do for pixel-mode wheels.
      const k = e.deltaMode === 1 ? 16 : e.deltaMode === 2 ? this.height : 1;
      if (M._jsui_send_mouse_wheel(this.rt, x, y, e.deltaX * k, e.deltaY * k)) e.preventDefault();
    }, active);
    c.addEventListener('contextmenu', (e) => e.preventDefault(), active);
    // The compat mousedown would move focus to <body> (the canvas is not
    // focusable), stealing it from the keyboard sink focused in pointerdown.
    c.addEventListener('mousedown', (e) => e.preventDefault(), active);
  }

  _installInput() {
    const M = this.module, signal = this._abort.signal;
    const opt = { signal };
    const active = { signal, passive: false };

    // Keyboard sink: a transparent textarea over the canvas. It gives us real
    // text input (IME composition, dead keys, emoji pickers, mobile keyboards)
    // instead of guessing text from keydown.
    const sink = document.createElement('textarea');
    sink.setAttribute('aria-hidden', 'true');
    sink.setAttribute('autocomplete', 'off');
    sink.setAttribute('autocapitalize', 'off');
    sink.setAttribute('spellcheck', 'false');
    sink.tabIndex = 0;
    Object.assign(sink.style, { position: 'fixed', left: '0px', top: '0px', width: '1px', height: '1px',
      opacity: '0', padding: '0', border: '0', margin: '0', resize: 'none', overflow: 'hidden',
      pointerEvents: 'none', zIndex: '-1', caretColor: 'transparent' });
    document.body.appendChild(sink);
    this._sink = sink;
    const focusSink = () => {
      const r = this._rect ??= this.canvas.getBoundingClientRect();
      sink.style.left = `${Math.max(0, r.left)}px`;
      sink.style.top = `${Math.max(0, r.top)}px`;
      if (document.activeElement !== sink) sink.focus({ preventScroll: true });
    };
    this.focus = focusSink;

    sink.addEventListener('keydown', (e) => {
      if (e.isComposing || e.keyCode === 229) return;  // IME owns this key
      if (!e.repeat) this._pressedKeys.set(e.code || e.key, e.key);
      const used = this._withStrings([e.key || null, e.code || null], (k, cd) =>
        M._jsui_send_key(this.rt, k, cd, 1, e.repeat ? 1 : 0, modsOf(e)));
      if ((used && (NAV_KEYS.has(e.key) || e.ctrlKey || e.metaKey)) || (e.key === 'Tab' && this._trapTab)) {
        e.preventDefault();
      }
    }, active);
    sink.addEventListener('keyup', (e) => {
      this._pressedKeys.delete(e.code || e.key);
      const used = this._withStrings([e.key || null, e.code || null], (k, cd) =>
        M._jsui_send_key(this.rt, k, cd, 0, 0, modsOf(e)));
      if (used && NAV_KEYS.has(e.key)) e.preventDefault();
    }, active);
    const flushText = () => {
      const v = sink.value;
      sink.value = '';
      if (v) this._withStrings([v], (t) => M._jsui_send_text(this.rt, t));
    };
    sink.addEventListener('input', (e) => { if (!e.isComposing) flushText(); }, opt);
    sink.addEventListener('compositionend', flushText, opt);
    // Losing focus must not leave keys or buttons stuck down in the UI.
    const releaseAll = () => {
      for (const [code, key] of this._pressedKeys) {
        this._withStrings([key, code], (k, cd) => M._jsui_send_key(this.rt, k, cd, 0, 0, 0));
      }
      this._pressedKeys.clear();
    };
    sink.addEventListener('blur', releaseAll, opt);
    addEventListener('blur', () => {
      releaseAll();
      if (this._mouseButtons) {
        for (let b = 0; b < 3; b++) {
          if (this._mouseButtons & (1 << b)) M._jsui_send_mouse_button(this.rt, -1, -1, mouseButton(b), 0, 0);
        }
        this._mouseButtons = 0;
      }
      M._jsui_send_mouse_leave(this.rt);
      this._releasePads();
    }, opt);
    if (this.opts.autoFocus !== false) focusSink();
  }

  // ---------------------------------------------------------------- gamepad

  _installGamepads() {
    if (typeof navigator === 'undefined' || !navigator.getGamepads) return;
    // Some browsers only expose pads to pages that listen for these events.
    const signal = this._abort.signal;
    addEventListener('gamepadconnected', () => { this._padsActive = true; }, { signal });
    addEventListener('gamepaddisconnected', () => { this._padsActive = true; }, { signal });
    this._padsActive = Array.from(navigator.getGamepads()).some(Boolean);
  }

  _pollGamepads() {
    const M = this.module, rt = this.rt;
    const pads = navigator.getGamepads();
    let any = false;
    const n = Math.max(pads.length, this._pads.length);
    for (let i = 0; i < n; i++) {
      const p = pads[i];
      let s = this._pads[i];
      if (!p || !p.connected) {
        if (s) {
          this._releasePad(i, s);
          M._jsui_set_gamepad_connected(rt, i, 0, 0);
          this._pads[i] = undefined;
        }
        continue;
      }
      any = true;
      if (!s) {
        s = this._pads[i] = { pressed: new Uint8Array(GAMEPAD_BUTTONS), axes: new Float32Array(GAMEPAD_AXES) };
        this._withStrings([p.id], (id) => M._jsui_set_gamepad_connected(rt, i, 1, id));
      }
      const nb = Math.min(GAMEPAD_BUTTONS, p.buttons.length);
      for (let b = 0; b < nb; b++) {
        const btn = p.buttons[b];
        const down = btn.pressed ? 1 : 0;
        if (down !== s.pressed[b]) {
          s.pressed[b] = down;
          M._jsui_send_gamepad_button(rt, i, b, down, btn.value);
        }
      }
      const na = Math.min(GAMEPAD_AXES, p.axes.length);
      for (let a = 0; a < na; a++) {
        const v = p.axes[a];
        const prev = s.axes[a];
        if (Math.abs(v - prev) > AXIS_EPSILON || (v === 0 && prev !== 0)) {
          s.axes[a] = v;
          M._jsui_send_gamepad_axis(rt, i, a, v);
        }
      }
    }
    this._padsActive = any;
  }

  _releasePad(i, s) {
    for (let b = 0; b < GAMEPAD_BUTTONS; b++) {
      if (s.pressed[b]) {
        s.pressed[b] = 0;
        this.module._jsui_send_gamepad_button(this.rt, i, b, 0, 0);
      }
    }
    for (let a = 0; a < GAMEPAD_AXES; a++) {
      if (s.axes[a] !== 0) {
        s.axes[a] = 0;
        this.module._jsui_send_gamepad_axis(this.rt, i, a, 0);
      }
    }
  }

  _releasePads() { this._pads.forEach((s, i) => s && this._releasePad(i, s)); }

  // --------------------------------------------------------------- dev mode

  // Polls GET <bundle origin>/__jsui/version (served by `jsui dev`); the body
  // is an opaque token that changes whenever the bundle was rebuilt. A
  // long-polling server simply holds the request; we re-ask right away.
  async _startDevPoll() {
    const url = new URL('/__jsui/version', this._bundleUrl).href;
    const signal = this._abort.signal;
    let last = null;
    while (!this._destroyed) {
      let ok = false;
      try {
        const res = await fetch(url, { cache: 'no-store', signal });
        if (res.ok) {
          const v = (await res.text()).trim();
          if (last !== null && v !== last && !this._destroyed) {
            this._emit('log', 1, 'bundle changed, reloading');
            this.reload();
          }
          last = v;
          ok = true;
        }
      } catch {
        if (this._destroyed) return;
      }
      await new Promise((r) => setTimeout(r, ok ? 500 : 2000));
    }
  }
}

function mouseButton(domButton) {
  // DOM: 0 left, 1 middle, 2 right == JsuiMouseButton. Others map to left.
  return domButton === 1 || domButton === 2 ? domButton : 0;
}

function ema(prev, v) { return prev ? prev * 0.9 + v * 0.1 : v; }

// Premultiplied -> straight alpha for the rect (x, y, rw, rh) of a frame
// `w` px wide (Canvas2D blitter only).
function unpremultiply(src, dst, w, x, y, rw, rh) {
  for (let row = y; row < y + rh; row++) {
    const end = (row * w + x + rw) * 4;
    for (let i = (row * w + x) * 4; i < end; i += 4) {
      const a = src[i + 3];
      if (a === 255) {
        dst[i] = src[i]; dst[i + 1] = src[i + 1]; dst[i + 2] = src[i + 2]; dst[i + 3] = 255;
      } else if (a === 0) {
        dst[i] = dst[i + 1] = dst[i + 2] = dst[i + 3] = 0;
      } else {
        const k = 255 / a;
        dst[i] = src[i] * k + 0.5; dst[i + 1] = src[i + 1] * k + 0.5; dst[i + 2] = src[i + 2] * k + 0.5; dst[i + 3] = a;
      }
    }
  }
}

// GL state Skia may have left behind, before the CPU blitter takes over its context.
function resetGLState(gl) {
  if (gl.isContextLost()) return;
  const gl2 = typeof WebGL2RenderingContext !== 'undefined' && gl instanceof WebGL2RenderingContext;
  gl.bindFramebuffer(gl.FRAMEBUFFER, null);
  gl.bindRenderbuffer(gl.RENDERBUFFER, null);
  gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, null);
  for (const cap of [gl.SCISSOR_TEST, gl.STENCIL_TEST, gl.DEPTH_TEST, gl.CULL_FACE, gl.BLEND, gl.DITHER]) gl.disable(cap);
  gl.colorMask(true, true, true, true);
  gl.activeTexture(gl.TEXTURE0);
  if (gl2) {
    gl.bindVertexArray(null);
    gl.bindBuffer(gl.PIXEL_UNPACK_BUFFER, null);
    gl.bindSampler(0, null);
    gl.pixelStorei(gl.UNPACK_ROW_LENGTH, 0);
    gl.pixelStorei(gl.UNPACK_SKIP_PIXELS, 0);
    gl.pixelStorei(gl.UNPACK_SKIP_ROWS, 0);
  } else {
    gl.getExtension('OES_vertex_array_object')?.bindVertexArrayOES(null);
  }
  const n = gl.getParameter(gl.MAX_VERTEX_ATTRIBS);
  for (let i = 0; i < n; i++) gl.disableVertexAttribArray(i);
}

export default createJsui;
