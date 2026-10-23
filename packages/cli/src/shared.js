// `jsui dev --shared`: one live runtime that a person and coding agents use
// together. The runtime runs in the dev server (the CPU WebAssembly module,
// on the real clock, hot-updating with every save); the person watches and
// plays it in a browser page, and agents drive it with the same commands as
// `jsui run`, over HTTP:
//
//   GET  /shared                     the viewer page (frames + mouse, keyboard, gamepad)
//   GET  /__jsui/shared/stream       frames and activity for viewers (binary, see below)
//   POST /__jsui/shared/input        viewer input: [{t: 'button', x, y, button, down, mods}, ...]
//   POST /__jsui/shared/run          {steps, calls?, keepGoing?} -> {ok, failed, results}
//   POST /__jsui/shared/call         {method, args} -> {value} | {error}  (attach() in scripts)
//   GET  /__jsui/shared/screenshot.png
//   GET  /__jsui/shared/tree         the view tree (JSON)
//   GET  /__jsui/shared/activity?since=<seq>   who did what: agent / user / ui events
//   GET  /__jsui/shared/info
//
// Stream messages: u32 LE length, u8 type, payload. Type 1 is a frame
// rectangle (u16 LE pixel width, height of the whole frame, then x, y, w, h,
// then the rect's premultiplied RGBA rows, deflate-raw compressed); type 2 is
// a UTF-8 JSON event ({type: 'hello' | 'activity' | 'pointer' | 'cursor', ...}).
//
// POST bodies must be application/json from this host: other web pages
// cannot drive the app (no CORS preflight is allowed for POST, and the Host
// header is checked against DNS rebinding).

import { readFileSync } from 'node:fs';
import { performance } from 'node:perf_hooks';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import zlib from 'node:zlib';
import { HeadlessApp, loadModule } from './headless.js';
import { RemoteRuntime } from './remote-runtime.js';
import { answerOf, runSteps } from './script.js';

const VIEWER = path.join(path.dirname(fileURLToPath(import.meta.url)), 'shared-viewer.html');
const MAX_ACTIVITY = 2000;
const MAX_BODY = 32 << 20;

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// Methods attach() may call (RemoteApp in attach.js), by name.
const RPC = new Set([
  'wait', 'frame', 'idle', 'screenshot', 'click', 'tap', 'hover', 'drag', 'wheel', 'key', 'type', 'focus',
  'press', 'stick', 'setState', 'patchState', 'getState', 'setBuffer', 'postMessage', 'handle', 'run',
  'resize', 'hitTest', 'tree', 'find', 'findAll', 'get', 'point', 'callUi', 'activity', 'reload',
]);

/** A HeadlessApp on the real clock, loading (and hot-updating) from the dev server. */
export class LiveApp extends HeadlessApp {
  constructor(M, url, o) {
    super(M, { url }, { ...o, devMode: true });
    this.activityLog = [];
    this._seq = 0;
    this._frameWaiters = [];
    this.onTick = null;      // after every frame
    this.onActivity = null;  // (event) for every activity entry
    this.onPointer = null;   // ({action, x, y, to?}) where the agent points
    this._timer = setInterval(() => this._loop(), this.frameMs);
    this.id = 'headless';
    this.kind = 'headless';
    this.label = 'headless';
    this.record = null;      // (who, kind, data) => event: set by the session
  }

  describe() {
    return { id: this.id, name: 'headless', platform: 'node', label: this.label, kind: this.kind, width: this.width, height: this.height, scale: this.scale };
  }

  _now() {
    if (this._t0 == null) this._t0 = performance.now();
    return performance.now() - this._t0;
  }

  _tick() {
    this.time = Math.max(this.time, this._now());
    this.M._jsui_tick(this.rt, this.time);
    this._checkError();
  }

  _loop() {
    if (!this.rt) return;
    this._tick();
    const waiters = this._frameWaiters;
    this._frameWaiters = [];
    for (const w of waiters) w();
    if (this.onTick) {
      try {
        this.onTick();
      } catch (e) {
        // Never take the dev server down over a viewer.
        if (!this._tickError) console.error('[jsui shared] frame stream:', e);
        this._tickError = true;
      }
    }
  }

  _nextFrame() {
    return new Promise((r) => this._frameWaiters.push(r));
  }

