// Host textures for web/jsui-web.js: pictures the page makes itself (a
// <video>, WebCodecs VideoFrames, a canvas, an image, a camera) shown inside
// the UI with <HostTexture name=...> / useHostTexture(name) (include/jsui/jsui.h
// "Host textures"). JSUI does not play or decode anything: the page names a
// texture, hands it a source whenever the picture changes, and the runtime
// repaints the views showing it (no layout, no JS).
//
//   jsui.setTexture('trailer', videoFrame);            // any TexImageSource-like source
//   const unbind = jsui.bindVideo('trailer', video);   // kept current per presented frame
//   jsui.removeTexture('trailer');
//
// Textures belong to the runtime, like state and frame data: every surface
// can show them. Each upload goes where the runtime's views can sample it:
//
//   every view on WebGPU   one GPUTexture per name on the page's device
//   (one device)           (rgba8unorm), filled with copyExternalImageToTexture
//                          and set with jsui_set_texture(JSUI_TEXTURE_WEBGPU);
//                          the same texture is refilled while the size stays
//                          (jsui_texture_updated), a new one on resize.
//   every view on one      one GL texture per name in that context, created
//   WebGL context (Skia)   through emscripten's GL object table so its name is
//                          valid for Ganesh, filled with texImage2D /
//                          texSubImage2D (JSUI_TEXTURE_GL).
//   otherwise (the CPU     the picture is drawn into a 2D canvas, read back
//   raster, mixed APIs)    and copied (jsui_set_texture_pixels, straight alpha).
//
// "Otherwise" includes a main canvas with a WebGL context of its own next to
// surfaces on the runtime's shared WebGL context: a GL texture lives in one
// context, and the runtime holds one texture per name.
//
// When the views change (a renderer falls back to the CPU, a lost WebGL
// context comes back, a WebGPU device is replaced, a surface is created or
// destroyed) the textures move: each is uploaded again from its last source
// and the old GPU objects are freed. Sources are kept for that, except
// VideoFrames, which the caller owns and closes: a VideoFrame texture is
// removed on such a change until the next setTexture().

const KIND_GL = 1, KIND_WEBGPU = 2;
export const TEXTURE_UNPREMULTIPLIED = 1, TEXTURE_OPAQUE = 2, TEXTURE_BOTTOM_LEFT_ORIGIN = 4;
const GL_TEXTURE_2D = 0x0de1;
// GPUTextureUsage: COPY_DST | TEXTURE_BINDING | RENDER_ATTACHMENT (copyExternalImageToTexture
// needs RENDER_ATTACHMENT on the destination).
const WGPU_USAGE = 0x02 | 0x04 | 0x10;
const DESC_BYTES = 28;  // JsuiTexture on wasm32: kind, handle, format, width, height, flags, usage

const has = (name) => typeof globalThis[name] !== 'undefined';

// A source whose upload threw SecurityError (cross-origin, not CORS-enabled)
// is not tried again until its URL changes.
export const taintKey = (s) => String(s.currentSrc ?? s.src ?? '');
/** The warning for such a source (main and worker mode). */
export const corsWarning = (name) => `host texture "${name}": the source is cross-origin and not CORS-enabled (set ` +
  'crossOrigin = \'anonymous\' on the <video> / <img> and serve it with Access-Control-Allow-Origin); ' +
  'not uploaded until its URL changes';

// ------------------------------------------------------ video frames
// bindVideo takes a video's frames from requestVideoFrameCallback, and polls
// once per display frame where there is none. Playwright WebKit 26 on macOS
// loses every WebGL context of the page within half a second of a frame
// callback on a playing video (restored ~40 ms later; the video's play()
// is aborted), or, with the runtime in a worker, presents nothing for ~6 s
// and then loses them. So WebKit (navigator.vendor: Safari and every iOS
// browser) polls from the start. Elsewhere a WebGL context loss within
// RVFC_SUSPECT_MS of play() on a video with a frame callback
// (videoFramesContextLost) switches every bound video of the page to
// polling, and later binds start there.
const RVFC_SUSPECT_MS = 1000;
let rvfcOff = false;
const webkit = () => globalThis.navigator?.vendor === 'Apple Computer, Inc.';
const frameWatches = new Set();

