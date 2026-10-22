// JSUI browser host: runs the WASM build of the JSUI runtime on a <canvas>.
//
//   import { createJsui } from './jsui-web.js';
//   const jsui = await createJsui({ canvas, bundleUrl: './bundle.js' });
//   jsui.on('message', (name, data) => ...);
//   jsui.postMessage('hud', { hp: 42 });
//
// Per frame (requestAnimationFrame): poll gamepads -> jsui_tick -> if the UI
// changed, jsui_render_rgba into a buffer in the WASM heap -> upload it as a
// WebGL texture straight from the heap (the runtime's premultiplied RGBA is
// exactly what a premultipliedAlpha:true context composites, so there is no
// CPU conversion and no copy besides the GPU upload). A Canvas2D fallback
// un-premultiplies into an ImageData when WebGL is unavailable.
//
// One WASM module instance is shared by all runtimes on the page (one per
// module URL); each createJsui() call owns an independent JsuiRuntime.

/** @typedef {{
 *   canvas: HTMLCanvasElement,
 *   bundleUrl?: string,          // URL of the JS bundle (resolved against the page)
 *   bundleSource?: string,       // or the bundle source itself
 *   devMode?: boolean,           // dev overlay + live reload
 *   devPoll?: boolean,           // host-side polling of /__jsui/version (default: devMode)
 *   pixelRatio?: number,         // default: window.devicePixelRatio (tracked live)
 *   moduleUrl?: string|URL,      // default: ./dist/jsui.js next to this file
 *   renderer?: 'auto'|'webgl'|'2d',
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
  const moduleUrl = opts.moduleUrl ?? new URL('./dist/jsui.js', import.meta.url);
  const M = await loadModule(moduleUrl);
  return new JsuiHost(M, canvas, opts);
}

class JsuiHost {
  constructor(M, canvas, opts) {
    this.module = M;
    this.canvas = canvas;
    this.opts = opts;
    this.hostId = nextHostId++;
    this.listeners = new Map();
    this.stats = { frames: 0, renders: 0, tickMs: 0, renderMs: 0, blitMs: 0 };
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

    (M.__jsuiHosts ??= new Map()).set(this.hostId, {
      onMessage: (name, json) => {
        let data;
        try { data = JSON.parse(json); } catch { data = json; }
        this._emit('message', name, data);
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
    this._installResize();
    if (opts.gamepads !== false) this._installGamepads();

    if (opts.bundleSource != null) this.loadSource(opts.bundleSource, this._bundleUrl ?? 'bundle.js');
    else if (this._bundleUrl) this.loadUrl(this._bundleUrl);
    if (opts.devMode && (opts.devPoll ?? true) && this._bundleUrl) this._startDevPoll();
    if (opts.autoStart !== false) this.start();
  }

  // ------------------------------------------------------------- public API

  /** Subscribe: 'message' (name, data), 'error' (message), 'log' (level, text), 'frame' (stats). */
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
    const data = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes.buffer ?? bytes);
    const p = M._malloc(data.length);
    M.HEAPU8.set(data, p);
    const r = this._withStrings([family], (f) => M._jsui_register_font(this.rt, f, p, data.length, weight, italic ? 1 : 0));
    M._free(p);
    return r === 0;
  }

  get lastError() { return this._lastError; }

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
      this._sizeChanged = false;
      this._ensureBuffer();
      M._jsui_render_rgba(rt, this._buf, this.pixelWidth, this.pixelHeight, this.pixelWidth * 4);
      const t2 = performance.now();
      this._blit();
      const t3 = performance.now();
      st.renderMs = ema(st.renderMs, t2 - t1);
      st.blitMs = ema(st.blitMs, t3 - t2);
      st.renders++;
    }
    this._checkError();
    this._updateCursor();
    if (this.listeners.get('frame')?.size) this._emit('frame', st);
  }

  /** The last rendered frame as premultiplied RGBA (copy), for tests and screenshots. */
  readPixels() {
    if (!this._buf) return null;
    return { width: this.pixelWidth, height: this.pixelHeight,
      data: this.module.HEAPU8.slice(this._buf, this._buf + this._bufSize) };
  }

  destroy() {
    if (this._destroyed) return;
    this.stop();
    this._destroyed = true;
    this._abort.abort();
    this._resizeObserver?.disconnect();
    this._sink?.remove();
    const M = this.module;
    M._jsui_destroy(this.rt);
    this.rt = 0;
    M.__jsuiHosts.delete(this.hostId);
    if (this._buf) M._free(this._buf);
    if (this._baseUrlPtr) M._free(this._baseUrlPtr);
    M._free(this._scratch);
    this._buf = this._scratch = this._baseUrlPtr = 0;
    if (this._gl) {
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
      try {
        this._resizeObserver.observe(this.canvas, { box: 'device-pixel-content-box' });
      } catch {
        this._resizeObserver.observe(this.canvas);
      }
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
    if (mode !== '2d') {
      const attrs = { alpha: true, premultipliedAlpha: true, antialias: false, depth: false, stencil: false,
        preserveDrawingBuffer: false, powerPreference: 'low-power' };
      const gl = this.canvas.getContext('webgl2', attrs) || this.canvas.getContext('webgl', attrs);
      if (gl) {
        this._setupGL(gl);
        return;
      }
      if (mode === 'webgl') throw new Error('createJsui: WebGL unavailable');
    }
    this._ctx2d = this.canvas.getContext('2d');
    if (!this._ctx2d) throw new Error('createJsui: no 2D context');
    this.renderer = '2d';
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
    this._prog = prog;
    this._tex = tex;
    this._vb = vb;
    this.renderer = typeof WebGL2RenderingContext !== 'undefined' && gl instanceof WebGL2RenderingContext ? 'webgl2' : 'webgl';
    this.canvas.addEventListener('webglcontextlost', (e) => { e.preventDefault(); this._glLost = true; },
      { signal: this._abort.signal });
    this.canvas.addEventListener('webglcontextrestored', () => {
      this._glLost = false;
      this._setupGL(gl);
      this._texW = this._texH = 0;
      this._sizeChanged = true;  // repaint into the new context
    }, { signal: this._abort.signal });
  }

  _blit() {
    const w = this.pixelWidth, h = this.pixelHeight;
    // Re-read HEAPU8 every time: memory growth replaces the ArrayBuffer.
    const px = new Uint8Array(this.module.HEAPU8.buffer, this._buf, w * h * 4);
    const gl = this._gl;
    if (gl) {
      if (this._glLost) return;
      if (this._texW !== w || this._texH !== h) {
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, px);
        this._texW = w;
        this._texH = h;
      } else {
        gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, px);
      }
      gl.viewport(0, 0, w, h);
      gl.drawArrays(gl.TRIANGLES, 0, 3);
      return;
    }
    // Canvas2D wants straight alpha.
    if (!this._imageData || this._imageData.width !== w || this._imageData.height !== h) {
      this._imageData = new ImageData(w, h);
    }
    unpremultiply(px, this._imageData.data);
    this._ctx2d.putImageData(this._imageData, 0, 0);
  }

  // ------------------------------------------------------------------ input

  _point(e) {
    const r = this._rect ??= this.canvas.getBoundingClientRect();
    // Map CSS px of the element box to logical px (equal unless the canvas is CSS-transformed).
    const sx = r.width ? this.width / r.width : 1, sy = r.height ? this.height / r.height : 1;
    return [(e.clientX - r.left) * sx, (e.clientY - r.top) * sy];
  }

  _installInput() {
    const M = this.module, c = this.canvas, signal = this._abort.signal;
    const opt = { signal };
    const active = { signal, passive: false };
    c.style.touchAction = 'none';
    c.style.userSelect = 'none';
    c.style.webkitUserSelect = 'none';
    c.style.outline = 'none';

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
      const r = this._rect ??= c.getBoundingClientRect();
      sink.style.left = `${Math.max(0, r.left)}px`;
      sink.style.top = `${Math.max(0, r.top)}px`;
      if (document.activeElement !== sink) sink.focus({ preventScroll: true });
    };
    this.focus = focusSink;

    c.addEventListener('pointerdown', (e) => {
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
      const [x, y] = this._point(e);
      if (e.pointerType === 'mouse') M._jsui_send_mouse_move(this.rt, x, y);
      else M._jsui_send_touch(this.rt, e.pointerId, 1, x, y);
    }, opt);
    const up = (e, cancel) => {
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

// Premultiplied -> straight alpha (Canvas2D fallback only).
function unpremultiply(src, dst) {
  const n = src.length;
  for (let i = 0; i < n; i += 4) {
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

export default createJsui;
