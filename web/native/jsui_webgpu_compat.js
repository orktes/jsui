// WebGPU glue fixes for browsers whose IDL differs from what Emdawnwebgpu
// emits, linked into the WebGPU build of jsui.js as a --pre-js.
//
// texture-component-swizzle: Emdawnwebgpu passes GPUTextureViewDescriptor's
// `swizzle` as the spec's four-character string ("000r"), but Chrome up to
// ~142 (Playwright 1.56's Chromium 141 among them) exposes the feature with
// the earlier dictionary form ({ r: 'zero', g: 'one', b: 'b', a: 'r' }) and
// throws a TypeError for a string. Skia Graphite only chains a swizzle when
// the device has the feature, which JSUI's own device never requests, but a
// shared device (createJsui({ device }), e.g. Three.js's, which requests every
// adapter feature) has it. On that TypeError, retry with the dictionary form.
(function () {
  const T = globalThis.GPUTexture;
  if (!T || !T.prototype.createView || T.prototype.createView.__jsuiSwizzleCompat) return;
  const createView = T.prototype.createView;
  const NAMES = { 0: 'zero', 1: 'one', r: 'r', g: 'g', b: 'b', a: 'a' };
  function compatCreateView(desc) {
    try {
      return createView.call(this, desc);
    } catch (e) {
      const s = desc && desc.swizzle;
      if (!(e instanceof TypeError) || typeof s !== 'string' || s.length !== 4) throw e;
      return createView.call(this, Object.assign({}, desc, {
        swizzle: { r: NAMES[s[0]], g: NAMES[s[1]], b: NAMES[s[2]], a: NAMES[s[3]] },
      }));
    }
  }
  compatCreateView.__jsuiSwizzleCompat = true;
  T.prototype.createView = compatCreateView;
})();
