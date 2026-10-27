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
// ImageData, { data, width, height } (RGBA bytes). Textures belong to the
// runtime (every surface can show them) and are uploaded with the renderer in
// use: a GPUTexture on the page's WebGPU device (copyExternalImageToTexture),
// a texture of the views' WebGL context, or CPU pixels when the views cannot
// share one (web/jsui-textures.js). They follow renderer fallbacks and
// context loss by themselves.
//
// Sharing the page's WebGPU device (a Three.js / Babylon / own WebGPU game):
// createJsui({ device }) draws with that GPUDevice instead of requesting one,
// so UI and game exchange GPU textures without copies, both ways:
//
//   const jsui = await createJsui({ device: renderer.backend.device, bundleUrl });
//   // the UI in the game: a surface rendered into a GPUTexture (in-world
//   // screens, or the game composites the HUD in its own final pass)
//   const panel = jsui.createTextureSurface({ component: 'Terminal', width: 800, height: 500 });
//   material.map = new THREE.ExternalTexture(panel.texture);
//   panel.dispatchEvent(pointerEvent, uv.x * panel.width, (1 - uv.y) * panel.height);
//   // the game in the UI: a render target sampled in place (no copy)
//   jsui.setTexture('minimap', minimapTarget);   // <HostTexture name="minimap" />
//
// Texture surfaces repaint only what changed, straight into their texture
// (it keeps the last frame). After a device loss the page passes its new
// device to jsui.setDevice(device) ('devicelost' says when). Main thread only
// (a GPUDevice cannot move to a worker).
//
// Worker mode (createJsui({ worker: true }), web/jsui-worker.js): the
// runtime, its frame loop and the renderers run in a dedicated Worker on
// OffscreenCanvases; this thread keeps the DOM side (input listeners, sizes,
// cursor, gamepad polling, video frame capture). Same API, but calls that
// answer from the runtime return Promises there (see web/README.md).
//
// UI audio (include/jsui/jsui.h "Audio", docs/AUDIO.md): the sounds the UI
// plays (Sound.play, useMusic, <UISounds>, Pressable `sounds`) go through
// Web Audio (web/jsui-audio.js): one AudioContext per page, a gain per bus,
// decoded sounds cached by URL. Browsers allow sound only after a user
// gesture: the host unlocks audio on the first click / tap / key press;
// until then one-shot sounds are dropped (they end as 'failed') and music
// (looping voices) starts once audio unlocks. Gamepad buttons do not count.
//
//   jsui.audio.unlocked; jsui.on('audiounlock', () => ...);
//   jsui.audio.setVolume(0.5, { fade: 300 }); jsui.audio.muted = true;
//   createJsui({ ..., audio: false })                  // silence
//   createJsui({ ..., audio: (cmd, audio) => ... })    // your own player (like JsuiConfig.on_audio)

/** @typedef {{
 *   canvas?: HTMLCanvasElement,  // the main surface's canvas; null / omitted: not shown (a page
 *                                // that only places createSurface() / createTextureSurface() widgets)
 *   device?: GPUDevice,          // the page's own WebGPU device (e.g. Three.js' renderer.backend.device):
 *                                // JSUI draws with it instead of requesting one, which texture
 *                                // surfaces and GPUTexture host textures need. Needs dist-gpu/; no
 *                                // worker mode. After a loss, pass the new one to setDevice()
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
 *                                // is likewise passed on to its jsui.wasm. Default: the
 *                                // modules `jsui build --web` deployed next to this file
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
 *   idleSleep?: boolean,         // pause the frame loop while nothing moves or waits to paint, and
 *                                // resume on input, any call that changes the runtime and its next
 *                                // timer (default true; off in dev mode). Idle frames cost the page
 *                                // a rAF task each; false keeps one every display frame.
 *   trapTab?: boolean,           // keep Tab inside the UI even if unhandled (default true, false
 *                                // with passThrough; false lets an unhandled Tab move page focus on)
 *   audio?: boolean|object|Function, // UI sounds (web/jsui-audio.js): true / omitted: Web Audio
 *                                // ({ context, streamBytes } options as an object); false: none;
 *                                // a function (cmd, jsui.audio) plays the commands itself and
 *                                // reports with jsui.audio.voiceEnded / soundLoaded
 *   passThrough?: boolean,       // the canvas is a layer over the page (a game's canvas): input the
 *                                // UI doesn't capture reaches what is under it, consumed input
 *                                // doesn't (default false). Turns autoFocus / trapTab off by default.
 *   worker?: boolean,            // run the runtime, frame loop and rendering in a dedicated Worker
 *                                // (web/jsui-worker.js; default false). Falls back to this thread,
 *                                // with one warning, where Worker / OffscreenCanvas /
 *                                // transferControlToOffscreen are missing or the worker fails to start.
 *   workerLoop?: 'raf'|'main',   // worker mode's frame clock: 'raf' (default) the worker's own
 *                                // requestAnimationFrame, 'main' this thread's rAF posting each
 *                                // frame (used anyway when the worker has none)
 *   jsFrameBudget?: number,      // ms of JS per frame before React's scheduler yields (default 5):
 *                                // transitions (startTransition) render in slices across frames.
 *                                // <= 0: no limit (jsui_set_js_frame_budget)
 *   components?: string|URL|object, // native components (registerComponent) registered before the
 *                                // bundle runs: { Name: impl } or the URL of a module exporting them
 *                                // (web/jsui-components.js); worker mode needs the URL (its draw
 *                                // functions run in the worker)
 *   js?: 'quickjs'|'host',       // the JS engine the bundle runs on: 'host' (default) this page's /
 *                                // worker's own engine (V8, JavaScriptCore, SpiderMonkey), with the
 *                                // native core still in WASM (docs/ARCHITECTURE.md "Host JS engine");
 *                                // it needs eval, so a CSP without 'unsafe-eval' or a module built
 *                                // without it runs QuickJS instead (a warning if 'host' was asked
 *                                // for). 'quickjs': QuickJS compiled into jsui.wasm, as natively.
 *                                // jsui.jsEngine says which one runs.
 * }} JsuiOptions */

import { SharedWebGPU, WebGPURenderer, externalWebGPU, requestWebGPU, standInPipeline } from './jsui-webgpu.js';
import { HostTextureSet, JsuiVideo, corsWarning, defaultOpaque, isVideoFrame, sourceSize, taintKey,
  videoFramesContextLost, watchVideoFrames } from './jsui-textures.js';
import { JsuiAudio } from './jsui-audio.js';
import { ComponentSet } from './jsui-components.js';

export { requestWebGPU, webgpuSupported } from './jsui-webgpu.js';
export { AudioOp, AudioStatus } from './jsui-audio.js';
export { VideoOp, VideoState } from './jsui-textures.js';

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

// Exports that change what a runtime does next (input, state, messages,
// props, sizes, fonts, textures, loads): calling one resumes the frame loop
// of every host on the module that paused it while idle (idleSleep). Reads
// and the frame's own calls (tick, render, needs_redraw) are left out.
const WAKING_EXPORT = new RegExp('^_jsui_(send_|set_(?!profiling)|patch_|post_|surface_(run|create|destroy)|' +
  'register_|unregister_|commit_buffer|remove_|texture_|audio_|web_audio_|web_resolve_|web_reject_|' +
  'web_call_ui|reload|load_bundle|dev_update|gpu_context_lost|debug_)');
