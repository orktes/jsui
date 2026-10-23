// main.gd's self-test probe for ShaderView (SkSL runtime effects), 64 x 32:
//   mode 'fill':    uniforms `a` | `b` in the left | right half;
//   mode 'content': the view's own painting (green, a red box on the left)
//                   mirrored by the shader: green | red;
//   mode 'texture': host texture `name` (two colours) as a child shader,
//                   mirrored: its right colour | its left colour;
//   mode 'clock':   red = fract(time), so the frame changes on its own.
import React from 'react';
import { ShaderView, View, sksl } from 'react-native';

const FILL = sksl`uniform float2 resolution;
layout(color) uniform half4 a;
layout(color) uniform half4 b;
half4 main(float2 p) { return p.x < resolution.x / 2 ? a : b; }`;

const MIRROR_CONTENT = sksl`uniform shader content;
uniform float2 resolution;
half4 main(float2 p) { return content.eval(float2(resolution.x - p.x, p.y)); }`;

const MIRROR_IMAGE = sksl`uniform shader image;
uniform float2 resolution;
half4 main(float2 p) { return image.eval(float2(resolution.x - p.x, p.y)); }`;

const CLOCK = sksl`uniform float time;
half4 main(float2 p) { return half4(half(fract(time)), 0, 0, 1); }`;

const size = { width: 64, height: 32 };

export function ShaderProbe({ mode = 'fill', a = '#dc283c', b = '#1ec85a', name = 'shaderprobe' }) {
  if (mode === 'content') {
    return (
      <ShaderView source={MIRROR_CONTENT} style={[size, { backgroundColor: '#1ec85a' }]}>
        <View style={{ width: 32, height: 32, backgroundColor: '#dc283c' }} />
      </ShaderView>
    );
  }
  if (mode === 'texture') {
    return <ShaderView source={MIRROR_IMAGE} images={{ image: { texture: name, fit: 'stretch' } }} style={size} />;
  }
  if (mode === 'clock') return <ShaderView source={CLOCK} style={size} />;
  return <ShaderView source={FILL} uniforms={{ a, b }} style={size} />;
}