/** Calls `onFrame` for each new frame of `video` (see above). Returns a
 * function that stops it. */
export function watchVideoFrames(video, onFrame) {
  const frames = () => video.getVideoPlaybackQuality?.().totalVideoFrames ?? -1;
  // The frame showing now is the caller's (bindVideo uploads it right away).
  const w = { mode: 'poll', handle: 0, raf: 0, armed: -Infinity, time: video.currentTime, frames: frames(), counts: false, stopped: false };
  const abort = has('AbortController') ? new AbortController() : null;
  const fire = () => {
    w.time = video.currentTime;
    w.frames = frames();
    onFrame();
  };
  // Polling uploads only when the decoded frame count or the playback
  // position moved. The position moves every display frame while playing,
  // so once the count is seen moving, only the count does then (one upload
  // per video frame, not per display frame); paused, a seek still does.
  const poll = () => {
    if (w.stopped) return;
    w.raf = requestAnimationFrame(poll);
    if (video.readyState < 2) return;
    const f = frames();
    if (f !== w.frames) {
      w.counts ||= f > 0;
      fire();
    } else if (video.currentTime !== w.time) {
      if (w.counts && video.paused === false) w.time = video.currentTime;
      else fire();
    }
  };
  const callback = () => {
    if (w.stopped || w.mode !== 'rvfc') return;
    w.handle = video.requestVideoFrameCallback(callback);
    fire();
  };
  w.poll = () => {
    if (w.handle) video.cancelVideoFrameCallback?.(w.handle);
    w.handle = 0;
    w.mode = 'poll';
    w.raf = requestAnimationFrame(poll);
  };
  if (!rvfcOff && !webkit() && typeof video.requestVideoFrameCallback === 'function') {
    w.mode = 'rvfc';
    // Armed from play() on: WebKit loses the context before 'playing' (and
    // then aborts the play()).
    const arm = () => { w.armed = performance.now(); };
    if (video.paused === false) arm();
    for (const type of ['play', 'playing']) video.addEventListener?.(type, arm, abort ? { signal: abort.signal } : undefined);
    w.handle = video.requestVideoFrameCallback(callback);
  } else {
    w.poll();
  }
  frameWatches.add(w);
  return () => {
    if (w.stopped) return;
    w.stopped = true;
    frameWatches.delete(w);
    if (w.handle) video.cancelVideoFrameCallback?.(w.handle);
    if (w.raf) cancelAnimationFrame(w.raf);
    abort?.abort();
  };
}

/** A WebGL context of the page was lost. If that switched the bound videos
 * to polling (see above): the line to log, else null. */
export function videoFramesContextLost() {
  const t = performance.now();
  if (![...frameWatches].some((w) => w.mode === 'rvfc' && t - w.armed < RVFC_SUSPECT_MS)) return null;
  rvfcOff = true;
  for (const w of frameWatches) if (w.mode === 'rvfc') w.poll();
  return 'bound videos: frames polled once per display frame from now on (a WebGL context was lost right ' +
    'after a video frame callback)';
}

/** The picture size of a texture source in px, or null while it has none
 * (a <video> before its first frame, an <img> still loading). */
export function sourceSize(s) {
  if (!s || typeof s !== 'object') return null;
  let w, h;
  if ('displayWidth' in s) [w, h] = [s.displayWidth, s.displayHeight];   // VideoFrame
  else if ('videoWidth' in s) {                                           // HTMLVideoElement
    if (!(s.readyState >= 2)) return null;                                // HAVE_CURRENT_DATA
    [w, h] = [s.videoWidth, s.videoHeight];
  } else if ('naturalWidth' in s) {                                       // HTMLImageElement
    if (s.complete === false) return null;
    [w, h] = [s.naturalWidth, s.naturalHeight];
  } else [w, h] = [s.width, s.height];                                    // canvases, ImageBitmap, ImageData
  return w > 0 && h > 0 ? { width: Math.round(w), height: Math.round(h) } : null;
}

