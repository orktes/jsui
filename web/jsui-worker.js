// Worker mode for web/jsui-web.js: createJsui({ worker: true, ... }) runs the
// runtime, its frame loop and its rendering (WebGPU, WebGL, CPU) in a
// dedicated Worker on OffscreenCanvases (transferControlToOffscreen), so the
// page's thread spends a frame only on what needs the DOM:
//
//   page thread (this file's WorkerHost / WorkerSurface)    worker (serve(): RemoteHost / RemoteSurface)
//   ----------------------------------------------------    ------------------------------------------------
//   pointer / wheel / key / IME / focus listeners  --ops-->  jsui_send_* (moves and wheels coalesced per frame)
//   canvas size and DPR (ResizeObserver)           --ops-->  OffscreenCanvas size, jsui_set_viewport
//   gamepad polling (only while a pad is connected)--ops-->  jsui_send_gamepad_*
//   bindVideo: requestVideoFrameCallback,          --ops-->  host textures (web/jsui-textures.js)
//     new VideoFrame(video), transferred
//   setState, setBuffer, postMessage, ... calls    --ops-->  the runtime; answers come back as replies
//   events, cursor, hit map, replies              <--evs--   the frame loop: its own requestAnimationFrame
//
// Both directions batch their messages (one postMessage per task / frame,
// buffers and frames transferred). The worker's entry is jsui-web.js itself
// (it calls serve() below when it finds itself in a worker named
// 'jsui-host'), so it loads the same, versioned, files as the page; this file
// imports nothing and gets jsui-web.js' classes from it (`I`, internals()).
//
// Synchronous answers the page's thread cannot wait for:
//  - What the UI captures under the pointer (pass-through routing, which
//    events to preventDefault): the worker posts a hit map, the rects of
//    jsui_hit_regions, whenever it changes (after a repaint, a resize, or
//    every 30 frames) while the page needs one (pass-through, a pointer over
//    the canvas, a touch screen, hitTest() called), and the page answers hitTest() and its
//    input listeners' "consumed?" from the last one (hitMapStats measures
//    how old it gets). The worker reports predictions the runtime disagreed
//    with.
//  - Keys: the last answer the runtime gave for that key and modifiers
//    (navigation keys are assumed consumed until it answered).
//  - Everything else that returns a runtime answer returns a Promise of it
//    (getState, getPerf, readPixels, setState's "changed", ...; web/README.md).
//
// Clocks: the worker's rAF times and performance.now() are moved to the
// page's clock (timeOrigin difference), so event timestamps from the page
// and frame times share the clock jsui_tick and jsui_set_event_time expect.

const HIT_REFRESH_FRAMES = 30;  // a hit map is recomputed at least this often (ms ~ 0.5 s at 60 Hz)
const MAX_VIDEO_FRAMES = 2;     // bindVideo frames in flight to the worker before frames are skipped
const RAF_WATCHDOG_MS = 1000;   // no worker rAF callback by then: the page's rAF times the frames
// Events posted only while the page listens; the others always go (the page
// keeps state from them, or the UI may send them before the page listens).
const GATED_EVENTS = new Set(['log', 'frame']);

const msgOf = (e) => (e && e.message !== undefined ? e.message : String(e));
const keyCombo = (key, mods) => `${key}|${mods & 15}`;

// ===================================================================== page

let pageClasses = null;

/**
 * Starts a runtime in a Worker for createJsui(): { host } (a WorkerHost), or
 * { canvas } when worker mode is unavailable here (logged once), with the
 * canvas the main-thread runtime should use (a clone when the original's
 * control already went to the worker).
 */
export async function startWorkerHost(I, opts) {
  pageClasses ??= definePage(I);
  let canvas = opts.canvas ?? null;
  const fallback = (why, c = canvas) => {
    console.warn(`[jsui] worker mode unavailable (${why}): running on the page's main thread`);
    return { canvas: c };
  };
  if (typeof Worker === 'undefined') return fallback('no Worker');
  if (typeof OffscreenCanvas === 'undefined') return fallback('no OffscreenCanvas');
  if (canvas && typeof canvas.transferControlToOffscreen !== 'function') return fallback('no transferControlToOffscreen');
  if (canvas && I.transferred.has(canvas)) canvas = I.replaceCanvas(canvas);
  let worker;
  try {
    worker = new Worker(I.hostUrl, { type: 'module', name: I.workerName });
  } catch (e) {
    return fallback(`new Worker: ${msgOf(e)}`);
  }
  const hello = await new Promise((resolve) => {
    const timer = setTimeout(() => resolve('the worker did not start within 10 s'), 10000);
    worker.onmessage = (e) => {
      if (e.data?.t !== 'evs') return;
      const ev = e.data.list.find((x) => x.ev === 'hello' || x.ev === 'initError');
      if (!ev) return;
      clearTimeout(timer);
      resolve(ev.ev === 'hello' ? true : ev.message);
    };
    worker.onerror = (e) => {
      clearTimeout(timer);
      e.preventDefault?.();
      resolve(`the worker failed to load: ${e.message || 'error'}`);
    };
  });
  if (hello !== true) {
    worker.terminate();
    return fallback(hello);
  }
  const host = new pageClasses.WorkerHost(worker, canvas, opts);
  try {
    await host._start();
  } catch (e) {
    host._abandon();
    const c = canvas && I.transferred.has(canvas) ? I.replaceCanvas(canvas) : canvas;
    return fallback(`the worker could not start the runtime: ${msgOf(e)}`, c);
  }
  return { host };
}

