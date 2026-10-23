// JSUI browser host: runs the WASM build of the JSUI runtime on a <canvas>.
//
//   import { createJsui } from './jsui-web.js';
//   const jsui = await createJsui({ canvas, bundleUrl: './bundle.js' });
//   jsui.on('message', (name, data) => ...);
//   jsui.postMessage('hud', { hp: 42 });
//
// Renderers, best first ('auto', the default):
//
//   'webgpu'  Skia Graphite straight into the canvas' WebGPU texture
//             (jsui_render_webgpu, web/jsui-webgpu.js); small changes are
//             repainted into a retained frame texture. Needs the all-backends
//             module (GPU=1 scripts/build-wasm.sh -> web/dist-gpu/jsui.js),
//             which createJsui() loads first when navigator.gpu gives a device.
//   'webgl'   Skia Ganesh straight into the canvas' WebGL2 / WebGL 1 context.
//   'cpu'     CPU raster, blitted with WebGL (or Canvas2D, '2d').
//
// 'auto' tries them in that order and skips a GPU API the browser only offers
// through a software implementation (a fallback WebGPU adapter / SwiftShader
// WebGL): there Skia's GPU repaints cost more than the CPU raster's
// incremental ones. 'gpu' is the best GPU renderer, software ones included;
// 'webgpu' / 'webgl' ask for that one (falling down the chain if it fails).
// The chosen renderer is logged and is `jsui.renderer`.
//
// Skia Ganesh on WebGL (jsui_render_gl): no CPU raster and no
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
//
// Surfaces (React Native root views): one runtime, one bundle and JS context
// can show several AppRegistry components on separate canvases, each with
// its own props, size, input and focus (a HUD next to a menu, or small
// widgets placed inside an existing page):
//
//   AppRegistry.registerComponent('Clock', () => Clock);       // in the bundle
//   const clock = jsui.createSurface(el, { component: 'Clock', initialProps: { tz: 'UTC' } });
//   clock.setProps({ tz: 'CET' });   // re-renders, keeps its state
//   clock.destroy();
//
// The runtime ticks once per frame and every canvas paints what changed.
// Surface canvases share the runtime's GPU context, so a page can hold many
// GPU-drawn widgets (browsers cap live WebGL contexts at about 16):
//
//   WebGPU  one Skia Graphite context on the page's device draws every
//           surface; each canvas has only its own GPUCanvasContext.
//   WebGL   one offscreen WebGL context (SharedGL) draws each surface, and
//           the frame is copied into that canvas' 2D context in the same task.
//
// See JsuiSurfaceOptions.renderer.
//
// Host textures (include/jsui/jsui.h): pictures the page makes itself, shown
// in the UI with <HostTexture name="trailer" /> and useHostTexture('trailer').
// JSUI plays nothing itself; a video is one source among others:
//
//   jsui.setTexture('minimap', canvas);               // any time its picture changes
//   jsui.setTexture('cam', videoFrame); videoFrame.close();
//   const unbind = jsui.bindVideo('trailer', video);  // per presented frame
//   jsui.removeTexture('trailer');
//
// Sources: VideoFrame, <video>, <canvas>, OffscreenCanvas, ImageBitmap, <img>,
// ImageData. Textures belong to the runtime (every surface can show them) and
// are uploaded with the renderer in use: a GPUTexture on the page's WebGPU
// device (copyExternalImageToTexture), a texture of the views' WebGL context,
// or CPU pixels when the views cannot share one (web/jsui-textures.js). They
// follow renderer fallbacks and context loss by themselves.

/** @typedef {{
 *   canvas?: HTMLCanvasElement,  // the main surface's canvas; null / omitted: not shown (a page
 *                                // that only places createSurface() widgets)
 *   component?: string,          // the main surface's AppRegistry key (default: the bundle's first
 *                                // registered component, or what it renders itself)
 *   initialProps?: object,       // props for `component`
 *   bundleUrl?: string,          // URL of the JS bundle (resolved against the page)
 *   bundleSource?: string,       // or the bundle source itself
 *   devMode?: boolean,           // dev overlay + live reload; with an http:// bundle the
 *                                // runtime also joins `jsui dev`'s shared session (agents
 *                                // attached to the dev server can drive and screenshot it)
 *   devName?: string,            // this page's name in that session (default: 'web')
 *   devSession?: boolean,        // false: stay out of the shared session
 *   devPoll?: boolean,           // host-side polling of /__jsui/version. Default: only for
 *                                // https:// bundles; the runtime itself polls http:// origins
 *                                // (through this module's fetch loader) when devMode is set.
 *   pixelRatio?: number,         // default: window.devicePixelRatio (tracked live)
 *   moduleUrl?: string|URL,      // the WASM module to use. Default: picked from moduleBase by
 *                                // what the renderer may use: dist-gpu/ (WebGPU + WebGL + CPU)
 *                                // when a WebGPU device is available, dist-webgl/ when WebGL is,
 *                                // else dist/ (CPU), each falling back to the others when not
 *                                // deployed
 *   moduleBase?: string|URL,     // directory holding dist/, dist-webgl/, dist-gpu/ (default:
 *                                // next to this file)
 *   moduleVersions?: { [dir: string]: string },
 *                                // the modules deployed under moduleBase and their versions
 *                                // (content hashes), e.g. { 'dist': 'a1b2', 'dist-gpu': 'c3d4' }:
 *                                // others are not tried, and each URL is loaded as
 *                                // jsui.js?v=<version> (its jsui.wasm too). A query on moduleUrl
 *                                // is likewise passed on to its jsui.wasm.
 *   renderer?: 'auto'|'gpu'|'webgpu'|'webgl'|'cpu'|'2d',
 *                                // 'auto' (default): Skia on WebGPU, else Skia on WebGL (hardware
 *                                // implementations only), else the CPU raster. 'gpu': the same,
 *                                // software GPUs included. 'webgpu' / 'webgl': Skia on that API
 *                                // (any implementation), falling back down the chain. 'cpu': force
 *                                // the CPU raster, blitted with WebGL (or Canvas2D without WebGL).
 *                                // '2d': CPU raster blitted with Canvas2D.
 *   contextRestoreTimeout?: number, // ms to wait for a lost WebGL context to come back before
 *                                // falling back to the CPU on a fresh canvas (default 3000)
 *   autoFocus?: boolean,         // focus the keyboard sink on start (default true)
 *   gamepads?: boolean,          // poll the Gamepad API (default true)
 *   autoStart?: boolean,         // start the frame loop immediately (default true)
 *   trapTab?: boolean,           // keep Tab inside the UI even if unhandled (default true, false
 *                                // with passThrough; false lets an unhandled Tab move page focus on)
 *   passThrough?: boolean,       // the canvas is a layer over the page (a game's canvas): input the
 *                                // UI doesn't capture reaches what is under it, consumed input
 *                                // doesn't (default false). Turns autoFocus / trapTab off by default.
 * }} JsuiOptions */

import { SharedWebGPU, WebGPURenderer, requestWebGPU } from './jsui-webgpu.js';
import { HostTextureSet } from './jsui-textures.js';

export { requestWebGPU, webgpuSupported } from './jsui-webgpu.js';

const modules = new Map();  // module URL -> Promise<Module>

