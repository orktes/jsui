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

/** @typedef {{ adapter: GPUAdapter, device: GPUDevice, software: boolean, description: string }} JsuiWebGPU */

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

const USAGE_RENDER_ATTACHMENT = 0x10, USAGE_COPY_SRC = 0x01, USAGE_TEXTURE_BINDING = 0x04;

export class WebGPURenderer {
  /**
   * Builds the Skia Graphite context on `webgpu.device`, then takes the
   * canvas' 'webgpu' context (only once Skia accepted the device, so a
   * failure leaves the canvas free for WebGL). Null on failure.
   */
  static create(host, webgpu, canvasContext = null) {
    const M = host.module;
    if (!webgpu || !M.WebGPU || !M._jsui_gpu_create_webgpu) return null;
    let gpu = 0, instance = 0;
    try {
      // Imported objects hang off an instance, which delivers the device's
      // async callbacks (buffer maps, queue work done) to Skia.
      instance = M._jsui_web_wgpu_create_instance();
      const dev = M.WebGPU.importJsDevice(webgpu.device, instance);
      gpu = M._jsui_gpu_create_webgpu(instance, dev, 0);
      M._jsui_web_wgpu_release_device(dev);  // the context holds its own reference
    } catch (e) {
      host._log(2, `WebGPU: Skia context creation threw: ${e?.message ?? e}`);
    }
    if (!gpu) {
      if (instance) M._jsui_web_wgpu_release_instance(instance);
      host._log(2, 'WebGPU: Skia Graphite context creation failed');
      return null;
    }
    let ctx = canvasContext;
    try { ctx ??= host.canvas.getContext('webgpu'); } catch {}
    if (!ctx) {
      M._jsui_gpu_destroy(gpu);
      M._jsui_web_wgpu_release_instance(instance);
      return null;
    }
    const format = navigator.gpu.getPreferredCanvasFormat();
    try {
      ctx.configure({ device: webgpu.device, format, alphaMode: 'premultiplied',
        usage: USAGE_RENDER_ATTACHMENT | USAGE_COPY_SRC | USAGE_TEXTURE_BINDING });
    } catch (e) {
      host._log(2, `WebGPU: canvas configure failed: ${e?.message ?? e}`);
      M._jsui_gpu_destroy(gpu);
      M._jsui_web_wgpu_release_instance(instance);
      return null;
    }
    return new WebGPURenderer(host, webgpu, gpu, instance, ctx, format);
  }

  constructor(host, webgpu, gpu, instance, ctx, format) {
    this.host = host;
    this.module = host.module;
    this.webgpu = webgpu;
    this.device = webgpu.device;
    this.gpu = gpu;
    this.instance = instance;
    this.ctx = ctx;
    this.format = format;
    this.lost = false;
    this.device.lost.then((info) => {
      if (this.gpu && !this._destroyed) this._onLost(info);
    });
  }

  get name() { return 'webgpu'; }

  // Draws a frame into the canvas' current texture: true when drawn, false
  // when the GPU path failed, undefined while recovering from device loss.
  render() {
    if (this.lost || !this.gpu) return undefined;
    return this._renderInto(this.ctx.getCurrentTexture());
  }

  _renderInto(texture) {
    const M = this.host.module;
    const ptr = M.WebGPU.importJsTexture(texture, this.instance);
    let r;
    try {
      r = M._jsui_render_webgpu(this.host.rt, this.gpu, ptr, this.host.pixelWidth, this.host.pixelHeight, 0);
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
    const M = this.module, gpu = this.gpu, instance = this.instance;
    this.gpu = this.instance = 0;
    if (!gpu) return;
    const done = () => setTimeout(() => {
      M._jsui_gpu_destroy(gpu);
      M._jsui_web_wgpu_release_instance(instance);
    }, 0);
    try {
      this.device.queue.onSubmittedWorkDone().then(done, done);
    } catch {
      done();
    }
  }

  async _onLost(info) {
    const host = this.host, M = host.module;
    this.lost = true;
    M._jsui_gpu_context_lost(this.gpu);
    this._release();
    host._log(2, `WebGPU device lost (${info?.reason ?? 'unknown'}: ${info?.message ?? ''}), recreating`);
    const webgpu = await requestWebGPU({ allowSoftware: true, fresh: true });
    if (this._destroyed || host._destroyed) return;
    const next = webgpu && WebGPURenderer.create(host, webgpu, this.ctx);
    host._webgpuRecovered(next, 'WebGPU device lost');
  }
}
