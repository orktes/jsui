// Headless JSUI: runs a bundle in Node on the CPU raster of the WebAssembly
// runtime (@orktes/jsui-web's dist/ module), on a simulated clock. Drives
// `jsui screenshot`, `jsui inspect` and `jsui run`, and is the programmatic
// `launch()` API for scripts and tests:
//
//   const app = await launch({ entry: 'app.jsx', width: 1280, height: 720 });
//   await app.click('#start');            // testID
//   await app.press('A');                  // gamepad A
//   await app.wait(500);                   // advance the UI clock 500 ms
//   await app.screenshot('after.png');
//   app.close();
//
// Rendering is deterministic: the same bundle and steps give the same pixels.

import * as esbuild from 'esbuild';
import { existsSync, readFileSync } from 'node:fs';
import { mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { esbuildOptions } from './config.js';
import { formatErrors } from './build.js';

const require = createRequire(import.meta.url);

/** Directory with the CPU module (jsui.js + jsui.wasm), or null. */
export function findWasmDir(dir) {
  const candidates = [];
  if (dir) candidates.push(dir);
  if (process.env.JSUI_WASM_DIR) candidates.push(process.env.JSUI_WASM_DIR);
  try {
    candidates.push(path.join(path.dirname(require.resolve('@orktes/jsui-web/package.json')), 'dist'));
  } catch {
    // not installed
  }
  for (const c of candidates) if (existsSync(path.join(c, 'jsui.wasm')) && existsSync(path.join(c, 'jsui.js'))) return path.resolve(c);
  return null;
}

let modulePromise = null;
let moduleDir = null;
async function loadModule(wasmDir) {
  const dir = findWasmDir(wasmDir);
  if (!dir) {
    throw new Error(
      'jsui: the JSUI WebAssembly runtime was not found. Install @orktes/jsui-web (it ships dist/jsui.wasm), ' +
        'or set JSUI_WASM_DIR to a directory with jsui.js and jsui.wasm (in the jsui repo: scripts/build-wasm.sh -> web/dist).',
    );
  }
  if (modulePromise && moduleDir === dir) return modulePromise;
  moduleDir = dir;
  installFileFetch();
  modulePromise = (async () => {
    const factory = (await import(pathToFileURL(path.join(dir, 'jsui.js')).href)).default;
    const M = await factory({ wasmBinary: readFileSync(path.join(dir, 'jsui.wasm')), print: () => {}, printErr: () => {} });
    M.__jsuiHosts ??= new Map();
    return M;
  })();
  return modulePromise;
}

// The runtime loads the bundle's images, fonts and fetch() with the global
// fetch, which in Node does not read file: URLs. Serve those from disk.
let fetchInstalled = false;
function installFileFetch() {
  if (fetchInstalled) return;
  fetchInstalled = true;
  const realFetch = globalThis.fetch;
  globalThis.fetch = async function jsuiFetch(input, init) {
    const url = typeof input === 'string' ? input : input && input.url ? input.url : String(input);
    if (url.startsWith('file:')) {
      try {
        return new Response(readFileSync(fileURLToPath(url)), { status: 200 });
      } catch {
        return new Response('', { status: 404, statusText: 'Not Found' });
      }
    }
    return realFetch(input, init);
  };
}

// Builds `entry` into dir/bundle.js. Entries that export a component but
// register none get it registered as 'main', so a single screen file works.
async function buildEntry(entry, dir, mode) {
  const abs = path.resolve(entry);
  const outfile = path.join(dir, 'bundle.js');
  const assets = new Map();
  const opts = esbuildOptions({ entry: abs, outfile, mode, assets });
  delete opts.entryPoints;
  const spec = JSON.stringify('./' + path.basename(abs));
  opts.stdin = {
    contents:
      `import * as Entry from ${spec};\n` +
      `import { AppRegistry } from 'react-native';\n` +
      `const C = Entry['default'];\n` +
      `if (AppRegistry.getAppKeys().length === 0 && typeof C === 'function') AppRegistry.registerComponent('main', () => C);\n`,
    resolveDir: path.dirname(abs),
    sourcefile: '<jsui-headless>',
    loader: 'jsx',
  };
  let result;
  try {
    result = await esbuild.build(opts);
  } catch (e) {
    if (e && e.errors) throw new Error('jsui: build failed\n' + (await formatErrors(e.errors)));
    throw e;
  }
  for (const f of result.outputFiles) {
    await mkdir(path.dirname(f.path), { recursive: true });
    await writeFile(f.path, f.contents);
  }
  for (const [rel, buf] of assets) {
    const p = path.join(dir, rel);
    await mkdir(path.dirname(p), { recursive: true });
    await writeFile(p, buf);
  }
  return outfile;
}

const BUFFER_TYPES = { f32: [0, Float32Array], f64: [1, Float64Array], i32: [2, Int32Array], u32: [3, Uint32Array], i16: [4, Int16Array], u16: [5, Uint16Array], i8: [6, Int8Array], u8: [7, Uint8Array] };

/** W3C standard gamepad mapping. */
export const PAD_BUTTONS = {
  a: 0, cross: 0, b: 1, circle: 1, x: 2, square: 2, y: 3, triangle: 3,
  lb: 4, l1: 4, rb: 5, r1: 5, lt: 6, l2: 6, rt: 7, r2: 7,
  back: 8, select: 8, view: 8, start: 9, menu: 9, options: 9, ls: 10, l3: 10, rs: 11, r3: 11,
  up: 12, dpadup: 12, down: 13, dpaddown: 13, left: 14, dpadleft: 14, right: 15, dpadright: 15, home: 16,
};
const PAD_AXES = { lx: 0, ly: 1, rx: 2, ry: 3 };
const MODS = { shift: 1, ctrl: 2, control: 2, alt: 4, option: 4, meta: 8, cmd: 8, command: 8 };

function keyCode(key) {
  if (key === ' ') return 'Space';
  if (/^[a-z]$/i.test(key)) return 'Key' + key.toUpperCase();
  if (/^[0-9]$/.test(key)) return 'Digit' + key;
  if (key.length > 1) return key;
  return null;
}

function modsOf(m) {
  if (typeof m === 'number') return m;
  let bits = 0;
  for (const name of [].concat(m || [])) bits |= MODS[String(name).toLowerCase()] || 0;
  return bits;
}

// --------------------------------------------------------------- selectors

/**
 * Selector forms (strings):
 *   '#play'          testID (or nativeID) "play"
 *   'text=Continue'  an element whose text is exactly "Continue" (trimmed)
 *   'Continue'       an element whose text contains "Continue" (case-insensitive)
 *   'label=Close'    accessibilityLabel / aria-label
 *   'type=TextInput' host type (View, Text, Image, ScrollView, TextInput, ...)
 * or an object { testID, text, textContains, label, type, pressable, index }.
 * A trailing ' >> n' (or `index`) picks the n-th match (0-based).
 */
export function parseSelector(sel) {
  if (sel && typeof sel === 'object' && !Array.isArray(sel)) return sel;
  let s = String(sel).trim();
  let index;
  const m = /\s*>>\s*(\d+)$/.exec(s);
  if (m) {
    index = +m[1];
    s = s.slice(0, m.index);
  }
  let q;
  if (s.startsWith('#')) q = { testID: s.slice(1) };
  else if (s.startsWith('text=')) q = { text: s.slice(5) };
  else if (s.startsWith('label=')) q = { label: s.slice(6) };
  else if (s.startsWith('type=')) q = { type: s.slice(5) };
  else q = { textContains: s };
  if (index !== undefined) q.index = index;
  return q;
}

function matches(el, q) {
  if (q.testID != null && el.testID !== q.testID && el.nativeID !== q.testID) return false;
  if (q.type != null && el.type !== q.type) return false;
  if (q.label != null && el.label !== q.label) return false;
  if (q.pressable != null && !!el.pressable !== !!q.pressable) return false;
  if (q.text != null && (el.text == null || el.text.trim() !== String(q.text).trim())) return false;
  if (q.textContains != null) {
    const hay = el.text != null ? el.text : el.label != null ? el.label : el.value != null ? el.value : el.placeholder;
    if (hay == null || !hay.toLowerCase().includes(String(q.textContains).toLowerCase())) return false;
  }
  return true;
}

function flatten(nodes, out = []) {
  for (const n of nodes) {
    out.push(n);
    if (n.children) flatten(n.children, out);
  }
  return out;
}

/** One line per element: `Type #testID "text" [flags] x,y wxh`, indented by depth. */
export function formatTree(tree) {
  const lines = [];
  const fmt = (n) => Math.round(n * 10) / 10;
  const walk = (el, depth) => {
    let line = '  '.repeat(depth) + el.type;
    if (el.testID) line += ' #' + el.testID;
    if (el.nativeID && el.nativeID !== el.testID) line += ' nativeID=' + el.nativeID;
    if (el.text != null) line += ' ' + JSON.stringify(el.text.length > 80 ? el.text.slice(0, 77) + '...' : el.text);
    if (el.label != null) line += ' label=' + JSON.stringify(el.label);
    if (el.value != null) line += ' value=' + JSON.stringify(el.value);
    if (el.placeholder != null) line += ' placeholder=' + JSON.stringify(el.placeholder);
    const flags = ['pressable', 'focused', 'disabled', 'hidden'].filter((f) => el[f]);
    if (el.role) flags.unshift('role=' + el.role);
    if (flags.length) line += ' [' + flags.join(' ') + ']';
    const r = el.rect;
    line += `  ${fmt(r.x)},${fmt(r.y)} ${fmt(r.width)}x${fmt(r.height)}`;
    lines.push(line);
    // Text children are already in the text.
    if (el.type !== 'Text') for (const c of el.children || []) walk(c, depth + 1);
  };
  for (const root of tree.roots) {
    const vp = root.viewport;
    lines.push(`surface ${root.rootTag}` + (vp ? ` ${vp.width}x${vp.height} @${vp.scale}x` : ''));
    for (const c of root.children) walk(c, 1);
  }
  return lines.join('\n');
}

/** Strips children for compact output. */
export function elementSummary(el) {
  if (!el) return null;
  const { children, ...rest } = el;
  return rest;
}

// ------------------------------------------------------------------- app

let nextHostId = 0x4a5000;

/**
 * Starts a headless runtime.
 * @param {object} o
 * @param {string} [o.entry]     app source (.jsx/.tsx/.js); built with the CLI's esbuild config
 * @param {string} [o.bundle]    an already built bundle.js (its assets next to it)
 * @param {number} [o.width=1280] @param {number} [o.height=720] logical px
 * @param {number} [o.scale=1]   pixel ratio (2: a Retina-sized PNG)
 * @param {number} [o.fps=60]    frames per simulated second
 * @param {string} [o.component] AppRegistry key for the main surface (default: the first registered)
 * @param {object} [o.props]     its initial props (without `component`: for 'main')
 * @param {object} [o.state]     shared state set before the bundle runs ({key: value})
 * @param {object} [o.calls]     Host.call answers: {name: value | (args) => value}
 * @param {boolean} [o.dev]      development React build (better errors)
 * @param {string} [o.wasmDir]   directory with jsui.js + jsui.wasm (CPU build)
 * @param {(level: number, text: string) => void} [o.onLog]
 */
export async function launch(o = {}) {
  if (!o.entry && !o.bundle) throw new Error('launch: entry or bundle is required');
  const M = await loadModule(o.wasmDir);
  let tmp = null;
  let bundlePath;
  if (o.entry) {
    tmp = await mkdtemp(path.join(tmpdir(), 'jsui-headless-'));
    bundlePath = await buildEntry(o.entry, tmp, o.dev ? 'development' : 'production');
  } else {
    bundlePath = path.resolve(o.bundle);
  }
  return new HeadlessApp(M, bundlePath, tmp, o);
}

class HeadlessApp {
  constructor(M, bundlePath, tmp, o) {
    this.M = M;
    this.tmp = tmp;
    this.width = o.width || 1280;
    this.height = o.height || 720;
    this.scale = o.scale || 1;
    this.frameMs = 1000 / (o.fps || 60);
    this.time = 0;
    this.messages = [];  // Host.postMessage from the UI: {name, data, time}
    this.logs = [];      // console output of the bundle: {level, text}
    this.calls = [];     // Host.call from the UI: {name, args, time}
    this.stateWrites = []; // HostState.set / patch from the UI: {key, value, time}
    this.errors = [];    // uncaught JS errors
    this._handlers = new Map(Object.entries(o.calls || {}));
    this._uiCalls = new Map();
    this._answering = 0;   // Host.calls being answered
    this._padConnected = false;
    this._pointer = { x: this.width / 2, y: this.height / 2 };
    this._onLog = o.onLog;
    this.hostId = nextHostId++;
    M.__jsuiHosts.set(this.hostId, {
      onLog: (level, text) => {
        this.logs.push({ level, text });
        if (this._onLog) this._onLog(level, text);
      },
      onMessage: (name, json) => {
        let data = null;
        if (json !== '') try { data = JSON.parse(json); } catch { data = json; }
        this.messages.push({ name, data, time: this.time });
      },
      onState: (key, json) => {
        let value;
        try { value = json == null ? null : JSON.parse(json); } catch { value = json; }
        this.stateWrites.push({ key, value, time: this.time });
      },
      onCall: (id, name, json) => {
        let args;
        try { args = JSON.parse(json); } catch { args = undefined; }
        this.calls.push({ name, args, time: this.time });
        // Answer outside on_call (the runtime must not be re-entered from it);
        // frames yield to the event loop while answers are pending.
        this._answering++;
        queueMicrotask(() => {
          if (!this.rt) return void this._answering--;
          if (!this._handlers.has(name)) {
            this._answering--;
            return void this._reject(id, `no handler for Host.call("${name}") (pass calls: {"${name}": ...})`);
          }
          const h = this._handlers.get(name);
          Promise.resolve()
            .then(() => (typeof h === 'function' ? h(args) : h))
            .then((v) => this.rt && this._withStrings([JSON.stringify(v === undefined ? null : v)], (j) => M._jsui_web_resolve_call(this.rt, id, j)),
              (e) => this._reject(id, e && e.message !== undefined ? e.message : String(e)))
            .finally(() => this._answering--);
        });
      },
      onCallResult: (id, ok, payload) => {
        const p = this._uiCalls.get(id);
        if (!p) return;
        this._uiCalls.delete(id);
        if (!ok) return void p.reject(new Error(payload));
        try { p.resolve(JSON.parse(payload)); } catch (e) { p.reject(e); }
      },
    });
    this.bundleUrl = pathToFileURL(bundlePath).href;
    this._baseUrlPtr = this._str(this.bundleUrl);
    this.rt = M._jsui_web_create(this.hostId, this.width, this.height, this.scale, 0, this._baseUrlPtr);
    if (!this.rt) throw new Error('jsui: runtime creation failed');
    for (const [k, v] of Object.entries(o.state || {})) this.setState(k, v);
    // Props alone go to 'main' (what single-screen entries register).
    if (o.component != null || o.props != null) this.run(o.component ?? 'main', o.props || {});
    const src = readFileSync(bundlePath, 'utf8');
    const len = M.lengthBytesUTF8(src);
    const sp = M._malloc(len + 1);
    M.stringToUTF8(src, sp, len + 1);
    const r = this._withStrings([this.bundleUrl], (n) => M._jsui_load_bundle_source(this.rt, sp, len, n));
    M._free(sp);
    this._checkError();
    if (r !== 0 && !this.errors.length) this.errors.push('bundle evaluation failed');
    this._ready = this._settle();
  }

  _str(s) {
    const M = this.M;
    const n = M.lengthBytesUTF8(s) + 1;
    const p = M._malloc(n);
    M.stringToUTF8(s, p, n);
    return p;
  }

  _withStrings(strs, fn) {
    const ptrs = strs.map((s) => (s == null ? 0 : this._str(s)));
    try {
      return fn(...ptrs);
    } finally {
      for (const p of ptrs) if (p) this.M._free(p);
    }
  }

  _reject(id, message) {
    if (this.rt) this._withStrings([message], (m) => this.M._jsui_web_reject_call(this.rt, id, m));
  }

  _checkError() {
    const p = this.M._jsui_last_error(this.rt);
    const e = p ? this.M.UTF8ToString(p) : '';
    if (e && this.errors[this.errors.length - 1] !== e) this.errors.push(e);
  }

  _alive() {
    if (!this.rt) throw new Error('jsui: app is closed');
  }

  _tick(advance = true) {
    if (advance) this.time += this.frameMs;
    this.M._jsui_tick(this.rt, this.time);
    this._checkError();
  }

  // Lets fetch() promises (images, fonts) settle between frames.
  async _yield() {
    await new Promise((r) => setImmediate(r));
  }

  // Runs frames until nothing is loading (bundle, images, fonts), then a few more.
  async _settle(maxMs = 10000) {
    const start = Date.now();
    let quiet = 0;
    while (quiet < 3 && Date.now() - start < maxMs) {
      this._tick();
      if (this.M._jsui_pending_loads(this.rt) > 0) {
        quiet = 0;
        await this._yield();
      } else {
        quiet++;
      }
    }
  }

  /** Resolves once the first frame is loaded. Every method awaits it. */
  ready() {
    return this._ready;
  }

  /** Advances the UI clock by `ms` (animations, timers, scrolling), frame by frame. */
  async wait(ms = 0) {
    this._alive();
    await this._ready;
    const end = this.time + Math.max(0, ms);
    while (this.time + this.frameMs <= end + 1e-6) {
      this._tick();
      if (this._answering || this.M._jsui_pending_loads(this.rt) > 0) await this._yield();
    }
    return this;
  }

  /** Runs one frame (state, buffers and messages set since the last frame reach the UI). */
  async frame() {
    return this.wait(this.frameMs);
  }

  /** Runs frames until nothing loads and nothing animates (or `maxMs` of UI time passes). */
  async idle(maxMs = 5000) {
    this._alive();
    await this._ready;
    const end = this.time + maxMs;
    let quiet = 0;
    while (quiet < 2 && this.time < end) {
      this._tick();
      const loading = this._answering > 0 || this.M._jsui_pending_loads(this.rt) > 0;
      if (loading) await this._yield();
      quiet = !loading && !this.M._jsui_wants_frame(this.rt) ? quiet + 1 : 0;
    }
    return this;
  }

  /** PNG of the current frame (width * scale x height * scale). Writes it to `file` when given. */
  async screenshot(file) {
    this._alive();
    await this._ready;
    const M = this.M;
    const pp = M._malloc(8);
    try {
      if (M._jsui_render_png(this.rt, pp, pp + 4) !== 0) throw new Error('jsui: render failed');
      const ptr = M.HEAPU32[pp >> 2];
      const size = M.HEAPU32[(pp + 4) >> 2];
      const png = Buffer.from(M.HEAPU8.slice(ptr, ptr + size));
      M._jsui_free(ptr);
      if (file) {
        await mkdir(path.dirname(path.resolve(file)), { recursive: true });
        await writeFile(file, png);
      }
      return png;
    } finally {
      M._free(pp);
    }
  }

  /** Calls a UI handler (Host.handle / useHostHandler); resolves with its result. */
  async callUi(name, args) {
    this._alive();
    await this._ready;
    const result = new Promise((resolve, reject) => {
      const id = this._withStrings([String(name), JSON.stringify(args === undefined ? null : args)], (n, j) =>
        this.M._jsui_web_call_ui(this.rt, this.hostId, n, j));
      this._uiCalls.set(id, { resolve, reject });
    });
    // Results are delivered at the end of a tick; handlers may be async.
    for (let i = 0; i < 600 && this._uiCalls.size; i++) {
      this._tick(i > 0);
      if (this._uiCalls.size) await this._yield();
    }
    return result;
  }

  /** The view tree: {roots: [{rootTag, viewport, children: [element]}], focused}. */
  async tree() {
    try {
      return await this.callUi('__jsui.inspect');
    } catch (e) {
      if (/no handler/i.test(e.message)) throw new Error('jsui: this bundle has no inspector (build it with @orktes/jsui-react >= 0.3)');
      throw e;
    }
  }

  /** All elements matching `selector`, in tree order (outermost first). */
  async findAll(selector) {
    const q = parseSelector(selector);
    const t = await this.tree();
    const all = flatten(t.roots.flatMap((r) => r.children));
    const found = all.filter((el) => matches(el, q));
    return q.index != null ? found.slice(q.index, q.index + 1) : found;
  }

  /** The first element matching `selector` (without children), or null. */
  async find(selector) {
    const [el] = await this.findAll(selector);
    return elementSummary(el) || null;
  }

  /** Like find(), but throws a helpful error when nothing matches. */
  async get(selector) {
    const el = await this.find(selector);
    if (!el) throw new Error(`jsui: no element matches ${JSON.stringify(selector)}`);
    return el;
  }

  /** Point of a target: [x, y], {x, y}, or a selector (the element's center). */
  async point(target) {
    if (Array.isArray(target)) return { x: +target[0], y: +target[1] };
    if (target && typeof target === 'object' && 'x' in target && 'y' in target && !('testID' in target)) return { x: +target.x, y: +target.y };
    const el = await this.get(target);
    if (!(el.rect.width > 0 && el.rect.height > 0)) throw new Error(`jsui: ${JSON.stringify(target)} has no size (hidden?)`);
    return { x: el.rect.x + el.rect.width / 2, y: el.rect.y + el.rect.height / 2 };
  }

  // ----------------------------------------------------------- pointer

  /** Moves the mouse to `target` (hover). */
  async hover(target) {
    const p = await this.point(target);
    this.M._jsui_send_mouse_move(this.rt, p.x, p.y);
    this._pointer = p;
    this._tick();
    return this;
  }

  /** Left click (move, press, release) at `target`. Returns whether the UI consumed it. */
  async click(target, { hold = 50, button = 0, mods } = {}) {
    const p = await this.point(target);
    const M = this.M;
    M._jsui_send_mouse_move(this.rt, p.x, p.y);
    const consumed = M._jsui_send_mouse_button(this.rt, p.x, p.y, button, 1, modsOf(mods)) !== 0;
    await this.wait(hold);
    M._jsui_send_mouse_button(this.rt, p.x, p.y, button, 0, modsOf(mods));
    this._pointer = p;
    this._tick();
    return consumed;
  }

  /** Touch tap at `target`. */
  async tap(target, { hold = 50 } = {}) {
    const p = await this.point(target);
    const consumed = this.M._jsui_send_touch(this.rt, 1, 0, p.x, p.y) !== 0;
    await this.wait(hold);
    this.M._jsui_send_touch(this.rt, 1, 2, p.x, p.y);
    this._tick();
    return consumed;
  }

  /** Drags from `from` to `to` over `duration` ms (mouse, or touch with {touch: true}). */
  async drag(from, to, { duration = 300, touch = false } = {}) {
    const a = await this.point(from);
    const b = await this.point(to);
    const M = this.M;
    const steps = Math.max(2, Math.round(duration / this.frameMs));
    if (touch) M._jsui_send_touch(this.rt, 1, 0, a.x, a.y);
    else {
      M._jsui_send_mouse_move(this.rt, a.x, a.y);
      M._jsui_send_mouse_button(this.rt, a.x, a.y, 0, 1, 0);
    }
    this._tick();
    for (let i = 1; i <= steps; i++) {
      const x = a.x + ((b.x - a.x) * i) / steps;
      const y = a.y + ((b.y - a.y) * i) / steps;
      if (touch) M._jsui_send_touch(this.rt, 1, 1, x, y);
      else M._jsui_send_mouse_move(this.rt, x, y);
      this._tick();
    }
    if (touch) M._jsui_send_touch(this.rt, 1, 2, b.x, b.y);
    else M._jsui_send_mouse_button(this.rt, b.x, b.y, 0, 0, 0);
    this._pointer = b;
    this._tick();
    return this;
  }

  /** Mouse wheel over `target` (default: the last pointer position). dy > 0 scrolls down. */
  async wheel(target, dy = 120, dx = 0) {
    const p = target == null ? this._pointer : await this.point(target);
    this.M._jsui_send_mouse_move(this.rt, p.x, p.y);
    const consumed = this.M._jsui_send_mouse_wheel(this.rt, p.x, p.y, dx, dy) !== 0;
    this._pointer = p;
    this._tick();
    return consumed;
  }

  // ---------------------------------------------------------- keyboard

  /** Presses a key (DOM `key`: 'Enter', 'ArrowDown', 'Tab', 'Escape', 'a', ...). */
  async key(key, { code, mods, hold = 0, count = 1, interval = 100 } = {}) {
    await this._ready;
    const M = this.M;
    let consumed = false;
    for (let i = 0; i < count; i++) {
      const c = code || keyCode(key);
      consumed = this._withStrings([key, c], (k, cc) => M._jsui_send_key(this.rt, k, cc, 1, 0, modsOf(mods))) !== 0 || consumed;
      if (hold) await this.wait(hold);
      this._withStrings([key, c], (k, cc) => M._jsui_send_key(this.rt, k, cc, 0, 0, modsOf(mods)));
      this._tick();
      if (i + 1 < count) await this.wait(interval);
    }
    return consumed;
  }

  /** Types text into the focused TextInput (key + text events per character). */
  async type(text, { interval = 0 } = {}) {
    await this._ready;
    const M = this.M;
    for (const ch of String(text)) {
      if (ch === '\n') {
        await this.key('Enter');
        continue;
      }
      const c = keyCode(ch);
      this._withStrings([ch, c], (k, cc) => M._jsui_send_key(this.rt, k, cc, 1, 0, 0));
      this._withStrings([ch], (t) => M._jsui_send_text(this.rt, t));
      this._withStrings([ch, c], (k, cc) => M._jsui_send_key(this.rt, k, cc, 0, 0, 0));
      if (interval) await this.wait(interval);
    }
    this._tick();
    return this;
  }

  /** Focuses `target` by clicking it (TextInputs, pressables). */
  async focus(target) {
    await this.click(target);
    return this;
  }

  // ----------------------------------------------------------- gamepad

  _pad(pad) {
    if (!this._padConnected) {
      this._withStrings(['jsui-cli standard gamepad'], (id) => this.M._jsui_set_gamepad_connected(this.rt, pad, 1, id));
      this._padConnected = true;
    }
  }

  /** Presses a gamepad button: 'A', 'B', 'X', 'Y', 'LB', 'RB', 'LT', 'RT', 'Start', 'Back', 'Up', 'Down', 'Left', 'Right' or an index. */
  async press(button, { pad = 0, hold = 80, count = 1, interval = 200 } = {}) {
    await this._ready;
    const b = typeof button === 'number' ? button : PAD_BUTTONS[String(button).toLowerCase().replace(/[\s_-]/g, '')];
    if (b == null) throw new Error(`jsui: unknown gamepad button ${JSON.stringify(button)}`);
    this._pad(pad);
    let consumed = false;
    for (let i = 0; i < count; i++) {
      consumed = this.M._jsui_send_gamepad_button(this.rt, pad, b, 1, 1) !== 0 || consumed;
      await this.wait(hold);
      this.M._jsui_send_gamepad_button(this.rt, pad, b, 0, 0);
      this._tick();
      if (i + 1 < count) await this.wait(interval);
    }
    return consumed;
  }

  /** Holds a stick axis ('lx', 'ly', 'rx', 'ry' or 0-3) at `value` for `duration` ms, then centers it. */
  async stick(axis, value, { pad = 0, duration = 300 } = {}) {
    await this._ready;
    const a = typeof axis === 'number' ? axis : PAD_AXES[String(axis).toLowerCase()];
    if (a == null) throw new Error(`jsui: unknown gamepad axis ${JSON.stringify(axis)}`);
    this._pad(pad);
    this.M._jsui_send_gamepad_axis(this.rt, pad, a, value);
    await this.wait(duration);
    this.M._jsui_send_gamepad_axis(this.rt, pad, a, 0);
    this._tick();
    return this;
  }

  // ------------------------------------------------------- game data

  /** jsui_set_state (useHostState). `undefined` removes the key. */
  setState(key, value) {
    this._alive();
    const json = value === undefined ? null : JSON.stringify(value);
    return this._withStrings([key, json], (k, j) => this.M._jsui_set_state(this.rt, k, j));
  }

  /** jsui_patch_state: RFC 7386 merge patch. */
  patchState(key, patch) {
    this._alive();
    return this._withStrings([key, JSON.stringify(patch)], (k, j) => this.M._jsui_patch_state(this.rt, k, j));
  }

  /** Current value of a shared state key (also keys the UI owns), or undefined. */
  getState(key) {
    this._alive();
    const p = this._withStrings([key], (k) => this.M._jsui_get_state(this.rt, k));
    return p ? JSON.parse(this.M.UTF8ToString(p)) : undefined;
  }

  /** jsui_set_buffer: frame data (useHostBuffer*, Animated.hostBuffer). `type`: f32 (default), f64, i32, u32, i16, u16, i8, u8. */
  setBuffer(key, values, type = 'f32') {
    this._alive();
    const t = BUFFER_TYPES[type];
    if (!t) throw new Error(`jsui: unknown buffer type ${type}`);
    const arr = values instanceof t[1] ? values : t[1].from(values);
    const bytes = new Uint8Array(arr.buffer, arr.byteOffset, arr.byteLength);
    const M = this.M;
    const p = M._malloc(Math.max(1, bytes.length));
    M.HEAPU8.set(bytes, p);
    const r = this._withStrings([key], (k) => M._jsui_set_buffer(this.rt, k, t[0], p, arr.length));
    M._free(p);
    return r;
  }

  /** jsui_post_message (useHostMessage / Host.onMessage). */
  postMessage(name, data) {
    this._alive();
    this._withStrings([name, JSON.stringify(data === undefined ? null : data)], (n, j) => this.M._jsui_post_message(this.rt, n, j));
    return this;
  }

  /** Answers Host.call(name) from the UI: a value or (args) => value / promise. */
  handle(name, fn) {
    this._handlers.set(name, fn);
    return this;
  }

  /** Runs `component` on the main surface with `props` (null keeps the current one). */
  run(component, props = {}) {
    this._alive();
    return this._withStrings([component == null ? null : String(component), JSON.stringify(props)], (k, p) => this.M._jsui_surface_run(this.rt, k, p)) === 0;
  }

  /** New viewport size (and pixel ratio). */
  resize(width, height, scale = this.scale) {
    this._alive();
    this.width = width;
    this.height = height;
    this.scale = scale;
    this.M._jsui_set_viewport(this.rt, width, height, scale);
    this._tick();
    return this;
  }

  /** What the UI captures at a point: {pointer, scroll} (input elsewhere passes through to the game). */
  async hitTest(target) {
    const p = await this.point(target);
    const f = this.M._jsui_hit_test(this.rt, p.x, p.y);
    return { pointer: !!(f & 1), scroll: !!(f & 2) };
  }

  /** Destroys the runtime and removes the temporary build. */
  close() {
    if (!this.rt) return;
    this.M._jsui_destroy(this.rt);
    this.rt = 0;
    this.M._free(this._baseUrlPtr);
    this.M.__jsuiHosts.delete(this.hostId);
    if (this.tmp) rm(this.tmp, { recursive: true, force: true }).catch(() => {});
  }
}