export const isVideoFrame = (s) => 'displayWidth' in s && typeof s.close === 'function';
const isImageData = (s) => s.data instanceof Uint8ClampedArray && !('getContext' in s);
// { data, width, height }: straight-alpha RGBA rows in any typed array (not an ImageData).
const isRawPixels = (s) => typeof ImageData !== 'undefined' && !(s instanceof ImageData) && ArrayBuffer.isView(s.data)
  && !('getContext' in s) && s.width > 0 && s.height > 0;

// A raw pixel source as an ImageData over the same bytes; null when too short.
function rawImageData(s) {
  const n = Math.round(s.width) * Math.round(s.height) * 4, d = s.data;
  if (d.byteLength < n) return null;
  return new ImageData(new Uint8ClampedArray(d.buffer, d.byteOffset, n), Math.round(s.width), Math.round(s.height));
}

// Video has no alpha (VideoFrames in an alpha format excepted): OPAQUE lets
// Skia skip blending.
export function defaultOpaque(s) {
  if ('videoWidth' in s) return true;
  if (isVideoFrame(s)) return !/A$|^RGBA|^BGRA/.test(String(s.format ?? ''));
  return false;
}

/**
 * Where textures go for a set of views (see the file comment): { kind:
 * 'webgpu', device }, { kind: 'gl', gl, owner, gen }, { kind: 'pixels' }, or
 * { kind: 'lost' } while a GPU context the views draw with is lost.
 * Views are CanvasView-like: `_wgpu` (WebGPURenderer: `lost`, `webgpu`),
 * `_sgl` (SharedGL: `gl`, `lost`, `gen`) or `_gpu` + `_gl` + `_glLost` + `_glGen`
 * (Skia on the canvas' own WebGL context); none of them: the CPU raster.
 */
export function textureTarget(views) {
  let t = null;
  for (const v of views) {
    let k;
    if (v._wgpu) {
      if (v._wgpu.lost) return { kind: 'lost' };
      k = { kind: 'webgpu', device: v._wgpu.webgpu.device, owner: v._wgpu.webgpu.device, gen: 0 };
    } else if (v._sgl) {
      // The context may be lost before its event said so (CanvasView._checkGL).
      if (v._sgl.lost || v._sgl.gl.isContextLost()) return { kind: 'lost' };
      k = { kind: 'gl', gl: v._sgl.gl, owner: v._sgl, gen: v._sgl.gen ?? 0 };
    } else if (v._gpu) {
      if (v._glLost || v._gl.isContextLost()) return { kind: 'lost' };
      k = { kind: 'gl', gl: v._gl, owner: v, gen: v._glGen ?? 0 };
    } else {
      return { kind: 'pixels' };
    }
    if (!t) t = k;
    else if (!sameTarget(t, k)) return { kind: 'pixels' };
  }
  return t ?? { kind: 'pixels' };
}

// Whether GPU objects made for `prev` are dead once textures go to `next`:
// a restored WebGL context's earlier objects are invalid, a destroyed one is lost.
function staleIn(prev, next) {
  return prev?.kind === 'gl' && (prev.gl.isContextLost() || (prev.gl === next.gl && prev.gen !== next.gen));
}

export function sameTarget(a, b) {
  return !!a && !!b && a.kind === b.kind && a.owner === b.owner && a.gen === b.gen &&
    (a.kind !== 'webgpu' || a.device === b.device) && (a.kind !== 'gl' || a.gl === b.gl);
}

/**
 * The host textures of one runtime. `host`: { module, rt, _views() (the
 * shown views), _withStrings(strs, fn), _log(level, text) }, optionally
 * _checkContexts() (before an upload) and _texturesReset() (the textures
 * were placed again after a GPU loss).
 */
