// GPU timestamps per render pass and copy in a page (web/test/frame-bench.mjs
// installs it with addInitScript before the host loads, with --gpu-timing).
//
// requestDevice() gets the 'timestamp-query' feature when the adapter has it.
// Every render pass Skia begins gets timestampWrites (begin / end of pass),
// and every run of texture / buffer copies recorded back to back is
// bracketed by two empty compute passes whose timestamps mark its start and
// end (WebGPU only writes timestamps at pass boundaries). Each command
// encoder resolves its queries at finish() into a buffer that is read back
// after the submit.
//
// window.__gpuTiming:
//   supported   true when the device has timestamps
//   frame       the frame number tagged onto entries (the bench sets it)
//   entries     [{ frame, kind, label, ms, copies, px }], kind 'pass' | 'copy';
//               a copy run's label names its kinds ("copyBufferToBuffer"),
//               `copies` counts them and `px` sums the texels of texture copies
//   pending()   resolves when every read-back has landed
//   clear()
// Labels name the attachment ("pass 3840x2160 bgra8unorm load=clear") or the
// copy ("copyTextureToTexture 120x40"). Chrome quantizes timestamps to 100 us
// unless it runs with --enable-webgpu-developer-features; the bench passes it.
(() => {
  if (typeof GPUDevice === 'undefined' || window.__gpuTiming) return;
  const T = window.__gpuTiming = { supported: false, frame: 0, entries: [], reads: new Set(), errors: 0,
    pending() { return Promise.all([...this.reads]); }, clear() { this.entries = []; } };
  const MAX_QUERIES = 4096;  // per command encoder (pairs of begin / end)

  const requestDevice = GPUAdapter.prototype.requestDevice;
  GPUAdapter.prototype.requestDevice = function (desc = {}) {
    if (this.features.has('timestamp-query')) {
      desc = { ...desc, requiredFeatures: [...(desc.requiredFeatures ?? []), 'timestamp-query'] };
    }
    return requestDevice.call(this, desc);
  };

  const texLabel = (view) => {
    const t = view?.__timingTex;
    return t ? `${t.width}x${t.height}${t.sampleCount > 1 ? `x${t.sampleCount}` : ''} ${t.format}` : '?';
  };
  const createView = GPUTexture.prototype.createView;
  GPUTexture.prototype.createView = function (...a) {
    const v = createView.apply(this, a);
    try { v.__timingTex = { width: this.width, height: this.height, sampleCount: this.sampleCount, format: this.format }; } catch {}
    return v;
  };

  // Per device: a pool of query sets with their resolve / read-back buffers.
  const pools = new WeakMap();
  const poolFor = (device) => {
    let p = pools.get(device);
    if (!p) pools.set(device, (p = []));
    return p;
  };
  const acquire = (device) => {
    const p = poolFor(device);
    const s = p.pop();
    if (s) return s;
    return {
      qs: device.createQuerySet({ type: 'timestamp', count: MAX_QUERIES }),
      resolve: device.createBuffer({ size: MAX_QUERIES * 8, usage: GPUBufferUsage.QUERY_RESOLVE | GPUBufferUsage.COPY_SRC }),
      read: device.createBuffer({ size: MAX_QUERIES * 8, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ }),
    };
  };

  const createCommandEncoder = GPUDevice.prototype.createCommandEncoder;
  GPUDevice.prototype.createCommandEncoder = function (...a) {
    const enc = createCommandEncoder.apply(this, a);
    if (this.features.has('timestamp-query')) {
      T.supported = true;
      enc.__timing = { device: this, set: null, n: 0, items: [], frame: T.frame };
    }
    return enc;
  };
  const slot = (enc, kind, label) => {
    const s = enc.__timing;
    if (!s || s.n + 2 > MAX_QUERIES) return -1;
    s.set ??= acquire(s.device);
    const i = s.n;
    s.n += 2;
    s.items.push({ kind, label, i });
    return i;
  };

  const beginRenderPass = GPUCommandEncoder.prototype.beginRenderPass;
  GPUCommandEncoder.prototype.beginRenderPass = function (desc) {
    if (this.__timing && desc && !desc.timestampWrites) {
      const c = desc.colorAttachments?.find?.((x) => x) ?? null;
      const ds = desc.depthStencilAttachment;
      const label = c ? `pass ${texLabel(c.view)} load=${c.loadOp}${ds ? ' +depth/stencil' : ''}`
        : `pass depth-only ${texLabel(ds?.view)}`;
      const i = slot(this, 'pass', label);
      if (i >= 0) {
        desc = { ...desc, timestampWrites: { querySet: this.__timing.set.qs, beginningOfPassWriteIndex: i, endOfPassWriteIndex: i + 1 } };
      }
    }
    return beginRenderPass.call(this, desc);
  };
  const beginComputePass = GPUCommandEncoder.prototype.beginComputePass;
  GPUCommandEncoder.prototype.beginComputePass = function (desc) {
    if (this.__timing && !this.__timingMarker && !desc?.timestampWrites) {
      const i = slot(this, 'compute', 'compute pass');
      if (i >= 0) desc = { ...desc, timestampWrites: { querySet: this.__timing.set.qs, beginningOfPassWriteIndex: i, endOfPassWriteIndex: i + 1 } };
    }
    return beginComputePass.call(this, desc);
  };
  // A timestamp between commands: an empty compute pass that writes one.
  const mark = (enc, index, end) => {
    enc.__timingMarker = true;
    const tw = { querySet: enc.__timing.set.qs };
    tw[end ? 'endOfPassWriteIndex' : 'beginningOfPassWriteIndex'] = index;
    beginComputePass.call(enc, { timestampWrites: tw }).end();
    enc.__timingMarker = false;
  };
  // Copies recorded back to back are timed as one run: the GPU (Dawn) runs
  // them in one blit encoder, and marks around each copy would add a compute
  // pass and two encoder switches per copy (on Apple GPUs most of what such
  // a span measures). The run's entry counts its copies and their texels.
  const closeRun = (enc) => {
    const run = enc.__timing?.run;
    if (!run) return;
    enc.__timing.run = null;
    mark(enc, run.item.i + 1, false);
  };
  const copyBufferToBuffer = GPUCommandEncoder.prototype.copyBufferToBuffer;
  for (const name of ['copyTextureToTexture', 'copyBufferToTexture', 'copyTextureToBuffer', 'copyBufferToBuffer', 'clearBuffer']) {
    const f = GPUCommandEncoder.prototype[name];
    GPUCommandEncoder.prototype[name] = function (...a) {
      const s = this.__timing;
      if (!s) return f.apply(this, a);
      let run = s.run;
      if (!run) {
        const i = slot(this, 'copy', '');
        if (i < 0) return f.apply(this, a);
        mark(this, i, true);
        run = s.run = { item: s.items[s.items.length - 1], names: new Set() };
        run.item.copies = 0;
        run.item.px = 0;
      }
      run.names.add(name);
      run.item.label = [...run.names].join('+');
      run.item.copies++;
      const e = name === 'copyBufferToBuffer' || name === 'clearBuffer' ? null : a[2];
      if (e) {
        const [w, h] = Array.isArray(e) ? e : [e.width, e.height ?? 1];
        run.item.px += (w ?? 0) * (h ?? 1);
      }
      return f.apply(this, a);
    };
  }
  for (const name of ['beginRenderPass', 'beginComputePass']) {
    const f = GPUCommandEncoder.prototype[name];
    GPUCommandEncoder.prototype[name] = function (...a) {
      if (!this.__timingMarker) closeRun(this);
      return f.apply(this, a);
    };
  }
  const finish = GPUCommandEncoder.prototype.finish;
  GPUCommandEncoder.prototype.finish = function (...a) {
    closeRun(this);
    const s = this.__timing;
    if (s?.set && s.n) {
      this.resolveQuerySet(s.set.qs, 0, s.n, s.set.resolve, 0);
      // The read-back buffer may still be mapped from an earlier use: copies
      // into it happen only when it is free (acquire() hands out free ones).
      copyBufferToBuffer.call(this, s.set.resolve, 0, s.set.read, 0, s.n * 8);
    }
    const cb = finish.apply(this, a);
    if (s?.set && s.n) cb.__timing = s;
    else if (s?.set) poolFor(s.device).push(s.set);
    return cb;
  };
  const submit = GPUQueue.prototype.submit;
  GPUQueue.prototype.submit = function (cbs) {
    const r = submit.call(this, cbs);
    for (const cb of cbs ?? []) {
      const s = cb?.__timing;
      if (!s) continue;
      cb.__timing = null;
      const p = s.set.read.mapAsync(GPUMapMode.READ, 0, s.n * 8).then(() => {
        const ts = new BigInt64Array(s.set.read.getMappedRange(0, s.n * 8).slice(0));
        s.set.read.unmap();
        for (const it of s.items) {
          const ns = Number(ts[it.i + 1] - ts[it.i]);
          // Unwritten or reset queries read as 0; a negative span is invalid.
          if (ts[it.i] > 0n && ns >= 0) {
            T.entries.push({ frame: s.frame, kind: it.kind, label: it.label, ms: ns / 1e6, copies: it.copies, px: it.px });
          }
        }
      }, () => { T.errors++; }).finally(() => {
        poolFor(s.device).push(s.set);
        T.reads.delete(p);
      });
      T.reads.add(p);
    }
    return r;
  };
})();