// The module's own files (jsui.wasm) are fetched with the module URL's query:
// a versioned URL (jsui.js?v=<hash>, see site/scripts/build.mjs) then names
// one build of both, so a browser cache cannot pair this jsui.js with a
// jsui.wasm of another deploy.
function locateIn(moduleUrl) {
  return (path) => {
    const u = new URL(path, moduleUrl);
    if (!u.search) u.search = new URL(moduleUrl).search;
    return u.href;
  };
}

function loadModule(moduleUrl) {
  const key = String(moduleUrl);
  let p = modules.get(key);
  if (!p) {
    p = import(/* @vite-ignore */ key).then((ns) => ns.default({ locateFile: locateIn(key) }));
    p.catch(() => modules.delete(key));
    modules.set(key, p);
  }
  return p;
}

// JsuiBufferType of a typed array (or its prototype).
const BUFFER_TYPES = [Float32Array, Float64Array, Int32Array, Uint32Array, Int16Array, Uint16Array, Int8Array, Uint8Array];
function bufferType(data) {
  const t = BUFFER_TYPES.findIndex((T) => data instanceof T);
  if (t < 0) throw new TypeError('setBuffer / mapBuffer: expected a Float32Array, Float64Array, Int32Array, Uint32Array, Int16Array, Uint16Array, Int8Array or Uint8Array');
  return t;
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
/** jsui_hit_test flags (view.hitTest()). */
export const HIT_POINTER = 1;  // presses / pointer input there are captured
export const HIT_SCROLL = 2;   // a ScrollView that can scroll (or a wheel handler) is there
const GAMEPAD_AXES = 4;
const AXIS_EPSILON = 1 / 256;

/**
 * Creates a JSUI runtime on `canvas` and starts its frame loop.
 * @param {JsuiOptions} opts
 */
export async function createJsui(opts) {
  // No canvas: the main surface is not shown, for pages that only place
  // createSurface() widgets.
  const canvas = opts.canvas ?? null;
  const webgpu = await webgpuFor(opts);
  return new JsuiHost(await loadModuleFor(opts, webgpu), canvas, { ...opts, _webgpu: webgpu });
}

const CPU_MODULE = './dist/jsui.js';
const GPU_MODULE = './dist-webgl/jsui.js';
const ALL_MODULE = './dist-gpu/jsui.js';  // WebGPU + WebGL + CPU

// The WebGPU device the renderer may use, or null (web/jsui-webgpu.js).
function webgpuFor(opts) {
  const mode = opts.renderer ?? 'auto';
  if (mode !== 'auto' && mode !== 'gpu' && mode !== 'webgpu') return null;
  return requestWebGPU({ allowSoftware: mode !== 'auto' });
}

// The GPU module when the renderer may use it and WebGL exists, else the
// CPU-only one; each falls back to the other if it is not deployed (both can
// run the CPU path).
// With a WebGPU device the all-backends module comes first (it can also do
// WebGL and CPU); the others are tried when it is not deployed.
async function loadModuleFor(opts, webgpu) {
  if (opts.moduleUrl) return loadModule(opts.moduleUrl);
  const mode = opts.renderer ?? 'auto';
  const gpu = mode === 'gpu' || mode === 'webgl' || mode === 'webgpu' ? webglAvailable()
    : mode === 'auto' && webglAvailable(true);
  const order = gpu ? [GPU_MODULE, CPU_MODULE] : [CPU_MODULE, GPU_MODULE];
  if (webgpu) order.unshift(ALL_MODULE);
  else order.push(ALL_MODULE);
  // moduleVersions: {dir: version} of the deployed modules; the others are
  // skipped, and each URL gets ?v=<version>.
  const versions = opts.moduleVersions;
  const errors = [];
  for (const m of order) {
    const dir = m.split('/')[1];
    if (versions && versions[dir] == null) continue;
    const url = new URL(m, opts.moduleBase ?? import.meta.url);
    if (versions) url.searchParams.set('v', versions[dir]);
    try {
      return await loadModule(url);
    } catch (e) {
      errors.push(`${url.href}: ${e?.message ?? e}`);
    }
  }
  // Name every module tried: browsers such as Safari report a failed import
  // as just "Importing a module script failed".
  throw new Error('could not load a JSUI module (' + (errors.join('; ') || 'none deployed (moduleVersions)') + ')');
}

// JsuiPerf's fields in order (include/jsui/jsui.h), for CanvasView.getPerf().
const PERF_TIMES = ['js_us', 'commit_us', 'animated_us', 'style_us', 'layout_us', 'input_us', 'paint_us', 'other_us',
  'gpu_submit_us', 'image_decode_us'];
const PERF_COUNTS = ['image_decodes', 'pipeline_compiles', 'paragraph_builds', 'text_measures', 'layout_passes',
  'bridge_calls'];

const webglProbe = {};
let noGpuWarned = false;  // CanvasView._warnNoGpu
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

// A canvas showing one surface of a runtime: its size, renderer (WebGPU /
// WebGL / CPU blit), input and cursor. JsuiHost is the main surface's view
// and owns the runtime; JsuiSurface views show the surfaces made with
// createSurface(). Every view talks to the runtime through its own handle
// (`rt`): the runtime handle for the main surface, a jsui_surface_create
// handle for the others (include/jsui/jsui.h, Surfaces).
class CanvasView {
  constructor(M, canvas, opts) {
    this.module = M;
    this.canvas = canvas;
    this.opts = opts;
    this.listeners = new Map();
    // renders: frames painted. blits / blitPixels: CPU path uploads and the
    // pixels they covered (lastBlit: the last uploaded rect, device px).
    this.stats = { frames: 0, renders: 0, tickMs: 0, renderMs: 0, blitMs: 0, blits: 0, blitPixels: 0, lastBlit: null };
    this._cursor = '';
    this._destroyed = false;
    this._abort = new AbortController();
    this._pressedKeys = new Map();  // code -> key, released on blur
    this._mouseButtons = 0;
    this._fixedRatio = opts.pixelRatio > 0 ? opts.pixelRatio : 0;
    this._trapTab = opts.trapTab ?? !opts.passThrough;
    // Auto-sized axes take the component's laid-out size (surfaces only).
    this._autoW = this._autoH = false;
    this._contentW = this._contentH = 0;
    this.width = this.height = this.pixelWidth = this.pixelHeight = 0;
    this.pixelRatio = 1;

    // Small reusable scratch area for short strings (key names, codes).
    this._scratch = M._malloc(512);
    this._dirty = M._malloc(32);  // JsuiRect filled by jsui_render_rgba_incremental / JsuiGLTarget
    this._buf = 0;
    this._bufSize = 0;
  }

  // The renderer, input and resize tracking, once `rt` exists.
  _initView(mode, webgpu) {
    this._mode = mode;
    this._webgpu = webgpu ?? null;
    this._initBlitter(mode);
    this._log(1, `renderer: ${this.renderer}` + (this._wgpu ? ` (${this._webgpu.description})` : ''));
    this._warnNoGpu(mode);
    this._installInput();
    this._installCanvasInput();
    this._installResize();
  }

  // A mode that may use the GPU ended on the CPU raster because the browser
  // has no hardware WebGPU or WebGL: say so once per page, since that is
  // several times slower and usually a browser setting or state, not JSUI.
  _warnNoGpu(mode) {
    if (noGpuWarned || mode === 'cpu' || mode === '2d') return;
    if (this.renderer === 'webgpu' || this.renderer.startsWith('gpu-') || webglAvailable(true)) return;
    noGpuWarned = true;
    this._log(2, 'no hardware GPU acceleration in this browser (WebGPU and WebGL are unavailable or software '
      + 'only), so the UI renders on the CPU, which is much slower. In Chrome, chrome://gpu shows why: after '
      + 'repeated GPU process crashes Chrome turns GPU access off until it is restarted.');
  }

  // The view that owns the runtime (gamepads, the frame loop).
  _hostOf() { return this; }

  /** Focuses the view's keyboard sink (replaced once input is installed). */
  focus() {}

  // Paints the surface if it changed (after the runtime's tick).
  _paint() {
    if (!this.canvas || this._destroyed) return;
    const M = this.module, rt = this.rt, st = this.stats;
    if (this._autoW || this._autoH) this._fitContent();
    if ((M._jsui_needs_redraw(rt) || this._sizeChanged) && this.pixelWidth > 0 && this.pixelHeight > 0) {
      const resized = this._sizeChanged;
      this._sizeChanged = false;
      const t1 = performance.now();
      let t2, t3;
      if (this._wgpu) {
        // Skia Graphite into the canvas' WebGPU texture (straight in when most
        // of the frame changed, else a damage-only repaint of its retained
        // frame + one copy).
        if (this._wgpu.render() === false) this._dropWebGPU('WebGPU frame failed');
        t2 = t3 = performance.now();
      } else if (this._sgl) {
        // Skia on the runtime's shared WebGL context, copied into this canvas.
        if (this._sgl.render(this) === false) this._switchToCpu('shared GPU frame failed', false, '2d');
        t2 = t3 = performance.now();
      } else if (this._gpu) {
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
    this._updateCursor();
  }

  // Shown and has something new to paint.
  _wantsPaint() { return !!this.canvas && !this._destroyed && !!this.module._jsui_needs_redraw(this.rt); }

  // The viewport handed to the runtime: 0 on auto-sized axes.
  _viewportSize() { return [this._autoW ? 0 : this.width, this._autoH ? 0 : this.height]; }

  // Auto-sized axes: the component's laid-out size (jsui_surface_get_size)
  // becomes the canvas' CSS size (and backing store).
  _fitContent() {
    const M = this.module;
    if (M._jsui_surface_get_size(this.rt, this._dirty, this._dirty + 4) !== 0) return;
    const f = new Float32Array(M.HEAPU8.buffer, this._dirty, 2);
    const w = Math.max(1, Math.ceil(f[0] - 0.001)), h = Math.max(1, Math.ceil(f[1] - 0.001));
    if (w === this._contentW && h === this._contentH) return;
    this._contentW = w;
    this._contentH = h;
    if (this._autoW) this.canvas.style.width = `${w}px`;
    if (this._autoH) this.canvas.style.height = `${h}px`;
    if (this._measure()) this._setViewport();
    this._rect = null;
  }
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

  /** The last rendered frame as premultiplied RGBA (copy), for tests and screenshots. */
  readPixels() {
    if (this._wgpu) return null;  // asynchronous on WebGPU: readPixelsAsync()
    if (this._sgl) return this._sgl.read(this);
    if (this._gpu) return this._readGpuPixels();
    if (!this._buf) return null;
    return { width: this.pixelWidth, height: this.pixelHeight,
      data: this.module.HEAPU8.slice(this._buf, this._buf + this._bufSize) };
  }

  /** readPixels() for every renderer (WebGPU reads back asynchronously). */
  async readPixelsAsync() {
    if (this._wgpu) return this._wgpu.readPixels();
    return this.readPixels();
  }

  /** Times every frame phase (jsui_set_profiling), for getPerf(). A runtime's
   * surfaces share its switch and totals. */
  setProfiling(on) {
    if (this.rt) this.module._jsui_set_profiling(this.rt, on ? 1 : 0);
  }

  /** What the frames since the last reset cost (jsui_get_perf): exclusive µs per
   * phase (`js_us`, `commit_us`, `animated_us`, `style_us`, `layout_us`,
   * `input_us`, `paint_us`, `other_us`, `gpu_submit_us`; with setProfiling(true))
   * and work counters. `reset` starts new totals. */
  getPerf(reset = true) {
    if (!this.rt) return null;
    const M = this.module, p = this._scratch;  // JsuiPerf: 10 doubles, 6 uint64 (128 bytes)
    M._jsui_get_perf(this.rt, p, reset ? 1 : 0);
    const v = new DataView(M.HEAPU8.buffer, p, 128);
    const out = {};
    PERF_TIMES.forEach((k, i) => { out[k] = v.getFloat64(i * 8, true); });
    PERF_COUNTS.forEach((k, i) => { out[k] = Number(v.getBigUint64(80 + i * 8, true)); });
    return out;
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

  /** What the UI captures at (x, y), CSS px from the canvas' top-left
   * (jsui_hit_test): HIT_POINTER / HIT_SCROLL flags, 0 where input passes
   * through to the page. */
  hitTest(x, y) { return this.rt ? this.module._jsui_hit_test(this.rt, x, y) : 0; }

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
      cssW = c ? c.clientWidth || c.width : 1;
      cssH = c ? c.clientHeight || c.height : 1;
    }
    // Auto-sized axes follow the content (see _fitContent).
    if (this._autoW) { cssW = this._contentW || 1; devW = devH = 0; }
    if (this._autoH) { cssH = this._contentH || 1; devW = devH = 0; }
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
    if (c) {
      c.width = devW;
      c.height = devH;
    }
    this._sizeChanged = true;
    return true;
  }

  _setViewport() {
    const [w, h] = this._viewportSize();
    this.module._jsui_set_viewport(this.rt, w, h, this.pixelRatio);
  }

  _installResize() {
    const apply = (entry) => {
      if (this._measure(entry) && this.rt) {
        this._setViewport();
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
    // WebGPU first (createJsui acquired the device only for the modes that
    // may use it), then Skia on WebGL, then the CPU raster.
    if (this._webgpu && (mode === 'auto' || mode === 'gpu' || mode === 'webgpu')) {
      this._wgpu = WebGPURenderer.create(this, this._webgpu, null, this._webgpuShare());
      if (this._wgpu) {
        this.renderer = 'webgpu';
        return;
      }
      this._webgpu = null;
    }
    if (this._initSharedGL(mode)) return;
    if (mode === 'auto' || mode === 'gpu' || mode === 'webgl' || mode === 'webgpu') {
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
      this._glGen = (this._glGen ?? 0) + 1;  // host textures: earlier GL objects are gone
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
    if (this._sgl) {
      // The canvas already has a 2D context: keep it, with the CPU raster.
      this._sgl.remove(this);
      this._sgl = null;
      freshCanvas = false;
      mode = '2d';
    }
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
    this._log(2, `${reason}: continuing with the ${this.renderer} renderer`);
    this._emit('renderer', this.renderer, reason);
  }

  // The WebGPU renderer failed for good: continue down the chain (Skia on
  // WebGL, then CPU) on a fresh canvas, since this one has a 'webgpu' context.
  _dropWebGPU(reason) {
    this._wgpu?.destroy();
    this._wgpu = null;
    this._webgpu = null;
    const mode = this._mode === 'cpu' || this._mode === '2d' ? this._mode : this._mode === 'auto' ? 'auto' : 'gpu';
    this._switchToCpu(reason, true, mode);
  }

  // After a WebGPU device loss (web/jsui-webgpu.js): `next` is the renderer
  // rebuilt on a new device, or null.
  _webgpuRecovered(next, reason) {
    if (this._destroyed) {
      next?.destroy();
      return;
    }
    if (!next) {
      this._dropWebGPU(`${reason}, no new device`);
      return;
    }
    this._wgpu = next;
    this._webgpu = next.webgpu;
    this._sizeChanged = true;  // full repaint on the next frame
    this._log(2, `${reason}: WebGPU renderer recreated`);
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
    if (this.opts.passThrough) {
      this._installPassThroughInput(signal);
      return;
    }
    c.style.touchAction = 'none';
    c.style.overscrollBehavior = 'none';
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
    // iOS Safari can still rubber-band the page under a touch-action: none
    // canvas; the canvas then moves under the finger and its scroll views see
    // phantom motion. A cancelled touchmove never pans or bounces the page.
    c.addEventListener('touchmove', (e) => { if (e.cancelable) e.preventDefault(); }, active);
    // The compat mousedown would move focus to <body> (the canvas is not
    // focusable), stealing it from the keyboard sink focused in pointerdown.
    c.addEventListener('mousedown', (e) => e.preventDefault(), active);
  }

  // passThrough: whatever the UI doesn't capture reaches the page under the
  // canvas (a game's own canvas or DOM), like a click on an empty part of a
  // HUD. The canvas has pointer-events: none ('auto' only while the mouse
  // hovers something the UI captures, for its cursor), and window capture
  // listeners route events: a press inside the canvas goes to the UI first.
  // When the UI consumes it (jsui_send_* returns non-zero), the event, its
  // gesture's moves and release and the click after it are stopped before the
  // page sees them; otherwise they go on to their target untouched. Touch
  // events (touchstart / touchmove / touchend) follow the hit test.
  _installPassThroughInput(signal) {
    const M = this.module, c = this.canvas;
    const opt = { signal, capture: true, passive: false };
    const stamp = (e) => M._jsui_set_event_time?.(this.rt, e.timeStamp);
    const inside = (x, y) => x >= 0 && y >= 0 && x < this.width && y < this.height && c.isConnected;
    const take = (e) => {
      if (e.cancelable) e.preventDefault();
      e.stopImmediatePropagation();
    };
    const gestures = new Map();  // pointerId -> whether the UI consumed its press
    const touches = new Set();   // Touch.identifier of touches the UI captures
    let hovering = false, swallowClick = false;
    const setHot = (hot) => {
      const v = hot ? 'auto' : 'none';
      if (c.style.pointerEvents !== v) c.style.pointerEvents = v;
    };
    setHot(false);
    addEventListener('pointerdown', (e) => {
      const [x, y] = this._point(e);
      if (!inside(x, y)) return;
      stamp(e);
      swallowClick = false;
      let used;
      if (e.pointerType === 'mouse') {
        this._mouseButtons |= 1 << e.button;
        used = M._jsui_send_mouse_button(this.rt, x, y, mouseButton(e.button), 1, modsOf(e));
      } else {
        used = M._jsui_send_touch(this.rt, e.pointerId, 0, x, y);
      }
      gestures.set(e.pointerId, !!used);
      if (used) {
        take(e);
        if (e.pointerType !== 'touch') this.focus();
      }
      this._updateCursor();
    }, opt);
    addEventListener('pointermove', (e) => {
      const [x, y] = this._point(e);
      const owned = gestures.get(e.pointerId);
      if (owned === undefined) {
        // Hover. A drag whose press the page got stays the page's.
        if (e.pointerType !== 'mouse') return;
        if (!inside(x, y)) {
          if (hovering) {
            hovering = false;
            M._jsui_send_mouse_leave(this.rt);
            setHot(false);
            this._updateCursor();
          }
          return;
        }
        hovering = true;
        stamp(e);
        const used = M._jsui_send_mouse_move(this.rt, x, y);
        setHot(!e.buttons && (M._jsui_hit_test(this.rt, x, y) & HIT_POINTER) !== 0);
        if (used && !e.buttons) e.stopImmediatePropagation();
        this._updateCursor();
        return;
      }
      stamp(e);
      if (e.pointerType === 'mouse') M._jsui_send_mouse_move(this.rt, x, y);
      else M._jsui_send_touch(this.rt, e.pointerId, 1, x, y);
      if (owned) take(e);
    }, opt);
    const up = (e, cancel) => {
      const owned = gestures.get(e.pointerId);
      if (owned === undefined) return;
      stamp(e);
      const [x, y] = this._point(e);
      if (e.pointerType === 'mouse') {
        if (!(this._mouseButtons & (1 << e.button))) return;
        this._mouseButtons &= ~(1 << e.button);
        M._jsui_send_mouse_button(this.rt, x, y, mouseButton(e.button), 0, modsOf(e));
        if (!this._mouseButtons) gestures.delete(e.pointerId);
      } else {
        M._jsui_send_touch(this.rt, e.pointerId, cancel ? 3 : 2, x, y);
        gestures.delete(e.pointerId);
      }
      if (owned) {
        take(e);
        swallowClick = !cancel;
      }
      this._updateCursor();
    };
    addEventListener('pointerup', (e) => up(e, false), opt);
    addEventListener('pointercancel', (e) => up(e, true), opt);
    // The click (and auxclick) of a press the UI consumed; right-clicks and
    // double clicks on what the UI captures.
    const captures = (e) => {
      const [x, y] = this._point(e);
      return inside(x, y) && (M._jsui_hit_test(this.rt, x, y) & HIT_POINTER) !== 0;
    };
    for (const type of ['click', 'auxclick']) {
      addEventListener(type, (e) => {
        if (swallowClick) take(e);
        swallowClick = false;
      }, opt);
    }
    addEventListener('dblclick', (e) => { if (captures(e)) take(e); }, opt);
    addEventListener('contextmenu', (e) => { if (captures(e)) take(e); }, opt);
    addEventListener('wheel', (e) => {
      const [x, y] = this._point(e);
      if (!inside(x, y)) return;
      const k = e.deltaMode === 1 ? 16 : e.deltaMode === 2 ? this.height : 1;
      if (M._jsui_send_mouse_wheel(this.rt, x, y, e.deltaX * k, e.deltaY * k)) take(e);
    }, opt);
    const touch = (e) => {
      let all = e.changedTouches.length > 0;
      for (const t of e.changedTouches) {
        if (e.type === 'touchstart') {
          const [x, y] = this._point(t);
          if (inside(x, y) && M._jsui_hit_test(this.rt, x, y) !== 0) touches.add(t.identifier);
        }
        if (!touches.has(t.identifier)) all = false;
        if (e.type === 'touchend' || e.type === 'touchcancel') touches.delete(t.identifier);
      }
      if (all) take(e);
    };
    for (const type of ['touchstart', 'touchmove', 'touchend', 'touchcancel']) addEventListener(type, touch, opt);
    addEventListener('blur', () => {
      gestures.clear();
      touches.clear();
      hovering = false;
      setHot(false);
    }, { signal });
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
      // passThrough: keys the UI consumed don't reach the page's listeners.
      if (used && this.opts.passThrough) e.stopPropagation();
    }, active);
    sink.addEventListener('keyup', (e) => {
      this._pressedKeys.delete(e.code || e.key);
      const used = this._withStrings([e.key || null, e.code || null], (k, cd) =>
        M._jsui_send_key(this.rt, k, cd, 0, 0, modsOf(e)));
      if (used && NAV_KEYS.has(e.key)) e.preventDefault();
      if (used && this.opts.passThrough) e.stopPropagation();
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
    // Gamepads drive the view that has (or last had) keyboard focus.
    sink.addEventListener('focus', () => {
      const host = this._hostOf();
      if ((host._padView ?? host) === this) return;
      host._releasePads();
      host._padView = this === host ? null : this;
    }, opt);
    addEventListener('blur', () => {
      releaseAll();
      if (this._mouseButtons) {
        for (let b = 0; b < 3; b++) {
          if (this._mouseButtons & (1 << b)) M._jsui_send_mouse_button(this.rt, -1, -1, mouseButton(b), 0, 0);
        }
        this._mouseButtons = 0;
      }
      M._jsui_send_mouse_leave(this.rt);
      this._hostOf()._releasePads();
    }, opt);
    if (this.opts.autoFocus ?? !this.opts.passThrough) focusSink();
  }


  // Frees the view's renderer, listeners and buffers (not its handle).
  _destroyView() {
    this._destroyed = true;
    this._abort.abort();
    this._blitAbort?.abort();
    this._canvasAbort?.abort();
    clearTimeout(this._restoreTimer);
    this._resizeObserver?.disconnect();
    this._sink?.remove();
    const M = this.module;
    this._wgpu?.destroy();
    this._wgpu = null;
    this._sgl?.remove(this);
    this._sgl = null;
    if (this._gpu) {
      M.GL.makeContextCurrent(this._glHandle);
      M._jsui_gpu_destroy(this._gpu);
      M.GL.makeContextCurrent(0);
      M.GL.deleteContext(this._glHandle);
      this._gpu = this._glHandle = 0;
    }
    if (this._buf) M._free(this._buf);
    M._free(this._scratch);
    M._free(this._dirty);
    this._buf = this._scratch = this._dirty = 0;
    if (this._gl && this._tex) {
      this._gl.deleteTexture(this._tex);
      this._gl.deleteBuffer(this._vb);
      this._gl.deleteProgram(this._prog);
    }
  }

  _releasePads() {}

  // Shared GPU contexts (surfaces only, see JsuiSurface): the provider of a
  // shared Skia WebGPU context, and whether the view now draws through the
  // runtime's shared WebGL context.
  _webgpuShare() { return null; }
  _initSharedGL() { return false; }
}

// One offscreen WebGL context that draws the surfaces of a runtime: Skia
// renders a surface into the bottom-left corner of its drawing buffer (grown
// to the largest surface), then the frame is copied into the surface canvas'
// 2D context. Both happen in the same task, so the drawing buffer need not be
// preserved. Per-surface retained frames live in the GPU context
// (src/render/gpu.cpp), so small changes still repaint only their damage.
class SharedGL {
  // Null when the module has no GL backend or no (hardware, for
  // `hardwareOnly`) WebGL context / Skia context can be created.
  static create(host, hardwareOnly) {
    const M = host.module;
    if (!M.GL || !M._jsui_gpu_create_gl || typeof document === 'undefined') return null;
    const canvas = typeof OffscreenCanvas !== 'undefined' ? new OffscreenCanvas(1, 1) : document.createElement('canvas');
    const attrs = { alpha: true, premultipliedAlpha: true, antialias: false, depth: false, stencil: true,
      preserveDrawingBuffer: false, powerPreference: 'high-performance', failIfMajorPerformanceCaveat: !!hardwareOnly };
    let gl = null;
    try { gl = canvas.getContext('webgl2', attrs) || canvas.getContext('webgl', attrs); } catch {}
    if (!gl || gl.isContextLost()) return null;
    const version = typeof WebGL2RenderingContext !== 'undefined' && gl instanceof WebGL2RenderingContext ? 2 : 1;
    const glAttrs = { ...attrs, majorVersion: version, minorVersion: 0, enableExtensionsByDefault: true };
    const handle = M.GL.registerContext(gl, glAttrs);
    if (!handle) return null;
    M.GL.makeContextCurrent(handle);
    const gpu = M._jsui_gpu_create_gl(0, 0);
    M.GL.makeContextCurrent(0);
    if (!gpu) {
      M.GL.deleteContext(handle);
      gl.getExtension('WEBGL_lose_context')?.loseContext();
      host._log(2, 'Skia GPU context creation failed for the shared WebGL context');
      return null;
    }
    return new SharedGL(host, canvas, gl, glAttrs, handle, gpu, version);
  }

  constructor(host, canvas, gl, glAttrs, handle, gpu, version) {
    this.host = host;
    this.module = host.module;
    this.canvas = canvas;
    this.gl = gl;
    this.glAttrs = glAttrs;
    this.handle = handle;
    this.gpu = gpu;
    this.renderer = version === 2 ? 'gpu-webgl2-shared' : 'gpu-webgl-shared';
    this.stencilBits = gl.getContextAttributes()?.stencil ? 8 : 0;
    this.views = new Set();
    this.lost = false;
    this.gen = 0;  // context restores (host textures re-upload)
    this.destroyed = false;
    this._abort = new AbortController();
    const M = this.module, signal = this._abort.signal;
    canvas.addEventListener('webglcontextlost', (e) => {
      e.preventDefault();
      this.lost = true;
      M._jsui_gpu_context_lost(this.gpu);  // drop GPU objects without GL calls
      clearTimeout(this._restoreTimer);
      const ms = host.opts.contextRestoreTimeout ?? 3000;
      if (!(ms >= 0 && ms < Infinity)) return;
      this._restoreTimer = setTimeout(() => {
        if (!this.lost || this.destroyed) return;
        for (const v of [...this.views]) v._switchToCpu('shared WebGL context lost and not restored', false, '2d');
      }, ms);
    }, { signal });
    canvas.addEventListener('webglcontextrestored', () => {
      clearTimeout(this._restoreTimer);
      // Extension objects of the lost context are dead: register afresh. The
      // GPU context rebuilds itself on the next render.
      M.GL.deleteContext(this.handle);
      this.handle = M.GL.registerContext(gl, glAttrs);
      this.lost = false;
      this.gen++;
      for (const v of this.views) v._sizeChanged = true;  // repaint into the new context
    }, { signal });
  }

  add(view) { this.views.add(view); }
  remove(view) { this.views.delete(view); }

  // Draws `view`'s frame and copies it into its canvas: true when drawn,
  // false when the GPU path failed, undefined while the context is lost.
  render(view) {
    const r = this._draw(view);
    if (r !== true) return r;
    const w = view.pixelWidth, h = view.pixelHeight, ctx = view._ctx2d;
    ctx.save();
    ctx.globalCompositeOperation = 'copy';  // replace, alpha included
    ctx.drawImage(this.canvas, 0, this.canvas.height - h, w, h, 0, 0, w, h);
    ctx.restore();
    return true;
  }

  _draw(view) {
    if (this.lost || this.destroyed || !this.handle) return undefined;
    const M = this.module, w = view.pixelWidth, h = view.pixelHeight, c = this.canvas;
    // Grow only: a resize clears the buffer and reallocates it.
    if (c.width < w || c.height < h) {
      c.width = Math.max(c.width, w);
      c.height = Math.max(c.height, h);
    }
    M.GL.makeContextCurrent(this.handle);
    // JsuiGLTarget {fbo, texture, format, width, height, samples, stencil_bits, flags}:
    // FBO 0 wrapped at the surface's size draws into the buffer's bottom-left corner.
    const t = view._dirty >> 2;
    M.HEAP32.fill(0, t, t + 8);
    M.HEAP32[t + 3] = w;
    M.HEAP32[t + 4] = h;
    M.HEAP32[t + 6] = this.stencilBits;
    const r = M._jsui_render_gl_target(view.rt, this.gpu, view._dirty);
    M.GL.makeContextCurrent(0);
    if (r === 0) return true;
    return this.gl.isContextLost() ? undefined : false;
  }

  // Renders `view` and reads it back. Premultiplied RGBA, top row first.
  read(view) {
    if (this._draw(view) !== true) return null;
    const gl = this.gl, w = view.pixelWidth, h = view.pixelHeight;
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

  destroy() {
    if (this.destroyed) return;
    this.destroyed = true;
    this._abort.abort();
    clearTimeout(this._restoreTimer);
    const M = this.module;
    if (this.handle) M.GL.makeContextCurrent(this.handle);
    M._jsui_gpu_destroy(this.gpu);
    M.GL.makeContextCurrent(0);
    if (this.handle) M.GL.deleteContext(this.handle);
    this.handle = this.gpu = 0;
    this.gl.getExtension('WEBGL_lose_context')?.loseContext();  // free it now, not at GC
  }
}

class JsuiHost extends CanvasView {
  constructor(M, canvas, opts) {
    super(M, canvas, opts);
    this.hostId = nextHostId++;
    this._lastError = null;
    this._raf = 0;
    this._pads = [];
    this._padView = null;            // the view gamepads drive (null: this one)
    this._surfaces = new Set();      // JsuiSurface views
    this._handlers = new Map();  // Host.call name -> fn (handle())
    this._stage = 0;             // setBuffer staging memory (WASM heap)
    this._stageSize = 0;
    this._uiCalls = new Map();   // callUi id -> {resolve, reject}
    this._textures = null;       // HostTextureSet (setTexture), made on first use
    this.rootTag = 1;

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

    this._measure();
    const pageUrl = typeof location !== 'undefined' ? location.href : undefined;
    this._bundleUrl = opts.bundleUrl ? new URL(opts.bundleUrl, pageUrl).href : null;
    // Lives as long as the runtime (JsuiConfig keeps the pointer).
    this._baseUrlPtr = this._bundleUrl ? this._newString(this._bundleUrl) : 0;
    this.rt = M._jsui_web_create(this.hostId, this.width, this.height, this.pixelRatio, opts.devMode ? 1 : 0,
      this._baseUrlPtr);
    if (!this.rt) throw new Error('jsui_web_create failed');
    if (opts.devMode && M._jsui_set_dev_session) {
      this._withStrings([opts.devName ?? null], (n) => M._jsui_set_dev_session(this.rt, n, opts.devSession === false ? 0 : 1));
    }
    // The main surface's AppRegistry component (else the bundle's first).
    if (opts.component != null) this.run(opts.component, opts.initialProps);

    this._mode = opts.renderer ?? 'auto';
    this._webgpu = opts._webgpu ?? null;
    // Without a canvas the main surface is not shown (a page of createSurface() widgets).
    if (canvas) this._initView(this._mode, this._webgpu);
    if (opts.gamepads !== false) this._installGamepads();

    if (opts.bundleSource != null) this.loadSource(opts.bundleSource, this._bundleUrl ?? 'bundle.js');
    else if (this._bundleUrl) this.loadUrl(this._bundleUrl);
    const runtimePolls = this._bundleUrl?.startsWith('http://');
    if (opts.devMode && (opts.devPoll ?? !runtimePolls) && this._bundleUrl) this._startDevPoll();
    if (opts.autoStart !== false) this.start();
  }

  // ------------------------------------------------------------- public API

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

  // ------------------------------------------------- frame data (jsui.h)

  /** Frame data: numbers the UI reads without JSON (useHostBuffer /
   * useHostBufferValue / HostBuffer.get). `data` is a typed array
   * (Float32Array, Float64Array, Int32Array, Uint32Array, Int16Array,
   * Uint16Array, Int8Array or Uint8Array), copied into the runtime; undefined
   * removes the buffer. Returns true if the contents changed. */
  setBuffer(key, data) {
    if (data == null) return this.removeBuffer(key);
    const type = bufferType(data);
    const M = this.module;
    const bytes = data.byteLength;
    if (bytes > this._stageSize) {
      if (this._stage) M._free(this._stage);
      this._stageSize = Math.max(bytes, this._stageSize * 2, 256);
      this._stage = M._malloc(this._stageSize);
    }
    // Read HEAPU8 after the malloc: memory growth replaces the ArrayBuffer.
    if (bytes) M.HEAPU8.set(new Uint8Array(data.buffer, data.byteOffset, bytes), this._stage);
    return this._withStrings([String(key)], (k) =>
      M._jsui_set_buffer(this.rt, k, type, this._stage, data.length)) === 1;
  }

  /** Zero-copy: a typed array of `count` elements of `Type` (Float32Array, ...)
   * over the runtime's buffer `key`, to fill and then commitBuffer(key). Map
   * again every frame (the view is invalid after the next mapBuffer /
   * setBuffer of the key, or when the WASM memory grows). */
  mapBuffer(key, Type, count) {
    const type = bufferType(Type.prototype);
    const M = this.module;
    const p = this._withStrings([String(key)], (k) => M._jsui_map_buffer(this.rt, k, type, count));
    return p ? new Type(M.HEAPU8.buffer, p, count) : null;
  }

  /** Publishes what was written through mapBuffer(key). */
  commitBuffer(key) {
    return this._withStrings([String(key)], (k) => this.module._jsui_commit_buffer(this.rt, k)) === 1;
  }

  /** Removes buffer `key` (the UI sees undefined). Returns true if it existed. */
  removeBuffer(key) {
    return this._withStrings([String(key)], (k) => this.module._jsui_remove_buffer(this.rt, k)) === 1;
  }

  // ------------------------------------------------- host textures (jsui.h)

  /** Host texture: shows `source`'s current picture wherever the UI has
   * <HostTexture name={name}> (useHostTexture(name) gives its size). `source`:
   * a VideoFrame, <video>, <canvas>, OffscreenCanvas, ImageBitmap, <img> or
   * ImageData. Call it again whenever the picture changes (each video
   * frame): the same size refills the same texture, and only the views
   * showing it repaint. Uploaded with the renderer in use (WebGPU
   * copyExternalImageToTexture, a WebGL texture, or CPU pixels; see
   * web/jsui-textures.js). `options.opaque`: ignore alpha (default: true for
   * video). Cross-origin sources need CORS (crossOrigin = 'anonymous').
   * VideoFrames are not kept: close them when this returns. Returns true
   * when uploaded, false when the source has no picture yet (a <video>
   * before its first frame) or the upload failed (logged). Null removes it. */
  setTexture(name, source, options) {
    if (this._destroyed) return false;
    return this._textureSet().set(name, source, options ?? {});
  }

  /** Removes host texture `name` (its views draw nothing; useHostTexture gives
   * null) and stops a bindVideo() of it. True if it existed. */
  removeTexture(name) {
    if (this._destroyed) return false;
    return this._textureSet().remove(name);
  }

  /** Keeps host texture `name` showing `video`'s current frame (one upload
   * per presented frame, requestVideoFrameCallback; else polled per display
   * frame), also while paused (first frame, seeks). Playback stays the
   * page's: play(), pause(), muted, ... on the element. Returns a function
   * that stops it (the texture keeps its last frame; removeTexture clears it).
   * The video must be same-origin or CORS-enabled (crossOrigin = 'anonymous'). */
  bindVideo(name, video, options) {
    if (this._destroyed) throw new Error('bindVideo: runtime destroyed');
    return this._textureSet().bindVideo(name, video, options ?? {});
  }

  /** Where host textures go now: 'webgpu' (GPU textures on the page's
   * device), 'gl' (textures of the views' WebGL context), 'pixels' (CPU
   * copies), or null while a GPU context is lost. */
  get textureBackend() { return this._textureSet().backend; }

  _textureSet() { return (this._textures ??= new HostTextureSet(this)); }

  // The views that draw this runtime's textures (host textures go where all can sample them).
  _views() {
    const v = this.canvas && !this._destroyed ? [this] : [];
    for (const s of this._surfaces) if (s.canvas && !s._destroyed) v.push(s);
    return v;
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

  /** The dev server has a new build: hot-updates a `jsui dev` bundle in place
   * (components keep their state), reloads any other. Dev mode with an
   * http:// bundle URL does this on its own. */
  devUpdate() {
    if (this.module._jsui_dev_update) this.module._jsui_dev_update(this.rt);
    else this.reload();
  }

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
   * fonts, fetch) and nothing left to paint on any canvas: the UI is on screen. */
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

  // ------------------------------------------------------------- surfaces

  /**
   * Shows the AppRegistry component `options.component` on another canvas,
   * like a React Native root view: same bundle, JS context, state and fonts,
   * its own props, size, renderer, input and focus. `target`: a <canvas>, or
   * an element that gets a canvas filling it (removed again by destroy()).
   * @param {HTMLCanvasElement|HTMLElement} target
   * @param {JsuiSurfaceOptions} options
   */
  createSurface(target, options) {
    if (this._destroyed) throw new Error('createSurface: runtime destroyed');
    const s = new JsuiSurface(this, target, options ?? {});
    this._surfaces.add(s);
    return s;
  }

  /** The surfaces made with createSurface() and not destroyed. */
  get surfaces() { return Array.from(this._surfaces); }

  /** Runs `component` (null: the current one) on the main surface with `props`
   * (jsui_surface_run). False for props that are not a JSON object. */
  run(component, props) {
    return this._withStrings([component == null ? null : String(component), JSON.stringify(props ?? {})],
      (k, p) => this.module._jsui_surface_run(this.rt, k, p)) === 0;
  }

  /** New props for the main surface's component (it re-renders, keeping its state). */
  setProps(props) { return this.run(null, props); }

  // The Skia WebGPU context the surfaces share on `webgpu`'s device (a new
  // one after a device loss). Each surface renderer takes a reference.
  _sharedWebGPU(webgpu) {
    const cur = this._swgpu;
    if (cur && !cur.lost && !cur.destroyed && cur.webgpu === webgpu) return cur;
    const next = SharedWebGPU.create(this.module, webgpu, (t) => this._log(2, t));
    if (!next) return null;
    next.refs = 0;  // held by the surface renderers only
    this._swgpu = next;
    return next;
  }

  // The offscreen WebGL context the surfaces share (created once; null when
  // that fails, and then not retried).
  _sharedGL(hardwareOnly) {
    if (this._sgl0 === undefined || this._sgl0?.destroyed) {
      this._sgl0 = SharedGL.create(this, hardwareOnly);
      if (this._sgl0) this._log(1, `surfaces share one ${this._sgl0.renderer.replace('-shared', '')} context`);
    }
    return this._sgl0;
  }

  _surfaceDestroyed(s) {
    this._surfaces.delete(s);
    if (this._padView === s) {
      this._releasePads();
      this._padView = null;
    }
  }

  /** Runs one frame (tick + paint if needed). Called by the rAF loop; usable for manual stepping. */
  frame(now = performance.now()) {
    if (this._destroyed) return;
    const M = this.module, rt = this.rt, st = this.stats;
    if (this._padsActive) this._pollGamepads();
    // Host textures follow renderer changes (fallbacks, context restores, surfaces).
    this._textures?.sync();
    const t0 = performance.now();
    M._jsui_tick(rt, now);
    st.tickMs = ema(st.tickMs, performance.now() - t0);
    st.frames++;
    // One tick for the runtime, then every canvas paints what changed.
    this._paint();
    for (const s of this._surfaces) {
      s.stats.frames++;
      s._paint();
    }
    this._checkError();
    if (this.listeners.get('frame')?.size) this._emit('frame', st);
    if (this._idleWaiters?.length && !M._jsui_pending_loads(rt) && !this._wantsPaint() &&
        !this.surfaces.some((s) => s._wantsPaint())) {
      const w = this._idleWaiters;
      this._idleWaiters = [];
      w.forEach((r) => r());
    }
  }

  destroy() {
    if (this._destroyed) return;
    this.stop();
    // GPU textures first, while their contexts exist.
    this._textures?.destroy();
    this._textures = null;
    // jsui_destroy frees the surface handles with the runtime.
    for (const s of this._surfaces) s._destroyView(true);
    this._surfaces.clear();
    this._sgl0?.destroy();
    // A shared WebGPU context no surface took (the surfaces' unref frees the others).
    if (this._swgpu?.refs === 0 && !this._swgpu.destroyed) {
      this._swgpu.ref();
      this._swgpu.unref();
    }
    this._destroyView();
    const M = this.module;
    M._jsui_destroy(this.rt);
    this.rt = 0;
    M.__jsuiHosts.delete(this.hostId);
    if (this._baseUrlPtr) M._free(this._baseUrlPtr);
    this._baseUrlPtr = 0;
    if (this._stage) M._free(this._stage);
    this._stage = this._stageSize = 0;
  }

  _checkError() {
    const p = this.module._jsui_last_error(this.rt);
    const msg = p ? this.module.UTF8ToString(p) : null;
    if (msg !== this._lastError) {
      this._lastError = msg;
      if (msg) this._emit('error', msg);
    }
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
    const M = this.module, rt = (this._padView ?? this).rt;
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
        s = this._pads[i] = { pressed: new Uint8Array(GAMEPAD_BUTTONS), used: new Uint8Array(GAMEPAD_BUTTONS),
          axes: new Float32Array(GAMEPAD_AXES), axisUsed: new Uint8Array(GAMEPAD_AXES) };
        this._withStrings([p.id], (id) => M._jsui_set_gamepad_connected(rt, i, 1, id));
      }
      const nb = Math.min(GAMEPAD_BUTTONS, p.buttons.length);
      for (let b = 0; b < nb; b++) {
        const btn = p.buttons[b];
        const down = btn.pressed ? 1 : 0;
        if (down !== s.pressed[b]) {
          s.pressed[b] = down;
          const used = M._jsui_send_gamepad_button(rt, i, b, down, btn.value);
          s.used[b] = down && used ? 1 : 0;
        }
      }
      const na = Math.min(GAMEPAD_AXES, p.axes.length);
      for (let a = 0; a < na; a++) {
        const v = p.axes[a];
        const prev = s.axes[a];
        if (Math.abs(v - prev) > AXIS_EPSILON || (v === 0 && prev !== 0)) {
          s.axes[a] = v;
          s.axisUsed[a] = M._jsui_send_gamepad_axis(rt, i, a, v) ? 1 : 0;
        }
      }
    }
    this._padsActive = any;
  }

  _releasePad(i, s) {
    for (let b = 0; b < GAMEPAD_BUTTONS; b++) {
      s.used[b] = 0;
      if (s.pressed[b]) {
        s.pressed[b] = 0;
        this.module._jsui_send_gamepad_button((this._padView ?? this).rt, i, b, 0, 0);
      }
    }
    for (let a = 0; a < GAMEPAD_AXES; a++) {
      if (s.axes[a] !== 0) {
        s.axes[a] = 0;
        this.module._jsui_send_gamepad_axis((this._padView ?? this).rt, i, a, 0);
      }
    }
  }

  /** Whether the UI consumed the current press of gamepad `pad`'s `button`
   * (W3C standard mapping): the game skips buttons the UI took, e.g. A on a
   * focused menu item, and handles the rest itself. */
  gamepadConsumed(pad, button) { return !!this._pads[pad]?.used[button]; }

  /** Whether the UI consumed the last change of the stick axis (it moved
   * focus or scrolls). */
  gamepadAxisConsumed(pad, axis) { return !!this._pads[pad]?.axisUsed[axis]; }

  _releasePads() { this._pads.forEach((s, i) => s && this._releasePad(i, s)); }

  // --------------------------------------------------------------- dev mode

  // Polls GET <bundle origin>/__jsui/version (served by `jsui dev`); the body
  // is an opaque token that changes whenever the bundle was rebuilt (or, with
  // hmr=1, failed to build). A long-polling server simply holds the request;
  // we re-ask right away.
  async _startDevPoll() {
    const url = new URL('/__jsui/version?hmr=1', this._bundleUrl).href;
    const signal = this._abort.signal;
    let last = null;
    while (!this._destroyed) {
      let ok = false;
      try {
        const res = await fetch(url, { cache: 'no-store', signal });
        if (res.ok) {
          const v = (await res.text()).trim();
          if (last !== null && v !== last && !this._destroyed) {
            this._emit('log', 1, 'bundle changed');
            this.devUpdate();
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

/** @typedef {{
 *   component: string,           // the AppRegistry key (AppRegistry.registerComponent)
 *   initialProps?: object,       // the component's props (JSON-serializable)
 *   width?: number,              // CSS px; sets the canvas' CSS width (default: its CSS box)
 *   height?: number,
 *   autoSize?: boolean|'width'|'height',
 *                                // size the canvas to the component's laid-out content (both axes,
 *                                // or one; the other follows the canvas' CSS box)
 *   renderer?: 'auto'|'gpu'|'webgpu'|'webgl'|'cpu'|'2d',
 *                                // default: the runtime's. 'auto' / 'gpu' / 'webgpu' draw with GPU
 *                                // contexts the runtime's surfaces share, so any number of GPU
 *                                // surfaces fit under the browser's ~16 live WebGL contexts: Skia
 *                                // Graphite on the page's WebGPU device (each canvas keeps only its
 *                                // GPUCanvasContext), else one offscreen WebGL context whose frames
 *                                // are copied into each canvas' 2D context. 'webgl': a WebGL
 *                                // context of the canvas' own (counts against that cap). 'cpu' /
 *                                // '2d': the CPU raster, as for the runtime
 *   pixelRatio?: number,         // default: the runtime's option, else window.devicePixelRatio
 *   autoFocus?: boolean,         // focus its keyboard sink on creation (default false)
 *   trapTab?: boolean,           // keep an unhandled Tab inside (default false: focus moves on in the page)
 *   passThrough?: boolean,       // input the surface doesn't capture reaches the page under it (see
 *                                // JsuiOptions.passThrough)
 * }} JsuiSurfaceOptions */

// A further root view of a runtime (JsuiHost.createSurface).
class JsuiSurface extends CanvasView {
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
    }
    super(host.module, canvas, { ...opts, autoFocus: opts.autoFocus === true, trapTab: opts.trapTab ?? false,
      pixelRatio: opts.pixelRatio ?? host.opts.pixelRatio });
    const M = host.module;
    this.host = host;
    this._ownsCanvas = owned;
    this.component = String(opts.component);
    this._autoW = opts.autoSize === true || opts.autoSize === 'width';
    this._autoH = opts.autoSize === true || opts.autoSize === 'height';
    if (opts.width > 0 && !this._autoW) canvas.style.width = `${opts.width}px`;
    if (opts.height > 0 && !this._autoH) canvas.style.height = `${opts.height}px`;
    this._measure();
    const [w, h] = this._viewportSize();
    this.rt = this._withStrings([this.component, JSON.stringify(opts.initialProps ?? {})],
      (k, p) => M._jsui_surface_create(host.rt, k, p, w, h, this.pixelRatio));
    if (!this.rt) {
      super._destroyView();
      if (owned) canvas.remove();
      throw new Error(`createSurface("${this.component}") failed (see the log)`);
    }
    /** The surface's React Native root tag (RootTagContext in its components). */
    this.rootTag = M._jsui_surface_tag(this.rt);
    this._initView(opts.renderer ?? host._mode, host._webgpu);
  }

  _hostOf() { return this.host; }

  // Surface renderers share the runtime's GPU contexts (see SharedGL).
  _webgpuShare() { return (webgpu) => this.host._sharedWebGPU(webgpu); }

  _initSharedGL(mode) {
    if (mode !== 'auto' && mode !== 'gpu' && mode !== 'webgpu') return false;
    const sgl = this.host._sharedGL(mode === 'auto');
    if (!sgl) return false;  // the canvas is untouched: the per-canvas chain follows
    const ctx = this.canvas.getContext('2d');
    if (!ctx) return false;
    this._ctx2d = ctx;
    this._sgl = sgl;
    sgl.add(this);
    this.renderer = sgl.renderer;
    return true;
  }

  _log(level, text) { this.host._log(level, `${this.component} (root ${this.rootTag ?? '?'}): ${text}`); }

  /** New props: the component re-renders with them, keeping its state. False for invalid props. */
  setProps(props) { return this.run(null, props); }

  /** Runs `component` (null: the current one) with `props` on this surface. */
  run(component, props) {
    if (this._destroyed) return false;
    const ok = this._withStrings([component == null ? null : String(component), JSON.stringify(props ?? {})],
      (k, p) => this.module._jsui_surface_run(this.rt, k, p)) === 0;
    if (ok && component != null) this.component = String(component);
    return ok;
  }

  /** Unmounts the component and frees the surface (and the canvas createSurface made). */
  destroy() { this._destroyView(false); }

  // `runtimeGone`: the runtime is being destroyed and frees the handle itself.
  _destroyView(runtimeGone) {
    if (this._destroyed) return;
    super._destroyView();
    if (!runtimeGone) {
      this.module._jsui_surface_destroy(this.rt);
      this.host._surfaceDestroyed(this);
    }
    this.rt = 0;
    if (this._ownsCanvas) this.canvas.remove();
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
