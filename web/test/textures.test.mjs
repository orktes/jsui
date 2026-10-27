// Unit tests of web/jsui-textures.js (host textures) against a mock WASM
// module, WebGPU device and WebGL context: no browser or WASM build needed.
//
//   node --test web/test/textures.test.mjs
import assert from 'node:assert/strict';
import test from 'node:test';
import { HostTextureSet, TEXTURE_OPAQUE, TEXTURE_UNPREMULTIPLIED, sourceSize, textureTarget, videoFramesContextLost }
  from '../jsui-textures.js';

function mockModule() {
  const mem = new ArrayBuffer(1 << 20);
  let top = 64;
  const calls = [];
  const M = {
    HEAP32: new Int32Array(mem), HEAPU8: new Uint8Array(mem),
    _malloc: (n) => { const p = top; top += (n + 7) & ~7; return p; },
    _free: () => {},
    calls,
    _jsui_set_texture: (rt, key, p) => {
      const d = p >> 2;
      calls.push(['set', key, { kind: M.HEAP32[d], handle: M.HEAP32[d + 1] >>> 0, format: M.HEAP32[d + 2],
        width: M.HEAP32[d + 3], height: M.HEAP32[d + 4], flags: M.HEAP32[d + 5], usage: M.HEAP32[d + 6] }]);
      return 1;
    },
    _jsui_set_texture_pixels: (rt, key, p, w, h, rowBytes, flags) => {
      calls.push(['pixels', key, { w, h, rowBytes, flags, first: Array.from(M.HEAPU8.slice(p, p + 4)) }]);
      return 1;
    },
    _jsui_texture_updated: (rt, key) => { calls.push(['updated', key]); return 1; },
    _jsui_remove_texture: (rt, key) => { calls.push(['remove', key]); return 1; },
    _jsui_web_wgpu_create_instance: () => 900,
    _jsui_web_wgpu_release_instance: (i) => calls.push(['releaseInstance', i]),
    _jsui_web_wgpu_release_texture: (p) => calls.push(['releaseTexture', p]),
    WebGPU: { next: 1000, importJsTexture(t) { t.ptr = this.next++; return t.ptr; } },
    GL: { counter: 1, textures: [null], getNewId(table) { const r = this.counter++; for (let i = table.length; i < r; i++) table[i] = null; return r; } },
  };
  return M;
}

function mockDevice() {
  const copies = [];
  return {
    copies,
    created: [],
    createTexture(desc) {
      const t = { desc, destroyed: false, destroy() { this.destroyed = true; } };
      this.created.push(t);
      return t;
    },
    queue: { copyExternalImageToTexture: (src, dst, size) => copies.push({ src: src.source, dst, size }) },
  };
}

function mockGL() {
  const log = [];
  const state = { bound: 'prev', premultiply: false, lost: false };
  return {
    log, state,
    TEXTURE_2D: 0xde1, TEXTURE_BINDING_2D: 0x8069, RGBA: 0x1908, RGBA8: 0x8058, UNSIGNED_BYTE: 0x1401,
    UNPACK_FLIP_Y_WEBGL: 0x9240, UNPACK_PREMULTIPLY_ALPHA_WEBGL: 0x9241, TEXTURE_MIN_FILTER: 1, TEXTURE_MAG_FILTER: 2,
    TEXTURE_WRAP_S: 3, TEXTURE_WRAP_T: 4, LINEAR: 5, CLAMP_TO_EDGE: 6,
    getParameter: (p) => (p === 0x8069 ? state.bound : null),
    createTexture: () => ({ gltex: true }),
    deleteTexture: (t) => log.push(['delete', t]),
    bindTexture: (target, t) => { state.bound = t; },
    pixelStorei: (k, v) => { if (k === 0x9241) state.premultiply = v; },
    texParameteri: () => {},
    texImage2D: (...a) => log.push(['texImage2D', a[2], a.at(-1), state.premultiply]),
    texSubImage2D: (...a) => log.push(['texSubImage2D', a.at(-1)]),
    isContextLost: () => state.lost,
  };
}

