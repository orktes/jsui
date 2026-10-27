// WebGPU renderer for web/jsui-web.js: Skia Graphite draws each frame
// straight into the canvas' WebGPU texture (jsui_render_webgpu). Needs the
// module built with every backend (GPU=1 scripts/build-wasm.sh ->
// web/dist-gpu/jsui.js, see scripts/build-skia-wasm-graphite.sh).
//
// Kept separate from jsui-web.js, which calls into it from three places:
// createJsui() acquires the device (requestWebGPU, async, before the host is
// built), JsuiHost._initBlitter() tries WebGPURenderer.create() first, and
// JsuiHost.frame() calls render(). Everything else (the WebGL -> CPU chain)
// is unchanged and is what we fall back to.
//
// Frames: when most of the frame changed (an animated full-screen UI) Skia
// paints it straight into the canvas texture, one pass. When little changed
// it repaints only the damage bounds into a texture of its own that keeps the
// last frame, then one draw copies that into the canvas texture (canvas
// textures start cleared every frame). Frames where nothing changed are
// skipped entirely: without getCurrentTexture() the canvas keeps showing the
// previous frame. Nothing here waits for the GPU (no onSubmittedWorkDone or
// buffer map is awaited per frame; Skia's own maps complete asynchronously).
//
// Device loss: we ask for a new device (once per loss) and rebuild the Skia
// context on the same canvas; if that fails the host continues on a fresh
// canvas with the WebGL / CPU renderers.
//
// The page's own device (createJsui({ device }), e.g. a Three.js or Babylon
// WebGPU renderer's): JSUI draws with it instead of requesting one, so the
// UI and the game share textures without copies (texture surfaces render the
// UI into a GPUTexture the game samples; GPUTexture host textures show the
// game's render targets in the UI). JSUI never replaces that device itself:
// after a loss the page makes a new one and hands it over (jsui.setDevice).

/** @typedef {{ adapter: GPUAdapter|null, device: GPUDevice, software: boolean, description: string,
 *   external?: boolean }} JsuiWebGPU */

let shared = null;  // Promise<JsuiWebGPU|null>: one device per page, shared by all runtimes

/** True when `navigator.gpu` exists (no adapter request). */
export function webgpuSupported() {
  return typeof navigator !== 'undefined' && !!navigator.gpu;
}

/**
 * A WebGPU device for JSUI (shared per page), or null when WebGPU is
 * unavailable. `allowSoftware` false also answers null for a software
 * ("fallback") adapter such as SwiftShader, where Skia's GPU path is slower
 * than the CPU raster's incremental repaints. `fresh`: after a device loss.
 * @returns {Promise<JsuiWebGPU|null>}
 */
export async function requestWebGPU({ allowSoftware = false, fresh = false } = {}) {
  if (!webgpuSupported()) return null;
  if (fresh || !shared) {
    shared = acquire();
    shared.then((w) => { if (!w) shared = null; });
  }
  const w = await shared;
  if (!w || (w.software && !allowSoftware)) return null;
  return w;
}

async function acquire() {
  try {
    const adapter = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
    if (!adapter) return null;
    const info = adapter.info ?? {};
    const desc = [info.vendor, info.architecture, info.device, info.description].filter(Boolean).join(' ');
    const software = !!(info.isFallbackAdapter ?? adapter.isFallbackAdapter) || /swiftshader|llvmpipe|lavapipe|software/i.test(desc);
    // Dual-source blending lets Skia blend anti-aliased draws in Porter-Duff
    // modes other than src-over (src, dst-out, clear, ...) in the blend unit
    // instead of reading the destination through a texture copy.
    const requiredFeatures = ['dual-source-blending'].filter((f) => adapter.features?.has(f));
    const device = await adapter.requestDevice({ label: 'jsui', requiredFeatures });
    const w = { adapter, device, software, description: desc || 'unknown adapter' };
    // A lost device is never handed out again.
    device.lost.then(() => { if (sharedValue === w) shared = sharedValue = null; });
    sharedValue = w;
    return w;
  } catch {
    return null;
  }
}
let sharedValue = null;

const externals = new WeakMap();  // GPUDevice -> JsuiWebGPU