export class HostTextureSet {
  constructor(host) {
    this.host = host;
    this.module = host.module;
    this.entries = new Map();   // name -> entry (see _upload)
    this.bindings = new Map();  // name -> unbind function (bindVideo)
    this.target = null;         // the textureTarget() textures were last placed for
    this._reset = false;        // a loss seen since (see sync)
    this._desc = 0;             // JsuiTexture scratch (WASM heap)
    this._stage = 0;            // CPU pixels staging (WASM heap)
    this._stageSize = 0;
    this._canvas = null;        // 2D scratch canvas (CPU path, fallbacks)
    this._ctx = null;
    this._instance = 0;         // WGPUInstance the imported textures hang off
    this._warned = new Set();
    this._tainted = new WeakMap();  // source -> taintKey() when its upload threw SecurityError
    this.uploads = 0;
  }

  /** 'webgpu' | 'gl' | 'pixels': where the textures currently go. */
  get backend() {
    const t = this.target ?? textureTarget(this.host._views());
    return t.kind === 'lost' ? null : t.kind;
  }

  // ------------------------------------------------------------- public

  set(name, source, options = {}) {
    name = String(name);
    if (source == null) return this.remove(name);
    if (isRawPixels(source)) {
      source = rawImageData(source);
      if (!source) return false;
    }
    const size = sourceSize(source);
    if (!size) return false;
    if (this._tainted.get(source) === taintKey(source)) return false;
    this.host._checkContexts?.();  // a loss the views have not handled yet
    const target = this.sync();
    if (target.kind === 'lost') {
      // Uploaded when the context is back (sync), if we may keep the source.
      const e = this.entries.get(name);
      if (e) Object.assign(e, { source: isVideoFrame(source) ? null : source, options });
      else if (!isVideoFrame(source)) this.entries.set(name, { name, kind: null, source, options, width: 0, height: 0 });
      return false;
    }
    return this._upload(name, source, options, size, target);
  }

  remove(name) {
    name = String(name);
    this.bindings.get(name)?.();
    const e = this.entries.get(name);
    this.entries.delete(name);
    const r = this.host._withStrings([name], (k) => this.module._jsui_remove_texture(this.host.rt, k)) === 1;
    if (e) this._free(e);
    return r;
  }

  bindVideo(name, video, options = {}) {
    name = String(name);
    this.bindings.get(name)?.();
    let stopped = false;
    const push = () => {
      if (!stopped) this.set(name, video, options);
    };
    const abort = typeof AbortController !== 'undefined' ? new AbortController() : null;
    const stopFrames = watchVideoFrames(video, push);
    // A paused video presents frames for these too (first frame, seeks, a new src).
    for (const type of ['loadeddata', 'seeked', 'resize']) {
      video.addEventListener?.(type, push, abort ? { signal: abort.signal } : undefined);
    }
    if (video.readyState >= 2) push();
    const unbind = () => {
      if (stopped) return;
      stopped = true;
      stopFrames();
      abort?.abort();
      if (!abort) for (const type of ['loadeddata', 'seeked', 'resize']) video.removeEventListener?.(type, push);
      if (this.bindings.get(name) === unbind) this.bindings.delete(name);
    };
    this.bindings.set(name, unbind);
    return unbind;
  }