function setup(views) {
  const M = mockModule();
  const warnings = [];
  const host = { module: M, rt: 1, _views: () => views, _withStrings: (strs, fn) => fn(...strs), _log: (l, t) => warnings.push(t) };
  return { M, host, set: new HostTextureSet(host), warnings };
}

const canvas = (w, h) => ({ width: w, height: h, getContext() {} });
const imageData = (w, h) => ({ width: w, height: h, data: new Uint8ClampedArray(w * h * 4).fill(7) });
const webgpuView = (device) => ({ _wgpu: { lost: false, webgpu: { device } } });

test('sourceSize: per source type, null without a picture', () => {
  assert.equal(sourceSize({ videoWidth: 640, videoHeight: 360, readyState: 1 }), null);
  assert.deepEqual(sourceSize({ videoWidth: 640, videoHeight: 360, readyState: 4 }), { width: 640, height: 360 });
  assert.deepEqual(sourceSize({ displayWidth: 1920, displayHeight: 1080, codedWidth: 1920, close() {} }), { width: 1920, height: 1080 });
  assert.equal(sourceSize({ naturalWidth: 0, naturalHeight: 0, complete: true }), null);
  assert.deepEqual(sourceSize(canvas(3, 2)), { width: 3, height: 2 });
  assert.equal(sourceSize(null), null);
});

test('textureTarget: one API and context, else CPU pixels', () => {
  const dev = mockDevice(), gl = mockGL(), sgl = { gl, lost: false, gen: 0 };
  assert.equal(textureTarget([]).kind, 'pixels');
  assert.equal(textureTarget([webgpuView(dev), webgpuView(dev)]).kind, 'webgpu');
  assert.equal(textureTarget([{ _sgl: sgl }, { _sgl: sgl }]).kind, 'gl');
  // Own WebGL context + the shared one: two GL contexts cannot share a texture.
  assert.equal(textureTarget([{ _gpu: 1, _gl: mockGL() }, { _sgl: sgl }]).kind, 'pixels');
  assert.equal(textureTarget([webgpuView(dev), { _sgl: sgl }]).kind, 'pixels');
  assert.equal(textureTarget([webgpuView(dev), {}]).kind, 'pixels');
  assert.equal(textureTarget([{ _sgl: { ...sgl, lost: true } }]).kind, 'lost');
});

test('WebGPU: one texture per name, refilled at the same size, replaced on resize', () => {
  const dev = mockDevice();
  const { M, set } = setup([webgpuView(dev)]);
  const src = canvas(64, 32);
  assert.equal(set.set('t', src), true);
  assert.equal(dev.created.length, 1);
  assert.deepEqual(dev.created[0].desc.size, [64, 32]);
  assert.equal(dev.created[0].desc.format, 'rgba8unorm');
  assert.equal(dev.created[0].desc.usage, 0x16);
  assert.equal(dev.copies[0].dst.premultipliedAlpha, true);
  const [, key, desc] = M.calls.find((c) => c[0] === 'set');
  assert.equal(key, 't');
  assert.deepEqual(desc, { kind: 2, handle: 1000, format: 0, width: 64, height: 32, flags: 0, usage: 0 });

  M.calls.length = 0;
  assert.equal(set.set('t', src), true);
  assert.deepEqual(M.calls, [['updated', 't']]);
  assert.equal(dev.created.length, 1);

  M.calls.length = 0;
  set.set('t', canvas(128, 64));
  assert.equal(dev.created.length, 2);
  // The new texture is set before the old one is freed.
  assert.equal(M.calls[0][0], 'set');
  assert.equal(M.calls[0][2].handle, 1001);
  assert.deepEqual(M.calls[1], ['releaseTexture', 1000]);
  assert.equal(dev.created[0].destroyed, true);

  set.destroy();
  assert.equal(dev.created[1].destroyed, true);
  assert.ok(M.calls.some((c) => c[0] === 'releaseInstance'));
});