/** The page's own GPUDevice as a JsuiWebGPU (one per device). */
export function externalWebGPU(device) {
  let w = externals.get(device);
  if (!w) {
    const info = device.adapterInfo ?? {};
    const desc = [info.vendor, info.architecture, info.device, info.description].filter(Boolean).join(' ');
    w = { adapter: null, device, software: false, external: true,
      description: `the page's device${device.label ? ` "${device.label}"` : ''}${desc ? `, ${desc}` : ''}` };
    externals.set(device, w);
  }
  return w;
}

const USAGE_RENDER_ATTACHMENT = 0x10, USAGE_COPY_SRC = 0x01, USAGE_TEXTURE_BINDING = 0x04;

export class WebGPURenderer {
  /**
   * Builds the Skia Graphite context on `webgpu.device`, then takes the
   * canvas' 'webgpu' context (only once Skia accepted the device, so a
   * failure leaves the canvas free for WebGL). Null on failure.
   */
  // `sharedFor(webgpu)`: a SharedWebGPU context to draw with instead of a
  // Skia context of the renderer's own (the surfaces of a runtime share one
  // per device; each canvas keeps only its GPUCanvasContext).
  static create(host, webgpu, canvasContext = null, sharedFor = null) {
    const M = host.module;
    if (!webgpu || !M.WebGPU || !M._jsui_gpu_create_webgpu) return null;
    const shared = sharedFor ? sharedFor(webgpu) : null;
    let own = null;
    if (!shared) {
      own = SharedWebGPU.create(M, webgpu, (t) => host._log(2, t));
      if (!own) return null;
    }
    const skia = shared ?? own;
    let ctx = canvasContext;
    try { ctx ??= host.canvas.getContext('webgpu'); } catch {}
    if (!ctx) {
      own?.unref();
      return null;
    }
    // globalThis.jsuiCanvasFormat = 'rgba8unorm' / 'bgra8unorm' overrides the
    // browser's choice (recording the pipeline lists of the other format,
    // see CanvasView.addPipelines).
    // A texture surface's stand-in context names the format of its textures.
    const format = canvasContext?.jsuiFormat ?? (/^(rgba|bgra)8unorm$/.test(globalThis.jsuiCanvasFormat ?? '')
      ? globalThis.jsuiCanvasFormat : navigator.gpu.getPreferredCanvasFormat());
    try {
      ctx.configure({ device: webgpu.device, format, alphaMode: 'premultiplied',
        usage: USAGE_RENDER_ATTACHMENT | USAGE_COPY_SRC | USAGE_TEXTURE_BINDING });
    } catch (e) {
      host._log(2, `WebGPU: canvas configure failed: ${e?.message ?? e}`);
      own?.unref();
      return null;
    }
    if (shared) shared.ref();
    return new WebGPURenderer(host, webgpu, skia, ctx, format, sharedFor);
  }

  constructor(host, webgpu, skia, ctx, format, sharedFor) {
    this.host = host;
    this.module = host.module;
    this.webgpu = webgpu;
    this.device = webgpu.device;
    this.skia = skia;  // SharedWebGPU holding a reference for us
    this.gpu = skia.gpu;
    this.instance = skia.instance;
    this.sharedFor = sharedFor;
    this.ctx = ctx;
    this.format = format;
    this.lost = false;
    this.flags = 0;  // JSUI_WEBGPU_* flags for jsui_render_webgpu (tests: 4 = NO_RETAIN)
    this.device.lost.then((info) => {
      if (this.gpu && !this._destroyed) this._onLost(info);
    });
  }

  get name() { return 'webgpu'; }

  // Draws a frame into the canvas' current texture: true when drawn, false
  // when the GPU path failed, undefined while recovering from device loss.
  render() {
    if (this.lost || !this.gpu) return undefined;
    if (this.warm) return this._renderWarm();
    return this._renderInto(this.ctx.getCurrentTexture());
  }