  /** Once per frame (before the tick), and before each upload: moves the
   * textures when the views changed. Returns where textures go now. */
  sync() {
    const t = textureTarget(this.host._views());
    if (t.kind === 'lost') {
      this._reset = true;
      return t;
    }
    if (sameTarget(t, this.target)) return t;
    // Back after a loss (a restored context, a new device, a fresh canvas):
    // the host is told once the textures are placed (_texturesReset; the
    // worker's ImageBitmaps may have lost their pixels with the GPU process).
    const reset = this._reset || staleIn(this.target, t);
    this._reset = false;
    this.target = t;
    for (const e of [...this.entries.values()]) {
      if (e.kind && sameTarget(e.target, t)) continue;
      const stale = staleIn(e.target, t);
      const size = e.source && sourceSize(e.source);
      if (size) {
        const old = this._detach(e);
        // Lost again during the upload: kept for the next restore.
        if (!this._upload(e.name, e.source, e.options, size, t) && !(t.kind === 'gl' && t.gl.isContextLost())) {
          this._drop(e);
        }
        this._free(old, stale);
      } else {
        // A VideoFrame (or a source with no picture now): nothing to upload
        // into the new place; the next setTexture() brings it back.
        this._drop(e);
        this._free(e, stale);
      }
    }
    if (reset) this.host._texturesReset?.();
    return t;
  }

  _drop(e) {
    this.entries.delete(e.name);
    this.host._withStrings([e.name], (k) => this.module._jsui_remove_texture(this.host.rt, k));
  }

  destroy() {
    for (const unbind of [...this.bindings.values()]) unbind();
    // The runtime is destroyed right after: its references go with it.
    for (const e of this.entries.values()) this._free(e);
    this.entries.clear();
    const M = this.module;
    if (this._instance) M._jsui_web_wgpu_release_instance(this._instance);
    if (this._desc) M._free(this._desc);
    if (this._stage) M._free(this._stage);
    this._instance = this._desc = this._stage = this._stageSize = 0;
    this._canvas = this._ctx = null;
  }

  // ----------------------------------------------------------- internals

  _upload(name, source, options, size, target) {
    let e = this.entries.get(name);
    if (e && e.kind && !sameTarget(e.target, target)) {
      const old = this._detach(e);
      this._free(old, staleIn(old.target, target));
    }
    if (!e) {
      e = { name, kind: null, width: 0, height: 0 };
      this.entries.set(name, e);
    }
    e.options = options;
    e.source = isVideoFrame(source) ? null : source;
    const opaque = options.opaque ?? defaultOpaque(source);
    const flags = opaque ? TEXTURE_OPAQUE : 0;
    try {
      if (target.kind === 'webgpu') this._uploadWebGPU(e, source, size, flags, target);
      else if (target.kind === 'gl') this._uploadGL(e, source, size, flags, target);
      else this._uploadPixels(e, source, size, flags);
    } catch (err) {
      // Nothing valid left under this name (a first upload, or one into a
      // new place): the runtime must not keep a freed handle.
      if (!e.kind) this.host._withStrings([name], (k) => this.module._jsui_remove_texture(this.host.rt, k));
      // The context was lost during the upload (WebKit notices a lost GPU
      // process in a GL call): the source is uploaded again once it is back.
      if (target.kind === 'gl' && target.gl.isContextLost()) return false;
      if (err?.name === 'SecurityError') {
        this._tainted.set(source, taintKey(source));
        // A texture shown before (a poster) stays.
        this._warnOnce(name, corsWarning(name));
      } else {
        this._warnOnce(name, `host texture "${name}": upload failed: ${err?.message ?? err}`);
      }
      return false;
    }
    e.target = target;
    this.uploads++;
    return true;
  }

  // jsui_set_texture with a JsuiTexture in the heap: 1 changed, 0 same.
  _setDesc(name, kind, handle, width, height, flags) {
    const M = this.module;
    if (!this._desc) this._desc = M._malloc(32);
    const d = this._desc >> 2;
    M.HEAP32.fill(0, d, d + DESC_BYTES / 4);
    M.HEAP32[d] = kind;
    M.HEAP32[d + 1] = handle | 0;  // pointer / GL name (wasm32; the module exports HEAP32, not HEAPU32)
    M.HEAP32[d + 2] = 0;        // format 0: RGBA8 UNORM
    M.HEAP32[d + 3] = width;
    M.HEAP32[d + 4] = height;
    M.HEAP32[d + 5] = flags;
    return this.host._withStrings([name], (k) => M._jsui_set_texture(this.host.rt, k, this._desc));
  }