test('WebGPU: a GPUTexture source is sampled in place, never copied or destroyed', () => {
  globalThis.GPUTexture ??= class GPUTexture {};
  const gpuTexture = (w, h, format = 'rgba8unorm', usage = 0x14) => Object.assign(new GPUTexture(),
    { width: w, height: h, format, usage, destroyed: false, destroy() { this.destroyed = true; } });
  const dev = mockDevice();
  const { M, set, warnings } = setup([webgpuView(dev)]);
  const game = gpuTexture(256, 128, 'rgba8unorm-srgb');
  assert.equal(set.set('minimap', game), true);
  assert.equal(dev.created.length, 0);
  assert.equal(dev.copies.length, 0);
  const [, , desc] = M.calls.find((c) => c[0] === 'set');
  assert.deepEqual(desc, { kind: 2, handle: game.ptr, format: 0, width: 256, height: 128, flags: 0, usage: 0 });

  // The same texture again: new contents.
  M.calls.length = 0;
  assert.equal(set.set('minimap', game), true);
  assert.deepEqual(M.calls, [['updated', 'minimap']]);

  // Another one replaces it; the game's texture is released, not destroyed.
  M.calls.length = 0;
  const next = gpuTexture(256, 128);
  set.set('minimap', next);
  assert.equal(M.calls[0][0], 'set');
  assert.deepEqual(M.calls[1], ['releaseTexture', game.ptr]);
  assert.equal(game.destroyed, false);

  // A canvas under the same name: a texture of our own from then on.
  set.set('minimap', canvas(16, 16));
  assert.equal(dev.created.length, 1);
  assert.equal(next.destroyed, false);

  // Unusable textures are reported, not thrown.
  assert.equal(set.set('bad', gpuTexture(8, 8, 'r8unorm')), false);
  assert.equal(set.set('nobind', gpuTexture(8, 8, 'rgba8unorm', 0x10)), false);
  assert.equal(warnings.length, 2);
  set.destroy();
  assert.equal(next.destroyed, false);
});

test('a GPUTexture source needs every view on WebGPU', () => {
  globalThis.GPUTexture ??= class GPUTexture {};
  const { set, warnings } = setup([{}]);
  const t = Object.assign(new GPUTexture(), { width: 8, height: 8, format: 'rgba8unorm', usage: 0x04 });
  assert.equal(set.set('x', t), false);
  assert.match(warnings[0], /needs the WebGPU renderer/);
});

test('video sources are opaque by default', () => {
  const dev = mockDevice();
  const { M, set } = setup([webgpuView(dev)]);
  set.set('v', { videoWidth: 32, videoHeight: 18, readyState: 4, currentTime: 0 });
  assert.equal(M.calls.find((c) => c[0] === 'set')[2].flags, TEXTURE_OPAQUE);
  set.set('c', canvas(4, 4), { opaque: true });
  assert.equal(M.calls.filter((c) => c[0] === 'set')[1][2].flags, TEXTURE_OPAQUE);
});

test('WebGL: a texture in the GL table of the views\' context; bindings restored', () => {
  const gl = mockGL();
  const sgl = { gl, lost: false, gen: 0 };
  const { M, set } = setup([{ _sgl: sgl }]);
  const src = canvas(10, 10);
  assert.equal(set.set('g', src), true);
  const desc = M.calls.find((c) => c[0] === 'set')[2];
  assert.equal(desc.kind, 1);
  assert.ok(M.GL.textures[desc.handle]?.gltex);
  assert.equal(M.GL.textures[desc.handle].name, desc.handle);
  assert.deepEqual(gl.log[0], ['texImage2D', gl.RGBA, src, true]);
  assert.equal(gl.state.bound, 'prev');
  assert.equal(gl.state.premultiply, false);
  set.set('g', src);
  assert.deepEqual(gl.log[1], ['texSubImage2D', src]);
  set.remove('g');
  assert.equal(M.GL.textures[desc.handle], null);
  assert.ok(gl.log.some((l) => l[0] === 'delete'));
});

test('CPU pixels: straight alpha copied into the runtime', () => {
  const { M, set } = setup([{}]);
  assert.equal(set.set('p', imageData(4, 2)), true);
  const [, key, p] = M.calls.find((c) => c[0] === 'pixels');
  assert.equal(key, 'p');
  assert.deepEqual(p, { w: 4, h: 2, rowBytes: 16, flags: TEXTURE_UNPREMULTIPLIED, first: [7, 7, 7, 7] });
  assert.equal(set.backend, 'pixels');
});