  async _settle(maxMs = 10000) {
    await null;  // the frame loop starts after the constructor
    const start = Date.now();
    let quiet = 0;
    while (quiet < 3 && Date.now() - start < maxMs && this.rt) {
      await this._nextFrame();
      quiet = this.M._jsui_pending_loads(this.rt) > 0 ? 0 : quiet + 1;
    }
  }

  async wait(ms = 0) {
    this._alive();
    await this._ready;
    if (ms > 0) await sleep(ms);
    await this._nextFrame();
    return this;
  }

  async frame() {
    this._alive();
    await this._nextFrame();
    return this;
  }

  async idle(maxMs = 5000) {
    this._alive();
    await this._ready;
    const end = Date.now() + maxMs;
    let quiet = 0;
    while (quiet < 2 && Date.now() < end && this.rt) {
      await this._nextFrame();
      const busy = this._answering > 0 || this.M._jsui_pending_loads(this.rt) > 0 || this.M._jsui_wants_frame(this.rt);
      quiet = busy ? 0 : quiet + 1;
    }
    return this;
  }

  async callUi(name, args, { timeout = 10000 } = {}) {
    this._alive();
    await this._ready;
    let timer;
    try {
      return await Promise.race([
        this._startUiCall(name, args),
        new Promise((_, reject) => {
          timer = setTimeout(() => reject(new Error(`jsui: UI handler ${JSON.stringify(name)} did not answer in ${timeout} ms`)), timeout);
        }),
      ]);
    } finally {
      clearTimeout(timer);
    }
  }

  // Right after a save the bundle may be reloading: wait for it once.
  async tree() {
    try {
      return await super.tree();
    } catch (e) {
      if (!/handler/i.test(e.message)) throw e;
      await this.idle(5000);
      return super.tree();
    }
  }

  // Pointer actions show where the agent points in the viewers.
  async hover(target) {
    const p = await this.point(target);
    this._pointAt('hover', p);
    return super.hover(p);
  }

  async click(target, opts) {
    const p = await this.point(target);
    this._pointAt('click', p);
    return super.click(p, opts);
  }

  async tap(target, opts) {
    const p = await this.point(target);
    this._pointAt('tap', p);
    return super.tap(p, opts);
  }

  async wheel(target, dy, dx) {
    const p = target == null ? this._pointer : await this.point(target);
    this._pointAt('wheel', p);
    return super.wheel(p, dy, dx);
  }

  // In real time: one move per frame, so velocities (flings) are real.
  async drag(from, to, { duration = 300, touch = false } = {}) {
    const a = await this.point(from);
    const b = await this.point(to);
    this._pointAt('drag', a, b);
    const M = this.M;
    if (touch) M._jsui_send_touch(this.rt, 1, 0, a.x, a.y);
    else {
      M._jsui_send_mouse_move(this.rt, a.x, a.y);
      M._jsui_send_mouse_button(this.rt, a.x, a.y, 0, 1, 0);
    }
    const start = performance.now();
    for (;;) {
      await this._nextFrame();
      if (!this.rt) return this;
      const k = Math.min(1, (performance.now() - start) / Math.max(1, duration));
      const x = a.x + (b.x - a.x) * k;
      const y = a.y + (b.y - a.y) * k;
      if (touch) M._jsui_send_touch(this.rt, 1, 1, x, y);
      else M._jsui_send_mouse_move(this.rt, x, y);
      if (k >= 1) break;
    }
    if (touch) M._jsui_send_touch(this.rt, 1, 2, b.x, b.y);
    else M._jsui_send_mouse_button(this.rt, b.x, b.y, 0, 0, 0);
    this._pointer = b;
    await this._nextFrame();
    return this;
  }

  resize(width, height, scale) {
    super.resize(width, height, scale);
    this._log('agent', 'resize', { width: this.width, height: this.height, scale: this.scale });
    return this;
  }

  _pointAt(action, p, to) {
    if (this.onPointer) this.onPointer({ action, x: p.x, y: p.y, ...(to ? { toX: to.x, toY: to.y } : {}) });
  }

  // Messages, calls, state writes, logs and errors of the UI.
  _event(kind, data) {
    this._log('ui', kind, data);
  }

