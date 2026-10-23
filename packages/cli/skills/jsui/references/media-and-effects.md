# Host textures, Lottie and shaders

## HostTexture: pictures the engine draws

JSUI shows textures the engine owns by name: a decoded video, a 3D render
target (character preview, minimap camera), a webcam. JSUI plays and decodes
nothing; it samples the engine's texture in place and repaints only the views
showing it.

```jsx
import { HostTexture, useHostTexture } from '@orktes/jsui-react';

const size = useHostTexture('trailer');     // { width, height } once the engine set it, else null
<HostTexture name="trailer" resizeMode="cover" style={{ width: 640, aspectRatio: 16 / 9, borderRadius: 16 }} />
```

Lays out like an `Image` (sized by the texture unless styled; `resizeMode`,
`tintColor`, rounded clipping, opacity, transforms). Show a placeholder while
`useHostTexture(name)` is null.

Engine side: C `jsui_set_texture(rt, name, &JsuiTexture{kind, handle, ...})`
(GL, WebGPU, Vulkan, Metal; RGBA8 / BGRA8) or `jsui_set_texture_pixels`
(CPU copy, every renderer) + `jsui_texture_updated(rt, name)` per new frame;
Godot `JsuiView.set_texture(name, texture, live)`; web
`jsui.bindVideo(name, videoElement)` / `jsui.setTexture(name, source)`.
Headless tools render nothing for unset textures (use the placeholder path).

## LottieView

Lottie (Bodymovin JSON) plays natively with Skia's Skottie (no JS per frame),
with lottie-react-native's API:

```jsx
import LottieView from 'lottie-react-native';      // or { LottieView } from '@orktes/jsui-react'

<LottieView source={require('./victory.json')} autoPlay loop={false}
  onAnimationFinish={() => setDone(true)} style={{ width: 240, height: 240 }} />

const ref = useRef(null);
ref.current.play(30, 60); ref.current.pause(); ref.current.resume(); ref.current.reset();
<LottieView source={anim} progress={animatedValue} />            // scrub; native driver works
<LottieView source={anim} colorFilters={[{ keypath: 'Star.**', color: '#f2c14e' }]} />
```

Props: `source` (require, object, JSON text, `{uri}`), `autoPlay`, `loop`
(default true), `speed` (negative reverses), `progress` 0-1, `resizeMode`
(`contain` default, `cover`, `center`, `stretch`), `colorFilters` (solid
fills / strokes; keypath names with `*` / `**`), `onAnimationLoaded`,
`onAnimationFinish`, `onAnimationFailure`. Without a size it takes the
composition's size. Not supported: expressions, external image assets,
dotLottie (`.lottie`) archives; embedded data: images and text with
registered fonts work.

## ShaderView: custom SkSL

Your own Skia shader language (SkSL, https://skia.org/docs/user/sksl/)
paints a view, on every renderer including the CPU raster.

```jsx
import { ShaderView, sksl } from '@orktes/jsui-react';

const aurora = sksl`
  uniform float2 resolution;   // filled in: view size in px
  uniform float time;          // filled in: seconds since mount (view repaints every frame)
  layout(color) uniform half4 tint;
  half4 main(float2 p) {
    float2 uv = p / resolution;
    float w = 0.5 + 0.5 * sin(uv.x * 6.2831 + time * 0.8 + uv.y * 3.0);
    return half4(tint.rgb * w, 1.0) * tint.a;
  }`;

<ShaderView source={aurora} uniforms={{ tint: '#5ec8ff' }} style={{ width: 400, height: 160, borderRadius: 16 }} />
```

* `uniforms`: numbers, arrays (`float2` = `[x, y]`), booleans, colors (for
  `layout(color)` uniforms) and `Animated` values (also in arrays; native
  driver and `Animated.hostBuffer` work). Built-ins when declared and not
  given: `time`, `resolution` (float2, px), `pixelRatio`. `paused` stops the
  clock.
* Coordinates: `p` is in view px from the padding box's top-left.
* Images as child shaders: `uniform shader image;` +
  `images={{ image: require('./card.png') }}` (or `{ texture: 'trailer' }`
  for a host texture; `fit`: cover / contain / stretch / center / none /
  repeat; `tile`: clamp / repeat / mirror / decal). Sample with
  `image.eval(p)` (view px).
* Post-processing: declare `uniform shader content;` and do not pass it in
  `images`: the view's own background and children become the input
  (`content.eval(p)`), e.g. a dissolve, chromatic aberration or CRT effect
  over a panel.
* Errors: a shader that fails to compile paints nothing and fires
  `onError` (`e.nativeEvent.error` has Skia's message). `compileShader(src)`
  returns `{ error }` or `{ uniforms, children }` synchronously: use it while
  iterating, and run `jsui screenshot` to see the result.

SkSL gotchas:

* SkSL is GLSL-like but not GLSL: `half4 main(float2 p)` is the entry point,
  types are `float2/3/4`, `half4`, `float2x2`; no `texture()`, use
  `child.eval(p)`; no `gl_FragCoord`, use `p`.
* Functions cannot take `shader` parameters; use the global child inside
  helper functions.
* `sin`-based hashes (`fract(sin(dot(p, k)) * 43758.5)`) break at large
  coordinates on the CPU raster. Use an integer-free hash such as Dave
  Hoskins' `hash12`: `p3 = fract(float3(p.xyx) * .1031); p3 += dot(p3,
  p3.yzx + 33.33); return fract((p3.x + p3.y) * p3.z);`.
* Return premultiplied color (`half4(rgb * a, a)`).
* Loops need constant bounds.
* A shader reading `time` repaints its view every frame; pass a fixed `time`
  uniform or `paused` when it should stay still.