test('renderer change: re-uploaded from the kept source; VideoFrames are dropped', () => {
  const dev = mockDevice();
  const views = [webgpuView(dev)];
  const { M, set } = setup(views);
  set.set('img', imageData(8, 8));
  set.set('frame', { displayWidth: 8, displayHeight: 8, format: 'I420', close() {} });
  assert.equal(dev.created.length, 2);
  M.calls.length = 0;
  views[0] = {};  // the view fell back to the CPU raster
  set.sync();
  assert.ok(M.calls.some((c) => c[0] === 'pixels' && c[1] === 'img'));
  assert.ok(M.calls.some((c) => c[0] === 'remove' && c[1] === 'frame'));
  assert.ok(dev.created.every((t) => t.destroyed));
  assert.equal(set.backend, 'pixels');
  // Nothing changed: no work.
  M.calls.length = 0;
  set.sync();
  assert.deepEqual(M.calls, []);
});

test('lost context: uploads wait for the restore, which re-uploads into new textures', () => {
  const gl = mockGL();
  const sgl = { gl, lost: false, gen: 0 };
  const { M, set } = setup([{ _sgl: sgl }]);
  const src = canvas(4, 4);
  set.set('g', src);
  const first = M.calls.find((c) => c[0] === 'set')[2].handle;
  sgl.lost = true;
  gl.state.lost = true;
  M.calls.length = 0;
  assert.equal(set.set('g', src), false);
  set.sync();
  assert.deepEqual(M.calls, []);
  sgl.lost = false;
  gl.state.lost = false;
  sgl.gen++;
  set.sync();
  const again = M.calls.find((c) => c[0] === 'set')[2].handle;
  assert.notEqual(again, first);
  // The old texture belonged to the lost context: only its table slot is cleared.
  assert.equal(M.GL.textures[first], null);
  assert.ok(!gl.log.some((l) => l[0] === 'delete'));
});

test('lost during an upload (WebKit: createTexture gives null): no warning, uploaded after the restore', () => {
  const gl = mockGL();
  const sgl = { gl, lost: false, gen: 0 };
  const { M, set, warnings } = setup([{ _sgl: sgl }]);
  gl.createTexture = () => {
    gl.state.lost = true;  // noticed by this call, after textureTarget() looked
    return null;
  };
  assert.equal(set.set('g', canvas(4, 4)), false);
  assert.deepEqual(warnings, []);
  assert.ok(!M.calls.some((c) => c[0] === 'set'));
  delete gl.createTexture;
  gl.createTexture = () => ({ gltex: true });
  gl.state.lost = false;
  sgl.gen++;
  set.sync();
  assert.ok(M.calls.some((c) => c[0] === 'set'));
});

test('restore: lost again during the re-upload keeps the source; the host hears of each reset once', () => {
  const gl = mockGL();
  const sgl = { gl, lost: false, gen: 0 };
  const { M, set } = setup([{ _sgl: sgl }]);
  let resets = 0;
  set.host._texturesReset = () => resets++;
  set.set('g', canvas(4, 4));
  set.sync();
  assert.equal(resets, 0);
  sgl.lost = gl.state.lost = true;
  set.sync();
  // Restored, and lost again by the re-upload's first GL call.
  sgl.lost = gl.state.lost = false;
  sgl.gen++;
  gl.createTexture = () => {
    gl.state.lost = true;
    return null;
  };
  M.calls.length = 0;
  set.sync();
  assert.equal(resets, 1);
  assert.ok(set.entries.get('g')?.source, 'the source is kept');
  assert.ok(!M.calls.some((c) => c[0] === 'set'));
  // Back for good.
  gl.createTexture = () => ({ gltex: true });
  gl.state.lost = false;
  sgl.gen++;
  set.sync();
  assert.ok(M.calls.some((c) => c[0] === 'set' && c[1] === 'g'));
  assert.equal(resets, 2);
  set.sync();
  assert.equal(resets, 2);
});