  _updated(name) {
    return this.host._withStrings([name], (k) => this.module._jsui_texture_updated(this.host.rt, k));
  }

  // WebGPU: copyExternalImageToTexture into a texture of the views' device.
  _uploadWebGPU(e, source, size, flags, target) {
    const M = this.module, device = target.device;
    const fresh = e.kind !== 'webgpu' || e.width !== size.width || e.height !== size.height || e.flags !== flags;
    let texture = e.texture, old = null;
    if (fresh) {
      old = e.kind ? this._detach(e) : null;
      texture = device.createTexture({ label: `jsui texture ${e.name}`, size: [size.width, size.height],
        format: 'rgba8unorm', usage: WGPU_USAGE });
    }
    try {
      this._copyExternal(device, source, texture, size);
    } catch (err) {
      if (fresh) texture.destroy();
      if (old) this._attach(e, old);
      throw err;
    }
    if (!fresh) {
      this._updated(e.name);
      return;
    }
    if (!this._instance) this._instance = M._jsui_web_wgpu_create_instance();
    const ptr = M.WebGPU.importJsTexture(texture, this._instance);
    Object.assign(e, { kind: 'webgpu', texture, ptr, width: size.width, height: size.height, flags });
    this._setDesc(e.name, KIND_WEBGPU, ptr, size.width, size.height, flags);
    if (old) this._free(old);
  }

  _copyExternal(device, source, texture, size) {
    const dst = { texture, premultipliedAlpha: true, colorSpace: 'srgb' };
    try {
      device.queue.copyExternalImageToTexture({ source, flipY: false }, dst, [size.width, size.height]);
    } catch (err) {
      // A source this browser cannot copy from (ImageData before some
      // versions, an <img> in older ones): through a 2D canvas instead.
      if (err?.name === 'SecurityError') throw err;
      device.queue.copyExternalImageToTexture({ source: this._draw(source, size), flipY: false }, dst,
        [size.width, size.height]);
    }
  }