function definePage(I) {
  const { DomView, HIT_POINTER, HIT_SCROLL, NAV_KEYS, bufferType, sourceSize, defaultOpaque, isVideoFrame, fontArgs,
    fontList, corsWarning, taintKey, videoFramesContextLost, watchVideoFrames } = I;
  const textureWarning = (name, e) => (e?.name === 'SecurityError' ? corsWarning(name)
    : `host texture "${name}": capture failed: ${msgOf(e)}`);

  // Options as the worker gets them: cloneable, URLs absolute (a worker
  // resolves relative URLs against its own script).
  const workerOpts = (opts) => {
    const o = {};
    for (const [k, v] of Object.entries(opts)) {
      if (k === 'canvas' || k === 'worker' || k === 'audio' || k.startsWith('_') || typeof v === 'function') continue;
      o[k] = v instanceof URL ? v.href : v;
    }
    const base = location.href;
    for (const k of ['bundleUrl', 'moduleUrl', 'moduleBase']) if (o[k] != null) o[k] = new URL(o[k], base).href;
    if (o.initialProps !== undefined) o.initialProps = JSON.parse(JSON.stringify(o.initialProps ?? {}));
    o.gamepads = false;  // polled here
    o.autoFocus = false;
    return o;
  };

  // Audio plays on the page (Web Audio needs the page's thread and its user
  // gestures): the worker's runtime hands every command over ('audio'
  // events) and gets the voice-ended / sound-loaded reports back
  // ('audioReports' ops).

  // A view as the page sees it: DomView's listeners, size and cursor, input
  // posted to the worker, hitTest() from the worker's last hit map.
  class ProxyView extends DomView {
    constructor(canvas, opts) {
      super(canvas, opts);
      this.v = 0;
      this.renderer = undefined;
      this.module = null;  // in the worker
      this._remoteCursor = 'default';
      this._hits = null;           // Float32Array: x, y, w, h, flags per region, bottom to top
      this._keyUsed = new Map();   // key|mods -> the runtime's last answer
      this._keyDown = new Map();   // code -> whether its press was predicted consumed
      this._hitsWant = new Set();  // why the page needs the hit map (_wantHits)
      this._touchUsed = new Map(); // touch id -> prediction for its begin
      this._press = 0;
      this._evT = undefined;
      this._transferred = false;
    }

    _post(op, transfer) {
      op.v = this.v;
      this._hostOf()._send(op, transfer);
    }

    _call(op, transfer, dflt) {
      op.v = this.v;
      return this._hostOf()._request(op, transfer, dflt);
    }

    _stamp(op) {
      if (this._evT !== undefined) {
        op.t = this._evT;
        this._evT = undefined;
      }
      return op;
    }

    // ---------------------------------------------------- input (DomView)

    _inTime(t) { this._evT = t; }
    _inButton(x, y, b, down, mods) {
      if (down) this._pointerSeen();
      const p = down ? +((this._hitAt(x, y) & HIT_POINTER) !== 0) : this._press;
      if (down) this._press = p;
      this._post(this._stamp({ op: 'button', x, y, b, down, mods, p }));
      return p;
    }
    _inMove(x, y) {
      this._post(this._stamp({ op: 'move', x, y }));
      return +((this._hitAt(x, y) & HIT_POINTER) !== 0);
    }
    _inTouch(id, phase, x, y) {
      let p;
      if (phase === 0) {
        this._pointerSeen();
        p = +(this._hitAt(x, y) !== 0);
        this._touchUsed.set(id, p);
      } else {
        p = this._touchUsed.get(id) ?? 0;
        if (phase >= 2) this._touchUsed.delete(id);
      }
      this._post(this._stamp({ op: 'touch', id, phase, x, y, p }));
      return p;
    }
    _inWheel(x, y, dx, dy) {
      this._pointerSeen();
      const p = +((this._hitAt(x, y) & HIT_SCROLL) !== 0);
      this._post(this._stamp({ op: 'wheel', x, y, dx, dy, p }));
      return p;
    }
    _inLeave() {
      this._post({ op: 'leave' });
      return 0;
    }
    _inKey(key, code, down, repeat, mods) {
      const k = code || key;
      let p;
      if (down && !repeat) {
        p = this._keyUsed.get(keyCombo(key, mods));
        // Not answered yet: navigation keys (no F-keys, no shortcuts) are taken.
        if (p === undefined) p = +(NAV_KEYS.has(key) && !/^F\d/.test(key) && !(mods & 10));
        this._keyDown.set(k, p);
      } else {
        p = this._keyDown.get(k) ?? 0;
        if (!down) this._keyDown.delete(k);
      }
      this._post(this._stamp({ op: 'key', key, code, down, repeat, mods, p }));
      return p;
    }
    _inText(text) {
      this._post({ op: 'text', text });
      return 1;
    }

    // Focus came back from the page: the next Tab moves focus inside the UI
    // (predicted consumed) whatever the last one did at the chain's end.
    _sinkFocused() {
      super._sinkFocused();
      for (const k of this._keyUsed.keys()) if (k.startsWith('Tab|')) this._keyUsed.delete(k);
    }

    /** What the UI captures at (x, y) (HIT_POINTER / HIT_SCROLL flags), from
     * the worker's last hit map: synchronous, possibly a frame or two old.
     * The worker keeps the map current from the first call on (before it,
     * only while pass-through or the pointer over the canvas need it). */
    hitTest(x, y) {
      this._wantHits('api', true);
      return this._hitAt(x, y);
    }

    _hitAt(x, y) {
      const r = this._hits;
      this._hostOf().hitMapStats.lookups++;
      if (!r) return 0;
      for (let i = r.length - 5; i >= 0; i -= 5) {
        if (x >= r[i] && y >= r[i + 1] && x < r[i] + r[i + 2] && y < r[i + 1] + r[i + 3]) return r[i + 4];
      }
      return 0;
    }

    // The worker computes and posts the hit map (jsui_hit_regions, the
    // costliest part of its frame's extras) only while something here needs
    // it: 'pass' (pass-through routes every press by it), 'pointer' (over the
    // canvas: presses and wheels ask it whether to preventDefault), 'touch'
    // (a touch screen: no hover fetches the map before a touch) or 'api'
    // (the page called hitTest()).
    _wantHits(reason, on) {
      const was = this._hitsWant.size > 0;
      if (on) this._hitsWant.add(reason);
      else this._hitsWant.delete(reason);
      const now = this._hitsWant.size > 0;
      if (now !== was && !this._destroyed) this._post({ op: 'hits', on: now });
    }

    _pointerOver(on) { this._wantHits('pointer', on); }

    // Input from the canvas without a pointerenter before it (the canvas
    // appeared under the mouse).
    _pointerSeen() { if (!this._hitsWant.has('pointer')) this._pointerOver(true); }

    _installDom() {
      super._installDom();
      if (this.opts.passThrough) this._wantHits('pass', true);
      if (globalThis.navigator?.maxTouchPoints > 0) this._wantHits('touch', true);
    }

    _cursorNow() { return this._remoteCursor; }

    // ----------------------------------------------------------- size

    _setBackingStore(w, h) {
      if (!this._transferred) super._setBackingStore(w, h);  // the worker sizes its OffscreenCanvas
    }

    _resized() {
      this._rect = null;
      this._post({ op: 'size', size: this._size() });
    }

    _size() {
      return { w: this.width, h: this.height, pw: this.pixelWidth, ph: this.pixelHeight, pr: this.pixelRatio,
        ratio: this._ratio(), cssW: this._cssW ?? this.width, cssH: this._cssH ?? this.height };
    }

    // An auto-sized axis: the worker laid the component out at w x h.
    _onContentSize(w, h) {
      this._contentW = w;
      this._contentH = h;
      if (this._autoW) this.canvas.style.width = `${w}px`;
      if (this._autoH) this.canvas.style.height = `${h}px`;
      if (this._measure()) this._resized();
      this._rect = null;
    }

    _transfer() {
      const off = this.canvas.transferControlToOffscreen();
      I.transferred.add(this.canvas);
      this._transferred = true;
      return off;
    }

    // The worker's renderer lost its canvas' context for good: a clone takes
    // the canvas' place and its control goes to the worker (as in main mode).
    _replaceCanvas() {
      if (this._destroyed) return;
      const old = this.canvas;
      const c = old.cloneNode(false);
      old.replaceWith(c);
      this.canvas = c;
      this._transferred = false;
      this._setBackingStore(this.pixelWidth, this.pixelHeight);
      this._resizeObserver?.unobserve(old);
      this._observeCanvas();
      this._installCanvasInput();
      this._rect = null;
      this._cursor = '';
      this._updateCursor();
      const off = this._transfer();
      this._post({ op: 'canvas', canvas: off }, [off]);
    }

    // ------------------------------------------------------- listeners

    on(type, cb) {
      const had = this._listens(type);
      const off = super.on(type, cb);
      if (!had && GATED_EVENTS.has(type)) this._post({ op: 'listen', type, on: true });
      return off;
    }

    off(type, cb) {
      const had = this._listens(type);
      super.off(type, cb);
      if (had && !this._listens(type) && GATED_EVENTS.has(type)) this._post({ op: 'listen', type, on: false });
    }

    _onEvent(ev) {
      switch (ev.ev) {
        case 'emit':
          if (ev.type === 'frame') {
            Object.assign(this.stats, ev.args[0]);
            this._emit('frame', this.stats);
          } else {
            this._emit(ev.type, ...ev.args);
          }
          break;
        case 'renderer':
          this.renderer = ev.renderer;
          this._emit('renderer', ev.renderer, ev.reason);
          break;
        case 'cursor':
          this._remoteCursor = ev.c;
          if (this.canvas) this._updateCursor();
          break;
        case 'hits':
          this._hits = ev.regions;
          this._hostOf()._hitStat(ev);
          break;
        case 'contentSize': this._onContentSize(ev.w, ev.h); break;
        case 'needCanvas': this._replaceCanvas(); break;
        case 'miss': this._hostOf()._missStat(this, ev); break;
      }
    }

    // ------------------------------------------------------------- API

    /** The last rendered frame as premultiplied RGBA: a Promise in worker mode. */
    readPixels() { return this._call({ op: 'read' }, null, null); }
    readPixelsAsync() { return this.readPixels(); }

    setProfiling(on) { this._post({ op: 'profiling', on: !!on }); }

    /** jsui_get_perf (see CanvasView.getPerf): a Promise in worker mode. */
    getPerf(reset = true) { return this._call({ op: 'perf', reset: !!reset }, null, null); }

    // Test hooks in the worker (web/test/browser.test.mjs): 'spy' records the
    // jsui_send_* calls, 'calls' takes them; 'loseGL' / 'loseGLForGood' /
    // 'destroyDevice' lose the view's GPU context.
    _debug(what, arg) { return this._call({ op: 'debug', what, arg }, null, null); }

    // web/test/frame-bench.mjs: the worker records its next warmup + n
    // frames (rAF times on the page's clock, frame() CPU ms and its parts:
    // input, core (tick + paint), after (hit map, page events), post (the
    // frame's postMessage); repaints).
    _bench({ n, warmup = 0 }) { return this._call({ op: 'bench', n, warmup }, null, null); }
  }

  class WorkerHost extends ProxyView {
    constructor(worker, canvas, opts) {
      super(canvas, opts);
      this.worker = worker;
      this.hostId = WorkerHost.nextId++;
      this.rootTag = 1;
      this._views = new Map([[0, this]]);
      this._nextView = 1;
      this._surfaces = new Set();
      this._handlers = new Map();
      this._replies = new Map();
      this._nextReply = 1;
      this._out = [];
      this._outTransfer = [];  // per op of _out: its transferables (or null)
      this._flushQueued = false;
      this._live = false;      // events wait in _backlog until createJsui's caller could subscribe
      this._backlog = [];
      this._lastError = null;
      this._backend = null;
      this._maps = new Map();      // mapBuffer arrays
      this._texSeq = new Map();    // name -> setTexture / video frame sequence (newest wins)
      this._bindings = new Map();  // bindVideo
      this._texSources = new Map();  // name -> { source, options } setTexture can capture again (_texturesReset)
      this._tainted = new WeakMap();  // source -> taintKey() when the browser refused it (SecurityError)
      this._padsUsed = [];
      this._padView = null;
      this._warned = new Set();
      /** How old the page's copy of the hit map gets (worker mode): `latency`
       * is ms from the start of the frame whose layout it shows until it is
       * here, `transit` the postMessage part of that. */
      this.hitMapStats = { updates: 0, regions: 0, lookups: 0, misses: 0, latency: newSeries(), transit: newSeries() };
      this.audio = new I.JsuiAudio(this, opts.audio);
      this.audio.sink = (list) => this._send({ op: 'audioReports', list });
      worker.onmessage = (e) => this._onMessage(e.data);
      worker.onerror = (e) => {
        e.preventDefault?.();
        const msg = `worker error: ${e.message || 'unknown'}`;
        if (this._starting) this._starting.reject(new Error(msg));
        else this._log(3, msg);
      };
      this._measure();
    }

    // Hands the canvas over, starts the runtime in the worker; resolves when it runs.
    _start() {
      const off = this.canvas ? this._transfer() : null;
      const p = new Promise((resolve, reject) => { this._starting = { resolve, reject }; });
      const opts = workerOpts(this.opts);
      opts.audio = this.audio.enabled;
      this.worker.postMessage({ t: 'init', opts, canvas: off, size: this._size(), timeOrigin: performance.timeOrigin },
        off ? [off] : []);
      return p.then((r) => {
        this._starting = null;
        this.renderer = r.renderer;
        this.jsEngine = r.jsEngine;
        this._backend = r.backend;
        this._lastError = r.lastError ?? null;
        if (this.canvas) this._installDom();
        if (this.opts.gamepads !== false) this._installGamepads();
        // Events the worker sent so far reach the listeners the caller adds
        // right after createJsui() resolves.
        setTimeout(() => {
          this._live = true;
          const b = this._backlog;
          this._backlog = [];
          b.forEach((ev) => this._dispatch(ev));
        }, 0);
        return this;
      }, (e) => {
        this._starting = null;
        throw e;
      });
    }

    // Startup failed: undo everything (the caller falls back to main mode).
    _abandon() {
      this._destroyed = true;
      this._destroyDom();
      this.worker.terminate();
    }

    _send(op, transfer) {
      if (this._dead) {
        for (const t of transfer ?? []) t?.close?.();
        return;
      }
      const q = this._out;
      // Coalesce: consecutive moves / wheels / touch moves of a view become one.
      if (op.op === 'move' || op.op === 'wheel' || (op.op === 'touch' && op.phase === 1)) {
        const last = q[q.length - 1];
        if (last && last.op === op.op && last.v === op.v && (op.op !== 'touch' || (last.phase === 1 && last.id === op.id))) {
          if (op.op === 'wheel') {
            op.dx += last.dx;
            op.dy += last.dy;
          }
          if (op.t === undefined) op.t = last.t;
          q[q.length - 1] = op;
          return;
        }
      }
      q.push(op);
      this._outTransfer.push(transfer?.length ? transfer.filter(Boolean) : null);
      if (!this._flushQueued) {
        this._flushQueued = true;
        queueMicrotask(() => this._flush());
      }
    }

    _flush() {
      this._flushQueued = false;
      if (!this._out.length || this._dead) return;
      const list = this._out, transfers = this._outTransfer;
      this._out = [];
      this._outTransfer = [];
      try {
        this.worker.postMessage({ t: 'ops', list }, transfers.flat().filter(Boolean));
      } catch {
        // One op could not be cloned (an ImageBitmap of a cross-origin <img>
        // without CORS cannot be sent): one by one, so only that op fails.
        list.forEach((op, i) => {
          try {
            this.worker.postMessage({ t: 'ops', list: [op] }, transfers[i] ?? []);
          } catch (e) {
            this._unsent(op, transfers[i] ?? [], e);
          }
        });
      }
    }

    // An op postMessage refused: its reply settles, a bound video's frame
    // slot is freed.
    _unsent(op, transfer, e) {
      // A picture that cannot be cloned has no origin-clean flag (a detached one has no size).
      const tainted = op.op === 'texture' && e?.name === 'DataCloneError' && !!sourceSize(op.source);
      for (const t of transfer) t?.close?.();
      if (tainted) this._warnOnce(corsWarning(op.name));
      else console.error('[jsui] worker postMessage failed:', e);
      if (op.ack) this._bindings.get(op.name)?.ack();
      const r = op.id && this._replies.get(op.id);
      if (!r) return;
      this._replies.delete(op.id);
      r.resolve(tainted ? 'tainted' : r.dflt);
    }

    // An op the worker answers: a Promise of its answer (`dflt` once destroyed).
    _request(op, transfer, dflt) {
      if (this._destroyed) {
        for (const t of transfer ?? []) t?.close?.();
        return Promise.resolve(dflt);
      }
      const id = this._nextReply++;
      op.id = id;
      return new Promise((resolve, reject) => {
        this._replies.set(id, { resolve, reject, dflt });
        this._send(op, transfer);
      });
    }

    _onMessage(m) {
      if (m?.t !== 'evs') return;
      for (const ev of m.list) {
        try {
          if (this._live || ev.ev === 'reply' || ev.ev === 'ready' || ev.ev === 'initError') this._dispatch(ev);
          else this._backlog.push(ev);
        } catch (e) {
          console.error(e);
        }
      }
    }

    _dispatch(ev) {
      switch (ev.ev) {
        case 'reply': {
          const r = this._replies.get(ev.id);
          if (!r) return;
          this._replies.delete(ev.id);
          if (ev.ok) r.resolve(ev.value);
          else {
            const e = new Error(ev.error?.message ?? 'failed');
            if (ev.error?.name) e.name = ev.error.name;
            r.reject(e);
          }
          return;
        }
        case 'ready': this._starting?.resolve(ev); return;
        case 'initError': this._starting?.reject(new Error(ev.message)); return;
        case 'call': this._onCall(ev.id, ev.name, ev.json); return;
        case 'lastError': this._lastError = ev.msg; return;
        case 'backend': this._backend = ev.value; return;
        case 'padsUsed': this._padsUsed = ev.pads; return;
        case 'texAck': this._bindings.get(ev.name)?.ack(); return;
        case 'audio': this.audio.command(ev.cmd); return;
        case 'texReset': this._texturesReset(); return;
        case 'glLost': {
          // A WebGL context in the worker was lost (see JsuiHost._contextLost).
          const switched = videoFramesContextLost();
          if (switched) this._log(1, switched);
          return;
        }
        case 'started': this._armWatchdog(); return;
        case 'raf':
          this._sawRaf = true;
          clearTimeout(this._watchdog);
          return;
        case 'ticks': this._mainTicks(ev.on); return;
        case 'stats':
          for (const [v, st] of ev.stats) {
            const view = this._views.get(v);
            if (view) Object.assign(view.stats, st);
          }
          return;
        default: this._views.get(ev.v ?? 0)?._onEvent(ev);
      }
    }

    // ------------------------------------------- frame loop (the worker's)

    start() { if (!this._destroyed) this._send({ op: 'start' }); }
    stop() { this._send({ op: 'stop' }); }
    /** Runs one frame in the worker at `now` (page clock). */
    frame(now = performance.now()) { this._send({ op: 'frame', t: now }); }

    // A worker without requestAnimationFrame (or whose rAF never fires):
    // this thread's rAF posts each frame.
    _armWatchdog() {
      if (this._sawRaf) return;
      clearTimeout(this._watchdog);
      this._watchdog = setTimeout(() => {
        if (this._sawRaf || this._destroyed) return;
        // Hidden pages get no rAF anywhere: ask again once visible.
        if (typeof document !== 'undefined' && document.visibilityState === 'hidden') this._armWatchdog();
        else this._send({ op: 'useTicks' });
      }, RAF_WATCHDOG_MS);
    }

    _mainTicks(on) {
      this._ticks = on;
      if (!on) {
        if (this._tickRaf) cancelAnimationFrame(this._tickRaf);
        this._tickRaf = 0;
        return;
      }
      if (this._tickRaf) return;
      const step = (t) => {
        this._tickRaf = this._ticks && !this._destroyed ? requestAnimationFrame(step) : 0;
        if (this._tickRaf) this._send({ op: 'tick', t });
      };
      this._tickRaf = requestAnimationFrame(step);
    }

    // ------------------------------------------------------- public API

    postMessage(name, data) {
      this._send({ op: 'post', name: String(name), json: JSON.stringify(data === undefined ? null : data) });
    }

    /** Promise<boolean>: whether the value changed. */
    setState(key, value) {
      return this._request({ op: 'setState', key: String(key), json: value === undefined ? null : JSON.stringify(value) },
        null, false);
    }

    /** Promise<boolean>: whether the value changed. */
    patchState(key, patch) {
      return this._request({ op: 'patchState', key: String(key), json: JSON.stringify(patch === undefined ? null : patch) },
        null, false);
    }

    /** Promise of the key's value (undefined when unset). */
    getState(key) {
      return this._request({ op: 'getState', key: String(key) }, null, null).then((j) => (j == null ? undefined : JSON.parse(j)));
    }

    /** Copies `data` to the worker; Promise<boolean>: whether the contents changed. */
    setBuffer(key, data) {
      if (data == null) return this.removeBuffer(key);
      key = String(key);
      const type = bufferType(data);
      const copy = data.slice();
      // What mapBuffer(key) returns next holds these values, as in main mode.
      const m = this._maps.get(key);
      if (m && m.constructor === data.constructor && m.length === data.length) m.set(data);
      else this._maps.set(key, data.slice());
      return this._request({ op: 'setBuffer', key, type, data: copy }, [copy.buffer], false);
    }

    /** A typed array of this thread to fill, then commitBuffer(key) (which
     * copies it to the worker). The same array comes back for the same key,
     * type and count, holding what was written last. */
    mapBuffer(key, Type, count) {
      bufferType(Type);
      key = String(key);
      let m = this._maps.get(key);
      if (!m || m.constructor !== Type || m.length !== count) {
        m = new Type(count);
        this._maps.set(key, m);
      }
      return m;
    }

    /** Promise<boolean>: whether the contents changed. */
    commitBuffer(key) {
      key = String(key);
      const m = this._maps.get(key);
      if (!m) return Promise.resolve(false);
      const copy = m.slice();
      return this._request({ op: 'commitBuffer', key, type: bufferType(m), data: copy }, [copy.buffer], false);
    }

    /** Promise<boolean>: whether it existed. */
    removeBuffer(key) {
      key = String(key);
      this._maps.delete(key);
      return this._request({ op: 'removeBuffer', key }, null, false);
    }

    /** Host texture (see JsuiHost.setTexture): the source's picture goes to
     * the worker as a VideoFrame (a clone of a VideoFrame; a <video>'s
     * current frame), an ImageBitmap (<img>, canvases), an ImageData or raw
     * pixels (copied; `options.transfer`: transferred instead, the caller's
     * object is unusable afterwards). Promise<boolean>: uploaded. */
    setTexture(name, source, options) {
      if (this._destroyed) return Promise.resolve(false);
      name = String(name);
      if (source == null) return this.removeTexture(name);
      if (!sourceSize(source)) return Promise.resolve(false);
      // Cross-origin without CORS: not tried again until its URL changes (as in main mode).
      if (this._tainted.get(source) === taintKey(source)) return Promise.resolve(false);
      const opts = { ...(options ?? {}) };
      const transfer = !!opts.transfer;
      delete opts.transfer;
      if (opts.opaque === undefined) opts.opaque = defaultOpaque(source);
      const seq = this._nextSeq(name);
      this._keepSource(name, source, options);
      const taint = (e) => {
        if (e?.name === 'SecurityError') this._tainted.set(source, taintKey(source));
        this._warnOnce(textureWarning(name, e));
        return false;
      };
      const send = (src, list) => {
        if (this._texSeq.get(name) !== seq || this._destroyed) {
          for (const t of list) t?.close?.();
          return false;
        }
        // 'tainted': the worker's upload threw SecurityError (and logged it).
        return this._request({ op: 'texture', name, source: src, options: opts }, list, false).then((ok) => {
          if (ok !== 'tainted') return ok;
          this._tainted.set(source, taintKey(source));
          return false;
        });
      };
      let r;
      try {
        r = this._textureSource(name, source, transfer);
      } catch (e) {
        return Promise.resolve(taint(e));
      }
      if (!r) return Promise.resolve(false);
      if (r instanceof Promise) return r.then((x) => (x ? send(x.src, x.transfer) : false), taint);
      return send(r.src, r.transfer);
    }

    // What the page captures again once the worker placed its textures anew
    // after a GPU loss (_texturesReset). The worker re-uploads its own copies
    // first, but an ImageBitmap made before WebKit lost its GPU process
    // uploads blank after the restore (texImage2D succeeds, the pixels are
    // zero; a bitmap made afterwards is fine). So <img>, canvases and <video>
    // are captured again here. ImageData and raw pixels are not: the worker
    // holds them in CPU memory. VideoFrames are dropped on such a change, as
    // in main mode.
    _keepSource(name, source, options) {
      if (isVideoFrame(source) || (typeof ImageData !== 'undefined' && source instanceof ImageData) ||
        ArrayBuffer.isView(source.data)) {
        this._texSources.delete(name);
      } else if (typeof ImageBitmap !== 'undefined' && source instanceof ImageBitmap) {
        this._texSources.set(name, { bitmap: true });  // the caller's: not ours to capture again
      } else {
        this._texSources.set(name, { source, options });
      }
    }

    // The worker placed its textures again after a GPU loss (see _keepSource).
    _texturesReset() {
      if (this._destroyed) return;
      for (const [name, k] of [...this._texSources]) {
        if (!k.bitmap) this.setTexture(name, k.source, k.options);
        else if (!this._warned.has(`reset ${name}`)) {
          this._warned.add(`reset ${name}`);
          this._log(1, `host texture "${name}": the GPU was reset, and an ImageBitmap source may come back blank ` +
            '(WebKit): set it again');
        }
      }
      for (const b of this._bindings.values()) b.push();  // a paused video's frame too
    }

    _nextSeq(name) {
      const seq = (this._texSeq.get(name) ?? 0) + 1;
      this._texSeq.set(name, seq);
      return seq;
    }

    // { src, transfer } (or a Promise of it) for what goes to the worker.
    _textureSource(name, s, transfer) {
      if (isVideoFrame(s)) {
        const f = transfer ? s : s.clone();
        return { src: f, transfer: [f] };
      }
      if (typeof ImageBitmap !== 'undefined' && s instanceof ImageBitmap) return { src: s, transfer: transfer ? [s] : [] };
      if (typeof ImageData !== 'undefined' && s instanceof ImageData) return { src: s, transfer: transfer ? [s.data.buffer] : [] };
      if (ArrayBuffer.isView(s.data)) {
        const data = transfer ? s.data : s.data.slice();
        return { src: { data, width: s.width, height: s.height }, transfer: [data.buffer] };
      }
      if ('videoWidth' in s && typeof VideoFrame === 'function') {
        const f = new VideoFrame(s);
        return { src: f, transfer: [f] };
      }
      // <img>, <canvas>, OffscreenCanvas, SVG images, ...: a snapshot.
      return createImageBitmap(s).then((b) => ({ src: b, transfer: [b] }));
    }

    /** Promise<boolean>: whether it existed. Stops a bindVideo() of it. */
    removeTexture(name) {
      if (this._destroyed) return Promise.resolve(false);
      name = String(name);
      this._bindings.get(name)?.unbind();
      this._texSources.delete(name);
      this._nextSeq(name);  // drops conversions still under way
      return this._request({ op: 'removeTexture', name }, null, false);
    }

    /** Keeps host texture `name` showing `video`'s current frame: captured
     * here per presented frame (requestVideoFrameCallback, else polled per
     * display frame) as a VideoFrame and transferred; at most two in flight
     * (a busy worker skips frames). Returns a function that stops it. */
    bindVideo(name, video, options) {
      if (this._destroyed) throw new Error('bindVideo: runtime destroyed');
      name = String(name);
      this._bindings.get(name)?.unbind();
      this._texSources.delete(name);  // the binding sends its frames (and again after a GPU loss)
      const opts = { ...(options ?? {}) };
      if (opts.opaque === undefined) opts.opaque = defaultOpaque(video);
      const b = { inflight: 0, again: false, stopped: false, tainted: null };
      const abort = new AbortController();
      const push = () => {
        if (b.stopped || this._destroyed || !(video.readyState >= 2)) return;
        // Cross-origin without CORS: not captured again until its URL changes.
        if (b.tainted === taintKey(video)) return;
        if (b.inflight >= MAX_VIDEO_FRAMES) {
          b.again = true;  // sent when the worker is done with one
          return;
        }
        const seq = this._nextSeq(name);
        let frame;
        try {
          if (typeof VideoFrame === 'function') frame = new VideoFrame(video);
        } catch (e) {
          if (e?.name === 'SecurityError') b.tainted = taintKey(video);
          this._warnOnce(textureWarning(name, e));
          return;
        }
        b.inflight++;
        const send = (src) => {
          if (b.stopped || this._texSeq.get(name) !== seq || this._destroyed) {
            src?.close?.();
            b.inflight--;
            return;
          }
          this._send({ op: 'texture', name, source: src, options: opts, ack: true }, [src]);
        };
        if (frame) send(frame);
        else {
          createImageBitmap(video).then(send, (e) => {
            b.inflight--;
            if (e?.name === 'SecurityError') b.tainted = taintKey(video);
            this._warnOnce(textureWarning(name, e));
          });
        }
      };
      b.push = push;
      b.ack = () => {
        b.inflight = Math.max(0, b.inflight - 1);
        if (b.again) {
          b.again = false;
          push();
        }
      };
      // Frame callbacks, or polling once per display frame (web/jsui-textures.js).
      const stopFrames = watchVideoFrames(video, push);
      // A paused video presents frames for these too (first frame, seeks, a new src).
      for (const type of ['loadeddata', 'seeked', 'resize']) video.addEventListener(type, push, { signal: abort.signal });
      if (video.readyState >= 2) push();
      b.unbind = () => {
        if (b.stopped) return;
        b.stopped = true;
        stopFrames();
        abort.abort();
        if (this._bindings.get(name) === b) this._bindings.delete(name);
      };
      this._bindings.set(name, b);
      return b.unbind;
    }

    /** Where host textures go (mirrored from the worker): 'webgpu', 'gl', 'pixels' or null. */
    get textureBackend() { return this._backend; }

    _warnOnce(text) {
      if (this._warned.has(text)) return;
      this._warned.add(text);
      this._log(2, text);
    }

    handle(name, fn) {
      this._handlers.set(String(name), fn);
      return () => { if (this._handlers.get(String(name)) === fn) this._handlers.delete(String(name)); };
    }

    /** Promise of jsui_resolve_call's result (0, or -1: unknown call). */
    resolveCall(id, value) {
      if (this._destroyed) return Promise.resolve(-1);
      return this._request({ op: 'resolveCall', callId: id, json: JSON.stringify(value === undefined ? null : value) },
        null, -1);
    }

    rejectCall(id, message) {
      if (this._destroyed) return Promise.resolve(-1);
      return this._request({ op: 'rejectCall', callId: id, message: String(message ?? 'rejected') }, null, -1);
    }

    callUi(name, args) {
      const json = JSON.stringify(args === undefined ? null : args);
      return this._request({ op: 'callUi', name: String(name), json }).then((j) => JSON.parse(j));
    }

    _onCall(id, name, json) {
      let args;
      try { args = JSON.parse(json); } catch { args = undefined; }
      const fn = this._handlers.get(name);
      if (!fn) {
        if (!this._emit('call', id, name, args)) this.rejectCall(id, `no host handler for "${name}"`);
        return;
      }
      Promise.resolve().then(() => fn(args)).then(
        (v) => this.resolveCall(id, v),
        (e) => this.rejectCall(id, e && e.message !== undefined ? e.message : String(e)));
    }

    loadUrl(url) {
      this._send({ op: 'loadUrl', url: new URL(url, location.href).href });
    }

    /** Promise<boolean>. */
    loadSource(source, name = 'bundle.js') {
      return this._request({ op: 'loadSource', source: String(source), name: String(name) }, null, false);
    }

    reload() { this._send({ op: 'reload' }); }

    devUpdate() { this._send({ op: 'devUpdate' }); }

    /** Copies the bytes to the worker (the copy is transferred); Promise<boolean>. */
    registerFont(family, bytes, weight, italic) {
      if (this._destroyed) return Promise.resolve(false);
      [family, bytes, weight, italic] = fontArgs(family, bytes, weight, italic);
      const data = (ArrayBuffer.isView(bytes) ? new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength)
        : new Uint8Array(bytes)).slice();
      return this._request({ op: 'registerFont', family, bytes: data, weight, italic }, [data.buffer], false);
    }

    /** The worker fetches the font (`url` resolved against the page); Promise<boolean>. */
    loadFont(family, url, weight, italic) {
      if (this._destroyed) return Promise.resolve(false);
      [family, , weight, italic] = fontArgs(family ?? '', null, weight, italic);
      return this._request({ op: 'loadFont', family, url: new URL(url, location.href).href, weight, italic },
        null, false);
    }

    /** Promise<boolean>: whether the family was registered. */
    unregisterFont(family) {
      if (this._destroyed) return Promise.resolve(false);
      return this._request({ op: 'unregisterFont', family: String(family) }, null, false);
    }

    setFontFallbacks(families) { this._send({ op: 'fontFallbacks', list: fontList(families) }); }

    setDefaultFontFamily(family) {
      this._send({ op: 'defaultFontFamily', family: family == null ? null : String(family) });
    }

    get lastError() { return this._lastError; }

    whenIdle() { return this._request({ op: 'idle' }, null, undefined).then(() => undefined); }

    // ------------------------------------------------------- surfaces

    createSurface(target, options) {
      if (this._destroyed) throw new Error('createSurface: runtime destroyed');
      const s = new WorkerSurface(this, target, options ?? {});
      this._surfaces.add(s);
      return s;
    }

    get surfaces() { return Array.from(this._surfaces); }

    /** Promise<boolean>: false for props that are not a JSON object. */
    run(component, props) {
      return this._call({ op: 'run', component: component == null ? null : String(component),
        props: JSON.stringify(props ?? {}) }, null, false);
    }

    setProps(props) { return this.run(null, props); }

    _surfaceDestroyed(s) {
      this._surfaces.delete(s);
      this._views.delete(s.v);
      if (this._padView === s) this._padView = null;
    }

    // -------------------------------------------------------- gamepads

    // Polled on this thread (the Gamepad API is not in workers), only while
    // a pad is connected; a changed snapshot goes to the worker, which sends
    // it into the runtime at its next frame.
    _installGamepads() {
      if (typeof navigator === 'undefined' || !navigator.getGamepads) return;
      const signal = this._abort.signal;
      const wake = () => this._padLoop();
      addEventListener('gamepadconnected', wake, { signal });
      addEventListener('gamepaddisconnected', wake, { signal });
      if (Array.from(navigator.getGamepads()).some(Boolean)) wake();
    }

    _padLoop() {
      if (this._padRaf || this._destroyed) return;
      const step = () => {
        this._padRaf = 0;
        if (this._destroyed) return;
        const snap = Array.from(navigator.getGamepads(), (p) => (p ? {
          id: p.id, connected: p.connected, axes: Array.from(p.axes),
          buttons: Array.from(p.buttons, (b) => ({ pressed: b.pressed, value: b.value })),
        } : null));
        const key = JSON.stringify(snap);
        if (key !== this._padKey) {
          this._padKey = key;
          this._send({ op: 'pads', pads: snap });
        }
        if (snap.some(Boolean)) this._padRaf = requestAnimationFrame(step);
      };
      this._padRaf = requestAnimationFrame(step);
    }

    _focusPads(view) {
      if ((this._padView ?? this) === view) return;
      this._padView = view === this ? null : view;
      this._send({ op: 'padView', v: view.v });
    }

    _releasePads() { this._send({ op: 'releasePads' }); }

    /** Whether the UI consumed the current press of gamepad `pad`'s `button`
     * (as the worker last reported it: up to a frame behind). */
    // The worker reports what the UI took from the pads from the first call on.
    gamepadConsumed(pad, button) {
      this._wantPadsUsed();
      return !!this._padsUsed[pad]?.used[button];
    }

    gamepadAxisConsumed(pad, axis) {
      this._wantPadsUsed();
      return !!this._padsUsed[pad]?.axisUsed[axis];
    }

    _wantPadsUsed() {
      if (this._padsWanted || this._destroyed) return;
      this._padsWanted = true;
      this._send({ op: 'padsUsed' });
    }

    // ---------------------------------------------------- hit map stats

    _hitStat(ev) {
      const st = this.hitMapStats, now = performance.now();
      st.updates++;
      st.regions = ev.regions.length / 5;
      addSample(st.latency, now - ev.t);
      addSample(st.transit, now - ev.sent);
    }

    _missStat(view, ev) {
      this.hitMapStats.misses++;
      if (ev.kind === 'key') view._keyUsed.set(ev.combo, ev.used);
    }

    // --------------------------------------------------------- destroy

    destroy() {
      if (this._destroyed) return;
      for (const b of [...this._bindings.values()]) b.unbind();
      this._texSources.clear();
      for (const s of this._surfaces) s._destroyLocal();
      this._surfaces.clear();
      this._mainTicks(false);
      if (this._padRaf) cancelAnimationFrame(this._padRaf);
      clearTimeout(this._watchdog);
      this._destroyDom();
      this.audio.destroy();
      const replies = [...this._replies.values()];
      this._replies.clear();
      // The worker frees the runtime, then goes; terminated anyway after 2 s.
      const id = this._nextReply++;
      this._replies.set(id, { resolve: () => this._kill(), reject: () => this._kill() });
      this._send({ op: 'destroy', id });
      this._flush();
      this._killTimer = setTimeout(() => this._kill(), 2000);
      this._destroyed = true;
      for (const r of replies) r.resolve(r.dflt);
    }

    _kill() {
      clearTimeout(this._killTimer);
      if (this._dead) return;
      this._dead = true;
      this.worker.terminate();
    }
  }
  WorkerHost.nextId = 1;

  class WorkerSurface extends ProxyView {
    constructor(host, target, opts) {
      if (!opts.component) throw new Error('createSurface: component is required');
      if (!target) throw new Error('createSurface: a canvas or element is required');
      let canvas = target, owned = false;
      if (target.tagName !== 'CANVAS') {
        canvas = document.createElement('canvas');
        canvas.style.display = 'block';
        if (!opts.autoSize) Object.assign(canvas.style, { width: '100%', height: '100%' });
        target.appendChild(canvas);
        owned = true;
      } else if (I.transferred.has(canvas)) {
        canvas = I.replaceCanvas(canvas);
      }
      super(canvas, { ...opts, autoFocus: opts.autoFocus === true, trapTab: opts.trapTab ?? false,
        pixelRatio: opts.pixelRatio ?? host.opts.pixelRatio });
      this.host = host;
      this.v = host._nextView++;
      this._ownsCanvas = owned;
      this.component = String(opts.component);
      this._autoW = opts.autoSize === true || opts.autoSize === 'width';
      this._autoH = opts.autoSize === true || opts.autoSize === 'height';
      if (opts.width > 0 && !this._autoW) canvas.style.width = `${opts.width}px`;
      if (opts.height > 0 && !this._autoH) canvas.style.height = `${opts.height}px`;
      this._measure();
      host._views.set(this.v, this);
      /** The surface's root tag: undefined until the worker made it (`ready`). */
      this.rootTag = undefined;
      const wopts = { component: this.component, initialProps: JSON.parse(JSON.stringify(opts.initialProps ?? {})),
        autoSize: opts.autoSize, renderer: opts.renderer, pixelRatio: this.opts.pixelRatio };
      const off = this._transfer();
      /** Resolves with the surface once the worker created it; rejects when that failed. */
      this.ready = this._call({ op: 'surface', canvas: off, opts: wopts, size: this._size() }, [off]).then((r) => {
        if (!r) return this;  // destroyed meanwhile
        this.rootTag = r.rootTag;
        this.renderer = r.renderer;
        return this;
      }, (e) => {
        this._destroyLocal();
        host._surfaceDestroyed(this);
        throw e;
      });
      this.ready.catch(() => {});  // also reported by the worker's log
      this._installDom();
    }

    _hostOf() { return this.host; }

    _log(level, text) { this.host._log(level, `${this.component} (root ${this.rootTag ?? '?'}): ${text}`); }

    /** Promise<boolean>: false for props that are not a JSON object. */
    setProps(props) { return this.run(null, props); }

    run(component, props) {
      if (this._destroyed) return Promise.resolve(false);
      return this._call({ op: 'run', component: component == null ? null : String(component),
        props: JSON.stringify(props ?? {}) }, null, false).then((ok) => {
        if (ok && component != null) this.component = String(component);
        return ok;
      });
    }

    destroy() {
      if (this._destroyed) return;
      this._post({ op: 'destroySurface' });
      this._destroyLocal();
      this.host._surfaceDestroyed(this);
    }

    _destroyLocal() {
      if (this._destroyed) return;
      this._destroyed = true;
      this._destroyDom();
      if (this._ownsCanvas) this.canvas.remove();
    }
  }

  return { ProxyView, WorkerHost, WorkerSurface };
}

