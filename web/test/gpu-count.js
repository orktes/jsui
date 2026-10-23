// Counts WebGPU API calls in a page (web/test/frame-bench.mjs installs it
// with addInitScript before the host loads). window.__gpuCounts maps
// "Interface.method" to a call count; render passes are also counted by
// their color / depth-stencil load and store ops and attachment size, and
// "bytes.*" entries sum the bytes created / written / copied.
(() => {
  if (typeof GPUDevice === 'undefined' || window.__gpuCounts) return;
  const counts = window.__gpuCounts = new Map();
  const bump = (k, n = 1) => counts.set(k, (counts.get(k) ?? 0) + n);
  const wrap = (cls, names, extra) => {
    const proto = globalThis[cls]?.prototype;
    if (!proto) return;
    for (const name of names) {
      const f = proto[name];
      if (typeof f !== 'function') continue;
      proto[name] = function (...a) {
        bump(`${cls}.${name}`);
        extra?.(name, a, this);
        return f.apply(this, a);
      };
    }
  };
  wrap('GPUDevice', ['createBuffer', 'createTexture', 'createBindGroup', 'createSampler', 'createCommandEncoder',
    'createRenderPipeline', 'createComputePipeline', 'createShaderModule', 'createBindGroupLayout',
    'createPipelineLayout', 'pushErrorScope', 'popErrorScope'], (name, a) => {
    if (name === 'createBuffer') bump('bytes.createBuffer', a[0]?.size ?? 0);
    if (name === 'createTexture') {
      const d = a[0] ?? {}, sz = d.size ?? {};
      const [w, h] = Array.isArray(sz) || sz[Symbol.iterator] ? [...sz] : [sz.width, sz.height];
      bump(`new texture ${w}x${h ?? 1}${d.sampleCount > 1 ? `x${d.sampleCount}` : ''} ${d.format}`);
    }
  });
  wrap('GPUQueue', ['submit', 'writeBuffer', 'writeTexture', 'onSubmittedWorkDone'], (name, a) => {
    if (name === 'writeBuffer') bump('bytes.writeBuffer', a[4] ?? (a[2]?.byteLength ?? 0));
  });
  wrap('GPUCommandEncoder', ['beginRenderPass', 'beginComputePass', 'copyBufferToBuffer', 'copyBufferToTexture',
    'copyTextureToTexture', 'copyTextureToBuffer', 'clearBuffer'], (name, a) => {
    if (name === 'beginRenderPass') {
      for (const c of a[0]?.colorAttachments ?? []) {
        if (!c) continue;
        bump(`pass color ${c.view?.__tex ?? '?'} load=${c.loadOp} store=${c.storeOp}${c.resolveTarget ? ' +resolve' : ''}`);
        // Memory traffic of the color attachment on a tiler (Apple GPUs):
        // loading it into tile memory and storing it back, all samples.
        const t = c.view?.__size;
        if (t) bump('bytes.colorLoadStore', t.w * t.h * t.samples * t.bpp * ((c.loadOp === 'load') + (c.storeOp === 'store')));
      }
      const ds = a[0]?.depthStencilAttachment;
      if (ds) bump(`pass depth/stencil ${ds.view?.__tex ?? '?'} load=${ds.depthLoadOp ?? ds.stencilLoadOp} store=${ds.depthStoreOp ?? ds.stencilStoreOp}`);
    }
    if (name === 'copyBufferToBuffer') bump('bytes.copyBufferToBuffer', a[4] ?? 0);
    if (name === 'copyBufferToTexture') {
      // Uploads by destination texture (a glyph / path atlas, an image), with the texels written.
      const t = a[1]?.texture, sz = a[2] ?? {};
      const [w, h] = Array.isArray(sz) || sz[Symbol.iterator] ? [...sz] : [sz.width, sz.height ?? 1];
      if (t) {
        // Atlas-sized textures by creation order (Graphite's text and path
        // atlases are both 2048x2048 r8unorm; the first one made is used first).
        const k = `upload to ${t.width}x${t.height} ${t.format}${t.label ? ` "${t.label}"` : ''}` +
          `${t.width >= 1024 && t.__seq ? ` #${t.__seq}` : ''}`;
        bump(k);
        bump(`${k} texels`, (w ?? 0) * (h ?? 1));
      }
    }
  });
  wrap('GPURenderPassEncoder', ['draw', 'drawIndexed', 'drawIndirect', 'drawIndexedIndirect', 'setPipeline',
    'setBindGroup', 'setVertexBuffer', 'setIndexBuffer', 'setScissorRect', 'setViewport', 'setStencilReference',
    'setBlendConstant']);
  wrap('GPUBuffer', ['mapAsync', 'unmap', 'getMappedRange', 'destroy'], (name, a, buf) => {
    // Staging memory mapped (emdawnwebgpu copies all of it from the wasm heap on unmap).
    if (name === 'getMappedRange') bump('bytes.getMappedRange', a[1] ?? buf.size - (a[0] ?? 0));
  });
  wrap('GPUTexture', ['destroy']);
  wrap('GPUCanvasContext', ['getCurrentTexture']);
  let seq = 0;
  const createTexture = GPUDevice.prototype.createTexture;
  GPUDevice.prototype.createTexture = function (...a) {
    const t = createTexture.apply(this, a);
    try { t.__seq = ++seq; } catch {}
    return t;
  };
  // Views carry their texture's size / samples / format for the pass entries.
  const createView = GPUTexture.prototype.createView;
  GPUTexture.prototype.createView = function (...a) {
    bump('GPUTexture.createView');
    const v = createView.apply(this, a);
    try {
      v.__tex = `${this.width}x${this.height}${this.sampleCount > 1 ? `x${this.sampleCount}` : ''} ${this.format}`;
      const bpp = /^r8/.test(this.format) ? 1 : /^rg8/.test(this.format) ? 2 : /16float/.test(this.format) ? 8 : 4;
      v.__size = { w: this.width, h: this.height, samples: this.sampleCount, bpp };
    } catch {}
    return v;
  };
})();