test('a cross-origin source is reported once, not thrown', () => {
  const dev = mockDevice();
  dev.queue.copyExternalImageToTexture = () => { const e = new Error('tainted'); e.name = 'SecurityError'; throw e; };
  const { M, set, warnings } = setup([webgpuView(dev)]);
  assert.equal(set.set('x', canvas(4, 4)), false);
  assert.equal(set.set('x', canvas(4, 4)), false);
  assert.equal(warnings.length, 1);
  assert.match(warnings[0], /crossOrigin/);
  assert.ok(!M.calls.some((c) => c[0] === 'set'));
  assert.ok(dev.created.every((t) => t.destroyed));
});

test('a cross-origin video is not tried again until its URL changes', () => {
  const dev = mockDevice();
  let tries = 0;
  dev.queue.copyExternalImageToTexture = () => { tries++; const e = new Error('tainted'); e.name = 'SecurityError'; throw e; };
  const { set } = setup([webgpuView(dev)]);
  const video = { videoWidth: 16, videoHeight: 9, readyState: 4, currentSrc: 'https://a.example/v.webm' };
  for (let i = 0; i < 3; i++) assert.equal(set.set('v', video), false);
  assert.equal(tries, 1);
  video.currentSrc = 'https://b.example/v.webm';
  assert.equal(set.set('v', video), false);
  assert.equal(tries, 2);
});

test('bindVideo: one upload per presented frame, stops on unbind / remove', () => {
  const dev = mockDevice();
  const { M, set } = setup([webgpuView(dev)]);
  const cbs = new Map();
  let nextId = 1;
  const listeners = {};
  const video = {
    videoWidth: 16, videoHeight: 9, readyState: 4, currentTime: 0,
    requestVideoFrameCallback(cb) { const id = nextId++; cbs.set(id, cb); return id; },
    cancelVideoFrameCallback(id) { cbs.delete(id); },
    addEventListener(type, fn, opt) { listeners[type] = fn; opt?.signal?.addEventListener('abort', () => delete listeners[type]); },
  };
  const present = () => { const [[id, cb]] = cbs; cbs.delete(id); cb(); };
  const unbind = set.bindVideo('v', video);
  assert.equal(M.calls.filter((c) => c[0] === 'set').length, 1);  // the current frame right away
  present();
  present();
  assert.equal(M.calls.filter((c) => c[0] === 'updated').length, 2);
  listeners.seeked();
  assert.equal(M.calls.filter((c) => c[0] === 'updated').length, 3);
  unbind();
  assert.equal(cbs.size, 0);
  assert.equal(listeners.seeked, undefined);
  set.bindVideo('v', video);
  set.remove('v');
  assert.equal(cbs.size, 0);
  assert.ok(M.calls.some((c) => c[0] === 'remove'));
});

