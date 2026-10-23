// Client of `jsui dev --shared` (see shared.js): drives the live app that a
// person is watching in the /shared viewer.
//
//   import { attach } from '@orktes/jsui-cli';
//   const app = await attach('http://127.0.0.1:5173');
//   await app.press('Down');
//   await app.click('#play');
//   await app.screenshot('shot.png');
//
// The methods are those of launch()'s app, all async (setState, getState,
// run, ... too). The clock is real: wait(500) takes 500 ms. handle() takes
// an answer value ({value} / {error}), not a function. close() detaches
// without stopping the shared app.

import { mkdir, writeFile } from 'node:fs/promises';
import path from 'node:path';

const METHODS = [
  'wait', 'frame', 'idle', 'click', 'tap', 'hover', 'drag', 'wheel', 'key', 'type', 'focus', 'press', 'stick',
  'setState', 'patchState', 'getState', 'setBuffer', 'postMessage', 'run', 'resize', 'hitTest', 'tree', 'find',
  'findAll', 'get', 'point', 'callUi', 'activity', 'reload',
];

/** Normalizes 'localhost:5173', 'http://host:port/shared', ... to the server origin. */
export function sharedOrigin(url) {
  let u = String(url || '').trim();
  if (!u) throw new Error('jsui: --attach needs the dev server URL (jsui dev --shared prints it)');
  if (!/^https?:\/\//.test(u)) u = 'http://' + u;
  return new URL(u).origin;
}

async function request(origin, p, body) {
  let res;
  try {
    res = await fetch(origin + p, body === undefined ? { cache: 'no-store' } : {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    });
  } catch (e) {
    throw new Error(`jsui: cannot reach ${origin} (is \`jsui dev --shared\` running?): ${e.cause?.message || e.message}`);
  }
  if (res.status === 404) throw new Error(`jsui: ${origin} has no shared view (start it with \`jsui dev <entry> --shared\`)`);
  const json = await res.json().catch(() => null);
  if (!json) throw new Error(`jsui: unexpected answer from ${origin}${p} (status ${res.status})`);
  if (res.status >= 400 && json.error) throw new Error('jsui: ' + json.error);
  return json;
}

class RemoteApp {
  constructor(origin, info) {
    this.origin = origin;
    this.width = info.width;
    this.height = info.height;
    this.scale = info.scale;
    this.time = 0;
    this.messages = [];
    this.calls = [];
    this.stateWrites = [];
    this.errors = [];
  }

  _sync(r) {
    if (r.time != null) this.time = r.time;
    for (const k of ['messages', 'calls', 'stateWrites', 'errors']) if (Array.isArray(r[k])) this[k] = r[k];
  }

  async _call(method, args) {
    const r = await request(this.origin, '/__jsui/shared/call', { method, args });
    this._sync(r);
    if (r.error) throw new Error(r.error);
    return r.value;
  }

  ready() {
    return Promise.resolve(this);
  }

  /** PNG of the current frame; written to `file` when given. */
  async screenshot(file) {
    const v = await this._call('screenshot', []);
    const png = Buffer.from(v.base64, 'base64');
    if (file) {
      await mkdir(path.dirname(path.resolve(file)), { recursive: true });
      await writeFile(file, png);
    }
    return png;
  }

  /** Answers the UI's Host.call(name) from now on: a value, or {value} / {error}. */
  async handle(name, answer) {
    if (typeof answer === 'function') throw new Error('jsui: attach(): handle() takes a value or {value} / {error}, not a function');
    await this._call('handle', [name, answer]);
    return this;
  }

  /** Runs `jsui run` steps on the server; returns {ok, failed, results}. */
  async runSteps(steps, { keepGoing = false, calls } = {}) {
    const r = await request(this.origin, '/__jsui/shared/run', { steps: absolutePaths(steps), keepGoing, calls });
    this._sync(r);
    return r;
  }

  /** Detaches (the shared app keeps running). */
  close() {}
}

for (const m of METHODS) {
  RemoteApp.prototype[m] = function remote(...args) {
    return this._call(m, args);
  };
}

/** Screenshot steps write on the server: give it absolute paths. */
export function absolutePaths(steps) {
  return steps.map((s) => (s && typeof s.screenshot === 'string' ? { ...s, screenshot: path.resolve(s.screenshot) } : s));
}

/** Connects to a `jsui dev --shared` server. */
export async function attach(url) {
  const origin = sharedOrigin(url);
  const info = await request(origin, '/__jsui/shared/info');
  return new RemoteApp(origin, info);
}