  _log(who, kind, data) {
    if (this.record) return this.record(who, kind, data);  // the session's feed
    const ev = { seq: ++this._seq, time: Math.round(this.time), who, kind, ...data };
    this.activityLog.push(ev);
    if (this.activityLog.length > MAX_ACTIVITY) this.activityLog.splice(0, this.activityLog.length - MAX_ACTIVITY);
    if (this.onActivity) this.onActivity(ev);
    return ev;
  }

  /** Activity entries after `since` (a seq): {seq, time, who: 'agent' | 'user' | 'ui', kind, ...}. */
  activity(since = 0) {
    return this.activityLog.filter((e) => e.seq > since);
  }

  close() {
    clearInterval(this._timer);
    const waiters = this._frameWaiters;
    this._frameWaiters = [];
    super.close();
    for (const w of waiters) w();
  }
}

// ------------------------------------------------------------- viewer input

const MOD_NAMES = ['shift', 'ctrl', 'alt', 'meta'];
function modText(mods) {
  return MOD_NAMES.filter((_, i) => mods & (1 << i)).map((m) => m + '+').join('');
}
const PAD_NAMES = ['A', 'B', 'X', 'Y', 'LB', 'RB', 'LT', 'RT', 'Back', 'Start', 'LS', 'RS', 'Up', 'Down', 'Left', 'Right', 'Home'];
const round = (v) => Math.round(v);

/** Applies one viewer input event; logs the meaningful ones as user activity. */
function applyInput(app, e) {
  const M = app.M;
  const rt = app.rt;
  if (!rt || !e || typeof e !== 'object') return;
  const num = (v, d = 0) => (Number.isFinite(+v) ? +v : d);
  const str = (v) => (v == null ? null : String(v));
  switch (e.t) {
    case 'move':
      M._jsui_send_mouse_move(rt, num(e.x), num(e.y));
      app._pointer = { x: num(e.x), y: num(e.y) };
      break;
    case 'leave':
      M._jsui_send_mouse_leave(rt);
      break;
    case 'button': {
      const used = M._jsui_send_mouse_button(rt, num(e.x), num(e.y), num(e.button), e.down ? 1 : 0, num(e.mods)) !== 0;
      if (e.down) app._log('user', 'click', { x: round(num(e.x)), y: round(num(e.y)), button: num(e.button), consumed: used });
      break;
    }
    case 'wheel':
      M._jsui_send_mouse_wheel(rt, num(e.x), num(e.y), num(e.dx), num(e.dy));
      break;
    case 'touch': {
      const used = M._jsui_send_touch(rt, num(e.id), num(e.phase), num(e.x), num(e.y)) !== 0;
      if (num(e.phase) === 0) app._log('user', 'tap', { x: round(num(e.x)), y: round(num(e.y)), consumed: used });
      break;
    }
    case 'key': {
      const used = app._withStrings([str(e.key), str(e.code)], (k, c) => M._jsui_send_key(rt, k, c, e.down ? 1 : 0, e.repeat ? 1 : 0, num(e.mods))) !== 0;
      // Typed characters are logged as text; named keys and shortcuts here.
      const named = e.key && (String(e.key).length > 1 || (num(e.mods) & ~1));
      if (e.down && !e.repeat && named) app._log('user', 'key', { key: modText(num(e.mods)) + e.key, consumed: used });
      break;
    }
    case 'text':
      app._withStrings([String(e.text || '')], (t) => M._jsui_send_text(rt, t));
      if (e.text) app._log('user', 'type', { text: String(e.text) });
      break;
    case 'padConnect':
      app._withStrings([e.connected ? String(e.id || 'gamepad') : null], (id) => M._jsui_set_gamepad_connected(rt, num(e.pad), e.connected ? 1 : 0, id));
      if (num(e.pad) === 0) app._padConnected = !!e.connected;
      break;
    case 'padButton': {
      const used = M._jsui_send_gamepad_button(rt, num(e.pad), num(e.button), e.down ? 1 : 0, num(e.value, e.down ? 1 : 0)) !== 0;
      if (e.down) app._log('user', 'press', { button: PAD_NAMES[num(e.button)] || num(e.button), pad: num(e.pad), consumed: used });
      break;
    }
    case 'reload':
      M._jsui_reload(rt);
      app._log('user', 'reload', {});
      break;
    case 'padAxis':
      M._jsui_send_gamepad_axis(rt, num(e.pad), num(e.axis), num(e.value));
      break;
    default:
      break;
  }
}

