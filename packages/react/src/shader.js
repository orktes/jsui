// ShaderView: a view painted by your own SkSL shader (Skia runtime effects,
// https://skia.org/docs/user/sksl/), run by the runtime on whatever renderer
// it uses (CPU, WebGL, WebGPU, Metal, Vulkan). See render/shader_view.h.
//
//   <ShaderView
//     source={sksl`
//       uniform float2 resolution;   // filled in: the view's size in px
//       uniform float time;          // filled in: seconds since mount (repaints every frame)
//       uniform float glow;
//       layout(color) uniform half4 tint;
//       uniform shader image;
//       half4 main(float2 p) { return image.eval(p) * tint * (0.5 + glow * sin(time)); }`}
//     uniforms={{ glow: glowAnim, tint: '#7cf' }}
//     images={{ image: require('./card.png') }}
//     style={{ width: 300, height: 200, borderRadius: 16 }}
//   />
//
// uniforms: numbers, arrays (vectors / matrices / arrays, flattened), colors
// (r, g, b, a in 0..1, unpremultiplied) and Animated values, also inside
// arrays. Animated uniforms become `uniform:<name>` style keys (one per
// component in an array, `uniform:<name>.<i>`), so they animate with no
// React render and, with useNativeDriver or Animated.hostBuffer, with no JS
// per frame. `time`, `resolution` and `pixelRatio` are filled in when the
// shader declares them and `uniforms` leaves them out; `paused` stops the
// clock.
//
// images: child shaders (`uniform shader name;`), each an image source
// (require(), {uri}, a URI) or {texture: 'name'} for a host texture, with
// optional fit ('cover' (default), 'contain', 'stretch', 'center', 'none',
// 'repeat') and tile ('clamp' (default), 'repeat', 'mirror', 'decal'). An
// image is fitted to the view, so image.eval(p) samples it at the view
// position p. A child named `content` that `images` leaves out is the view's
// own painting (background and children): the shader post-processes it.
//
// onError({nativeEvent: {error}}): the source failed to compile (the view
// then paints only its background and children). compileShader(source)
// compiles it now and returns {error} or {uniforms, children}.
//
// precompileShaders([source | {source, images}]) (also ShaderView.preload):
// compiles shaders ahead of use and has the next frame build their GPU
// pipelines off-screen, so the first frame that shows them does not hitch
// (a GPU compiles a shader's pipelines the first time it draws it). Call it
// while loading, or when idle before the screen that uses them. Give
// `images` as the view will (which children are images, fit and tile) so
// the warm-up samples the same kind of child. Resolves with each entry's
// compileShader() result once that frame ran.

import { jsx } from 'react/jsx-runtime';
import { native, warnOnce } from './native.js';
import { resolveAssetSource } from './components/basic.js';
import { createAnimatedComponent } from './animated/createAnimatedComponent.js';
import { AnimatedNode } from './animated/nodes.js';

const hasNative = typeof native.shaderInfo === 'function';

/** Tagged template for SkSL sources (editors highlight it; it returns the text). */
export function sksl(strings, ...values) {
  let s = strings[0];
  for (let i = 0; i < values.length; i++) s += String(values[i]) + strings[i + 1];
  return s;
}

/** Compiles `source`: {error} or {uniforms: [{name, type, count, color}], children: [name]}. */
export function compileShader(source) {
  if (!hasNative) return { error: 'this host has no native shader support' };
  return native.shaderInfo(String(source));
}

function imageSpec(v) {
  if (v == null) return null;
  if (typeof v === 'object' && !Array.isArray(v) && (v.texture != null || v.source != null)) {
    const out = {};
    if (v.texture != null) out.texture = String(v.texture);
    else {
      const a = resolveAssetSource(v.source);
      if (!a || !a.uri) return null;
      out.uri = String(a.uri);
      if (a.scale && a.scale !== 1) out.scale = a.scale;
    }
    if (v.fit) out.fit = v.fit;
    if (v.tile) out.tile = v.tile;
    if (v.scale) out.scale = v.scale;
    return out;
  }
  const a = resolveAssetSource(v);
  if (!a || !a.uri) return null;
  const out = { uri: String(a.uri) };
  if (typeof v === 'object' && !Array.isArray(v)) {
    if (v.fit) out.fit = v.fit;
    if (v.tile) out.tile = v.tile;
  }
  if (a.scale && a.scale !== 1) out.scale = a.scale;
  return out;
}