  // Warm-up (CanvasView.beginWarmup): frames go to a throwaway Skia context
  // on the same device instead of the canvas. Its createRenderPipeline only
  // starts createRenderPipelineAsync for the descriptor (the browser compiles
  // those on worker threads, several at once, into the caches the blocking
  // one reads) and hands Skia a stand-in; nothing it records is submitted.
  // Every kind of draw the frames contain, ShaderView effects included, is
  // compiled in parallel; the real context then creates each from a warm
  // cache when it first draws it.
  beginWarmup(log) {
    if (this.warm || this.lost || !this.gpu) return !!this.warm;
    const probe = SharedWebGPU.create(this.module, this.webgpu, log);
    if (!probe) return false;
    this.warm = { probe, started: [], texture: null };
    return true;
  }

  /** Ends the warm-up: resolves once its compiles finished (or `timeoutMs`). */
  async endWarmup(timeoutMs = 5000) {
    const w = this.warm;
    if (!w) return 0;
    this.warm = null;
    let timer;
    await Promise.race([Promise.all(w.started),
      new Promise((r) => { timer = setTimeout(r, timeoutMs); })]);
    clearTimeout(timer);
    w.texture?.destroy();
    w.probe.unref();
    return w.started.length;
  }

  _renderWarm() {
    const w = this.warm, M = this.host.module, dev = this.device;
    const width = this.host.pixelWidth, height = this.host.pixelHeight;
    if (!w.texture || w.texture.width !== width || w.texture.height !== height) {
      w.texture?.destroy();
      w.texture = dev.createTexture({ size: [width, height], format: this.format,
        usage: USAGE_RENDER_ATTACHMENT | USAGE_COPY_SRC | USAGE_TEXTURE_BINDING });
    }
    const standIn = standInPipeline(dev);
    const queue = dev.queue;
    dev.createRenderPipeline = (desc) => {
      w.started.push(dev.createRenderPipelineAsync(desc).catch(() => null));
      return standIn;
    };
    queue.submit = () => {};
    // Encoding draws with the stand-in is invalid: kept out of the console.
    dev.pushErrorScope('validation');
    const ptr = M.WebGPU.importJsTexture(w.texture, w.probe.instance);
    try {
      // NO_RETAIN (4): a full frame each time, nothing kept.
      M._jsui_render_webgpu(this.host.rt, w.probe.gpu, ptr, width, height, 4);
    } finally {
      M._jsui_web_wgpu_release_texture(ptr);
      delete dev.createRenderPipeline;
      delete queue.submit;
      dev.popErrorScope().catch(() => {});
    }
    return true;
  }

  _renderInto(texture) {
    const M = this.host.module;
    const ptr = M.WebGPU.importJsTexture(texture, this.instance);
    let r;
    try {
      r = M._jsui_render_webgpu(this.host.rt, this.gpu, ptr, this.host.pixelWidth, this.host.pixelHeight, this.flags);
    } finally {
      M._jsui_web_wgpu_release_texture(ptr);
    }
    if (r === 0) return true;
    return this.lost ? undefined : false;
  }