// ------------------------------------------------------------ frame stream

function message(type, payload) {
  const head = Buffer.alloc(5);
  head.writeUInt32LE(payload.length + 1, 0);
  head.writeUInt8(type, 4);
  return Buffer.concat([head, payload]);
}

const jsonMessage = (obj) => message(2, Buffer.from(JSON.stringify(obj)));

class FrameSource {
  constructor(app) {
    this.app = app;
    this.buf = 0;
    this.w = 0;
    this.h = 0;
    this.dirty = app.M._malloc(16);
    this.fresh = true;  // the buffer has not been rendered yet
  }

  // (Re)allocates the buffer for the app's pixel size; true if it changed.
  size() {
    const app = this.app;
    const M = app.M;
    const w = Math.max(1, Math.round(app.width * app.scale));
    const h = Math.max(1, Math.round(app.height * app.scale));
    if (w === this.w && h === this.h) return false;
    if (this.buf) M._free(this.buf);
    this.buf = M._malloc(w * h * 4);
    M.HEAPU8.fill(0, this.buf, this.buf + w * h * 4);
    this.w = w;
    this.h = h;
    this.fresh = true;
    return true;
  }

  // Renders what changed; returns the damaged rect or null.
  render() {
    const M = this.app.M;
    const w = this.w;
    const h = this.h;
    const changed = M._jsui_render_rgba_incremental(this.app.rt, this.buf, w, h, w * 4, this.dirty);
    const d = this.dirty >> 2;
    if (this.fresh) {
      this.fresh = false;
      return { x: 0, y: 0, w, h };
    }
    if (!changed) return null;
    const r = { x: M.HEAP32[d], y: M.HEAP32[d + 1], w: M.HEAP32[d + 2], h: M.HEAP32[d + 3] };
    return r.w > 0 && r.h > 0 ? r : null;
  }

  encode(r) {
    const M = this.app.M;
    const rowBytes = r.w * 4;
    let pixels;
    if (r.x === 0 && r.w === this.w) {
      const start = this.buf + r.y * this.w * 4;
      pixels = M.HEAPU8.subarray(start, start + r.h * rowBytes);
    } else {
      pixels = Buffer.alloc(r.h * rowBytes);
      for (let row = 0; row < r.h; row++) {
        const start = this.buf + ((r.y + row) * this.w + r.x) * 4;
        pixels.set(M.HEAPU8.subarray(start, start + rowBytes), row * rowBytes);
      }
    }
    const head = Buffer.alloc(12);
    head.writeUInt16LE(this.w, 0);
    head.writeUInt16LE(this.h, 2);
    head.writeUInt16LE(r.x, 4);
    head.writeUInt16LE(r.y, 6);
    head.writeUInt16LE(r.w, 8);
    head.writeUInt16LE(r.h, 10);
    return message(1, Buffer.concat([head, zlib.deflateRawSync(pixels, { level: 1 })]));
  }

  dispose() {
    const M = this.app.M;
    if (this.buf) M._free(this.buf);
    M._free(this.dirty);
    this.buf = 0;
  }
}

const union = (a, b) => {
  if (!a) return b;
  if (!b) return a;
  const x = Math.min(a.x, b.x);
  const y = Math.min(a.y, b.y);
  return { x, y, w: Math.max(a.x + a.w, b.x + b.w) - x, h: Math.max(a.y + a.h, b.y + b.h) - y };
};

// ------------------------------------------------------------------ server

const POLL_HOLD_MS = 20000;   // a runtime's poll is held this long without commands
const GONE_AFTER_MS = 10000;  // a runtime not polling for this long has left
const WAIT_FOR_CLIENT_MS = 30000;

/**
 * Starts the shared session of a dev server.
 * @param {object} o
 * @param {string} o.bundleUrl  the dev server's /bundle.js (http)
 * @param {string} o.entry      shown in the viewer
 * @param {string[]} o.hosts    Host header names allowed to POST from browsers (DNS rebinding guard)
 * @param {boolean} [o.headless]  also run a headless runtime in this process
 * @param {string} [o.webHost]  URL path of the web host page (/shared runs it when nothing is connected)
 * @param {number} [o.waitMs]   how long agent commands wait for a runtime to join
 */