function imageSpecs(images) {
  let out = null;
  if (images) {
    for (const name in images) {
      const spec = imageSpec(images[name]);
      if (spec) (out || (out = {}))[name] = spec;
    }
  }
  return out;
}

const nextFrame =
  typeof globalThis.requestAnimationFrame === 'function'
    ? (fn) => globalThis.requestAnimationFrame(fn)
    : (fn) => setTimeout(fn, 16);

/** Compiles shaders now and warms the GPU for them; see the top of this file. */
export function precompileShaders(list) {
  const entries = Array.isArray(list) ? list : [list];
  if (!hasNative || typeof native.shaderPreload !== 'function') {
    return Promise.resolve(entries.map(() => ({ error: 'this host has no native shader support' })));
  }
  const results = entries.map((e) => {
    const isObj = e != null && typeof e === 'object';
    const src = isObj ? (e.source != null ? e.source : e.sksl) : e;
    if (src == null) return { error: 'precompileShaders: no source' };
    const images = isObj ? imageSpecs(e.images) : null;
    return images ? native.shaderPreload(String(src), images) : native.shaderPreload(String(src));
  });
  // Until a frame ran the warm-up (a host that renders no frame for long,
  // e.g. hidden: give up waiting after a few seconds' worth of frames).
  return new Promise((resolve) => {
    let frames = 0;
    const check = () => {
      if (native.shadersWarming() === 0 || ++frames > 240) resolve(results);
      else nextFrame(check);
    };
    check();
  });
}

const AnimatedShaderHost = createAnimatedComponent('ShaderView');

const OMIT = { source: 1, sksl: 1, uniforms: 1, images: 1, paused: 1, style: 1, onError: 1 };

export function ShaderView(props) {
  const out = {};
  for (const k in props) if (OMIT[k] !== 1) out[k] = props[k];
  if (!hasNative) {
    warnOnce('shader-unavailable', 'ShaderView: this host has no native shader support; showing a plain view');
    out.style = props.style;
    return jsx('View', out);
  }
  const src = props.source != null ? props.source : props.sksl;
  out.source = src == null ? null : String(src);
  // Plain uniforms go as a prop; Animated ones as style keys.
  let plain = null;
  let animated = null;
  const u = props.uniforms;
  if (u) {
    for (const name in u) {
      const v = u[name];
      if (v instanceof AnimatedNode) {
        (animated || (animated = {}))['uniform:' + name] = v;
      } else if (Array.isArray(v) && v.some((e) => e instanceof AnimatedNode)) {
        const arr = v.map((e, i) => {
          if (!(e instanceof AnimatedNode)) return e;
          (animated || (animated = {}))['uniform:' + name + '.' + i] = e;
          return 0;
        });
        (plain || (plain = {}))[name] = arr;
      } else if (v != null) {
        (plain || (plain = {}))[name] = v;
      }
    }
  }
  if (plain) out.uniforms = plain;
  const images = imageSpecs(props.images);
  if (images) out.images = images;
  if (props.paused) out.paused = true;
  out.style = animated ? [props.style, animated] : props.style;
  if (props.onError) {
    const onError = props.onError;
    out.onError = (e) => onError(e);
  }
  // Always through the animated host, so a uniform can turn into an
  // Animated value (and back) without remounting the native view.
  return jsx(AnimatedShaderHost, out);
}
ShaderView.displayName = 'ShaderView';
ShaderView.preload = precompileShaders;

export default ShaderView;