// Latency samples: count, mean, max and the last 512 for percentiles.
function newSeries() { return { n: 0, mean: 0, max: 0, samples: [] }; }
function addSample(s, v) {
  s.n++;
  s.mean += (v - s.mean) / s.n;
  if (v > s.max) s.max = v;
  if (s.samples.length >= 512) s.samples.shift();
  s.samples.push(v);
}

// =================================================================== worker

/**
 * The worker's side (called by jsui-web.js when it runs as the worker's
 * entry): builds the runtime on the 'init' message and applies the page's
 * ops. `early`: messages that arrived before this file was loaded.
 */
export function serve(I, early) {
  const { JsuiHost, JsuiSurface } = I;
  let host = null, pending = [], OFF = 0;
  const views = new Map();
  const out = { list: [], transfer: [], queued: false };
  const post = (ev, transfer) => {
    out.list.push(ev);
    if (transfer) for (const t of transfer) out.transfer.push(t);
    if (!out.queued) {
      out.queued = true;
      queueMicrotask(flush);
    }
  };
  const flush = () => {
    out.queued = false;
    if (!out.list.length) return;
    const list = out.list, transfer = out.transfer;
    out.list = [];
    out.transfer = [];
    const b = host?._bench, t0 = b ? performance.now() : 0;
    globalThis.postMessage({ t: 'evs', list }, transfer);
    if (b) b.postMs += performance.now() - t0;
  };
  const reply = (id, ok, value, transfer) => {
    if (!id) return;
    if (ok) post({ ev: 'reply', id, ok: true, value }, transfer);
    else post({ ev: 'reply', id, ok: false, error: { name: value?.name, message: msgOf(value) } });
  };
  const now = () => performance.now() + OFF;  // the page's clock

  // Input the page coalesced is applied at the start of the next frame (moves
  // and wheels of one frame become one), or before any other op. A few ops
  // at most: a short array, no keys built per event.
  let pendIn = [];
  const queueIn = (op) => {
    for (let i = pendIn.length - 1; i >= 0; i--) {
      const prev = pendIn[i];
      if (prev.op !== op.op || prev.v !== op.v || prev.id !== op.id) continue;
      if (op.op === 'wheel') {
        op.dx += prev.dx;
        op.dy += prev.dy;
      }
      if (op.t === undefined) op.t = prev.t;
      pendIn.splice(i, 1);  // the merged one goes last: arrival order
      break;
    }
    pendIn.push(op);
    if (host && !host._raf) queueMicrotask(flushIn);
  };
  const flushIn = () => {
    if (!pendIn.length) return;
    const list = pendIn;
    pendIn = [];
    for (const op of list) applyInput(op);
  };

  const applyInput = (op) => {
    const view = views.get(op.v);
    if (!view || view._destroyed || !view.rt) return;
    if (op.t !== undefined) view._inTime(op.t);
    let used;
    switch (op.op) {
      case 'button': used = view._inButton(op.x, op.y, op.b, op.down, op.mods); if (!op.down) return; break;
      case 'move': view._inMove(op.x, op.y); return;
      case 'touch': used = view._inTouch(op.id, op.phase, op.x, op.y); if (op.phase !== 0) return; break;
      case 'wheel': used = view._inWheel(op.x, op.y, op.dx, op.dy); break;
      case 'leave': view._inLeave(); return;
      case 'key':
        used = view._inKey(op.key, op.code, op.down, op.repeat, op.mods);
        if (!op.down || op.repeat) return;
        if (!!used !== !!op.p) post({ ev: 'miss', v: op.v, kind: 'key', combo: keyCombo(op.key, op.mods), used: +!!used });
        return;
      case 'text': view._inText(op.text); return;
    }
    if (op.p !== undefined && !!used !== !!op.p) post({ ev: 'miss', v: op.v, kind: op.op, x: op.x, y: op.y, used: +!!used });
  };

  // The worker-side overrides shared by the main surface and the others.
  const remote = (Base) => class extends Base {
    get v() { return this.opts._v ?? 0; }
    _sz() { return this._sizeNow ?? this.opts._size ?? {}; }
    _installDom() {}  // the page's thread has the DOM
    focus() {}
    _listens(type) { return !!this._remoteListens?.has(type); }
    _emit(type, ...args) {
      if (type === 'renderer') {
        post({ ev: 'renderer', v: this.v, renderer: this.renderer, reason: args[1] ?? null });
        return true;
      }
      if (GATED_EVENTS.has(type) && !this._listens(type)) return false;
      if (type === 'frame') args = [{ ...args[0] }];
      post({ ev: 'emit', v: this.v, type, args });
      return true;
    }
    _updateCursor() {
      const c = this._cursorNow();
      if (c === this._cursor) return;
      this._cursor = c;
      post({ ev: 'cursor', v: this.v, c });
    }
    _ratio() { return this._fixedRatio || this._sz().ratio || 1; }
    _cssBox() {
      const m = this._sz();
      return [m.cssW ?? m.w ?? 1, m.cssH ?? m.h ?? 1];
    }
    _setBackingStore(w, h) {
      if (!this.canvas) return;
      this.canvas.width = w;
      this.canvas.height = h;
    }
    // The page measured the canvas: take its numbers (auto-sized axes follow
    // the content, which only the worker knows).
    _measure() {
      const m = this._sz();
      if (this._autoW || this._autoH || !m.pw) return super._measure();
      if (m.w === this.width && m.h === this.height && m.pw === this.pixelWidth && m.ph === this.pixelHeight) return false;
      this.width = m.w;
      this.height = m.h;
      this.pixelRatio = m.pr;
      this.pixelWidth = m.pw;
      this.pixelHeight = m.ph;
      this._setBackingStore(m.pw, m.ph);
      this._sizeChanged = true;
      return true;
    }
    _applySize(m) {
      this._sizeNow = m;
      if (this._measure() && this.rt) this._setViewport();
    }
    _contentSized(w, h) {
      if (this._measure()) this._setViewport();
      post({ ev: 'contentSize', v: this.v, w, h });
    }
    // A fresh canvas comes from the page (op 'canvas'); nothing paints meanwhile.
    _freshCanvas(then) {
      this._canvasWait = then;
      this.canvas = null;
      post({ ev: 'needCanvas', v: this.v });
    }
    _gotCanvas(c) {
      if (this._destroyed) return;
      this.canvas = c;
      this._setBackingStore(this.pixelWidth, this.pixelHeight);
      const then = this._canvasWait;
      this._canvasWait = null;
      then?.();
    }
    // Posts the hit map when the page needs it (op 'hits'), it may have
    // changed and did.
    _postHits(frameT) {
      if (!this._hitsOn || !this.rt || this._destroyed || !this.canvas) return;
      const st = this.stats;
      if (this._hitRenders === st.renders && !this._hitSized && ++this._hitAge < HIT_REFRESH_FRAMES) return;
      this._hitRenders = st.renders;
      this._hitSized = false;
      this._hitAge = 0;
      const M = this.module;
      if (!M._jsui_hit_regions) return;
      host._hitCalls = (host._hitCalls ?? 0) + 1;  // _debug('info'): tests check the gating
      let n = M._jsui_hit_regions(this.rt, this._hitBuf ?? 0, this._hitCap ?? 0);
      if (n > (this._hitCap ?? 0)) {
        if (this._hitBuf) M._free(this._hitBuf);
        this._hitCap = Math.max(64, Math.ceil(n * 1.5));
        this._hitBuf = M._malloc(this._hitCap * 20);
        n = Math.min(this._hitCap, M._jsui_hit_regions(this.rt, this._hitBuf, this._hitCap));
      }
      const cur = new Float32Array(M.HEAPU8.buffer, this._hitBuf, n * 5);
      const last = this._lastHits;
      if (last && last.length === cur.length && last.every((x, i) => x === cur[i])) return;
      this._lastHits = cur.slice();
      const regions = cur.slice();
      post({ ev: 'hits', v: this.v, t: frameT, sent: now(), regions }, [regions.buffer]);
    }
    _destroyView(...a) {
      if (this._hitBuf) this.module._free(this._hitBuf);
      this._hitBuf = 0;
      this._hitCap = 0;
      views.delete(this.v);
      super._destroyView(...a);
    }
  };

  class RemoteSurface extends remote(JsuiSurface) {}

  class RemoteHost extends remote(JsuiHost) {
    _makeSurface(target, options) { return new RemoteSurface(this, target, options); }

    // The page binds the videos (see JsuiHost._contextLost). The textures
    // tell the page once they are placed again (_texturesReset), also when
    // none has arrived yet: the page's may be on the way, made before the loss.
    _contextLost() {
      post({ ev: 'glLost' });
      if (!this._destroyed) this._textureSet()._reset = true;
    }

    // The textures were placed again after a GPU loss: the page captures
    // its sources again (WorkerHost._keepSource).
    _texturesReset() { post({ ev: 'texReset' }); }

    _onCall(id, name, json) { post({ ev: 'call', id, name, json }); }

    _checkError() {
      const p = this.module._jsui_last_error(this.rt);
      const msg = p ? this.module.UTF8ToString(p) : null;
      if (msg === this._lastError) return;
      this._lastError = msg;
      post({ ev: 'lastError', msg });
      if (msg) this._emit('error', msg);
    }

    _pollGamepads(pads = this._padSnap ?? []) {
      super._pollGamepads(pads);
      if (!this._padsWanted) return;  // nobody asked gamepadConsumed() yet
      const used = this._pads.map((s) => (s ? { used: Array.from(s.used), axisUsed: Array.from(s.axisUsed) } : null));
      const key = JSON.stringify(used);
      if (key !== this._padsUsedKey) {
        this._padsUsedKey = key;
        post({ ev: 'padsUsed', pads: used });
      }
    }

    // The worker's own rAF (else the page's, 'ticks'), times moved to the page's clock.
    start() {
      if (this._raf || this._destroyed) return;
      if (this._ticks || typeof requestAnimationFrame !== 'function') {
        this._ticks = true;
        this._raf = -1;
        post({ ev: 'ticks', on: true });
        return;
      }
      // Two callbacks per frame: the frame, then what it tells the page (hit
      // map, texture backend, replies). A microtask checkpoint runs between
      // them, which flushes the frame's WebGPU commands to the GPU process
      // before the OffscreenCanvas frame is committed after the second, so
      // they get a head start on the compositor (which waits for them).
      let first = true, ft = 0;
      const loop = (t) => {
        this._raf = requestAnimationFrame(loop);
        this._raf2 = requestAnimationFrame(after);
        if (first) {
          first = false;
          post({ ev: 'raf' });
        }
        ft = t + OFF;
        this._frameCore(ft);
      };
      const after = () => this._frameAfter(ft);
      this._raf = requestAnimationFrame(loop);
      this._raf2 = requestAnimationFrame(after);
      post({ ev: 'started' });
    }

    stop() {
      if (this._raf > 0) {
        cancelAnimationFrame(this._raf);
        cancelAnimationFrame(this._raf2);
      } else if (this._raf === -1) {
        post({ ev: 'ticks', on: false });
      }
      this._raf = this._raf2 = 0;
    }

    _useTicks() {
      const running = !!this._raf;
      this.stop();
      this._ticks = true;
      if (running) this.start();
    }

    /** One frame (the rAF loop splits it in two, see start()). */
    frame(t = now()) {
      this._frameCore(t);
      this._frameAfter(t);
    }

    // Input since the last frame, the tick and the paint (WebGPU: submitted).
    _frameCore(t) {
      if (this._destroyed) return;
      const b = this._bench;
      const t0 = b ? performance.now() : 0;
      flushIn();
      const t1 = b ? performance.now() : 0;
      super.frame(t);
      this._inFrame = this._raf > 0;  // until _frameAfter (not if the frame stopped the loop)
      if (b) {
        b.inMs = t1 - t0;
        b.coreMs = performance.now() - t1;
      }
    }

    // Runs `fn` once the current frame told the page everything (its hit
    // map included), or now between frames.
    _afterFrame(fn) {
      if (this._inFrame) (this._afterFns ??= []).push(fn);
      else fn();
    }

    // What the frame tells the page.
    _frameAfter(t) {
      this._inFrame = false;
      if (this._destroyed) return;
      const b = this._bench;
      const t0 = b ? performance.now() : 0;
      for (const v of views.values()) v._postHits(t);
      const backend = this.textureBackend;
      if (backend !== this._backend) {
        this._backend = backend;
        post({ ev: 'backend', value: backend });
      }
      if (this._afterFns) {
        const fns = this._afterFns;
        this._afterFns = null;
        for (const fn of fns) fn();
      }
      if (b) this._benchFrame(b, t, performance.now() - t0);
    }

    // web/test/frame-bench.mjs: records the frame (ms). A frame's events go
    // out after _frameAfter: the postMessage time since the last frame is
    // the previous one's.
    _benchFrame(b, t, afterMs) {
      if (b.stamps.length) b.parts[b.parts.length - 1].post = b.postMs;
      b.postMs = 0;
      b.stamps.push(t);
      b.cpu.push(b.inMs + b.coreMs + afterMs);
      b.parts.push({ input: b.inMs, core: b.coreMs, after: afterMs, post: 0 });
      if (b.stamps.length === b.warmup) b.r0 = this.stats.renders;
      if (b.stamps.length >= b.n + b.warmup) {
        this._bench = null;
        reply(b.id, true, { stamps: b.stamps.slice(b.warmup), cpu: b.cpu.slice(b.warmup),
          parts: b.parts.slice(b.warmup), renders: this.stats.renders - b.r0 });
      }
    }

    _statsOf() { return [...views.values()].map((v) => [v.v, { ...v.stats }]); }
  }

  // ------------------------------------------------------------ messages

  const init = async (m) => {
    OFF = performance.timeOrigin - m.timeOrigin;
    try {
      const opts = m.opts;
      const webgpu = await I.webgpuFor(opts);
      const M = await I.loadModuleFor(opts, webgpu);
      host = new RemoteHost(M, m.canvas, { ...opts, _webgpu: webgpu, _size: m.size, _v: 0, gamepads: false,
        autoStart: false, autoFocus: false,
        audio: opts.audio === false ? false : (cmd) => post({ ev: 'audio', cmd }) });
      if (opts.workerLoop === 'main') host._ticks = true;
    } catch (e) {
      post({ ev: 'initError', message: msgOf(e) });
      return;
    }
    views.set(0, host);
    host._backend = host.textureBackend;
    post({ ev: 'ready', renderer: host.renderer, backend: host._backend, lastError: host.lastError, jsEngine: host.jsEngine });
    const p = pending;
    pending = null;
    for (const list of p) apply(list);
    if (m.opts.autoStart !== false) host.start();
  };

  const apply = (list) => {
    for (const op of list) {
      try {
        handle(op);
      } catch (e) {
        if (op.id) reply(op.id, false, e);
        else console.error('[jsui] worker:', e);
      }
    }
  };

  const strings = (view, strs, fn) => view._withStrings(strs, fn);

  const handle = (op) => {
    if (!host || host._destroyed) {
      if (op.id) reply(op.id, true, null);
      return;
    }
    if (op.op === 'move' || op.op === 'wheel' || (op.op === 'touch' && op.phase === 1)) {
      queueIn(op);
      return;
    }
    flushIn();
    const view = views.get(op.v ?? 0);
    const M = host.module;
    switch (op.op) {
      case 'button': case 'touch': case 'leave': case 'key': case 'text': applyInput(op); return;
      case 'start': host.start(); return;
      case 'stop': host.stop(); return;
      case 'frame': case 'tick': host.frame(op.t); return;
      case 'useTicks': host._useTicks(); return;
      case 'size': view?._applySize(op.size); if (view) view._hitSized = true; return;
      case 'listen':
        if (!view) return;
        view._remoteListens ??= new Set();
        if (op.on) view._remoteListens.add(op.type);
        else view._remoteListens.delete(op.type);
        return;
      case 'hits':
        // The page needs the hit map (or no longer): computed from the next frame on.
        if (!view) return;
        view._hitsOn = !!op.on;
        view._hitSized = true;
        return;
      case 'padsUsed': host._padsWanted = true; host._padsUsedKey = null; return;
      case 'canvas': view?._gotCanvas(op.canvas); return;
      case 'padView': if (view) host._focusPads(view); return;
      case 'releasePads': host._releasePads(); return;
      case 'pads':
        host._padSnap = op.pads;
        host._padsActive = true;
        return;
      case 'post': host._withStrings([op.name, op.json], (n, j) => M._jsui_post_message(host.rt, n, j)); return;
      case 'setState':
        reply(op.id, true, strings(host, [op.key, op.json], (k, j) => M._jsui_set_state(host.rt, k, j)) === 1);
        return;
      case 'patchState':
        reply(op.id, true, strings(host, [op.key, op.json], (k, j) => M._jsui_patch_state(host.rt, k, j)) === 1);
        return;
      case 'getState': {
        const p = strings(host, [op.key], (k) => M._jsui_get_state(host.rt, k));
        reply(op.id, true, p ? M.UTF8ToString(p) : null);
        return;
      }
      case 'setBuffer': reply(op.id, true, host.setBuffer(op.key, op.data)); return;
      case 'commitBuffer': {
        const a = host.mapBuffer(op.key, I.BUFFER_TYPES[op.type], op.data.length);
        if (!a) {
          reply(op.id, true, false);
          return;
        }
        a.set(op.data);
        reply(op.id, true, host.commitBuffer(op.key));
        return;
      }
      case 'removeBuffer': reply(op.id, true, host.removeBuffer(op.key)); return;
      case 'texture': {
        const src = op.source;
        let ok = host.setTexture(op.name, src, op.options);
        // Cross-origin (SecurityError, logged): the page does not send it again until its URL changes.
        if (!ok && host._textures?._tainted.has(src)) ok = 'tainted';
        host._bitmaps ??= new Map();
        const prev = host._bitmaps.get(op.name);
        if (typeof ImageBitmap !== 'undefined' && src instanceof ImageBitmap) {
          // Kept by the texture set (re-uploads after renderer changes).
          if (prev && prev !== src) prev.close();
          host._bitmaps.set(op.name, src);
        } else {
          if (prev) prev.close();
          host._bitmaps.delete(op.name);
          if (typeof src?.close === 'function') src.close();  // a VideoFrame: uploaded, not kept
        }
        if (op.ack) post({ ev: 'texAck', name: op.name });
        reply(op.id, true, ok);
        return;
      }
      case 'removeTexture': {
        const ok = host.removeTexture(op.name);
        host._bitmaps?.get(op.name)?.close();
        host._bitmaps?.delete(op.name);
        reply(op.id, true, ok);
        return;
      }
      case 'resolveCall':
        reply(op.id, true, strings(host, [op.json], (j) => M._jsui_web_resolve_call(host.rt, op.callId, j)));
        return;
      case 'rejectCall':
        reply(op.id, true, strings(host, [op.message], (s) => M._jsui_web_reject_call(host.rt, op.callId, s)));
        return;
      case 'callUi':
        new Promise((resolve, reject) => {
          const id = strings(host, [op.name, op.json], (n, j) => M._jsui_web_call_ui(host.rt, host.hostId, n, j));
          host._uiCalls.set(id, { resolve, reject });
        }).then((v) => reply(op.id, true, JSON.stringify(v)), (e) => reply(op.id, false, e));
        return;
      case 'run': {
        if (!view) return reply(op.id, true, false);
        let props;
        try { props = JSON.parse(op.props); } catch { props = op.props; }
        reply(op.id, true, view.run(op.component, props));
        return;
      }
      case 'surface': {
        const s = host.createSurface(op.canvas, { ...op.opts, _v: op.v, _size: op.size });
        views.set(op.v, s);
        reply(op.id, true, { rootTag: s.rootTag, renderer: s.renderer });
        return;
      }
      case 'destroySurface': view?.destroy(); return;
      case 'audioReports':
        for (const r of op.list) {
          if (r[0] === 0) host.audio.voiceEnded(r[1], r[2]);
          else host.audio.soundLoaded(r[1], r[2], r[3]);
        }
        return;
      case 'loadUrl': host.loadUrl(op.url); return;
      case 'loadSource': reply(op.id, true, host.loadSource(op.source, op.name)); return;
      case 'reload': host.reload(); return;
      case 'devUpdate': host.devUpdate(); return;
      case 'registerFont': reply(op.id, true, host.registerFont(op.family, op.bytes, op.weight, op.italic)); return;
      case 'loadFont':
        host.loadFont(op.family, op.url, op.weight, op.italic).then((ok) => reply(op.id, true, ok),
          (e) => reply(op.id, false, e));
        return;
      case 'unregisterFont': reply(op.id, true, host.unregisterFont(op.family)); return;
      case 'fontFallbacks': host.setFontFallbacks(op.list); return;
      case 'defaultFontFamily': host.setDefaultFontFamily(op.family); return;
      case 'idle':
        // After the frame's hit map: the page's view is current once this resolves.
        host.whenIdle().then(() => host._afterFrame(() => {
          post({ ev: 'stats', stats: host._statsOf() });
          reply(op.id, true, null);
        }));
        return;
      case 'profiling': view?.setProfiling(op.on); return;
      case 'perf': reply(op.id, true, view ? view.getPerf(op.reset) : null); return;
      case 'read':
        if (!view) return reply(op.id, true, null);
        Promise.resolve(view.readPixelsAsync()).then((px) => {
          post({ ev: 'stats', stats: host._statsOf() });
          reply(op.id, true, px, px ? [px.data.buffer] : []);
        }, (e) => reply(op.id, false, e));
        return;
      case 'bench':
        // r0: the renders before the recorded frames (after the warmup ones, see _benchFrame).
        host._bench = { id: op.id, n: op.n, warmup: op.warmup ?? 0, stamps: [], cpu: [], parts: [],
          r0: host.stats.renders, inMs: 0, coreMs: 0, postMs: 0 };
        return;
      case 'debug': reply(op.id, true, debug(view, op.what, op.arg)); return;
      case 'destroy':
        host.destroy();
        reply(op.id, true, null);
        flush();
        return;
    }
  };

  // Test hooks (ProxyView._debug).
  let spyCalls = null;
  const debug = (view, what, arg) => {
    const M = host.module;
    switch (what) {
      case 'spy': {
        spyCalls = [];
        const strArgs = { _jsui_send_key: [1, 2], _jsui_send_text: [1], _jsui_set_gamepad_connected: [3] };
        for (const name of ['_jsui_send_mouse_move', '_jsui_send_mouse_button', '_jsui_send_mouse_wheel',
          '_jsui_send_mouse_leave', '_jsui_send_touch', '_jsui_send_key', '_jsui_send_text', '_jsui_send_gamepad_button',
          '_jsui_send_gamepad_axis', '_jsui_set_gamepad_connected']) {
          const f = M[name];
          const wrapper = (...a) => {
            const rec = a.slice(1).map((v, i) => (strArgs[name]?.includes(i + 1) ? (v ? M.UTF8ToString(v) : null) : v));
            spyCalls.push([name.slice(6), ...rec]);
            const r = f(...a);
            M[name] = wrapper;
            return r;
          };
          M[name] = wrapper;
        }
        return true;
      }
      case 'calls': return spyCalls ? spyCalls.splice(0) : [];
      case 'loseGL': case 'loseGLForGood': {
        const gl = view?._gl ?? view?._sgl?.gl;
        const ext = gl?.getExtension('WEBGL_lose_context');
        if (!ext) return false;
        // Lost as in WebKit, whose create* calls then return null (Chromium's
        // return objects). arg.mid: in the middle of the frame below, at its
        // first shader compile (WebKit notices a lost GPU process in a GL call).
        const CREATE = ['createProgram', 'createShader', 'createTexture'];
        const lose = () => {
          ext.loseContext();
          for (const k of CREATE) gl[k] = () => null;
        };
        // arg.blankBitmaps: the kept ImageBitmaps lose their pixels, as
        // WebKit's do with its GPU process (re-uploaded blank after the restore).
        if (arg?.blankBitmaps) {
          for (const e of host._textures?.entries.values() ?? []) {
            if (!(e.source instanceof ImageBitmap)) continue;
            const c = new OffscreenCanvas(e.source.width, e.source.height);
            c.getContext('2d');
            const blank = c.transferToImageBitmap();
            if (host._bitmaps?.get(e.name) === e.source) host._bitmaps.set(e.name, blank);
            e.source.close();
            e.source = blank;
          }
        }
        // arg.onTexture: lost when the next host texture arrives, right before
        // it is set (isContextLost() true, the event not handled yet).
        if (arg?.onTexture) {
          const set = host.setTexture;
          host.setTexture = function (...a) {
            delete host.setTexture;
            lose();
            try {
              return set.apply(this, a);
            } finally {
              for (const k of CREATE) delete gl[k];
              setTimeout(() => ext.restoreContext(), 50);
            }
          };
          return true;
        }
        let fired = !arg?.mid;
        if (arg?.mid) {
          gl.compileShader = function (sh) {
            delete gl.compileShader;
            fired = true;
            lose();
            return gl.compileShader(sh);
          };
        } else {
          lose();
        }
        // arg.work: GL work in the same task, before the webglcontextlost
        // event (as when a lost GPU process is noticed first, as WebKit
        // does): new shaders, a texture upload, a frame.
        let error = null;
        if (arg?.work) {
          try {
            host.postMessage('fx', null);
            const img = new ImageData(8, 8);
            img.data.fill(255);
            host.setTexture('tex', img);
            host.frame(now());
          } catch (e) {
            error = String(e?.stack ?? e);
          }
        }
        delete gl.compileShader;
        for (const k of CREATE) delete gl[k];
        if (what === 'loseGL' && fired) setTimeout(() => ext.restoreContext(), 50);
        return arg?.work ? { error, fired } : true;
      }
      case 'destroyDevice':
        if (!view?._wgpu) return false;
        view._wgpu.device.destroy();
        return true;
      case 'info': {
        const gl = view?._gl, ext = gl?.getExtension('WEBGL_debug_renderer_info');
        return { renderer: view?.renderer, backend: host.textureBackend, rafs: typeof requestAnimationFrame === 'function',
          hitCalls: host._hitCalls ?? 0,
          ticks: !!host._ticks, offset: OFF, adapter: view?._webgpu?.description ?? null,
          gl: gl ? gl.getParameter(ext ? ext.UNMASKED_RENDERER_WEBGL : gl.RENDERER) : null };
      }
    }
    return null;
  };

  const onMessage = (e) => {
    const m = e.data;
    if (m?.t === 'init') init(m);
    else if (m?.t === 'ops') {
      if (pending) pending.push(m.list);
      else apply(m.list);
    }
  };
  globalThis.onmessage = onMessage;
  for (const e of early) onMessage(e);
  post({ ev: 'hello' });
}