export async function createShared(o) {
  const log = o.log || console.log;
  const waitMs = o.waitMs ?? WAIT_FOR_CLIENT_MS;
  const t0 = performance.now();
  const clock = () => Math.round(performance.now() - t0);
  const allowedHosts = new Set(['localhost', '127.0.0.1', '[::1]', ...(o.hosts || [])]);
  const viewers = new Set();  // {res, pending}
  const activity = [];
  let seq = 0;
  let agentBusy = 0;
  let queue = Promise.resolve();
  let closed = false;

  const broadcast = (buf) => {
    for (const v of viewers) v.res.write(buf);
  };

  /** One feed for the session: {seq, time, who: 'agent' | 'user' | 'ui', kind, client, ...}. */
  const record = (client, who, kind, data) => {
    const ev = { seq: ++seq, time: clock(), who, kind, ...(client ? { client } : {}), ...data };
    activity.push(ev);
    if (activity.length > MAX_ACTIVITY) activity.splice(0, activity.length - MAX_ACTIVITY);
    if (viewers.size) broadcast(jsonMessage({ type: 'activity', event: ev }));
    return ev;
  };
  const activitySince = (since = 0) => activity.filter((e) => e.seq > since);

  // ------------------------------------------------------------ clients

  // Runtimes in the session, in the order they joined; the last one is live.
  const clients = [];
  let joinWaiters = [];
  let headless = null;
  let frames = null;
  let cursor = 'default';

  const describeClients = () => clients.map((c) => ({ ...c.describe(), live: c === liveClient() }));
  const announce = () => {
    if (viewers.size) broadcast(jsonMessage({ type: 'clients', clients: describeClients(), live: liveClient()?.id ?? null }));
  };

  const addClient = (c) => {
    clients.push(c);
    record(c.label, 'ui', 'join', { platform: c.platform });
    log(`[jsui] ${c.label} joined the shared session`);
    const w = joinWaiters;
    joinWaiters = [];
    for (const f of w) f();
    announce();
  };

  const removeClient = (c, reason) => {
    const i = clients.indexOf(c);
    if (i < 0) return;
    clients.splice(i, 1);
    c.close(reason);
    record(c.label, 'ui', 'leave', {});
    log(`[jsui] ${c.label} left the shared session`);
    announce();
  };

  // The live runtime: the last one to join (the headless one when it is alone).
  function liveClient() {
    return clients[clients.length - 1] || null;
  }

  // A runtime by --client: its id (or a prefix), 'headless', or part of its name.
  const findClient = (sel) => {
    const q = String(sel).toLowerCase();
    return (
      clients.find((c) => c.id === sel) ||
      clients.find((c) => c.id.startsWith(q)) ||
      [...clients].reverse().find((c) => c.label.toLowerCase().includes(q) || c.platform.toLowerCase() === q)
    );
  };

  const howToConnect = () => {
    const origin = o.origin || '';
    return [
      'no JSUI runtime is connected to this dev server. Open one:',
      `  browser:   ${origin}/shared`,
      `  native:    jsui-viewer --dev ${origin}/bundle.js`,
      `  Godot:     a JsuiView with bundle_url = "${origin}/bundle.js" and dev_mode on`,
      '  or start the dev server with --headless (a runtime inside it).',
    ].join('\n');
  };

  /** The runtime agent commands go to; waits for one to join. */
  async function pick(sel) {
    const end = Date.now() + waitMs;
    for (;;) {
      if (closed) throw new Error('the dev server is shutting down');
      if (sel != null && sel !== '') {
        const c = findClient(sel);
        if (c) return c;
        if (Date.now() >= end) {
          const names = clients.map((x) => `${x.id.slice(0, 8)} ${x.label}`).join(', ') || 'none';
          throw new Error(`no runtime matches --client ${JSON.stringify(sel)} (connected: ${names})`);
        }
      } else if (clients.length) return liveClient();
      if (Date.now() >= end) throw new Error(howToConnect());
      await new Promise((r) => {
        const t = setTimeout(r, Math.max(10, end - Date.now()));
        joinWaiters.push(() => {
          clearTimeout(t);
          r();
        });
      });
    }
  }

  const sweep = setInterval(() => {
    for (const c of [...clients]) if (c.kind === 'remote' && c.idleFor() > GONE_AFTER_MS) removeClient(c, `${c.label} stopped polling (closed, or no longer ticking)`);
  }, 2000);

  if (o.headless) {
    const M = await loadModule(o.wasmDir);
    headless = new LiveApp(M, o.bundleUrl, {
      width: o.width || 1280,
      height: o.height || 720,
      scale: o.scale || 1,
      fps: o.fps || 60,
      component: o.component,
      props: o.props,
      state: o.state,
      onLog: (level, text) => {
        if (level >= 2) log(`[jsui headless] ${level >= 3 ? 'error' : 'warn'}: ${text}`);
      },
    });
    headless.record = (who, kind, data) => record(headless.label, who, kind, data);
    frames = new FrameSource(headless);
    headless.onPointer = (p) => {
      if (viewers.size) broadcast(jsonMessage({ type: 'pointer', ...p }));
    };
    headless.onTick = () => {
      if (!viewers.size) return;
      const resized = frames.size();
      const r = frames.render();
      if (resized) {
        broadcast(jsonMessage({ type: 'hello', ...info() }));
        for (const v of viewers) v.pending = { x: 0, y: 0, w: frames.w, h: frames.h };
      } else if (r) {
        for (const v of viewers) v.pending = union(v.pending, r);
      }
      // A slow viewer gets the union of what changed once it has caught up.
      for (const v of viewers) {
        if (!v.pending || v.res.writableLength > (2 << 20)) continue;
        v.res.write(frames.encode(v.pending));
        v.pending = null;
      }
      const p = headless.M._jsui_get_cursor(headless.rt);
      const c = p ? headless.M.UTF8ToString(p) : 'default';
      if (c !== cursor) {
        cursor = c;
        broadcast(jsonMessage({ type: 'cursor', cursor }));
      }
    };
    addClient(headless);
  }

  const info = (c = liveClient()) => ({
    entry: o.entry,
    width: c?.width,
    height: c?.height,
    scale: c?.scale,
    pixelWidth: frames ? frames.w || Math.round(headless.width * headless.scale) : undefined,
    pixelHeight: frames ? frames.h || Math.round(headless.height * headless.scale) : undefined,
    headless: !!headless,
    webHost: o.webHost || null,
    clients: describeClients(),
    live: c?.id ?? null,
    viewers: viewers.size,
    agentBusy: agentBusy > 0,
    seq,
  });

  // Agent requests run one at a time, in order.
  const agent = (label, client, fn) => {
    const run = queue.then(async () => {
      agentBusy++;
      if (label) record(client.label, 'agent', 'command', { text: label });
      if (viewers.size) broadcast(jsonMessage({ type: 'agent', busy: true }));
      try {
        return await fn();
      } finally {
        agentBusy--;
        if (viewers.size && !agentBusy) broadcast(jsonMessage({ type: 'agent', busy: false }));
      }
    });
    queue = run.catch(() => {});
    return run;
  };

  const sendJson = (res, status, obj) => {
    res.writeHead(status, { 'Content-Type': 'application/json; charset=utf-8', 'Cache-Control': 'no-store' });
    res.end(JSON.stringify(obj));
  };

  const readJson = (req) =>
    new Promise((resolve, reject) => {
      const chunks = [];
      let size = 0;
      req.on('data', (c) => {
        size += c.length;
        if (size > MAX_BODY) {
          reject(new Error('request body too large'));
          req.destroy();
        } else chunks.push(c);
      });
      req.on('end', () => {
        try {
          resolve(chunks.length ? JSON.parse(Buffer.concat(chunks).toString('utf8')) : {});
        } catch (e) {
          reject(new Error('invalid JSON: ' + e.message));
        }
      });
      req.on('error', reject);
    });

  // Browsers send Origin with every POST: only pages of this server may
  // drive the session (other sites, DNS rebinding). Runtimes and agents
  // outside a browser (native, Godot, Node) send none.
  const hostOk = (req) => {
    const host = String(req.headers.host || '');
    const origin = req.headers.origin;
    if (origin == null) return true;
    let o2;
    try {
      o2 = new URL(origin);
    } catch {
      return false;
    }
    return o2.host === host && allowedHosts.has(host.replace(/:\d+$/, ''));
  };

  // {"click": "#play", "hold": 50} -> 'click #play hold=50'
  const describeStep = (s) => {
    if (!s || typeof s !== 'object') return JSON.stringify(s);
    const text = Object.entries(s)
      .filter(([k]) => k !== 'label')
      .map(([k, v], i) => (i === 0 ? k + (v === true ? '' : ' ' + (typeof v === 'string' ? v : JSON.stringify(v))) : `${k}=${JSON.stringify(v)}`))
      .join(' ');
    return text.length > 160 ? text.slice(0, 157) + '...' : text;
  };

  const encodeValue = (v) => (Buffer.isBuffer(v) ? { base64: v.toString('base64') } : v instanceof LiveApp || v instanceof RemoteRuntime ? null : v);

  const snapshot = (app, since) => ({
    time: Math.round(app.time),
    client: app.describe(),
    errors: app.errors.slice(-50),
    messages: app.messages.slice(-200),
    calls: app.calls.slice(-200),
    stateWrites: app.stateWrites.slice(-200),
    seq,
    ...(since != null ? { activity: activitySince(since) } : {}),
  });

  // ------------------------------------------------------------ runtimes' side

  async function session(req, res, p, body) {
    const id = String(body.runtime || '');
    if (!/^[\w-]{4,64}$/.test(id)) return sendJson(res, 400, { error: 'expected {"runtime": "<id>"}' });
    let c = clients.find((x) => x.id === id);
    if (p === '/__jsui/session/poll') {
      if (closed) return sendJson(res, 200, { commands: [], leave: true });
      if (!c) {
        c = new RemoteRuntime(id, body.info || {}, { record: (who, kind, data) => record(c.label, who, kind, data) });
        addClient(c);
      } else c.update(body.info || {});
      const held = c.poll((answer) => sendJson(res, 200, answer), POLL_HOLD_MS);
      // The runtime went away mid-poll (tab closed, game quit).
      if (held) res.on('close', () => held.answer({ commands: [] }));
      return undefined;
    }
    // push
    if (c) c.pushed(body.results, body.events);
    res.writeHead(204, { 'Cache-Control': 'no-store' });
    return res.end();
  }

  // ------------------------------------------------------------ agents' side

  async function post(req, res, p) {
    if (!hostOk(req)) return sendJson(res, 403, { error: 'forbidden origin' });
    if (!/^application\/json\b/i.test(String(req.headers['content-type'] || ''))) return sendJson(res, 415, { error: 'Content-Type must be application/json' });
    let body;
    try {
      body = await readJson(req);
    } catch (e) {
      return sendJson(res, 400, { error: e.message });
    }
    if (p.startsWith('/__jsui/session/')) return session(req, res, p, body);
    if (p === '/__jsui/shared/input') {
      // The streamed viewer of the headless runtime.
      if (headless && headless.rt) for (const e of [].concat(body)) applyInput(headless, e);
      res.writeHead(204, { 'Cache-Control': 'no-store' });
      return res.end();
    }
    if (p === '/__jsui/shared/call' && body.method === 'activity') {
      const args = Array.isArray(body.args) ? body.args : [];
      return sendJson(res, 200, { value: activitySince(Number(args[0]) || 0), seq });
    }
    let app;
    try {
      app = await pick(body.client);
    } catch (e) {
      return sendJson(res, 503, { error: e.message });
    }
    if (p === '/__jsui/shared/run') {
      const steps = Array.isArray(body) ? body : body.steps;
      if (!Array.isArray(steps)) return sendJson(res, 400, { error: 'expected {"steps": [...]}' });
      const results = [];
      const r = await agent(null, app, async () => {
        if (body.calls) for (const [name, answer] of Object.entries(body.calls)) app.handle(name, answerOf(answer));
        await app.ready();
        return runSteps(app, steps, {
          keepGoing: !!body.keepGoing,
          onResult: (out) => {
            results.push(out);
            record(app.label, 'agent', 'step', { text: describeStep(steps[out.step]), ok: out.ok, ...(out.ok ? {} : { error: out.error }) });
          },
        });
      }).catch((e) => ({ ok: false, failed: 1, error: e.message }));
      if (r.error) return sendJson(res, 200, { error: r.error, ...snapshot(app) });
      return sendJson(res, 200, { ok: r.ok, failed: r.failed, results, ...snapshot(app) });
    }
    if (p === '/__jsui/shared/call') {
      const method = String(body.method || '');
      const args = Array.isArray(body.args) ? body.args : [];
      if (!RPC.has(method)) return sendJson(res, 400, { error: `unknown method ${JSON.stringify(method)}` });
      try {
        const quiet = method === 'getState' || method === 'tree' || method === 'find' || method === 'findAll' || method === 'get' || method === 'point';
        const value = await agent(quiet ? null : `${method}(${args.map((a) => JSON.stringify(a)).join(', ').slice(0, 140)})`, app, async () => {
          await app.ready();
          if (method === 'handle') return void app.handle(String(args[0]), answerOf(args[1] && typeof args[1] === 'object' && ('value' in args[1] || 'error' in args[1]) ? args[1] : { value: args[1] }));
          if (method === 'screenshot') return app.screenshot();  // the client writes the file
          return app[method](...args);
        });
        return sendJson(res, 200, { value: encodeValue(value) ?? null, ...snapshot(app) });
      } catch (e) {
        return sendJson(res, 200, { error: e && e.message ? e.message : String(e), ...snapshot(app) });
      }
    }
    return sendJson(res, 404, { error: 'not found' });
  }

  function stream(req, res) {
    res.writeHead(200, { 'Content-Type': 'application/octet-stream', 'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff' });
    const v = { res, pending: null };
    viewers.add(v);
    if (frames) {
      frames.size();
      frames.fresh = true;  // a full frame for everyone (cheap; joins are rare)
    }
    res.write(jsonMessage({ type: 'hello', ...info(), activity: activitySince(Math.max(0, seq - 200)), cursor }));
    if (agentBusy) res.write(jsonMessage({ type: 'agent', busy: true }));
    const done = () => viewers.delete(v);
    req.on('close', done);
    res.on('error', done);
  }

  // GETs for agents: ?client= picks the runtime.
  function get(res, url, fn) {
    pick(url.searchParams.get('client'))
      .then((app) => agent(null, app, () => fn(app)))
      .then(
        (out) => {
          if (Buffer.isBuffer(out)) {
            res.writeHead(200, { 'Content-Type': 'image/png', 'Cache-Control': 'no-store' });
            res.end(out);
          } else sendJson(res, 200, out);
        },
        (e) => sendJson(res, /no JSUI runtime|no runtime matches/.test(e.message) ? 503 : 500, { error: e.message }),
      );
  }

  /** Handles a shared-session request; returns false for other URLs. */
  function handle(req, res, url) {
    const p = decodeURIComponent(url.pathname);
    if (p === '/shared' || p === '/shared/') {
      if (req.method !== 'GET') return false;
      res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store' });
      res.end(readFileSync(VIEWER));
      return true;
    }
    if (!p.startsWith('/__jsui/shared/') && !p.startsWith('/__jsui/session/')) return false;
    if (req.method === 'POST') {
      post(req, res, p).catch((e) => {
        if (!res.headersSent) sendJson(res, 500, { error: String((e && e.message) || e) });
      });
      return true;
    }
    if (req.method !== 'GET') return false;
    if (p === '/__jsui/shared/stream') {
      stream(req, res);
      return true;
    }
    if (p === '/__jsui/shared/info') {
      const sel = url.searchParams.get('client');
      sendJson(res, 200, info(sel ? findClient(sel) || null : undefined));
      return true;
    }
    if (p === '/__jsui/shared/activity') {
      const since = Number(url.searchParams.get('since') || 0) || 0;
      sendJson(res, 200, { events: activitySince(since), seq });
      return true;
    }
    if (p === '/__jsui/shared/screenshot.png') {
      get(res, url, (app) => app.screenshot());
      return true;
    }
    if (p === '/__jsui/shared/tree') {
      get(res, url, (app) => app.tree());
      return true;
    }
    return false;
  }

  return {
    get app() {
      return headless;
    },
    clients,
    handle,
    info,
    async close() {
      closed = true;
      clearInterval(sweep);
      for (const c of [...clients]) if (c.kind === 'remote') removeClient(c, 'the dev server is shutting down');
      for (const f of joinWaiters) f();
      joinWaiters = [];
      for (const v of viewers) v.res.end();
      viewers.clear();
      if (headless) {
        headless.close();
        frames.dispose();
      }
    },
  };
}