  /** Renders a frame and reads it back (premultiplied RGBA, top row first). */
  async readPixels() {
    if (this.lost || !this.gpu) return null;
    const w = this.host.pixelWidth, h = this.host.pixelHeight, dev = this.device;
    const texture = this.ctx.getCurrentTexture();
    if (!this._renderInto(texture)) return null;
    const bpr = Math.ceil((w * 4) / 256) * 256;
    const buf = dev.createBuffer({ size: bpr * h, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
    const enc = dev.createCommandEncoder();
    enc.copyTextureToBuffer({ texture }, { buffer: buf, bytesPerRow: bpr }, [w, h]);
    dev.queue.submit([enc.finish()]);
    await buf.mapAsync(GPUMapMode.READ);
    const src = new Uint8Array(buf.getMappedRange());
    const px = new Uint8Array(w * h * 4);
    const bgra = this.format.startsWith('bgra');
    for (let y = 0; y < h; y++) {
      const s = y * bpr, d = y * w * 4;
      px.set(src.subarray(s, s + w * 4), d);
      if (bgra) {
        for (let i = d; i < d + w * 4; i += 4) {
          const b = px[i];
          px[i] = px[i + 2];
          px[i + 2] = b;
        }
      }
    }
    buf.unmap();
    buf.destroy();
    return { width: w, height: h, data: px };
  }

  destroy() {
    this._destroyed = true;
    this._release();
    try { this.ctx.unconfigure(); } catch {}
  }

  // Graphite in the browser never waits for the GPU, so its context may only
  // be destroyed once the work it submitted is done: wait for the queue, then
  // destroy (a lost device finishes everything at once).
  _release() {
    const skia = this.skia;
    this.skia = null;
    this.gpu = this.instance = 0;
    skia?.unref();
  }

  async _onLost(info) {
    const host = this.host;
    this.lost = true;
    this.skia?.markLost();
    this._release();
    if (this.webgpu.external) {
      // The page's device: the page makes the next one (jsui.setDevice).
      host._hostOf()._externalDeviceLost(this.webgpu, info);
      return;
    }
    host._log(2, `WebGPU device lost (${info?.reason ?? 'unknown'}: ${info?.message ?? ''}), recreating`);
    const webgpu = await requestWebGPU({ allowSoftware: true, fresh: true });
    if (this._destroyed || host._destroyed) return;
    const next = webgpu && WebGPURenderer.create(host, webgpu, this.ctx, this.sharedFor);
    host._webgpuRecovered(next, 'WebGPU device lost');
  }
}

/**
 * A Skia Graphite context on a WebGPU device, reference counted: a
 * renderer's own, or one shared by every surface canvas of a runtime (the
 * runtime keeps per-surface frames apart, src/render/gpu_webgpu.cpp), so a
 * page of many small widgets needs one Skia context, not one per canvas.
 */
// What warm-ups hand Skia's throwaway context for each pipeline (its
// recordings are never submitted): one trivial pipeline per device.
const standIns = new WeakMap();
export function standInPipeline(dev) {
  let p = standIns.get(dev);
  if (!p) {
    const module = dev.createShaderModule({ code: '@vertex fn v() -> @builtin(position) vec4f { return vec4f(0.0); }\n'
      + '@fragment fn f() -> @location(0) vec4f { return vec4f(0.0); }' });
    p = dev.createRenderPipeline({ layout: 'auto', vertex: { module, entryPoint: 'v' },
      fragment: { module, entryPoint: 'f', targets: [{ format: 'rgba8unorm' }] } });
    standIns.set(dev, p);
  }
  return p;
}

export class SharedWebGPU {
  /** Null (logged through `log`) when Skia cannot use the device. */
  static create(M, webgpu, log) {
    let gpu = 0, instance = 0;
    try {
      // Imported objects hang off an instance, which delivers the device's
      // async callbacks (buffer maps, queue work done) to Skia.
      instance = M._jsui_web_wgpu_create_instance();
      const dev = M.WebGPU.importJsDevice(webgpu.device, instance);
      gpu = M._jsui_gpu_create_webgpu(instance, dev, 0);
      M._jsui_web_wgpu_release_device(dev);  // the context holds its own reference
    } catch (e) {
      log(`WebGPU: Skia context creation threw: ${e?.message ?? e}`);
    }
    if (!gpu) {
      if (instance) M._jsui_web_wgpu_release_instance(instance);
      log('WebGPU: Skia Graphite context creation failed');
      return null;
    }
    return new SharedWebGPU(M, webgpu, gpu, instance);
  }

  constructor(M, webgpu, gpu, instance) {
    this.module = M;
    this.webgpu = webgpu;
    this.gpu = gpu;
    this.instance = instance;
    this.refs = 1;
    this.lost = false;
    this.destroyed = false;
  }

  ref() { this.refs++; }

  // The device was lost: drop the GPU objects (no GPU calls).
  markLost() {
    if (this.lost || this.destroyed) return;
    this.lost = true;
    this.module._jsui_gpu_context_lost(this.gpu);
  }

  // Graphite in the browser never waits for the GPU, so its context may only
  // be destroyed once the work it submitted is done: wait for the queue, then
  // destroy (a lost device finishes everything at once).
  unref() {
    if (--this.refs > 0 || this.destroyed) return;
    this.destroyed = true;
    const M = this.module, gpu = this.gpu, instance = this.instance;
    const done = () => setTimeout(() => {
      M._jsui_gpu_destroy(gpu);
      M._jsui_web_wgpu_release_instance(instance);
    }, 0);
    try {
      this.webgpu.device.queue.onSubmittedWorkDone().then(done, done);
    } catch {
      done();
    }
  }
}