function wakeOnCalls(M) {
  if (M.__jsuiWake) return M.__jsuiWake;
  const hosts = M.__jsuiWake = new Set();
  for (const k of Object.keys(M)) {
    const f = M[k];
    if (typeof f !== 'function' || !WAKING_EXPORT.test(k)) continue;
    M[k] = function (...args) {
      const r = f.apply(this, args);
      for (const h of hosts) h._wake();
      return r;
    };
  }
  return hosts;
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

// JsuiBufferType of a typed array, or of a typed array constructor.
const BUFFER_TYPES = [Float32Array, Float64Array, Int32Array, Uint32Array, Int16Array, Uint16Array, Int8Array, Uint8Array];
function bufferType(data) {
  const t = typeof data === 'function' ? BUFFER_TYPES.indexOf(data) : BUFFER_TYPES.findIndex((T) => data instanceof T);
  if (t < 0) throw new TypeError('setBuffer / mapBuffer: expected a Float32Array, Float64Array, Int32Array, Uint32Array, Int16Array, Uint16Array, Int8Array or Uint8Array');
  return t;
}

// registerFont / loadFont arguments as jsui_register_font takes them:
// [family ('' = the file's), bytes, weight (0 = the file's), italic (-1 = the
// file's, 0, 1)]. Accepts (family, bytes, weight?, italic?), (family, bytes,
// { weight, style | italic }) and (bytes, options?).
function fontArgs(family, bytes, weight, italic) {
  if (family != null && typeof family === 'object') [family, bytes, weight, italic] = ['', family, bytes, weight];
  if (weight != null && typeof weight === 'object') {
    const o = weight;
    weight = o.weight;
    italic = o.style !== undefined ? o.style : o.italic;
  }
  let w = weight === 'normal' ? 400 : weight === 'bold' ? 700 : Number(weight ?? 0);
  w = Number.isFinite(w) && w > 0 ? Math.min(1000, Math.round(w)) : 0;
  let i;
  if (italic == null) i = -1;
  else if (typeof italic === 'string') i = italic === 'italic' || italic === 'oblique' ? 1 : 0;
  else if (typeof italic === 'number') i = italic < 0 ? -1 : italic ? 1 : 0;
  else i = italic ? 1 : 0;
  return [family == null ? '' : String(family), bytes, w, i];
}

// setFontFallbacks' list as jsui_set_font_fallbacks takes it (null clears).
function fontList(families) {
  if (families == null) return null;
  const list = (typeof families === 'string' ? families.split(',') : Array.from(families, String))
    .map((f) => f.trim().replace(/^(['"])(.*)\1$/, '$2')).filter(Boolean);
  return list.length ? list.join(',') : null;
}

let nextHostId = 1;
const modsOf = (e) => (e.shiftKey ? 1 : 0) | (e.ctrlKey ? 2 : 0) | (e.altKey ? 4 : 0) | (e.metaKey ? 8 : 0);

// Puts text the UI copied (TextInput, selectable Text) on the page's
// clipboard, from inside the key event that copied it: the async Clipboard
// API, else (an insecure context) a copy command from the focused keyboard
// sink.
function writeClipboard(text) {
  const c = typeof navigator !== 'undefined' ? navigator.clipboard : null;
  if (c?.writeText) {
    c.writeText(text).catch(() => {});
    return;
  }
  const sink = typeof document !== 'undefined' ? document.activeElement : null;
  if (!sink?.__jsuiSink) return;
  sink.value = text;
  sink.select();
  try { document.execCommand('copy'); } catch {}
  sink.value = '';
}
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
  let canvas = opts.canvas ?? null;
  if (typeof opts.pipelines === 'string' && typeof location !== 'undefined') {
    // Relative to the page (also for the worker); `{format}` survives.
    opts = { ...opts, pipelines: decodeURI(new URL(opts.pipelines, location.href).href) };
  }
  if (opts.components instanceof URL) opts = { ...opts, components: opts.components.href };
  if (typeof opts.components === 'string' && typeof location !== 'undefined') {
    opts = { ...opts, components: new URL(opts.components, location.href).href };
  }
  if (opts.worker && opts.components && typeof opts.components === 'object') {
    // Functions can't reach a worker: its runtime imports a module URL instead.
    console.warn('[jsui] worker mode needs `components` as a module URL (draw functions run in the worker): running on the page\'s main thread');
    opts = { ...opts, worker: false };
  }
  if (opts.worker && opts.device) {
    console.warn('[jsui] worker mode cannot use the page\'s GPUDevice (createJsui({ device })): running on the ' +
      'page\'s main thread');
    opts = { ...opts, worker: false };
  }
  if (opts.worker) {
    let r;
    try {
      const { startWorkerHost } = await import('./jsui-worker.js');
      r = await startWorkerHost(internals(), opts);
    } catch (e) {
      console.warn(`[jsui] worker mode unavailable (${e?.message ?? e}): running on the page's main thread`);
      r = { canvas };
    }
    if (r.host) return r.host;
    canvas = r.canvas;
  }
  // A canvas a worker-mode runtime had (its control went to an OffscreenCanvas
  // for good): a clone of it in its place.
  if (canvas && transferred.has(canvas)) canvas = replaceCanvas(canvas);
  if (typeof opts.components === 'string') opts = { ...opts, components: await import(opts.components) };
  const webgpu = await webgpuFor(opts);
  return new JsuiHost(await loadModuleFor(opts, webgpu), canvas, { ...opts, canvas, _webgpu: webgpu });
}

const transferred = new WeakSet();  // canvases given to a worker (transferControlToOffscreen)

// ------------------------------------------------- GPU pipeline lists
// (createJsui's `pipelines` and `rememberPipelines`; see CanvasView.addPipelines)

// The list createJsui's `pipelines` names: bytes, or a URL fetched with
// `{format}` replaced by the canvas' texture format (a list recorded on a
// bgra8unorm canvas, macOS and Windows, builds other pipelines than one of
// an rgba8unorm canvas, Android and Linux). A missing file (404) is no list.
async function pipelineList(src, format) {
  if (typeof src !== 'string' && !(src instanceof URL)) return src;
  const url = String(src).replace('{format}', format ?? 'bgra8unorm');
  const res = await fetch(url);
  if (res.status === 404) return null;
  if (!res.ok) throw new Error(`${url}: HTTP ${res.status}`);
  return new Uint8Array(await res.arrayBuffer());
}

// Which list of this browser's own a page remembers: per page path and
// canvas format (a page with several apps names its own pipelineKey).
function pipelineKeyFor(view) {
  const where = typeof location !== 'undefined' ? location.origin + location.pathname : '';
  return `${where} ${view._wgpu?.format ?? ''}`;
}

const PIPELINE_DB = 'jsui-pipelines';

// The IndexedDB store of remembered lists; null where there is none
// (private modes may refuse it). Every call opens it anew: a few per minute.
function pipelineStore(mode, fn) {
  return new Promise((resolve) => {
    let req;
    try {
      req = indexedDB.open(PIPELINE_DB, 1);
    } catch {
      return resolve(null);
    }
    req.onupgradeneeded = () => req.result.createObjectStore('lists');
    req.onerror = req.onblocked = () => resolve(null);
    req.onsuccess = () => {
      const db = req.result;
      try {
        const tx = db.transaction('lists', mode);
        const r = fn(tx.objectStore('lists'));
        tx.oncomplete = () => { db.close(); resolve(r?.result ?? null); };
        tx.onerror = tx.onabort = () => { db.close(); resolve(null); };
      } catch {
        db.close();
        resolve(null);
      }
    };
  });
}

async function rememberedPipelines(key) {
  const v = await pipelineStore('readonly', (s) => s.get(key));
  return v instanceof Uint8Array ? v : v instanceof ArrayBuffer ? new Uint8Array(v) : null;
}

function storePipelines(key, list) {
  return pipelineStore('readwrite', (s) => s.put(list, key));
}

function replaceCanvas(old) {
  const c = old.cloneNode(false);
  old.replaceWith(c);
  return c;
}

// What web/jsui-worker.js needs from this module (it imports nothing itself,
// so the worker's copy of this file and the page's stay one module graph each).
function internals() {
  return { DomView, CanvasView, JsuiHost, JsuiSurface, HIT_POINTER, HIT_SCROLL, NAV_KEYS, webgpuFor, loadModuleFor,
    bufferType, BUFFER_TYPES, sourceSize, defaultOpaque, isVideoFrame, transferred, replaceCanvas, fontArgs, fontList, JsuiAudio, JsuiVideo,
    corsWarning, taintKey, videoFramesContextLost, watchVideoFrames, writeClipboard, hostUrl: import.meta.url,
    workerName: WORKER_NAME };
}
const WORKER_NAME = 'jsui-host';

// Whether this thread may eval (the host JS engine needs it; a CSP without
// 'unsafe-eval' throws an EvalError).
function evalAllowed() {
  try {
    return (0, eval)('1') === 1;
  } catch {
    return false;
  }
}

const CPU_MODULE = './dist/jsui.js';
const GPU_MODULE = './dist-webgl/jsui.js';
const ALL_MODULE = './dist-gpu/jsui.js';  // WebGPU + WebGL + CPU
// The modules deployed next to this file, {dir: version}, when `jsui build
// --web` wrote it (packages/cli/src/web.js sets this line): moduleVersions'
// default unless the page names its own moduleBase.
const DEPLOYED_MODULES = null;
const moduleVersionsOf = (opts) => opts.moduleVersions ?? (opts.moduleBase == null ? DEPLOYED_MODULES : null);

// The WebGPU device the renderer may use, or null (web/jsui-webgpu.js).
function webgpuFor(opts) {
  const mode = opts.renderer ?? 'auto';
  // The page's device: also with a renderer mode that keeps the main canvas
  // off WebGPU (texture surfaces and GPUTexture host textures use it).
  if (mode !== 'auto' && mode !== 'gpu' && mode !== 'webgpu' && !opts.device) return null;
  // Only dist-gpu/ draws with WebGPU: when moduleVersions leaves it out, no
  // device is needed. Say so, since the page then silently renders on WebGL.
  const versions = moduleVersionsOf(opts);
  if (!opts.moduleUrl && versions && versions[ALL_MODULE.split('/')[1]] == null) {
    if (typeof navigator !== 'undefined' && navigator.gpu) {
      console.warn('[jsui] this browser has WebGPU, but dist-gpu/ is not deployed (moduleVersions), so the '
        + 'UI renders with WebGL. Deploy dist-gpu/jsui.{js,wasm} from @orktes/jsui-web and list it in '
        + 'moduleVersions to render with Skia Graphite on WebGPU.');
    }
    return null;
  }
  if (opts.device) return externalWebGPU(opts.device);
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
  const versions = moduleVersionsOf(opts);
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

// GL work that may find its context lost halfway: WebKit marks a context
// lost when a call into its GPU process fails, in the middle of a frame. A
// throw while the context reports itself lost is that loss, not an error:
// `lost(error)` runs and the result is undefined. The WASM stack pointer goes
// back to where it was (a throw through Skia's frames skipped their
// epilogues; web/native/jsui_webgl_lost.js keeps the GL glue from throwing).
export function guardGL(M, gl, fn, lost) {
  const sp = M.stackSave?.();
  try {
    return fn();
  } catch (e) {
    if (!gl?.isContextLost()) throw e;
    if (sp !== undefined) M.stackRestore(sp);
    lost?.(e);
    return undefined;
  }
}

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
    // An OffscreenCanvas in worker mode (web/jsui-worker.js): no document there.
    const c = typeof document !== 'undefined' ? document.createElement('canvas') : new OffscreenCanvas(1, 1);
    const gl = c.getContext('webgl2', attrs) || c.getContext('webgl', attrs);
    webglProbe[key] = !!gl;
    gl?.getExtension('WEBGL_lose_context')?.loseContext();  // free it now, not at GC
  } catch {
    webglProbe[key] = false;
  }
  return webglProbe[key];
}

// The page side of a view: a canvas element's size and device pixel ratio,
// its input listeners (pointer, wheel, keyboard sink, focus) and cursor, and
// the view's event listeners. Input goes to the runtime through the _in*
// methods, and hitTest() answers what the UI captures: CanvasView calls the
// module, worker mode's proxy views (web/jsui-worker.js) post them to the
// worker and answer from its last hit map.
// The page's safe-area insets (env(safe-area-inset-*): a notch, the Dynamic
// Island, rounded corners, the home indicator; nonzero only on a page with
// viewport-fit=cover) where they overlap `canvas`, in CSS px. null where
// there is no DOM (a worker).
let safeAreaProbe = null;
function safeAreaOver(canvas) {
  if (typeof document === 'undefined' || !document.body || !canvas?.getBoundingClientRect) return null;
  if (!safeAreaProbe?.isConnected) {
    safeAreaProbe = document.createElement('div');
    safeAreaProbe.style.cssText = 'position:fixed;left:0;top:0;width:0;height:0;visibility:hidden;pointer-events:none;' +
      'padding:env(safe-area-inset-top,0px) env(safe-area-inset-right,0px) env(safe-area-inset-bottom,0px) env(safe-area-inset-left,0px)';
    document.body.appendChild(safeAreaProbe);
  }
  const cs = getComputedStyle(safeAreaProbe);
  const r = canvas.getBoundingClientRect();
  const vw = document.documentElement.clientWidth || innerWidth, vh = document.documentElement.clientHeight || innerHeight;
  const px = (v) => Math.round(Math.max(0, v) * 100) / 100;
  return {
    top: px(parseFloat(cs.paddingTop) - r.top),
    right: px(r.right - (vw - parseFloat(cs.paddingRight))),
    bottom: px(r.bottom - (vh - parseFloat(cs.paddingBottom))),
    left: px(parseFloat(cs.paddingLeft) - r.left),
  };
}

class DomView {
  // A new size or context means a repaint: an idle frame loop resumes.
  get _sizeChanged() { return this.__sizeChanged ?? false; }
  set _sizeChanged(v) {
    this.__sizeChanged = v;
    if (v) this._wakeLoop();
  }
  _wakeLoop() {}

  constructor(canvas, opts) {
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
  }

  // Input, focus and resize tracking on the page.
  _installDom() {
    this._installInput();
    this._installCanvasInput();
    this._installResize();
  }

  // Undoes _installDom.
  _destroyDom() {
    this._abort.abort();
    this._canvasAbort?.abort();
    this._resizeObserver?.disconnect();
    this._sink?.remove();
  }

  // The view that owns the runtime (gamepads, the frame loop).
  _hostOf() { return this; }

  /** Focuses the view's keyboard sink (replaced once input is installed). */
  focus() {}

  // The viewport handed to the runtime: 0 on auto-sized axes.
  _viewportSize() { return [this._autoW ? 0 : this.width, this._autoH ? 0 : this.height]; }

  /** Subscribe: 'message' (name, data), 'state' (key, value: HostState.set in the UI),
   * 'call' (id, name, args: Host.call without a handle() handler), 'callCancel' (id),
   * 'error' (message), 'log' (level, text), 'frame' (stats), 'renderer' (name, reason),
   * 'audiounlock' (sound can play now). */
  on(type, cb) {
    let set = this.listeners.get(type);
    if (!set) this.listeners.set(type, (set = new Set()));
    set.add(cb);
    return () => this.off(type, cb);
  }

  off(type, cb) { this.listeners.get(type)?.delete(cb); }

  _listens(type) { return !!this.listeners.get(type)?.size; }

  _emit(type, ...args) {
    const set = this.listeners.get(type);
    if (!set || set.size === 0) return false;
    for (const cb of set) {
      try { cb(...args); } catch (e) { console.error(e); }
    }
    return true;
  }

  _log(level, text) {
    if (!this._emit('log', level, text)) (level >= 2 ? console.warn : console.log)('[jsui] ' + text);
  }

  _updateCursor() {
    const c = this._cursorNow();
    if (c !== this._cursor) {
      this._cursor = c;
      this.canvas.style.cursor = c;
    }
  }

  // ------------------------------------------------------------ size / DPR

  _ratio() { return this._fixedRatio || (typeof devicePixelRatio === 'number' ? devicePixelRatio : 1) || 1; }

  // The canvas' CSS box when no ResizeObserver entry gives it.
  _cssBox() {
    const c = this.canvas;
    return c ? [c.clientWidth || c.width, c.clientHeight || c.height] : [1, 1];
  }

  _setBackingStore(w, h) {
    if (!this.canvas) return;
    this.canvas.width = w;
    this.canvas.height = h;
  }

  // Logical size = the canvas' CSS box; device size = logical * ratio, which
  // is also the backing store size, so frames are blitted 1:1.
  _measure(entry) {
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
      [cssW, cssH] = this._cssBox();
    }
    this._cssW = cssW;
    this._cssH = cssH;
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
    this._setBackingStore(devW, devH);
    this._sizeChanged = true;
    return true;
  }

  // The size changed (CanvasView: tell the runtime).
  _resized() { this._rect = null; }

  _installResize() {
    const apply = (entry) => {
      if (this._measure(entry)) {
        this._resized();
        this._publishSafeArea();
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
    addEventListener('resize', () => { this._rect = null; this._publishSafeArea(); }, { passive: true, signal: this._abort.signal });
  }

  // The main view publishes the safe-area insets over its canvas as shared
  // state jsui.safeAreaInsets (SafeAreaView, useSafeAreaInsets()), unless
  // `safeArea: false` (the page sets the key itself). Zero insets are only
  // published to replace nonzero ones.
  _publishesSafeArea() { return false; }
  _publishSafeArea() {
    if (!this.canvas || this.opts.safeArea === false || !this._publishesSafeArea()) return;
    const v = safeAreaOver(this.canvas);
    if (!v) return;
    const key = `${v.top},${v.right},${v.bottom},${v.left}`;
    if (key === (this._safeAreaKey ?? '0,0,0,0')) return;
    this._safeAreaKey = key;
    this.setState('jsui.safeAreaInsets', v);
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

  // ------------------------------------------------------------------ input

  // A touch pointermove with the samples the browser coalesced into it
  // (a 120 Hz+ digitizer under a 60 Hz frame), each at its own time: drag
  // resampling and fling velocities want them all.
  _touchMoves(e) {
    const list = e.getCoalescedEvents?.();
    for (const s of list?.length ? list : [e]) {
      this._inTime(s.timeStamp);
      const [x, y] = this._point(s);
      this._inTouch(e.pointerId, 1, x, y);
    }
  }

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
    const c = this.canvas, signal = this._canvasAbort.signal;
    const opt = { signal };
    const active = { signal, passive: false };
    const focusSink = () => this.focus();
    if (this.opts.passThrough) {
      this._installPassThroughInput(signal);
      return;
    }
    this._pointerOver(false);  // until this canvas says otherwise
    c.addEventListener('pointerenter', () => this._pointerOver(true), opt);
    c.style.touchAction = 'none';
    c.style.overscrollBehavior = 'none';
    c.style.userSelect = 'none';
    c.style.webkitUserSelect = 'none';
    c.style.outline = 'none';

    // The DOM event time (same clock as the rAF timestamps given to
    // jsui_tick) stamps the event: gesture velocities see the real spacing
    // of the events that arrive between two frames.
    const stamp = (e) => this._inTime(e.timeStamp);
    c.addEventListener('pointerdown', (e) => {
      stamp(e);
      const [x, y] = this._point(e);
      let used;
      if (e.pointerType === 'mouse') {
        this._mouseButtons |= 1 << e.button;
        used = this._inButton(x, y, mouseButton(e.button), 1, modsOf(e));
      } else {
        used = this._inTouch(e.pointerId, 0, x, y);
      }
      try { c.setPointerCapture(e.pointerId); } catch {}
      // Not for touch: focusing a textarea pops up the virtual keyboard.
      if (e.pointerType !== 'touch') focusSink();
      if (used) e.preventDefault();
      this._updateCursor();
    }, active);
    c.addEventListener('pointermove', (e) => {
      if (e.pointerType !== 'mouse') return this._touchMoves(e);
      stamp(e);
      const [x, y] = this._point(e);
      this._inMove(x, y);
    }, opt);
    const up = (e, cancel) => {
      stamp(e);
      const [x, y] = this._point(e);
      let used;
      if (e.pointerType === 'mouse') {
        if (!(this._mouseButtons & (1 << e.button))) return;
        this._mouseButtons &= ~(1 << e.button);
        used = this._inButton(x, y, mouseButton(e.button), 0, modsOf(e));
      } else {
        used = this._inTouch(e.pointerId, cancel ? 3 : 2, x, y);
      }
      if (used) e.preventDefault();
      this._updateCursor();
    };
    c.addEventListener('pointerup', (e) => up(e, false), active);
    c.addEventListener('pointercancel', (e) => up(e, true), active);
    c.addEventListener('pointerleave', (e) => {
      if (e.pointerType === 'mouse' && !this._mouseButtons) this._inLeave();
      this._pointerOver(false);
    }, opt);
    c.addEventListener('wheel', (e) => {
      const [x, y] = this._point(e);
      // Normalise to px like browsers do for pixel-mode wheels.
      const k = e.deltaMode === 1 ? 16 : e.deltaMode === 2 ? this.height : 1;
      if (this._inWheel(x, y, e.deltaX * k, e.deltaY * k)) e.preventDefault();
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
    const c = this.canvas;
    const opt = { signal, capture: true, passive: false };
    const stamp = (e) => this._inTime(e.timeStamp);
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
        used = this._inButton(x, y, mouseButton(e.button), 1, modsOf(e));
      } else {
        used = this._inTouch(e.pointerId, 0, x, y);
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
            this._inLeave();
            setHot(false);
            this._updateCursor();
          }
          return;
        }
        hovering = true;
        stamp(e);
        const used = this._inMove(x, y);
        setHot(!e.buttons && (this._hitAt(x, y) & HIT_POINTER) !== 0);
        if (used && !e.buttons) e.stopImmediatePropagation();
        this._updateCursor();
        return;
      }
      if (e.pointerType === 'mouse') {
        stamp(e);
        this._inMove(x, y);
      } else {
        this._touchMoves(e);
      }
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
        this._inButton(x, y, mouseButton(e.button), 0, modsOf(e));
        if (!this._mouseButtons) gestures.delete(e.pointerId);
      } else {
        this._inTouch(e.pointerId, cancel ? 3 : 2, x, y);
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
      return inside(x, y) && (this._hitAt(x, y) & HIT_POINTER) !== 0;
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
      if (this._inWheel(x, y, e.deltaX * k, e.deltaY * k)) take(e);
    }, opt);
    const touch = (e) => {
      let all = e.changedTouches.length > 0;
      for (const t of e.changedTouches) {
        if (e.type === 'touchstart') {
          const [x, y] = this._point(t);
          if (inside(x, y) && this._hitAt(x, y) !== 0) touches.add(t.identifier);
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
    const signal = this._abort.signal;
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
    // No on-screen keyboard for the sink: touch never focuses it, but iOS
    // Safari restores focus to it when the page comes back (bfcache, tab
    // switch) and would pop the keyboard up with nothing to type into.
    // Hardware keyboards and IMEs still deliver keys and text.
    sink.setAttribute('inputmode', 'none');
    sink.tabIndex = 0;
    sink.__jsuiSink = true;  // writeClipboard's fallback copies from it
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

    // Ctrl / Cmd+V: the key goes to the UI once the browser's paste event
    // (right after it) has told what the page's clipboard holds, so a
    // TextInput pastes that (a page cannot read the clipboard synchronously).
    let pendingPaste = null;
    const sendPaste = () => {
      const k = pendingPaste;
      pendingPaste = null;
      if (k) this._inKey(k.key, k.code, 1, k.repeat, k.mods);
    };
    sink.addEventListener('paste', (e) => {
      e.preventDefault();  // the text goes in through the key, not the sink
      this._pasted(e.clipboardData?.getData('text/plain') ?? '');
      sendPaste();
    }, active);
    sink.addEventListener('keydown', (e) => {
      if (e.isComposing || e.keyCode === 229) return;  // IME owns this key
      if (!e.repeat) this._pressedKeys.set(e.code || e.key, e.key);
      if ((e.ctrlKey || e.metaKey) && !e.altKey && (e.key === 'v' || e.key === 'V')) {
        sendPaste();
        pendingPaste = { key: e.key, code: e.code || null, repeat: e.repeat ? 1 : 0, mods: modsOf(e) };
        setTimeout(sendPaste, 0);  // no paste event came (nothing to paste)
        return;
      }
      const used = this._inKey(e.key || null, e.code || null, 1, e.repeat ? 1 : 0, modsOf(e));
      if ((used && (NAV_KEYS.has(e.key) || e.ctrlKey || e.metaKey)) || (e.key === 'Tab' && this._trapTab)) {
        e.preventDefault();
      }
      // passThrough: keys the UI consumed don't reach the page's listeners.
      if (used && this.opts.passThrough) e.stopPropagation();
    }, active);
    sink.addEventListener('keyup', (e) => {
      this._pressedKeys.delete(e.code || e.key);
      const used = this._inKey(e.key || null, e.code || null, 0, 0, modsOf(e));
      if (used && NAV_KEYS.has(e.key)) e.preventDefault();
      if (used && this.opts.passThrough) e.stopPropagation();
    }, active);
    const flushText = () => {
      const v = sink.value;
      sink.value = '';
      if (v) this._inText(v);
    };
    sink.addEventListener('input', (e) => { if (!e.isComposing) flushText(); }, opt);
    sink.addEventListener('compositionend', flushText, opt);
    // Losing focus must not leave keys or buttons stuck down in the UI.
    const releaseAll = () => {
      for (const [code, key] of this._pressedKeys) {
        this._inKey(key, code, 0, 0, 0);
      }
      this._pressedKeys.clear();
    };
    sink.addEventListener('blur', releaseAll, opt);
    // Gamepads drive the view that has (or last had) keyboard focus.
    sink.addEventListener('focus', () => this._sinkFocused(), opt);
    addEventListener('blur', () => {
      releaseAll();
      if (this._mouseButtons) {
        for (let b = 0; b < 3; b++) {
          if (this._mouseButtons & (1 << b)) this._inButton(-1, -1, mouseButton(b), 0, 0);
        }
        this._mouseButtons = 0;
      }
      this._inLeave();
      this._hostOf()._releasePads();
    }, opt);
    // Leaving the page (bfcache, tab switch) drops the sink's focus so the
    // browser has nothing to restore when the page returns; it comes back
    // only where focusing it can't raise a virtual keyboard (a fine pointer).
    let refocus = false;
    const leave = () => {
      if (document.activeElement !== sink) return;
      refocus = true;
      sink.blur();
    };
    const back = () => {
      if (!refocus) return;
      refocus = false;
      if (!matchMedia('(pointer: coarse)').matches && document.activeElement === document.body) focusSink();
    };
    addEventListener('pagehide', leave, opt);
    addEventListener('pageshow', back, opt);
    document.addEventListener('visibilitychange', () => (document.hidden ? leave() : back()), opt);
    if (this.opts.autoFocus ?? !this.opts.passThrough) focusSink();
  }

  _releasePads() {}

  // Gamepads drive the view that has (or last had) keyboard focus.
  _focusPads() {}

  // The keyboard sink got focus (a click, Tab from the page, focus()).
  _sinkFocused() { this._hostOf()._focusPads(this); }

  // The page pasted `text` (the paste key's clipboard, see _installInput).
  _pasted(text) { this._hostOf()._clipText = text; }

  // A pointer entered / left the canvas (worker mode needs the hit map then).
  _pointerOver() {}

  // hitTest() for this file's own listeners (worker mode: no hitTest() caller).
  _hitAt(x, y) { return this.hitTest(x, y); }
}

// A canvas showing one surface of a runtime: its size, renderer (WebGPU /
// WebGL / CPU blit), input and cursor. JsuiHost is the main surface's view
// and owns the runtime; JsuiSurface views show the surfaces made with
// createSurface(). Every view talks to the runtime through its own handle
// (`rt`): the runtime handle for the main surface, a jsui_surface_create
// handle for the others (include/jsui/jsui.h, Surfaces).
class CanvasView extends DomView {
  constructor(M, canvas, opts) {
    super(canvas, opts);
    this.module = M;
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
    this._installDom();
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

  // Paints the surface if it changed (after the runtime's tick).
  _paint() {
    if (!this.canvas || this._destroyed) return;
    this._checkGL();
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
    this._contentSized(w, h);
  }

  _contentSized(w, h) {
    if (this._autoW) this.canvas.style.width = `${w}px`;
    if (this._autoH) this.canvas.style.height = `${h}px`;
    if (this._measure()) this._setViewport();
    this._rect = null;
  }

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
   * `input_us`, `paint_us`, `other_us`, `gpu_submit_us`; with setProfiling(true)),
   * work counters and JS garbage collection (`gc_collections`,
   * `gc_full_collections`, `gc_us`, `gc_max_us`). `reset` starts new totals. */
  getPerf(reset = true) {
    if (!this.rt) return null;
    // JsuiPerf: 10 doubles, 6 uint64, then the GC fields (2 uint64, 2 doubles): 160 bytes
    const M = this.module, p = this._scratch;
    M._jsui_get_perf(this.rt, p, reset ? 1 : 0);
    const v = new DataView(M.HEAPU8.buffer, p, 160);
    const out = {};
    PERF_TIMES.forEach((k, i) => { out[k] = v.getFloat64(i * 8, true); });
    PERF_COUNTS.forEach((k, i) => { out[k] = Number(v.getBigUint64(80 + i * 8, true)); });
    out.gc_collections = Number(v.getBigUint64(128, true));
    out.gc_full_collections = Number(v.getBigUint64(136, true));
    out.gc_us = v.getFloat64(144, true);
    out.gc_max_us = v.getFloat64(152, true);
    return out;
  }

  // ------------------------------------------------- GPU pipelines (WebGPU)
  //
  // Skia Graphite creates a GPU pipeline the first time a kind of draw is
  // needed, inside that frame. In the browser that is a createRenderPipeline
  // the GPU process compiles (WGSL -> the platform's shaders) before it can
  // go on: with a cold browser shader cache (a first visit, a browser
  // update) tens of milliseconds per pipeline, hundreds for a screen that
  // brings several. A list of the pipelines an app uses (savePipelines(),
  // recorded once and shipped with it) lets the page build them while it
  // still shows its splash: addPipelines(list), then precompilePipelines().
  // WebGPU only (WebGL and the CPU raster have no pipeline lists).

  // The Skia WebGPU context this view draws with, 0 without one.
  _pipelineGpu() { return this._wgpu && !this._wgpu.lost ? this._wgpu.gpu || 0 : 0; }

  /** Queues a pipeline list (the bytes of savePipelines(): an ArrayBuffer or
   * a typed array) for precompilePipelines(). Returns the number of pipelines
   * queued (those already known are skipped), -1 when the list is not one of
   * this module's Skia build, 0 without WebGPU. The list is queued again on
   * a new device after a device loss. */
  addPipelines(list) {
    const bytes = list instanceof Uint8Array ? list : ArrayBuffer.isView(list)
      ? new Uint8Array(list.buffer, list.byteOffset, list.byteLength) : new Uint8Array(list);
    (this._pipelineLists ??= []).push(bytes.slice());
    return this._queuePipelines(bytes);
  }

  _queuePipelines(bytes) {
    const gpu = this._pipelineGpu();
    return gpu && bytes.length ? this._queuePipelinesOn(gpu, bytes) : 0;
  }

  /** Every pipeline the WebGPU context has created or queued, as a list for
   * addPipelines() (Uint8Array), or null without WebGPU. */
  savePipelines() {
    const gpu = this._pipelineGpu();
    if (!gpu) return null;
    const M = this.module, out = this._scratch;
    if (M._jsui_gpu_save_pipelines(gpu, out, out + 4) !== 0) return null;
    const ptr = M.HEAPU32[out >> 2], size = M.HEAPU32[(out >> 2) + 1];
    const bytes = M.HEAPU8.slice(ptr, ptr + size);
    M._jsui_free(ptr);
    return bytes;
  }

  /** The pipeline cache's counters (jsui_gpu_get_pipeline_stats): `queued`,
   * `precompiled`, `failed`, `compiled` (in frames: cache misses), `added`,
   * `unserializable`, `known`; null without WebGPU. */
  pipelineStats() {
    const gpu = this._pipelineGpu();
    if (!gpu) return null;
    // JsuiPipelineStats: 2 ints, 6 uint64
    const M = this.module, p = this._scratch;
    M._jsui_gpu_get_pipeline_stats(gpu, p);
    const v = new DataView(M.HEAPU8.buffer, p, 56);
    const out = { mode: v.getInt32(0, true), queued: v.getInt32(4, true) };
    ['precompiled', 'failed', 'compiled', 'added', 'unserializable', 'known'].forEach((k, i) => {
      out[k] = Number(v.getBigUint64(8 + i * 8, true));
    });
    return out;
  }

  /** Builds the queued pipelines (addPipelines) now, without stalling the
   * page: the GPU process first compiles them all in parallel, off its main
   * thread (warmBrowserCache), then the context creates them, a few at a
   * time, each from the browser's now warm cache. Resolves with
   * pipelineStats() once the queue is empty, `timeoutMs` passed or the
   * context went away; after a timeout the rest is still created in the
   * background as its compiles finish. */
  async precompilePipelines({ timeoutMs = Infinity, batch = 4 } = {}) {
    const gpu = this._pipelineGpu();
    if (!gpu) return null;
    const queued = this.pipelineStats()?.queued ?? 0;
    const warm = this._warmBrowserCache(gpu);
    // A browser whose caches already hold these pipelines answers every
    // warm-up at once; a cold one takes seconds (pipelineCacheWarm()).
    this._cacheWarm ??= warm.length
      ? Promise.race([Promise.all(warm).then(() => true), new Promise((r) => setTimeout(() => r(false), 250))])
      : null;
    // The warm-ups also cover the pipelines the context has already: the
    // i-th queued one is at most that many places further on.
    const done = this._createWarmed(gpu, warm, batch, Math.max(0, warm.length - queued));
    if (timeoutMs !== Infinity) await Promise.race([done, new Promise((r) => setTimeout(r, timeoutMs))]);
    else await done;
    return this.pipelineStats();
  }

  // The browser compiles a pipeline in its GPU process: createRenderPipeline
  // blocks that process (and every frame) until it is done, tens of ms each
  // with a cold shader cache, hundreds on a cold Metal cache.
  // createRenderPipelineAsync compiles on worker threads, several at once,
  // and fills the same caches. Graphite only calls the blocking one, so the
  // queued keys are first precompiled by a throwaway Skia context on the
  // same device whose createRenderPipeline only starts the async compile of
  // the same descriptor (and hands Skia a stand-in it never draws with).
  // Resolves with the compiles' promises, in order.
  _warmBrowserCache(gpu) {
    const M = this.module, wgpu = this._wgpu;
    const queued = this.pipelineStats()?.queued ?? 0;
    if (!queued || !wgpu?.webgpu) return [];
    const list = this.savePipelines();
    const dev = wgpu.device;
    let probe = null;
    const started = [];
    try {
      probe = SharedWebGPU.create(M, wgpu.webgpu, (t) => this._log(1, t));
      if (!probe) return [];
      const n = list ? this._queuePipelinesOn(probe.gpu, list) : 0;
      if (n <= 0) return [];
      const standIn = standInPipeline(dev);
      dev.createRenderPipeline = (desc) => {
        started.push(dev.createRenderPipelineAsync(desc).catch(() => null));
        return standIn;
      };
      while (M._jsui_gpu_precompile_step(probe.gpu, 1000) > 0);
    } catch (e) {
      this._log(1, `pipelines: warming the browser cache failed: ${e?.message ?? e}`);
    } finally {
      delete dev.createRenderPipeline;  // the prototype's again
      probe?.unref();
    }
    return started;
  }

  /** Starts a warm-up (WebGPU): until endWarmup(), frames are not shown but
   * drawn by a throwaway Skia context whose pipelines the browser compiles
   * in parallel, off the frame. Drive the UI through what is about to play
   * (an intro's timeline, a screen's states) while a splash covers the
   * canvas; afterwards the real frames find every pipeline in the browser's
   * cache, ShaderView effects included (which no pipeline list holds).
   * False without WebGPU. */
  beginWarmup() { return this._wgpu ? this._wgpu.beginWarmup((t) => this._log(1, t)) : false; }

  /** Ends the warm-up: resolves with the number of pipelines it compiled once
   * they are done (or after `timeoutMs`); the next frame repaints fully. */
  async endWarmup({ timeoutMs = 5000 } = {}) {
    const n = this._wgpu ? await this._wgpu.endWarmup(timeoutMs) : 0;
    this._sizeChanged = true;
    (this.host ?? this)._wake?.();
    return n;
  }

  _queuePipelinesOn(gpu, bytes) {
    const M = this.module;
    const p = M._malloc(bytes.length);
    try {
      M.HEAPU8.set(bytes, p);
      return M._jsui_gpu_precompile_pipelines(gpu, p, bytes.length);
    } finally {
      M._free(p);
    }
  }

  // Creates the queued pipelines on the context, `batch` at a time, each
  // batch once its warm-up compiles have finished (the queue and the
  // warm-ups are in the same order, `ahead` more of the latter) and
  // followed by a wait for the GPU process. Then the slices after frames
  // take over again.
  async _createWarmed(gpu, warm, batch, ahead) {
    const M = this.module;
    let made = 0;
    for (;;) {
      if (this._pipelineGpu() !== gpu || this._destroyed) return;
      await Promise.all(warm.slice(0, made + batch + ahead));
      if (this._pipelineGpu() !== gpu || this._destroyed) return;
      let left = 0;
      // A budget of 0 creates one pipeline per call.
      for (let i = 0; i < batch; i++, made++) if (!(left = M._jsui_gpu_precompile_step(gpu, 0))) break;
      try {
        await this._wgpu.device.queue.onSubmittedWorkDone();
      } catch {}
      if (!left) break;
    }
    if (this._pipelineGpu() === gpu) M._jsui_gpu_precompile_step(gpu, -1);
  }

  // ---------------------------------------------------------------- helpers

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

  _cursorNow() {
    const p = this.module._jsui_get_cursor(this.rt);
    return p ? this.module.UTF8ToString(p) : 'default';
  }

  // Input into the runtime (DomView's listeners). Each returns whether the UI
  // consumed the event (jsui_send_*). `_inTime`: the DOM time of the events
  // sent next (same clock as the rAF timestamps given to jsui_tick).
  _inTime(t) { this.module._jsui_set_event_time?.(this.rt, t); }
  _inButton(x, y, button, down, mods) { return this.module._jsui_send_mouse_button(this.rt, x, y, button, down, mods); }
  _inMove(x, y) { return this.module._jsui_send_mouse_move(this.rt, x, y); }
  _inTouch(id, phase, x, y) { return this.module._jsui_send_touch(this.rt, id, phase, x, y); }
  _inWheel(x, y, dx, dy) { return this.module._jsui_send_mouse_wheel(this.rt, x, y, dx, dy); }
  _inLeave() { return this.module._jsui_send_mouse_leave(this.rt); }
  _inKey(key, code, down, repeat, mods) {
    return this._withStrings([key, code], (k, c) => this.module._jsui_send_key(this.rt, k, c, down, repeat, mods));
  }
  _inText(text) { return this._withStrings([text], (t) => this.module._jsui_send_text(this.rt, t)); }

  // ------------------------------------------------------------ size / DPR

  _setViewport() {
    const [w, h] = this._viewportSize();
    this.module._jsui_set_viewport(this.rt, w, h, this.pixelRatio);
  }

  _resized() {
    if (!this.rt) return;
    this._setViewport();
    this._rect = null;
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
    this._glOnLost = this._glOnRestored = null;
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
        this._watchGL(() => this._armRestoreTimer('2d'), () => {
          this._setupGL(gl);
          this._texW = this._texH = 0;
          this._sizeChanged = true;  // repaint into the new context
        });
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
    this._watchGL(() => {
      if (this._gpu) M._jsui_gpu_context_lost(this._gpu);  // drop GPU objects without GL calls
      this._armRestoreTimer('cpu');
    }, () => {
      // Extension objects of the lost context are dead: register afresh. The
      // GPU context rebuilds itself on the next render.
      M.GL.deleteContext(this._glHandle);
      this._glHandle = M.GL.registerContext(gl, this._glAttrs);
      this._glGen = (this._glGen ?? 0) + 1;  // host textures: earlier GL objects are gone
      this._sizeChanged = true;  // repaint into the new context
    });
    return true;
  }

  // The canvas' WebGL context (this._gl): `lost` runs once per loss and
  // `restored` once it is back, on the webglcontextlost / -restored events
  // or, when _checkGL() notices first, before them.
  _watchGL(lost, restored) {
    const signal = this._blitAbort.signal;
    this._glOnLost = lost;
    this._glOnRestored = restored;
    this.canvas.addEventListener('webglcontextlost', (e) => {
      e.preventDefault();  // we want it back
      this._glLostNow();
    }, { signal });
    this.canvas.addEventListener('webglcontextrestored', () => this._glRestoredNow(), { signal });
  }

  // A throw that guardGL took for a context loss (logged: the GL glue should not throw).
  _lostInFrame = (e) => this._log(1, `WebGL context lost during a frame (${e?.message ?? e})`);

  _glLostNow() {
    if (this._glLost) return;
    this._glLost = true;
    this._log(1, 'WebGL context lost: waiting for the browser to restore it');
    this._glOnLost?.();
    this._hostOf()._contextLost();
  }

  _glRestoredNow() {
    if (!this._glLost) return;
    clearTimeout(this._restoreTimer);
    this._glLost = false;
    this._log(1, 'WebGL context restored');
    // Lost again while setting up: wait for the next restore.
    guardGL(this.module, this._gl, () => this._glOnRestored?.(), () => this._glLostNow());
  }

  // Before GL work. A browser marks a context lost (isContextLost()) when it
  // notices, and fires webglcontextlost in a later task: a frame or a texture
  // upload can run in between (WebKit; the worker's rAF), and on a lost
  // context Skia's shader compiles fail and emscripten's glCreateProgram
  // throws on the null program. A restore without its event counts too.
  _checkGL() {
    const gl = this._gl;
    if (!gl || !this._glOnLost) return;
    if (gl.isContextLost()) this._glLostNow();
    else if (this._glLost) this._glRestoredNow();
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
      this._gl = null;
      this._glLost = false;
      this._tex = this._prog = this._vb = null;
      this._freshCanvas(() => this._cpuReady(reason, mode));
      return;
    }
    if (this._gl) resetGLState(this._gl);
    this._cpuReady(reason, mode);
  }

  // Replaces the canvas by a clone (same id / class / style / place in the
  // DOM), then calls `then` (worker mode: once the page sent the clone).
  _freshCanvas(then) {
    const old = this.canvas;
    const c = old.cloneNode(false);
    old.replaceWith(c);
    this.canvas = c;
    this._resizeObserver?.unobserve(old);
    this._observeCanvas();
    this._installCanvasInput();
    c.width = this.pixelWidth;
    c.height = this.pixelHeight;
    this._rect = null;
    then();
  }

  _cpuReady(reason, mode) {
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
    for (const list of this._pipelineLists ?? []) this._queuePipelines(list);
    this._log(2, `${reason}: WebGPU renderer recreated`);
    this._emit('renderer', this.renderer, reason);
  }

  // setDevice(): a view on WebGPU moves to the page's new device (on the same
  // canvas context, configured again).
  _useDevice(webgpu) {
    const old = this._wgpu;
    if (this._destroyed || !old || old.webgpu === webgpu) return;
    const ctx = old.ctx, sharedFor = old.sharedFor;
    old.destroy();
    this._wgpu = null;
    this._webgpuRecovered(WebGPURenderer.create(this, webgpu, ctx, sharedFor), 'new WebGPU device');
  }

  // Draws a GPU frame: true when drawn, false when the GPU path failed (the
  // caller falls back to the CPU), undefined while the context is lost.
  _renderGpu() {
    this._checkGL();
    if (this._glLost || !this._glHandle) return undefined;
    const M = this.module;
    M.GL.makeContextCurrent(this._glHandle);
    // JsuiGLTarget {fbo, texture, format, width, height, samples, stencil_bits, flags}
    const t = this._dirty >> 2;
    M.HEAP32.fill(0, t, t + 8);
    M.HEAP32[t + 3] = this.pixelWidth;
    M.HEAP32[t + 4] = this.pixelHeight;
    M.HEAP32[t + 6] = this._stencilBits;
    const r = guardGL(M, this._gl, () => M._jsui_render_gl_target(this.rt, this._gpu, this._dirty), this._lostInFrame);
    // Lost during the frame (WebKit): not drawn, whatever Skia said.
    if (this._gl.isContextLost()) {
      this._glLostNow();
      return undefined;
    }
    return r === 0;
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

  // Frees the view's renderer, listeners and buffers (not its handle).
  _destroyView() {
    this._destroyed = true;
    this._destroyDom();
    this._blitAbort?.abort();
    clearTimeout(this._restoreTimer);
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
    if (!M.GL || !M._jsui_gpu_create_gl) return null;
    if (typeof OffscreenCanvas === 'undefined' && typeof document === 'undefined') return null;
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
    const signal = this._abort.signal;
    canvas.addEventListener('webglcontextlost', (e) => {
      e.preventDefault();  // we want it back
      this._lostNow();
    }, { signal });
    canvas.addEventListener('webglcontextrestored', () => this._restoredNow(), { signal });
  }

  // Once per loss: on webglcontextlost, or before it when check() notices
  // (see CanvasView._checkGL).
  _lostNow() {
    if (this.lost) return;
    this.lost = true;
    this.host._log(1, 'shared WebGL context lost: waiting for the browser to restore it');
    this.module._jsui_gpu_context_lost(this.gpu);  // drop GPU objects without GL calls
    this.host._contextLost();
    clearTimeout(this._restoreTimer);
    const ms = this.host.opts.contextRestoreTimeout ?? 3000;
    if (!(ms >= 0 && ms < Infinity)) return;
    this._restoreTimer = setTimeout(() => {
      if (!this.lost || this.destroyed) return;
      for (const v of [...this.views]) v._switchToCpu('shared WebGL context lost and not restored', false, '2d');
    }, ms);
  }

  _restoredNow() {
    if (!this.lost) return;
    const M = this.module;
    clearTimeout(this._restoreTimer);
    // Extension objects of the lost context are dead: register afresh. The
    // GPU context rebuilds itself on the next render.
    M.GL.deleteContext(this.handle);
    this.handle = M.GL.registerContext(this.gl, this.glAttrs);
    this.lost = false;
    this.gen++;
    this.host._log(1, 'shared WebGL context restored');
    for (const v of this.views) v._sizeChanged = true;  // repaint into the new context
  }

  // Before GL work: a loss or restore the events have not reported yet.
  check() {
    if (this.destroyed) return;
    if (this.gl.isContextLost()) this._lostNow();
    else if (this.lost) this._restoredNow();
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
    this.check();
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
    const r = guardGL(M, this.gl, () => M._jsui_render_gl_target(view.rt, this.gpu, view._dirty), view._lostInFrame);
    M.GL.makeContextCurrent(0);
    // Lost during the frame (WebKit): not drawn, whatever Skia said.
    if (this.gl.isContextLost()) {
      this._lostNow();
      return undefined;
    }
    return r === 0;
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
  _publishesSafeArea() { return !!this.rt; }

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
    this._components = null;     // ComponentSet (registerComponent), made on first use
    this.rootTag = 1;
    /** UI audio (web/jsui-audio.js): unlocked, unlock(), setVolume(), muted, context. */
    this.audio = new JsuiAudio(this, opts.audio);
    // <Video> views (include/jsui/jsui.h "Video"): played with <video>
    // elements here (web/jsui-textures.js JsuiVideo); a worker's runtime
    // forwards the commands to the page instead (opts.video).
    this._videoForward = typeof opts.video === 'function' ? opts.video : null;
    this.video = new JsuiVideo(this, (...a) => this._videoStatus(...a));
    this._idleSleep = opts.idleSleep !== false && !opts.devMode;
    this._wakeTimer = 0;
    this._loop = null;
    wakeOnCalls(M).add(this);

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
      onComponent: (cls, kind, view, canvas, info, a, b) => this._components?.call(cls, kind, view, canvas, info, a, b),
      // Clipboard (jsui_set_clipboard_callbacks): pastes read what the page
      // pasted last (or the UI copied), copies go to the page's clipboard.
      clipboardText: () => this._clipText ?? '',
      onClipboard: (text) => this._copied(text),
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
      onAudio: (cmd) => this.audio.command(cmd),
      onVideo: (cmd) => (this._videoForward ? this._videoForward(cmd) : this.video.command(cmd)),
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
    // The JS engine: QuickJS in WASM, or this thread's own (the module's
    // jsui_web_host_js says whether it was built with it).
    // Default: the host engine where it can run (built in, eval allowed).
    const js = opts.js ?? 'host';
    if (js !== 'quickjs' && js !== 'host') throw new TypeError(`createJsui: js must be 'quickjs' or 'host', got ${js}`);
    const hostBuilt = !!M._jsui_web_host_js?.();
    const hostEval = hostBuilt && evalAllowed();
    /** 'quickjs' | 'host': the engine the bundle runs on. */
    this.jsEngine = js === 'host' && hostEval ? 'host' : 'quickjs';
    if (opts.js === 'host' && this.jsEngine !== 'host') {
      console.warn(hostBuilt ? '[jsui] eval is blocked (Content-Security-Policy without \'unsafe-eval\'): running the bundle on QuickJS'
                             : '[jsui] this jsui.wasm has no host JS engine: running the bundle on QuickJS');
    }
    this.rt = M._jsui_web_create(this.hostId, this.width, this.height, this.pixelRatio, opts.devMode ? 1 : 0,
      this._baseUrlPtr, this.audio.enabled ? 1 : 0, this.jsEngine === 'host' ? 1 : 0);
    if (!this.rt) throw new Error('jsui_web_create failed');
    this._publishSafeArea();
    if (opts.jsFrameBudget != null) M._jsui_set_js_frame_budget(this.rt, +opts.jsFrameBudget);
    if (opts.devMode && M._jsui_set_dev_session) {
      this._withStrings([opts.devName ?? null], (n) => M._jsui_set_dev_session(this.rt, n, opts.devSession === false ? 0 : 1));
    }
    // The main surface's AppRegistry component (else the bundle's first).
    if (opts.component != null) this.run(opts.component, opts.initialProps);

    this._mode = opts.renderer ?? 'auto';
    this._webgpu = opts._webgpu ?? null;
    this._pageWebGPU = this._webgpu?.external ? this._webgpu : null;  // createJsui({ device })
    // Without a canvas the main surface is not shown (a page of createSurface() widgets).
    if (canvas) this._initView(this._mode, this._webgpu);
    if (opts.gamepads !== false) this._installGamepads();
    this._pipelinesQueued = new Promise((r) => { this._queuedDone = r; });
    this._pipelinesReady = this._loadPipelines(opts).finally(() => this._queuedDone());

    // Before the bundle runs, so its first render sees them (hasNativeComponent).
    if (opts.components && typeof opts.components === 'object') this._registerComponents(opts.components);
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
    const type = bufferType(Type);
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
   * a VideoFrame, <video>, <canvas>, OffscreenCanvas, ImageBitmap, <img>,
   * ImageData or { data, width, height } (straight-alpha RGBA in any typed
   * array). Call it again whenever the picture changes (each video
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

  // ------------------------------------------- native components (jsui.h)

  /** Native component `name`, drawn by `impl` (web/jsui-components.js: draw(ctx,
   * view), optional create / props / command / destroy, clocked): the bundle
   * shows it with requireNativeComponent(name). Registering again replaces
   * it; mounted views bind to it at once. Returns true when registered. */
  registerComponent(name, impl) {
    if (this._destroyed) return false;
    this._components ??= new ComponentSet(this);
    return this._components.register(String(name), impl);
  }

  /** Registers every export of `components`: an object { Name: impl }, or
   * the URL of a module whose exports are (worker mode imports it in the
   * worker, where its draw functions run). Resolves to the names registered. */
  async loadComponents(components) {
    const mod = typeof components === 'string' || components instanceof URL
      ? await import(new URL(components, typeof location !== 'undefined' ? location.href : undefined).href)
      : components;
    return this._registerComponents(mod);
  }

  _registerComponents(mod) {
    const names = [];
    for (const [name, impl] of Object.entries(mod ?? {})) {
      if (name === 'default' || !impl || typeof impl.draw !== 'function') continue;
      if (this.registerComponent(name, impl)) names.push(name);
    }
    return names;
  }

  /** Unregisters native component `name` (its views show their View part). */
  unregisterComponent(name) {
    if (this._destroyed || !this._components) return false;
    return this._components.unregister(String(name));
  }

  /** Repaints native component view `view` (an id, as view.id in draw); 0:
   * every view of class `name` (undefined: of every class). Returns how
   * many were marked. */
  invalidateComponent(view, name) {
    if (this._destroyed) return 0;
    return this._withStrings([name == null ? null : String(name)],
      (n) => this.module._jsui_component_invalidate(this.rt, view | 0, n));
  }

  /** Calls the view's on<Event> prop (onContactPress for 'contactPress')
   * with nativeEvent = `data`, in the next frame. True if it listens. */
  emitComponentEvent(view, event, data) {
    if (this._destroyed) return false;
    return this._withStrings([String(event), JSON.stringify(data ?? {})],
      (e, j) => this.module._jsui_component_emit(this.rt, view | 0, e, j)) === 1;
  }

  // A <Video>'s status (JsuiVideo's report) to the runtime.
  _videoStatus(id, state, duration, position, buffered, width, height, error) {
    if (this._destroyed || !this.rt) return;
    this._withStrings([error || null], (e) =>
      this.module._jsui_web_video_status(this.rt, id, state, duration, position, buffered, width, height, e));
  }

  /** Where host textures go now: 'webgpu' (GPU textures on the page's
   * device), 'gl' (textures of the views' WebGL context), 'pixels' (CPU
   * copies), or null while a GPU context is lost. */
  get textureBackend() { return this._textureSet().backend; }

  _textureSet() { return (this._textures ??= new HostTextureSet(this)); }

  // A loss or restore of any of the runtime's WebGL contexts not handled yet
  // (CanvasView._checkGL), before GL work.
  _checkContexts() {
    this._checkGL();
    if (this._sgl0 && !this._sgl0.destroyed) this._sgl0.check();
    for (const s of this._surfaces) s._checkGL();
  }

  _anyContextLost() {
    const lost = (v) => !!v._gl?.isContextLost?.();
    return lost(this) || !!this._sgl0?.gl.isContextLost() || [...this._surfaces].some(lost);
  }

  // One of the runtime's WebGL contexts was lost. Bound videos switch to
  // polled frames if a frame callback seems to have cost it (jsui-textures.js).
  _contextLost() {
    const switched = videoFramesContextLost();
    if (switched) this._log(1, switched);
  }

  // The views that draw this runtime's textures (host textures go where all can sample them).
  _views() {
    const v = this.canvas && !this._destroyed ? [this] : [];
    for (const s of this._surfaces) if ((s.canvas || s._wgpu) && !s._destroyed) v.push(s);
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

  // The UI copied `text` (jsui_set_clipboard_callbacks); a worker's host
  // forwards it to the page.
  _copied(text) {
    this._clipText = text;
    writeClipboard(text);
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

  /** Evaluates a bundle: JS source (string) or the bytes of a `jsui build` bundle (precompiled bytecode). */
  loadSource(source, name = 'bundle.js') {
    const M = this.module;
    let len, src;
    if (typeof source === 'string') {
      len = M.lengthBytesUTF8(source);
      src = M._malloc(len + 1);
      M.stringToUTF8(source, src, len + 1);
    } else {
      const bytes = source instanceof Uint8Array ? source : new Uint8Array(source);
      len = bytes.length;
      src = M._malloc(len + 1);
      M.HEAPU8.set(bytes, src);
      M.HEAPU8[src + len] = 0;
    }
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

  /**
   * Registers a font from bytes (ArrayBuffer / typed array: TTF, OTF, TTC /
   * OTC, WOFF or WOFF2; copied) for every surface. `family` '' / null: the
   * family name in the file. `weight` (1..1000) and `italic` (boolean, or
   * 'italic' / 'normal') override the file's; omitted, they come from the
   * file. `weight` may instead be `{ weight, style }` (style 'italic' /
   * 'normal'), and `registerFont(bytes, { ... })` drops the family. True on
   * success, false for data that is not a font.
   */
  registerFont(family, bytes, weight, italic) {
    if (this._destroyed) return false;
    [family, bytes, weight, italic] = fontArgs(family, bytes, weight, italic);
    const M = this.module;
    const data = ArrayBuffer.isView(bytes) ? new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength)
      : new Uint8Array(bytes);
    const p = M._malloc(Math.max(1, data.length));
    M.HEAPU8.set(data, p);
    const r = this._withStrings([family], (f) => M._jsui_register_font(this.rt, f, p, data.length, weight, italic));
    M._free(p);
    return r === 0;
  }

  /** Fetches a font (`url` resolved against the page) and registers it as
   * registerFont(family, bytes, weight, italic) does. Promise<boolean>:
   * false (logged) on a failed fetch or data that is not a font. */
  async loadFont(family, url, weight, italic) {
    if (this._destroyed) return false;
    const abs = new URL(url, typeof location !== 'undefined' ? location.href : undefined).href;
    let bytes;
    try {
      const res = await fetch(abs);
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      bytes = await res.arrayBuffer();
    } catch (e) {
      this._log(2, `loadFont ${abs}: ${e?.message ?? e}`);
      return false;
    }
    if (this._destroyed) return false;
    const ok = this.registerFont(family ?? '', bytes, weight, italic);
    if (!ok) this._log(2, `loadFont ${abs}: not a font`);
    return ok;
  }

  /** Removes every face registered under `family` (text using it falls back
   * to the default family). True if it was registered. */
  unregisterFont(family) {
    if (this._destroyed) return false;
    return this._withStrings([String(family)], (f) => this.module._jsui_unregister_font(this.rt, f)) === 0;
  }

  /** Families tried, in order, for glyphs the styled font lacks (emoji, CJK):
   * an array or a comma-separated string. Null / [] clears the list. Every
   * registered font is searched after these anyway. */
  setFontFallbacks(families) {
    if (this._destroyed) return;
    this._withStrings([fontList(families)], (f) => this.module._jsui_set_font_fallbacks(this.rt, f));
  }

  /** The family used when a style names none (default 'Inter'). */
  setDefaultFontFamily(family) {
    if (this._destroyed) return;
    this._withStrings([family == null ? null : String(family)],
      (f) => this.module._jsui_set_default_font_family(this.rt, f));
  }

  get lastError() { return this._lastError; }

  /** Resolves after a frame with no resource loads in flight (bundle, images,
   * fonts, fetch) and nothing left to paint on any canvas: the UI is on screen. */
  whenIdle() {
    return new Promise((resolve) => (this._idleWaiters ??= []).push(resolve));
  }

  /** Resolves once the GPU pipelines this page is known to need are built
   * (createJsui's `pipelines` list and those this browser built on earlier
   * visits; see addPipelines), with pipelineStats(), or null without WebGPU.
   * Wait for it before an animation that must not stutter, e.g. while a
   * splash is up. Gives up after `pipelineTimeout` ms (default 5000): the
   * rest is then built one per frame. */
  whenPipelinesReady() { return this._pipelinesReady; }

  /** Whether the browser's shader caches already held the pipelines of
   * createJsui's lists (all compiled within 250 ms of asking): true, false,
   * or null without lists or WebGPU. A warm-up (beginWarmup) can be skipped
   * when true: what it would compile is most likely cached too. */
  async pipelineCacheWarm() {
    await this._pipelinesQueued;
    return (await this._cacheWarm) ?? null;
  }

  // createJsui({ pipelines, rememberPipelines, pipelineKey, pipelineTimeout }):
  // queues the shipped list and the one remembered from this browser's
  // earlier visits, builds them, then keeps the remembered list up to date.
  async _loadPipelines(opts) {
    if (!this._pipelineGpu() || opts.pipelines === false) return null;
    const remember = opts.rememberPipelines !== false;
    const key = String(opts.pipelineKey ?? pipelineKeyFor(this));
    const [shipped, known] = await Promise.all([
      opts.pipelines != null ? pipelineList(opts.pipelines, this._wgpu?.format).catch((e) => {
        this._log(2, `pipelines: ${e?.message ?? e}`);
        return null;
      }) : null,
      remember ? rememberedPipelines(key) : null,
    ]);
    if (this._destroyed) return null;
    for (const list of [shipped, known]) {
      if (list && this.addPipelines(list) < 0) this._log(1, 'pipelines: a list of another JSUI build, skipped');
    }
    if (remember) this._rememberPipelines(key);
    const ready = this.precompilePipelines({ timeoutMs: opts.pipelineTimeout ?? 5000 });
    this._queuedDone();
    return ready;
  }

  // Saves the pipelines this browser built (savePipelines) under `key` when
  // new ones appeared: checked every few seconds and when the page is hidden.
  _rememberPipelines(key) {
    let saved = -1;
    const save = () => {
      const st = this.pipelineStats();
      if (!st || st.known === saved) return;
      const list = this.savePipelines();
      if (!list) return;
      saved = st.known;
      storePipelines(key, list);
    };
    this._pipelineTimer = setInterval(save, 4000);
    if (typeof document !== 'undefined') {
      this._pipelineHidden = () => document.visibilityState === 'hidden' && save();
      document.addEventListener('visibilitychange', this._pipelineHidden);
    }
  }

  start() {
    if (this._loop || this._destroyed) return;
    const loop = this._loop = (t) => {
      this._raf = requestAnimationFrame(loop);
      this.frame(t);
      if (this._loop === loop && this._idleSleep && this._idle()) this._sleep();
    };
    this._raf = requestAnimationFrame(loop);
  }

  stop() {
    if (this._raf) cancelAnimationFrame(this._raf);
    clearTimeout(this._wakeTimer);
    this._raf = this._wakeTimer = 0;
    this._loop = null;
  }

  /** Whether the frame loop runs (start() and not paused while idle). */
  get running() { return !!this._raf; }

  // Nothing to tick or paint until input, a call into the runtime or a timer
  // (see idleSleep): no animation, JS work, load, pending paint, resize,
  // polled gamepad or per-frame listener.
  _idle() {
    const M = this.module, rt = this.rt;
    if (!rt || M._jsui_wants_frame(rt) || this._wantsPaint() || this._sizeChanged || this._padsActive ||
        this._autoW || this._autoH || this._listens('frame') || this._idleWaiters?.length || this._anyContextLost())
      return false;
    for (const s of this._surfaces)
      if (s._wantsPaint() || s._sizeChanged || s._autoW || s._autoH) return false;
    return true;
  }

  // Pauses the loop until _wake(): at the next JS timer, and once a second
  // anyway (a safety net: a change that reached the runtime some other way).
  _sleep() {
    cancelAnimationFrame(this._raf);
    this._raf = 0;
    const due = this.module._jsui_next_timer(this.rt) - performance.now();
    this._wakeTimer = setTimeout(() => this._wake(), Math.max(0, Math.min(due, 1000)));
  }

  _wake() {
    if (this._raf || !this._loop || this._destroyed) return;
    clearTimeout(this._wakeTimer);
    this._wakeTimer = 0;
    this._raf = requestAnimationFrame(this._loop);
  }

  _wakeLoop() { this._wake(); }

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
    const s = this._makeSurface(target, options ?? {});
    this._surfaces.add(s);
    return s;
  }

  _makeSurface(target, options) { return new JsuiSurface(this, target, options); }

  /**
   * Shows the AppRegistry component `options.component` in a GPUTexture
   * instead of a canvas: the UI as a texture of a WebGPU game (screens and
   * panels in the 3D world, or a HUD the game composites in its own final
   * pass). Same runtime as createSurface(): its own props, size, input and
   * focus. The texture is on the runtime's WebGPU device (createJsui({
   * device }) to share the game's). `options.texture`: a GPUTexture of the
   * game's to render into (RENDER_ATTACHMENT | TEXTURE_BINDING usage,
   * rgba8unorm / bgra8unorm, COPY_SRC too for readPixelsAsync); without one
   * the surface makes its own (`surface.texture`, replaced on resize:
   * 'texture' event). Frames render in the runtime's frame loop when
   * something changed (`autoRender: false`: only when the page calls
   * renderTo()). Input comes from the page: dispatchEvent(e, x, y) or
   * pointerDown / pointerMove / ... with surface coordinates (e.g. a ray
   * hit's UV times width / height).
   * @param {JsuiTextureSurfaceOptions} options
   */
  createTextureSurface(options) {
    if (this._destroyed) throw new Error('createTextureSurface: runtime destroyed');
    const s = new JsuiTextureSurface(this, options ?? {});
    this._surfaces.add(s);
    return s;
  }

  /** The runtime's WebGPU device (the page's, with createJsui({ device })), or
   * null (also when the loaded module has no WebGPU backend: not dist-gpu/). */
  get device() { return this.module.WebGPU ? (this._pageWebGPU ?? this._webgpu)?.device ?? null : null; }

  /** After the page's GPUDevice was lost ('devicelost'): the page's new
   * device. Every WebGPU view, texture surface and GPUTexture host texture
   * moves to it (setTexture GPUTextures of the new device again; other host
   * textures are uploaded again by themselves). Texture surfaces without a
   * texture of the page's make a new one ('texture' event). */
  setDevice(device) {
    if (this._destroyed) return;
    const webgpu = externalWebGPU(device);
    this._pageWebGPU = webgpu;
    this._deviceLost = null;
    if (this._webgpu || this.canvas == null) this._webgpu = webgpu;
    if (this.canvas) this._useDevice(webgpu);
    for (const s of this._surfaces) s._useDevice(webgpu);
    this._wake();
  }

  // A renderer's page device was lost (web/jsui-webgpu.js): said once per device.
  _externalDeviceLost(webgpu, info) {
    if (this._deviceLost === webgpu) return;
    this._deviceLost = webgpu;
    this._log(2, `the page's WebGPU device was lost (${info?.reason ?? 'unknown'}: ${info?.message ?? ''}): ` +
      'the UI shows again once the page passes its new device to setDevice()');
    this._emit('devicelost', info);
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
    const sp = this.module.stackSave?.();
    try {
      this._frameNow(now);
    } catch (e) {
      // A context lost in the middle of GL work (see guardGL): the loss, not an error.
      if (!this._anyContextLost()) throw e;
      if (sp !== undefined) this.module.stackRestore(sp);
      this._lostInFrame(e);
      this._checkContexts();
    }
  }

  _frameNow(now) {
    const M = this.module, rt = this.rt, st = this.stats;
    if (this._padsActive) this._pollGamepads();
    // Context losses and restores first: the textures then move in this frame.
    this._checkContexts();
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
    if (this._listens('frame')) this._emit('frame', st);
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
    clearInterval(this._pipelineTimer);
    if (this._pipelineHidden) document.removeEventListener('visibilitychange', this._pipelineHidden);
    // GPU textures first, while their contexts exist.
    this._textures?.destroy();
    this._textures = null;
    this._components?.destroy();
    this._components = null;
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
    this.video.destroy();  // before the runtime's own CLOSEs
    M._jsui_destroy(this.rt);  // stops every voice (STOP_ALL)
    this.rt = 0;
    M.__jsuiHosts.delete(this.hostId);
    M.__jsuiWake?.delete(this);
    // Voices, nodes; the page's AudioContext closes with the last runtime.
    this.audio.destroy();
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
    const poll = () => { this._padsActive = true; this._wake(); };
    addEventListener('gamepadconnected', poll, { signal });
    addEventListener('gamepaddisconnected', poll, { signal });
    this._padsActive = Array.from(navigator.getGamepads()).some(Boolean);
  }

  _pollGamepads(pads = navigator.getGamepads()) {
    const M = this.module, rt = (this._padView ?? this).rt;
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

  _focusPads(view) {
    if ((this._padView ?? this) === view) return;
    this._releasePads();
    this._padView = view === this ? null : view;
  }

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
  _wakeLoop() { this.host?._wake(); }

  constructor(host, target, opts) {
    if (!opts.component) throw new Error('createSurface: component is required');
    if (!target) throw new Error('createSurface: a canvas or element is required');
    let canvas = target, owned = false;
    // An OffscreenCanvas: worker mode's surfaces (web/jsui-worker.js).
    const offscreen = typeof OffscreenCanvas !== 'undefined' && target instanceof OffscreenCanvas;
    if (target.tagName !== 'CANVAS' && !offscreen) {
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
    if (opts.width > 0 && !this._autoW && canvas.style) canvas.style.width = `${opts.width}px`;
    if (opts.height > 0 && !this._autoH && canvas.style) canvas.style.height = `${opts.height}px`;
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

/** @typedef {{
 *   component: string,           // the AppRegistry key to show
 *   initialProps?: object,
 *   width?: number, height?: number,  // logical px (default 512 x 512; with `texture`: its size / pixelRatio)
 *   pixelRatio?: number,         // device px per logical px (default 1)
 *   texture?: GPUTexture,        // render into this texture of the game's instead of one of the surface's own
 *   format?: 'rgba8unorm'|'bgra8unorm', // the surface's own texture's format (default 'rgba8unorm')
 *   autoRender?: boolean,        // render in the runtime's frame loop when something changed (default
 *                                // true); false: only renderTo() renders
 *   renderer?: never,
 * }} JsuiTextureSurfaceOptions */

const TEXTURE_FORMATS = new Set(['rgba8unorm', 'bgra8unorm', 'rgba16float']);
// GPUTextureUsage
const TEX_COPY_SRC = 0x01, TEX_BINDING = 0x04, TEX_RENDER_ATTACHMENT = 0x10;
const WEBGPU_INCREMENTAL = 1 << 6;  // JSUI_WEBGPU_INCREMENTAL

// A surface of the runtime (like JsuiSurface) shown in a GPUTexture instead
// of a canvas (createTextureSurface). It draws with a WebGPURenderer whose
// "canvas context" is a stand-in handing out the current texture, so warm-
// ups, pipeline lists, device loss and readback work as on a canvas. The
// texture keeps its pixels between frames, so frames repaint only the damage
// straight into it (JSUI_WEBGPU_INCREMENTAL) while it is the texture of the
// last render. No DOM: the page forwards input and shows the cursor itself.
class JsuiTextureSurface extends CanvasView {
  _wakeLoop() { this.host?._wake(); }

  constructor(host, opts) {
    if (!opts.component) throw new Error('createTextureSurface: component is required');
    const webgpu = host._pageWebGPU ?? host._webgpu;
    if (!webgpu || !host.module.WebGPU) {
      throw new Error('createTextureSurface: needs WebGPU and dist-gpu/ (createJsui({ device }) shares the ' +
        'page\'s device)');
    }
    const ratio = opts.pixelRatio > 0 ? opts.pixelRatio : 1;
    super(host.module, null, { ...opts, pixelRatio: ratio, autoFocus: false, trapTab: false });
    const M = host.module;
    this.host = host;
    this.component = String(opts.component);
    this._autoRender = opts.autoRender !== false;
    this._target = null;        // the game's texture (options.texture / renderTo)
    this._own = null;           // the surface's own texture (no options.texture)
    this._ownFormat = opts.format ?? 'rgba8unorm';
    if (!TEXTURE_FORMATS.has(this._ownFormat)) throw new Error(`createTextureSurface: format ${this._ownFormat}`);
    this._rendering = null;     // the texture the current render() draws into
    this._last = null;          // the texture of the last render (it holds that frame)
    /** Renders so far (each a new picture in the texture). */
    this.version = 0;
    if (opts.texture) this._checkTarget(opts.texture);
    const tex = opts.texture;
    this._setSize(tex ? tex.width / ratio : opts.width > 0 ? opts.width : 512,
      tex ? tex.height / ratio : opts.height > 0 ? opts.height : 512);
    this._target = tex ?? null;
    this.rt = this._withStrings([this.component, JSON.stringify(opts.initialProps ?? {})],
      (k, p) => M._jsui_surface_create(host.rt, k, p, this.width, this.height, this.pixelRatio));
    if (!this.rt) {
      super._destroyView();
      throw new Error(`createTextureSurface("${this.component}") failed (see the log)`);
    }
    /** The surface's React Native root tag (RootTagContext in its components). */
    this.rootTag = M._jsui_surface_tag(this.rt);
    this._mode = 'webgpu';
    this._webgpu = webgpu;
    if (!this._makeRenderer(webgpu)) {
      M._jsui_surface_destroy(this.rt);
      super._destroyView();
      throw new Error('createTextureSurface: Skia could not use the WebGPU device (see the log)');
    }
    this._log(1, `renders into a ${this.pixelWidth}x${this.pixelHeight} texture (${webgpu.description})`);
  }

  _makeRenderer(webgpu) {
    const ctx = {
      jsuiFormat: this._format(),
      configure() {}, unconfigure() {},
      getCurrentTexture: () => this._rendering ?? this._texture(),
    };
    this._wgpu = WebGPURenderer.create(this, webgpu, ctx, (w) => this.host._sharedWebGPU(w));
    this.renderer = this._wgpu ? 'webgpu' : 'none';
    return !!this._wgpu;
  }

  _hostOf() { return this.host; }
  _log(level, text) { this.host._log(level, `${this.component} (texture, root ${this.rootTag ?? '?'}): ${text}`); }

  /** The texture the surface renders into: the game's (options.texture /
   * the last renderTo()) or its own (made on first use, replaced on resize
   * and device change: 'texture' event). */
  get texture() { return this._target ?? this._texture(); }

  _format() { return this._target?.format ?? this._ownFormat; }

  _texture() {
    if (this._target) return this._target;
    const o = this._own;
    if (o && o.width === this.pixelWidth && o.height === this.pixelHeight) return o;
    const device = this._webgpu.device;
    this._own = device.createTexture({ label: `jsui ${this.component}`, size: [this.pixelWidth, this.pixelHeight],
      format: this._ownFormat, usage: TEX_RENDER_ATTACHMENT | TEX_BINDING | TEX_COPY_SRC });
    // The old one may still be sampled by work the game encodes this frame.
    if (o) requestAnimationFrame(() => o.destroy());
    this._emit('texture', this._own);
    return this._own;
  }

  _checkTarget(t) {
    if (typeof GPUTexture !== 'undefined' && !(t instanceof GPUTexture)) throw new TypeError('not a GPUTexture');
    if (!(t.usage & TEX_RENDER_ATTACHMENT)) throw new Error('the texture needs RENDER_ATTACHMENT usage');
    if (!TEXTURE_FORMATS.has(t.format)) throw new Error(`texture format ${t.format} (rgba8unorm, bgra8unorm or rgba16float)`);
  }

  // Logical size; the texture is that times pixelRatio.
  _setSize(w, h) {
    const r = this._fixedRatio || 1;
    const pw = Math.max(1, Math.round(w * r)), ph = Math.max(1, Math.round(h * r));
    if (pw === this.pixelWidth && ph === this.pixelHeight && w === this.width) return false;
    this.width = w;
    this.height = h;
    this.pixelRatio = r;
    this.pixelWidth = pw;
    this.pixelHeight = ph;
    this._sizeChanged = true;
    return true;
  }

  /** New logical size (and pixel ratio): the own texture is replaced
   * ('texture' event). With a texture of the game's, use renderTo() with one
   * of the new size instead. */
  setSize(width, height, pixelRatio) {
    if (this._destroyed) return;
    if (pixelRatio > 0) this._fixedRatio = pixelRatio;
    if (this._setSize(width, height)) this._setViewport();
  }

  // No DOM: nothing to measure, observe or listen to.
  _measure() { return false; }
  _installDom() {}
  _publishSafeArea() {}

  _updateCursor() {
    const c = this._cursorNow();
    if (c === this._cursor) return;
    this._cursor = c;
    this._emit('cursor', c);
  }

  /** The CSS cursor the UI wants where the pointer last was ('default', 'pointer', 'text', ...). */
  get cursor() { return this._cursorNow(); }

  _wantsPaint() { return this._autoRender && !this._destroyed && !!this.module._jsui_needs_redraw(this.rt); }

  _paint() {
    if (this._autoRender) this._render(null);
  }

  /**
   * Renders the UI's current frame into `texture` (a GPUTexture of the
   * device, RENDER_ATTACHMENT usage, rgba8unorm / bgra8unorm / rgba16float),
   * or into the surface's texture when omitted. A texture of another size
   * resizes the surface to it (logical size = its size / pixelRatio). Only
   * what changed since the last render into the same texture is repainted;
   * another texture gets the whole frame. Call it after the runtime's frame
   * (the 'frame' event), or from the game's loop. Returns whether it drew.
   */
  renderTo(texture) {
    if (this._destroyed) return false;
    if (texture && texture !== this._target) {
      this._checkTarget(texture);
      this._target = texture;
      const own = this._own;
      this._own = null;
      if (own) requestAnimationFrame(() => own.destroy());
    }
    if (this._target && (this._target.width !== this.pixelWidth || this._target.height !== this.pixelHeight)) {
      const r = this._fixedRatio || 1;
      if (this._setSize(this._target.width / r, this._target.height / r)) this._setViewport();
    }
    return this._render(this._target);
  }

  _render(target) {
    const wgpu = this._wgpu;
    if (this._destroyed || !wgpu || wgpu.lost) return false;
    const tex = target ?? this.texture;
    if (wgpu.format !== tex.format) wgpu.format = tex.format;
    const same = tex === this._last;
    if (same && !this.module._jsui_needs_redraw(this.rt) && !this._sizeChanged) return false;
    this._sizeChanged = false;
    const t1 = performance.now();
    wgpu.flags = same ? WEBGPU_INCREMENTAL : 0;
    this._rendering = tex;
    let r;
    try {
      r = wgpu.render();
    } finally {
      this._rendering = null;
    }
    this._last = r ? tex : null;
    if (r === false) this._log(2, 'WebGPU frame failed');
    if (!r) return false;
    this.stats.renderMs = ema(this.stats.renderMs, performance.now() - t1);
    this.stats.renders++;
    this.version++;
    this._updateCursor();
    this._emit('render', tex);
    return true;
  }

  // The renderer failed for good: nothing to fall back to without a canvas.
  _dropWebGPU(reason) {
    this._log(2, `${reason}: the texture is not updated any more`);
    this._wgpu?.destroy();
    this._wgpu = null;
    this.renderer = 'none';
  }

  _webgpuRecovered(next, reason) {
    if (this._destroyed) {
      next?.destroy();
      return;
    }
    if (!next) return this._dropWebGPU(`${reason}, no new device`);
    this._wgpu = next;
    this._webgpu = next.webgpu;
    this._last = null;
    this._sizeChanged = true;
    this._log(1, `${reason}: renderer recreated`);
  }

  _useDevice(webgpu) {
    if (this._destroyed || this._webgpu === webgpu) return;
    this._wgpu?.destroy();
    this._wgpu = null;
    this._webgpu = webgpu;
    // Textures of the old device are dead: a new own one, and the game's
    // comes again with renderTo().
    if (this._own) this._own = null;
    if (this._target) {
      this._target = null;
      this._log(1, 'new device: render again with renderTo(a texture of the new device)');
    }
    this._last = null;
    this._sizeChanged = true;
    if (!this._makeRenderer(webgpu)) this._log(2, 'Skia could not use the new WebGPU device');
  }

  /** Renders the current frame into the texture and reads it back
   * (premultiplied RGBA, top row first; 8-bit formats; COPY_SRC usage). */
  async readPixelsAsync() {
    if (!this._wgpu || this._wgpu.lost) return null;
    this._wgpu.flags = 0;
    const px = await this._wgpu.readPixels();
    this._last = px ? this.texture : null;
    return px;
  }

  /** Focus for gamepads (keys and pointers go where the page sends them). */
  focus() { this.host._focusPads(this); }

  // ------------------------------------------------------------- input
  // Coordinates are logical px of the surface (0..width, 0..height from the
  // top-left); e.g. a raycast hit's UV: (u * width, (1 - v) * height). Each
  // returns whether the UI consumed it.

  pointerMove(x, y, { pointerId = 1, pointerType = 'mouse', timeStamp } = {}) {
    this._stamp(timeStamp);
    const r = pointerType === 'mouse' ? this._inMove(x, y) : this._inTouch(pointerId, 1, x, y);
    this._updateCursor();
    return !!r;
  }

  pointerDown(x, y, { button = 0, pointerId = 1, pointerType = 'mouse', modifiers = 0, timeStamp } = {}) {
    this._stamp(timeStamp);
    let r;
    if (pointerType === 'mouse') {
      this._mouseButtons |= 1 << button;
      r = this._inButton(x, y, mouseButton(button), 1, modifiers);
    } else {
      r = this._inTouch(pointerId, 0, x, y);
    }
    this._updateCursor();
    return !!r;
  }

  pointerUp(x, y, { button = 0, pointerId = 1, pointerType = 'mouse', modifiers = 0, timeStamp, cancel = false } = {}) {
    this._stamp(timeStamp);
    let r;
    if (pointerType === 'mouse') {
      if (!(this._mouseButtons & (1 << button))) return false;
      this._mouseButtons &= ~(1 << button);
      r = this._inButton(x, y, mouseButton(button), 0, modifiers);
    } else {
      r = this._inTouch(pointerId, cancel ? 3 : 2, x, y);
    }
    this._updateCursor();
    return !!r;
  }

  /** The pointer left the surface (hover states end). */
  pointerLeave() {
    const r = this._inLeave();
    this._updateCursor();
    return !!r;
  }

  wheel(x, y, deltaX, deltaY) { return !!this._inWheel(x, y, deltaX, deltaY); }

  key(key, code, down, { repeat = false, modifiers = 0 } = {}) {
    return !!this._inKey(key ?? null, code ?? null, down ? 1 : 0, repeat ? 1 : 0, modifiers);
  }

  text(text) { return !!this._inText(String(text)); }

  /**
   * Forwards a DOM event the page received: a PointerEvent / MouseEvent
   * (pointerdown, pointermove, pointerup, pointercancel, pointerleave),
   * WheelEvent, or KeyboardEvent (keydown, keyup; a printable keydown also
   * types its text). `x`, `y`: where it hits the surface (logical px), for
   * pointer and wheel events. Returns whether the UI consumed it.
   */
  dispatchEvent(e, x, y) {
    const mods = modsOf(e), timeStamp = e.timeStamp;
    const pointer = { button: e.button ?? 0, pointerId: e.pointerId ?? 1, pointerType: e.pointerType || 'mouse',
      modifiers: mods, timeStamp };
    switch (e.type) {
      case 'pointerdown': case 'mousedown': return this.pointerDown(x, y, pointer);
      case 'pointermove': case 'mousemove': return this.pointerMove(x, y, pointer);
      case 'pointerup': case 'mouseup': return this.pointerUp(x, y, pointer);
      case 'pointercancel': return this.pointerUp(x, y, { ...pointer, cancel: true });
      case 'pointerleave': case 'pointerout': case 'mouseleave': return this.pointerLeave();
      case 'wheel': {
        const k = e.deltaMode === 1 ? 16 : e.deltaMode === 2 ? this.height : 1;
        return this.wheel(x, y, e.deltaX * k, e.deltaY * k);
      }
      case 'keydown': case 'keyup': {
        if (e.isComposing) return false;
        const down = e.type === 'keydown';
        let used = this.key(e.key || null, e.code || null, down, { repeat: e.repeat, modifiers: mods });
        if (down && e.key?.length === 1 && !e.ctrlKey && !e.metaKey) used = this.text(e.key) || used;
        return used;
      }
      default: return false;
    }
  }

  _stamp(t) { if (t != null) this._inTime(t); }

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

  /** Unmounts the component and frees the surface and its own texture (a
   * texture of the game's stays the game's). */
  destroy() { this._destroyView(false); }

  _destroyView(runtimeGone) {
    if (this._destroyed) return;
    super._destroyView();
    if (!runtimeGone) {
      this.module._jsui_surface_destroy(this.rt);
      this.host._surfaceDestroyed(this);
    }
    this.rt = 0;
    const own = this._own;
    this._own = this._target = this._last = null;
    if (own) requestAnimationFrame(() => own.destroy());
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

// Worker mode: this file is also the worker's entry (new Worker(import.meta.url),
// web/jsui-worker.js), so the worker loads the same versioned module the page did.
if (typeof WorkerGlobalScope !== 'undefined' && typeof document === 'undefined' &&
    globalThis instanceof WorkerGlobalScope && globalThis.name === WORKER_NAME) {
  const early = [];  // messages that arrive before jsui-worker.js is loaded
  globalThis.onmessage = (e) => early.push(e);
  import('./jsui-worker.js').then((w) => w.serve(internals(), early), (e) => {
    globalThis.postMessage({ t: 'evs', list: [{ ev: 'initError', message: `jsui-worker.js: ${e?.message ?? e}` }] });
  });
}