  // WebGL: a texture of the views' context, registered in emscripten's GL
  // table (Ganesh refers to it by that name).
  _uploadGL(e, source, size, flags, target) {
    const GL = this.module.GL, gl = target.gl;
    const fresh = e.kind !== 'gl' || e.width !== size.width || e.height !== size.height || e.flags !== flags;
    const prev = gl.getParameter(gl.TEXTURE_BINDING_2D);
    let tex = e.tex, id = e.id, old = null;
    if (fresh) {
      tex = gl.createTexture();
      if (!tex) throw new Error('no texture: the WebGL context is lost');  // lost since textureTarget()
      old = e.kind ? this._detach(e) : null;
      id = GL.getNewId(GL.textures);
      tex.name = id;
      GL.textures[id] = tex;
    }
    try {
      gl.bindTexture(gl.TEXTURE_2D, tex);
      const gl2 = typeof WebGL2RenderingContext !== 'undefined' && gl instanceof WebGL2RenderingContext;
      // State Skia's frames may have left that changes how DOM sources upload.
      if (gl2) {
        gl.bindBuffer(gl.PIXEL_UNPACK_BUFFER, null);
        gl.pixelStorei(gl.UNPACK_ROW_LENGTH, 0);
        gl.pixelStorei(gl.UNPACK_SKIP_ROWS, 0);
        gl.pixelStorei(gl.UNPACK_SKIP_PIXELS, 0);
      }
      gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
      gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, true);
      const upload = (src) => {
        if (fresh) {
          gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
          gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
          gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
          gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
          gl.texImage2D(gl.TEXTURE_2D, 0, gl2 ? gl.RGBA8 : gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, src);
        } else {
          gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, gl.RGBA, gl.UNSIGNED_BYTE, src);
        }
      };
      try {
        upload(source);
      } catch (err) {
        if (err?.name === 'SecurityError') throw err;
        upload(this._draw(source, size));  // a source type this browser's WebGL does not take
      }
    } catch (err) {
      if (fresh) {
        gl.deleteTexture(tex);
        GL.textures[id] = null;
        if (old) this._attach(e, old);
      }
      throw err;
    } finally {
      // Skia only ever sets these two to their defaults; its other state it
      // re-reads at the start of each frame (resetContext).
      gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
      gl.bindTexture(gl.TEXTURE_2D, prev);
    }
    if (!fresh) {
      this._updated(e.name);
      return;
    }
    Object.assign(e, { kind: 'gl', tex, id, gl, width: size.width, height: size.height, flags });
    this._setDesc(e.name, KIND_GL, id, size.width, size.height, flags);
    if (old) this._free(old);
  }

  // CPU pixels: straight-alpha RGBA copied into the runtime.
  _uploadPixels(e, source, size, flags) {
    const M = this.module, { width, height } = size;
    let data;
    if (isImageData(source)) data = source.data;
    else {
      this._draw(source, size);
      data = this._ctx.getImageData(0, 0, width, height).data;
    }
    const bytes = width * height * 4;
    if (bytes > this._stageSize) {
      if (this._stage) M._free(this._stage);
      this._stageSize = bytes;
      this._stage = M._malloc(bytes);
    }
    // Read HEAPU8 after the malloc: memory growth replaces the ArrayBuffer.
    M.HEAPU8.set(data.subarray(0, bytes), this._stage);
    const r = this.host._withStrings([e.name], (k) =>
      M._jsui_set_texture_pixels(this.host.rt, k, this._stage, width, height, width * 4, flags | TEXTURE_UNPREMULTIPLIED));
    if (r !== 1) throw new Error('jsui_set_texture_pixels failed');
    const old = e.kind && e.kind !== 'pixels' ? this._detach(e) : null;
    Object.assign(e, { kind: 'pixels', width, height, flags });
    if (old) this._free(old);
  }

  // Draws `source` into the scratch 2D canvas at its size; returns the canvas.
  _draw(source, { width, height }) {
    if (!this._canvas) {
      this._canvas = has('OffscreenCanvas') ? new OffscreenCanvas(width, height) : document.createElement('canvas');
    }
    const c = this._canvas;
    if (c.width !== width || c.height !== height || !this._ctx) {
      c.width = width;
      c.height = height;
      this._ctx = c.getContext('2d', { willReadFrequently: true });
    }
    const ctx = this._ctx;
    if (isImageData(source)) ctx.putImageData(source, 0, 0);
    else {
      ctx.clearRect(0, 0, width, height);
      ctx.drawImage(source, 0, 0, width, height);
    }
    return c;
  }

  // Takes the GPU objects off an entry (to free once their replacement is set).
  _detach(e) {
    const old = { kind: e.kind, texture: e.texture, ptr: e.ptr, tex: e.tex, id: e.id, gl: e.gl, target: e.target };
    e.kind = null;
    e.texture = e.ptr = e.tex = e.id = e.gl = undefined;
    e.width = e.height = 0;
    return old;
  }

  _attach(e, old) { Object.assign(e, old); }

  // Frees an entry's GPU objects. `stale`: their context may be gone (a
  // restored WebGL context's old objects are invalid; only the table entry goes).
  _free(e, stale = false) {
    if (!e) return;
    const M = this.module;
    if (e.kind === 'webgpu') {
      // Destroying is safe with frames in flight: WebGPU finishes submitted
      // work first. The runtime already holds the replacement (or nothing).
      try { e.texture.destroy(); } catch {}
      M._jsui_web_wgpu_release_texture(e.ptr);
    } else if (e.kind === 'gl') {
      if (!stale && !e.gl.isContextLost()) {
        try { e.gl.deleteTexture(e.tex); } catch {}
      }
      if (M.GL.textures[e.id] === e.tex) M.GL.textures[e.id] = null;
    }
    e.kind = null;
  }

  _warnOnce(name, text) {
    if (this._warned.has(text)) return;
    this._warned.add(text);
    this.host._log(2, text);
  }
}