test('bindVideo: WebKit polls from the start; a frame count that ticks per frame decides alone', () => {
  const dev = mockDevice();
  const { M, set } = setup([webgpuView(dev)]);
  const rafs = new Map();
  let nextRaf = 1;
  const nav = Object.getOwnPropertyDescriptor(globalThis, 'navigator');
  Object.defineProperty(globalThis, 'navigator', { value: { vendor: 'Apple Computer, Inc.' }, configurable: true });
  globalThis.requestAnimationFrame = (cb) => { rafs.set(nextRaf, cb); return nextRaf++; };
  globalThis.cancelAnimationFrame = (id) => rafs.delete(id);
  const displayFrame = () => { const cbs = [...rafs.values()]; rafs.clear(); cbs.forEach((cb) => cb()); };
  try {
    let callbacks = 0;
    const mkVideo = (name) => {
      const v = {
        videoWidth: 16, videoHeight: 9, readyState: 4, currentTime: 0, paused: false, decoded: 0,
        getVideoPlaybackQuality() { return { totalVideoFrames: this.decoded }; },
        requestVideoFrameCallback() { return ++callbacks; },
        addEventListener() {},
      };
      set.bindVideo(name, v);
      return { v, uploads: () => M.calls.filter((c) => c[0] === 'updated' && c[1] === name).length };
    };
    // 60 Hz display frames (the position moves 1/60 s each).
    const run = (v, n, tick) => {
      for (let i = 1; i <= n; i++) {
        v.currentTime += 1 / 60;
        v.decoded += tick(i);
        displayFrame();
      }
    };
    // Chrome-like: a 30 fps video ticks every other display frame.
    const a = mkVideo('a');
    assert.equal(callbacks, 0);
    run(a.v, 5, (i) => i % 2);         // three ticks; until then the position uploads too
    assert.equal(a.uploads(), 5);
    run(a.v, 8, (i) => 1 - (i % 2));   // now one upload per video frame
    assert.equal(a.uploads(), 5 + 4);
    // The count stops ticking while playing: after 1/15 s the position decides again.
    a.v.currentTime += 0.05;
    displayFrame();
    assert.equal(a.uploads(), 9);
    a.v.currentTime += 0.05;
    displayFrame();
    assert.equal(a.uploads(), 10);
    run(a.v, 2, () => 0);
    assert.equal(a.uploads(), 12);
    // Paused, a seek uploads.
    a.v.paused = true;
    displayFrame();
    const paused = a.uploads();
    a.v.currentTime = 3;
    displayFrame();
    assert.equal(a.uploads(), paused + 1);
    // WebKit-like (H.264): the count jumps ~45 frames now and then, so every display frame uploads.
    const b = mkVideo('b');
    run(b.v, 90, (i) => (i % 45 === 0 ? 45 : 0));
    assert.equal(b.uploads(), 90);
    set.destroy();
  } finally {
    if (nav) Object.defineProperty(globalThis, 'navigator', nav);
    else delete globalThis.navigator;
    delete globalThis.requestAnimationFrame;
    delete globalThis.cancelAnimationFrame;
  }
});

// Last: the switch to polling is for the rest of the page (module state).
test('bindVideo: a WebGL context lost right after a frame callback switches every video to polling', () => {
  const dev = mockDevice();
  const { M, set } = setup([webgpuView(dev)]);
  const rafs = new Map();
  let nextRaf = 1;
  globalThis.requestAnimationFrame = (cb) => { rafs.set(nextRaf, cb); return nextRaf++; };
  globalThis.cancelAnimationFrame = (id) => rafs.delete(id);
  const displayFrame = () => { const cbs = [...rafs.values()]; rafs.clear(); cbs.forEach((cb) => cb()); };
  try {
    const mkVideo = () => {
      const cbs = new Map();
      let nextId = 1;
      return {
        cbs, videoWidth: 16, videoHeight: 9, readyState: 4, currentTime: 0, paused: true, decoded: 0,
        getVideoPlaybackQuality() { return { totalVideoFrames: this.decoded }; },
        requestVideoFrameCallback(cb) { const id = nextId++; cbs.set(id, cb); return id; },
        cancelVideoFrameCallback(id) { cbs.delete(id); },
        addEventListener(type, fn) { (this.on ??= {})[type] = fn; },
      };
    };
    const a = mkVideo();
    set.bindVideo('a', a);
    assert.equal(a.cbs.size, 1);
    // Paused: a loss now is no frame callback's doing.
    assert.equal(videoFramesContextLost(), null);
    // play() arms it: WebKit loses the context before 'playing'.
    a.paused = false;
    a.on.play();
    assert.match(videoFramesContextLost(), /polled/);
    assert.equal(a.cbs.size, 0);  // the frame callback is cancelled
    const uploads = () => M.calls.filter((c) => c[0] === 'updated' && c[1] === 'a').length;
    const before = uploads();
    displayFrame();
    assert.equal(uploads(), before);  // no new frame: no upload
    a.currentTime = 0.04;
    displayFrame();
    assert.equal(uploads(), before + 1);
    a.decoded = 5;  // a new frame at the same position (a live stream)
    displayFrame();
    assert.equal(uploads(), before + 2);
    // Later binds poll from the start.
    const b = mkVideo();
    set.bindVideo('b', b);
    assert.equal(b.cbs.size, 0);
    b.currentTime = 1;
    displayFrame();
    assert.ok(M.calls.some((c) => c[0] === 'updated' && c[1] === 'b'));
    set.destroy();
    assert.equal(rafs.size, 0);
  } finally {
    delete globalThis.requestAnimationFrame;
    delete globalThis.cancelAnimationFrame;
  }
});
