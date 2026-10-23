// A runtime that joined the dev server's shared session (see shared.js and
// src/dev_session.h): the native viewer, a Godot JsuiView, a web host page,
// any embedder in dev mode with an http:// bundle. It long-polls
// POST /__jsui/session/poll for commands and pushes their results and what
// happens in it (the person's input, UI messages, calls, state writes, logs,
// errors) to POST /__jsui/session/push.
//
// RemoteRuntime has the methods of launch()'s app, on the runtime's own
// clock: every command runs at the end of one of its ticks, so a reply means
// at least one frame passed. What it cannot do: answer Host.call (the host
// does: the game, or the web page) and resize (the host owns the viewport).

import { mkdir, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { BUFFER_TYPES, HeadlessApp, keyCode, modsOf, PAD_AXES, PAD_BUTTONS } from './headless.js';

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const COMMAND_TIMEOUT = 15000;
const MAX_LOG = 200;

const push = (arr, v) => {
  arr.push(v);
  if (arr.length > MAX_LOG * 2) arr.splice(0, arr.length - MAX_LOG);
};

export class RemoteRuntime {
  /**
   * @param {string} id     the runtime's session id
   * @param {object} info   {name, platform, bundle, width, height, scale}
   * @param {object} o      {record(who, kind, data), now()}
   */
  constructor(id, info, o) {
    this.id = id;
    this.kind = 'remote';
    this._o = o;
    this.messages = [];
    this.calls = [];
    this.stateWrites = [];
    this.errors = [];
    this.logs = [];
    this.time = 0;
    this.joinedAt = Date.now();
    this._queue = [];          // commands not yet handed to the runtime
    this._pending = new Map(); // id -> {resolve, reject, timer}
    this._nextId = 1;
    this._poll = null;         // the held poll: {answer(body)}
    this._pollEnded = Date.now();
    this._padConnected = false;
    this.update(info);
    this._pointer = { x: this.width / 2, y: this.height / 2 };
  }

  update(info = {}) {
    this.name = String(info.name || '');
    this.platform = String(info.platform || 'native');
    this.bundle = info.bundle || '';
    if (info.width > 0) this.width = info.width;
    if (info.height > 0) this.height = info.height;
    if (info.scale > 0) this.scale = info.scale;
  }

  /** "Godot: Menu (macOS)", "web", ... */
  get label() {
    return this.name && this.name !== this.platform ? `${this.name} (${this.platform})` : this.platform;
  }

  describe() {
    return { id: this.id, name: this.name, platform: this.platform, label: this.label, kind: this.kind, width: this.width, height: this.height, scale: this.scale };
  }

  // ------------------------------------------------------------ protocol

  /** A poll arrived: answers it now if commands wait, else holds it. */
  poll(answer, holdMs) {
    this.endPoll();
    if (this._queue.length) return answer({ commands: this._take() });
    let timer = null;
    const p = {
      answer: (body) => {
        if (this._poll !== p) return;
        this._poll = null;
        this._pollEnded = Date.now();
        clearTimeout(timer);
        answer(body);
      },
    };
    this._poll = p;
    timer = setTimeout(() => p.answer({ commands: [] }), holdMs);
    return p;
  }

  /** The held poll's connection closed (or is answered empty). */
  endPoll(body = { commands: [] }) {
    if (this._poll) this._poll.answer(body);
  }

  /** Not polling for `ms`: the runtime is gone (closed, or stopped ticking). */
  idleFor() {
    return this._poll ? 0 : Date.now() - this._pollEnded;
  }

  _take() {
    const cmds = this._queue;
    this._queue = [];
    return cmds;
  }

  /** Results and events pushed by the runtime. */
  pushed(results, events) {
    for (const r of Array.isArray(results) ? results : []) {
      const p = this._pending.get(r && r.id);
      if (!p) continue;
      this._pending.delete(r.id);
      clearTimeout(p.timer);
      if (r.ok) p.resolve(r.value);
      else p.reject(new Error('jsui: ' + (r.error || 'command failed')));
    }
    for (const e of Array.isArray(events) ? events : []) {
      if (!e || typeof e !== 'object') continue;
      const { who = 'ui', kind, time, ...data } = e;
      if (typeof time === 'number') this.time = Math.max(this.time, time);
      const t = Math.round(this.time);
      if (who === 'ui') {
        if (kind === 'message') push(this.messages, { name: data.name, data: data.data, time: t });
        else if (kind === 'call') push(this.calls, { name: data.name, args: data.args, time: t });
        else if (kind === 'state') push(this.stateWrites, { key: data.key, value: data.value, time: t });
        else if (kind === 'error') push(this.errors, data.text);
        else if (kind === 'log') push(this.logs, { level: data.level, text: data.text });
      }
      // Debug and info logs stay out of the feed (the runtime's own output).
      if (kind === 'log' && !(data.level >= 2)) continue;
      this._o.record(who, kind, data);
    }
  }

  /** Fails what is in flight; tells a held poll to stop. */
  close(reason = 'the runtime left the session') {
    this.closed = true;
    this.endPoll({ commands: [], leave: true });
    for (const p of this._pending.values()) {
      clearTimeout(p.timer);
      p.reject(new Error('jsui: ' + reason));
    }
    this._pending.clear();
    this._queue = [];
  }

  /** Sends one command; resolves with its value once the runtime ran it. */
  cmd(op, args = {}, { timeout = COMMAND_TIMEOUT } = {}) {
    if (this.closed) return Promise.reject(new Error(`jsui: ${this.label} left the session`));
    const id = this._nextId++;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this._pending.delete(id);
        reject(new Error(`jsui: ${this.label} did not run ${op} within ${timeout} ms (is it still ticking? Background browser tabs and paused games stop ticking)`));
      }, timeout);
      this._pending.set(id, { resolve, reject, timer });
      this._queue.push({ id, op, ...args });
      // Commands sent together (move + press) reach the runtime in one tick.
      if (!this._flushing) {
        this._flushing = true;
        queueMicrotask(() => {
          this._flushing = false;
          if (this._poll && this._queue.length) this._poll.answer({ commands: this._take() });
        });
      }
    });
  }

  // ------------------------------------------------------------ app API

  ready() {
    return Promise.resolve(this);
  }

  async _status() {
    const s = await this.cmd('status');
    this.update(s);
    if (typeof s.time === 'number') this.time = Math.max(this.time, s.time);
    if (s.error && this.errors[this.errors.length - 1] !== s.error) push(this.errors, s.error);
    return s;
  }

  async wait(ms = 0) {
    if (ms > 0) await sleep(ms);
    await this._status();
    return this;
  }

  async frame() {
    await this._status();
    return this;
  }

  async idle(maxMs = 5000) {
    const end = Date.now() + maxMs;
    let quiet = 0;
    while (quiet < 2 && Date.now() < end) {
      const s = await this._status();
      quiet = !s.wantsFrame && !s.pendingLoads ? quiet + 1 : 0;
      if (!quiet) await sleep(16);
    }
    return this;
  }

  async screenshot(file) {
    const png = Buffer.from(await this.cmd('screenshot'), 'base64');
    if (file) {
      await mkdir(path.dirname(path.resolve(file)), { recursive: true });
      await writeFile(file, png);
    }
    return png;
  }

  async callUi(name, args, { timeout = 10000 } = {}) {
    return this.cmd('callUi', { name: String(name), args: args === undefined ? null : args }, { timeout });
  }

  async hover(target) {
    const p = await this.point(target);
    await this.cmd('mouseMove', p);
    this._pointer = p;
    return this.frame();
  }

  async click(target, { hold = 50, button = 0, mods } = {}) {
    const p = await this.point(target);
    const m = modsOf(mods);
    this.cmd('mouseMove', p).catch(() => {});
    const consumed = await this.cmd('mouseButton', { ...p, button, down: true, mods: m });
    await this.wait(hold);
    await this.cmd('mouseButton', { ...p, button, down: false, mods: m });
    this._pointer = p;
    await this.frame();
    return consumed;
  }

  async tap(target, { hold = 50 } = {}) {
    const p = await this.point(target);
    const consumed = await this.cmd('touch', { id: 1, phase: 0, ...p });
    await this.wait(hold);
    await this.cmd('touch', { id: 1, phase: 2, ...p });
    await this.frame();
    return consumed;
  }

  // In real time: one move per runtime frame, so velocities (flings) are real.
  async drag(from, to, { duration = 300, touch = false } = {}) {
    const a = await this.point(from);
    const b = await this.point(to);
    if (touch) await this.cmd('touch', { id: 1, phase: 0, ...a });
    else {
      this.cmd('mouseMove', a).catch(() => {});
      await this.cmd('mouseButton', { ...a, button: 0, down: true, mods: 0 });
    }
    const start = Date.now();
    for (;;) {
      const k = Math.min(1, (Date.now() - start) / Math.max(1, duration));
      const p = { x: a.x + (b.x - a.x) * k, y: a.y + (b.y - a.y) * k };
      await (touch ? this.cmd('touch', { id: 1, phase: 1, ...p }) : this.cmd('mouseMove', p));
      if (k >= 1) break;
    }
    await (touch ? this.cmd('touch', { id: 1, phase: 2, ...b }) : this.cmd('mouseButton', { ...b, button: 0, down: false, mods: 0 }));
    this._pointer = b;
    return this.frame();
  }

  async wheel(target, dy = 120, dx = 0) {
    const p = target == null ? this._pointer : await this.point(target);
    this.cmd('mouseMove', p).catch(() => {});
    const consumed = await this.cmd('wheel', { ...p, dx, dy });
    this._pointer = p;
    await this.frame();
    return consumed;
  }

  async key(key, { code, mods, hold = 0, count = 1, interval = 100 } = {}) {
    let consumed = false;
    const c = code || keyCode(key);
    for (let i = 0; i < count; i++) {
      consumed = (await this.cmd('key', { key, code: c, down: true, repeat: false, mods: modsOf(mods) })) || consumed;
      if (hold) await this.wait(hold);
      await this.cmd('key', { key, code: c, down: false, repeat: false, mods: modsOf(mods) });
      await this.frame();
      if (i + 1 < count) await this.wait(interval);
    }
    return consumed;
  }

  async type(text, { interval = 0 } = {}) {
    for (const ch of String(text)) {
      if (ch === '\n') {
        await this.key('Enter');
        continue;
      }
      const code = keyCode(ch);
      this.cmd('key', { key: ch, code, down: true, repeat: false, mods: 0 }).catch(() => {});
      this.cmd('text', { text: ch }).catch(() => {});
      const up = this.cmd('key', { key: ch, code, down: false, repeat: false, mods: 0 });
      if (interval) {
        await up;
        await this.wait(interval);
      } else up.catch(() => {});
    }
    return this.frame();
  }

  async focus(target) {
    await this.click(target);
    return this;
  }

  async _pad(pad) {
    if (this._padConnected) return;
    await this.cmd('padConnect', { pad, connected: true, name: 'jsui-cli standard gamepad' });
    this._padConnected = true;
  }

  async press(button, { pad = 0, hold = 80, count = 1, interval = 200 } = {}) {
    const b = typeof button === 'number' ? button : PAD_BUTTONS[String(button).toLowerCase().replace(/[\s_-]/g, '')];
    if (b == null) throw new Error(`jsui: unknown gamepad button ${JSON.stringify(button)}`);
    await this._pad(pad);
    let consumed = false;
    for (let i = 0; i < count; i++) {
      consumed = (await this.cmd('padButton', { pad, button: b, down: true, value: 1 })) || consumed;
      await this.wait(hold);
      await this.cmd('padButton', { pad, button: b, down: false, value: 0 });
      await this.frame();
      if (i + 1 < count) await this.wait(interval);
    }
    return consumed;
  }

  async stick(axis, value, { pad = 0, duration = 300 } = {}) {
    const a = typeof axis === 'number' ? axis : PAD_AXES[String(axis).toLowerCase()];
    if (a == null) throw new Error(`jsui: unknown gamepad axis ${JSON.stringify(axis)}`);
    await this._pad(pad);
    await this.cmd('padAxis', { pad, axis: a, value });
    await this.wait(duration);
    await this.cmd('padAxis', { pad, axis: a, value: 0 });
    return this.frame();
  }

  async setState(key, value) {
    await this.cmd('setState', { key: String(key), value: value === undefined ? null : value });
    return 1;
  }

  async patchState(key, patch) {
    await this.cmd('patchState', { key: String(key), value: patch });
    return 1;
  }

  async getState(key) {
    const v = await this.cmd('getState', { key: String(key) });
    return v === null ? undefined : v;
  }

  async setBuffer(key, values, type = 'f32') {
    const t = BUFFER_TYPES[type];
    if (!t) throw new Error(`jsui: unknown buffer type ${type}`);
    const arr = values instanceof t[1] ? values : t[1].from(values);
    const data = Buffer.from(arr.buffer, arr.byteOffset, arr.byteLength).toString('base64');
    await this.cmd('setBuffer', { key: String(key), type: t[0], data });
    return 1;
  }

  async postMessage(name, data) {
    await this.cmd('postMessage', { name: String(name), data: data === undefined ? null : data });
    return this;
  }

  handle() {
    throw new Error(`jsui: ${this.label} answers Host.call itself (answers / handle() work with \`jsui dev --headless\` only)`);
  }

  async run(component, props = {}) {
    await this.cmd('run', { component: component == null ? null : String(component), props });
    return true;
  }

  async reload() {
    await this.cmd('reload');
    await this.frame();
    return this.idle(10000);
  }

  resize() {
    throw new Error(`jsui: ${this.label} owns its viewport size (resize works with \`jsui dev --headless\` only)`);
  }

  async hitTest(target) {
    const p = await this.point(target);
    const f = await this.cmd('hitTest', p);
    return { pointer: !!(f & 1), scroll: !!(f & 2) };
  }
}

// The inspector-based lookups only use callUi().
for (const m of ['tree', 'findAll', 'find', 'get', 'point']) RemoteRuntime.prototype[m] = HeadlessApp.prototype[m];
