// Rift (interactive). Feature spotlight: custom SkSL shaders.
//
// A champion-select screen where everything that glows is a <ShaderView>:
// your own SkSL (Skia runtime effects) run by the runtime on whatever
// renderer it has (WebGPU, WebGL, Metal, Vulkan, CPU).
//
// - The nebula behind everything and the portal are shaders on the runtime
//   clock: they declare `uniform float time`, so they repaint every frame
//   with no JS and no React render.
// - Picking a champion animates the shaders' tint (a color interpolation)
//   and a surge uniform with the native Animated driver.
// - The champion card is a hologram: the card art is a child shader
//   (images={{ image: require(...) }}) sampled with scanlines, chromatic
//   split and a glitch burst (G).
// - The stats panel is ordinary JSUI UI post-processed by a shader that
//   declares `uniform shader content`: CRT, a ripple from where you click
//   (center + wave as native-driven uniforms) or a dissolve.
// - Live SkSL: edit the badge shader; it recompiles as you type and shows
//   Skia's compile errors.
import React, { memo, useEffect, useMemo, useRef, useState } from 'react';
import { Animated, Easing, Image, Pressable, ShaderView, StyleSheet, Text, TextInput, View, compileShader, sksl } from 'react-native';
import { colors, font } from '../../../examples/theme.js';
import { mono } from '../theme.js';

export const feature = {
  title: 'Custom SkSL shaders',
  points: [
    '<ShaderView source={sksl`…`}>: your own SkSL (Skia runtime effects) on every renderer: WebGPU, WebGL, Metal, Vulkan and the CPU',
    'uniforms take numbers, vectors, colors and Animated values: the tint and surge here run on the native driver, no JS per frame',
    'A shader declaring `time` animates on the runtime clock and repaints only its own view (the nebula, the portal, the CRT flicker)',
    'Child shaders: images and host textures (the card art); `uniform shader content` post-processes the view’s own UI (CRT, ripple, dissolve)',
  ],
  keys: ['1–4 / LB RB: champion', 'E / Y: panel effect', 'G / X: glitch', 'Click the panel: ripple', 'P: pause shaders'],
};

// --------------------------------------------------------------- shaders

const NOISE = `
// Dave Hoskins' hash (no sin(): stable at large coordinates on every backend).
float hash(float2 p) {
  float3 p3 = fract(float3(p.xyx) * 0.1031);
  p3 += dot(p3, p3.yzx + 33.33);
  return fract((p3.x + p3.y) * p3.z);
}
float noise(float2 p) {
  float2 i = floor(p);
  float2 f = fract(p);
  float2 u = f * f * (3.0 - 2.0 * f);
  return mix(mix(hash(i), hash(i + float2(1, 0)), u.x), mix(hash(i + float2(0, 1)), hash(i + float2(1, 1)), u.x), u.y);
}
float fbm(float2 p) {
  float v = 0.0;
  float a = 0.5;
  for (int i = 0; i < 5; i++) {
    v += a * noise(p);
    p = p * 2.03 + float2(1.7, 9.2);
    a *= 0.5;
  }
  return v;
}`;

const NEBULA = sksl`uniform float2 resolution;
uniform float time;
layout(color) uniform half4 tint;
uniform float surge;
${NOISE}
half4 main(float2 fc) {
  float2 uv = (fc - 0.5 * resolution) / resolution.y;
  float t = time * 0.035;
  float2 q = float2(fbm(uv * 2.2 + t), fbm(uv * 2.2 - t + 3.1));
  float n = fbm(uv * 3.0 + q * (1.6 + surge) + float2(t * 2.0, -t));
  half3 col = half3(0.02, 0.025, 0.06);
  col += half3(0.06, 0.08, 0.26) * half(smoothstep(0.25, 0.75, q.y));
  col += tint.rgb * half(smoothstep(0.42, 0.9, n) * (0.6 + 0.6 * surge));
  col += half3(0.5, 0.55, 0.9) * half(pow(n, 4.0) * 0.3);
  float s = hash(floor(fc / 3.0));
  col += half3(step(0.9975, s) * (0.55 + 0.45 * sin(time * 2.0 + s * 300.0)));
  col *= half(1.0 - 0.9 * dot(uv, uv));
  return half4(col, 1);
}`;

const PORTAL = sksl`uniform float2 resolution;
uniform float time;
layout(color) uniform half4 tint;
uniform float surge;
half4 main(float2 fc) {
  float2 uv = (fc - 0.5 * resolution) / (0.5 * resolution.y);
  float r = length(uv);
  float a = atan(uv.y, uv.x);
  float swirl = a + 2.6 / (r + 0.3) - time * (0.7 + surge * 3.0);
  float bands = 0.5 + 0.5 * sin(swirl * 5.0);
  float ring = smoothstep(1.0, 0.8, r) * smoothstep(0.2, 0.62, r);
  float core = smoothstep(0.75, 0.0, r);
  float glow = bands * ring;
  half3 col = tint.rgb * half(glow * (1.1 + surge)) + half3(pow(glow, 5.0) * 0.8) + tint.rgb * half(core * (0.3 + 0.5 * surge));
  float alpha = clamp(ring * (0.3 + 0.7 * bands) + core * 0.45, 0.0, 1.0);
  return half4(col * half(alpha), half(alpha));
}`;

const HOLO = sksl`uniform shader backdrop;
uniform shader portrait;
uniform float2 resolution;
uniform float time;
uniform float glitch;
layout(color) uniform half4 tint;
float h1(float y) { return fract(sin(y * 91.7) * 43758.5453); }
// The portrait over the card art, both pushed toward the champion's tint.
half4 layer(float2 p) {
  half4 b = backdrop.eval(p);
  half4 f = portrait.eval(p);
  half4 c = f + b * (1 - f.a);
  half lum = dot(c.rgb, half3(0.3, 0.59, 0.11));
  c.rgb = mix(c.rgb, tint.rgb * lum * 1.6 * c.a, 0.45);
  return c;
}
half4 main(float2 p) {
  float band = floor(p.y / 7.0);
  float tick = floor(time * 14.0);
  float jitter = (h1(band + tick) - 0.5) * 46.0 * glitch * step(0.55, h1(band * 1.3 + tick));
  float2 q = p + float2(jitter, 0);
  float split = 1.2 + 7.0 * glitch;
  half4 c = layer(q);
  c.r = layer(q + float2(split, 0)).r;
  c.b = layer(q - float2(split, 0)).b;
  c.rgb *= half(0.86 + 0.14 * sin(p.y * 1.7 - time * 7.0));
  float y = p.y / resolution.y;
  float sweep = 1.0 - smoothstep(0.0, 0.06, abs(fract(time * 0.22) * 1.5 - 0.25 - y));
  c.rgb += tint.rgb * half(sweep * 0.45) * c.a;
  float2 d = min(p, resolution - p);
  float edge = 1.0 - smoothstep(0.0, 9.0, min(d.x, d.y));
  c.rgb = c.rgb + tint.rgb * half(edge * 0.9);
  c.a = max(c.a, half(edge * 0.9));
  return c;
}`;

const CRT = sksl`uniform shader content;
uniform float2 resolution;
uniform float time;
half4 main(float2 p) {
  float2 uv = p / resolution - 0.5;
  float2 q = (uv * (1.0 + 0.1 * dot(uv, uv)) + 0.5) * resolution;
  if (q.x < 0.0 || q.y < 0.0 || q.x > resolution.x || q.y > resolution.y) return half4(0);
  half4 c = content.eval(q);
  c.r = content.eval(q + float2(1.4, 0)).r;
  c.b = content.eval(q - float2(1.4, 0)).b;
  c.rgb *= half(0.8 + 0.2 * sin(q.y * 2.1));
  c.rgb *= half((1.0 - 0.7 * dot(uv, uv)) * (0.96 + 0.04 * sin(time * 47.0)));
  return c;
}`;

const RIPPLE = sksl`uniform shader content;
uniform float2 center;
uniform float wave;
half4 main(float2 p) {
  float2 d = p - center;
  float dist = length(d);
  float k = (dist - wave * 560.0) / 38.0;
  float fall = exp(-k * k) * (1.0 - wave);
  float2 off = dist > 0.001 ? d / dist * fall * 16.0 * sin(k * 3.0) : float2(0);
  half4 c = content.eval(p - off);
  c.rgb += half(fall * 0.18) * c.a;
  return c;
}`;

const DISSOLVE = sksl`uniform shader content;
uniform float amount;
layout(color) uniform half4 edge;
${NOISE}
half4 main(float2 p) {
  half4 c = content.eval(p);
  float e = fbm(p / 46.0) - amount;
  if (e < 0.0) return half4(0);
  float glow = 1.0 - smoothstep(0.0, 0.07, e);
  c.rgb = mix(c.rgb, edge.rgb * c.a, half(glow));
  return c;
}`;

const BADGE = `// Edit me: recompiles as you type
uniform float2 resolution;
uniform float time;
layout(color) uniform half4 tint;

half4 main(float2 p) {
  float2 uv = p / resolution - 0.5;
  float r = length(uv) * 2;
  float a = atan(uv.y, uv.x);
  float s = 0.6 + 0.25 * sin(a * 5 + time);
  float m = smoothstep(s + 0.02, s, r);
  half3 c = mix(tint.rgb, half3(1), 1 - r);
  return half4(c * m, m);
}`;

// ---------------------------------------------------------------- data

const MAGE = require('../../../examples/assets/portrait-mage.png');
const ROGUE = require('../../../examples/assets/portrait-rogue.png');
const HEROES = [
  { id: 'ember', name: 'Kaelith', role: 'Duelist · Fire', art: require('../../../examples/assets/card-ember.png'), portrait: ROGUE, crest: require('../../../examples/assets/crest-flame.png'), tint: '#ff6a3d', stats: [0.92, 0.4, 0.66] },
  { id: 'tide', name: 'Maren', role: 'Controller · Tide', art: require('../../../examples/assets/card-tide.png'), portrait: MAGE, crest: require('../../../examples/assets/crest-wolf.png'), tint: '#3fa9ff', stats: [0.5, 0.86, 0.72] },
  { id: 'verdant', name: 'Sylvar', role: 'Warden · Grove', art: require('../../../examples/assets/card-verdant.png'), portrait: ROGUE, crest: require('../../../examples/assets/crest-eye.png'), tint: '#3fe08a', stats: [0.44, 0.94, 0.58] },
  { id: 'void', name: 'Nyxa', role: 'Assassin · Void', art: require('../../../examples/assets/card-void.png'), portrait: MAGE, crest: require('../../../examples/assets/crest-moon.png'), tint: '#b46bff', stats: [0.97, 0.3, 0.9] },
];
// Every shader the screen draws, for precompileShaders(): the site preloads
// them while idle, so opening the screen does not wait on the GPU building
// their pipelines (the HOLO card as it samples its art and portrait).
export const preload = [
  NEBULA,
  PORTAL,
  { source: HOLO, images: { backdrop: HEROES[0].art, portrait: { source: ROGUE, fit: 'contain' } } },
  CRT,
  RIPPLE,
  DISSOLVE,
  BADGE,
];
const STAT_NAMES = ['Power', 'Resilience', 'Mobility'];
const EFFECTS = [
  { id: 'none', label: 'None' },
  { id: 'crt', label: 'CRT' },
  { id: 'ripple', label: 'Ripple' },
  { id: 'dissolve', label: 'Dissolve' },
];
const PANEL_W = 440;
const PANEL_H = 300;

// ------------------------------------------------------------ components

function Chip({ label, active, color = colors.cyan, onPress, hint }) {
  return (
    <Pressable
      onPress={onPress}
      style={({ hovered, focused }) => [styles.chip, active && { borderColor: color, backgroundColor: color + '26' }, (hovered || focused) && styles.chipHover]}
    >
      <Text style={[styles.chipText, active && { color: colors.text }]}>{label}</Text>
      {hint ? <Text style={styles.chipHint}>{hint}</Text> : null}
    </Pressable>
  );
}

const HeroRow = memo(function HeroRow({ hero, index, selected, onSelect }) {
  return (
    <Pressable
      onPress={() => onSelect(index)}
      style={({ hovered, focused }) => [
        styles.heroRow,
        selected && { borderColor: hero.tint, backgroundColor: hero.tint + '22' },
        (hovered || focused) && !selected && styles.heroRowHover,
      ]}
    >
      <Image source={hero.portrait} style={[styles.heroThumb, { backgroundColor: hero.tint + '33' }, { borderColor: selected ? hero.tint : colors.line }]} resizeMode="cover" />
      <View style={styles.heroText}>
        <Text style={[styles.heroName, selected && { color: colors.text }]}>{hero.name}</Text>
        <Text style={styles.heroRole}>{hero.role}</Text>
      </View>
      <Text style={styles.heroKey}>{index + 1}</Text>
    </Pressable>
  );
});

/** The post-processed panel: plain JSUI UI inside a ShaderView whose
 * shader samples it as `content`. */
function StatsPanel({ hero, effect, paused, ripple, center, amount, onRipple }) {
  const source = effect === 'crt' ? CRT : effect === 'ripple' ? RIPPLE : effect === 'dissolve' ? DISSOLVE : null;
  const uniforms =
    effect === 'ripple' ? { center: [center.x, center.y], wave: ripple } : effect === 'dissolve' ? { amount, edge: hero.tint } : undefined;
  return (
    <ShaderView source={source} uniforms={uniforms} paused={paused} style={styles.panel}>
      <Pressable focusable={false} style={styles.panelInner} onPressIn={(e) => onRipple(e.nativeEvent.locationX, e.nativeEvent.locationY)}>
        <Text style={styles.panelKicker}>Champion dossier</Text>
        <Text style={[styles.panelName, { color: hero.tint }]}>{hero.name}</Text>
        <Text style={styles.panelRole}>{hero.role}</Text>
        {STAT_NAMES.map((name, i) => (
          <View key={name} style={styles.statRow}>
            <Text style={styles.statName}>{name}</Text>
            <View style={styles.statTrack}>
              <View style={[styles.statFill, { width: `${Math.round(hero.stats[i] * 100)}%`, backgroundColor: hero.tint }]} />
            </View>
            <Text style={styles.statValue}>{Math.round(hero.stats[i] * 100)}</Text>
          </View>
        ))}
        <View style={styles.abilities}>
          {['Q', 'E', 'R', 'F'].map((k, i) => (
            <View key={k} style={[styles.ability, i === 2 && { borderColor: hero.tint }]}>
              <Text style={[styles.abilityKey, i === 2 && { color: hero.tint }]}>{k}</Text>
            </View>
          ))}
          <Text style={styles.panelHint}>{effect === 'ripple' ? 'Click anywhere here' : 'Ordinary UI, post-processed'}</Text>
        </View>
      </Pressable>
    </ShaderView>
  );
}

/** Live SkSL: a TextInput whose text is the badge's shader source. */
function LiveShader({ tint, paused, onEditing }) {
  const [code, setCode] = useState(BADGE);
  const info = useMemo(() => compileShader(code), [code]);
  const [good, setGood] = useState(BADGE);
  useEffect(() => {
    if (!info.error) setGood(code);
  }, [info]);
  const error = info.error ? String(info.error).split('\n').filter(Boolean)[0] : null;
  return (
    <View style={styles.live}>
      <View style={styles.liveHead}>
        <Text style={styles.panelKicker}>Live SkSL</Text>
        <Text style={[styles.liveStatus, { color: error ? colors.red : colors.green }]}>{error ? 'Compile error' : 'Compiled'}</Text>
      </View>
      <View style={styles.liveBody}>
        <TextInput
          value={code}
          onChangeText={setCode}
          multiline
          spellCheck={false}
          autoCorrect={false}
          onFocus={() => onEditing(true)}
          onBlur={() => onEditing(false)}
          style={styles.code}
        />
        <View style={styles.badgeCol}>
          <View style={styles.badgeWell}>
            <ShaderView source={good} uniforms={{ tint }} paused={paused} style={styles.badge} />
          </View>
          <Text style={styles.badgeNote} numberOfLines={4}>
            {error || 'Uniforms: resolution, time (built in), tint (from the champion).'}
          </Text>
        </View>
      </View>
    </View>
  );
}

// ---------------------------------------------------------------- screen

export default function Rift({ active = true }) {
  const [hero, setHero] = useState(0);
  const [effectIx, setEffectIx] = useState(1);
  const [userPaused, setUserPaused] = useState(false);
  const editing = useRef(false); // keys go to the SkSL editor, not the shortcuts
  const paused = !active || userPaused;
  const h = HEROES[hero];
  const effect = EFFECTS[effectIx].id;

  // Tint: a color interpolation from the previous champion to the new one.
  const mix = useRef(new Animated.Value(1)).current;
  const [colorsFrom, setColorsFrom] = useState([h.tint, h.tint]);
  const tint = useMemo(() => mix.interpolate({ inputRange: [0, 1], outputRange: colorsFrom }), [colorsFrom]);
  const surge = useRef(new Animated.Value(0)).current;
  const glitch = useRef(new Animated.Value(0)).current;
  const ripple = useRef(new Animated.Value(1)).current;
  const center = useRef(new Animated.ValueXY({ x: PANEL_W / 2, y: PANEL_H / 2 })).current;
  const amount = useRef(new Animated.Value(-0.1)).current;
  const cardIn = useRef(new Animated.Value(1)).current;

  const doGlitch = () => {
    glitch.setValue(1);
    Animated.timing(glitch, { toValue: 0, duration: 650, easing: Easing.out(Easing.quad), useNativeDriver: true }).start();
  };

  const select = (i) => {
    if (i === hero) return;
    setColorsFrom([HEROES[hero].tint, HEROES[i].tint]);
    setHero(i);
    mix.setValue(0);
    Animated.timing(mix, { toValue: 1, duration: 700, easing: Easing.inOut(Easing.cubic), useNativeDriver: true }).start();
    surge.setValue(1);
    Animated.timing(surge, { toValue: 0, duration: 1400, easing: Easing.out(Easing.cubic), useNativeDriver: true }).start();
    cardIn.setValue(0);
    Animated.spring(cardIn, { toValue: 1, damping: 14, stiffness: 140, mass: 1, useNativeDriver: true }).start();
    doGlitch();
  };

  const onRipple = (x, y) => {
    if (effect !== 'ripple') return;
    center.setValue({ x, y });
    ripple.setValue(0);
    Animated.timing(ripple, { toValue: 1, duration: 1300, easing: Easing.out(Easing.quad), useNativeDriver: true }).start();
  };

  // Dissolve: burns away and back in, looping on the native driver.
  useEffect(() => {
    if (effect !== 'dissolve' || paused) return;
    const loop = Animated.loop(
      Animated.sequence([
        Animated.timing(amount, { toValue: 0.85, duration: 1800, easing: Easing.inOut(Easing.quad), useNativeDriver: true }),
        Animated.timing(amount, { toValue: -0.1, duration: 1800, easing: Easing.inOut(Easing.quad), useNativeDriver: true }),
      ]),
    );
    loop.start();
    return () => loop.stop();
  }, [effect, paused]);

  // A ripple when the effect is picked, so it shows without a click.
  useEffect(() => {
    if (effect === 'ripple') onRipple(PANEL_W * 0.3, PANEL_H * 0.4);
  }, [effect]);

  const handled = (e) => {
    e.preventDefault();
    e.stopPropagation();
  };
  const onKeyDown = (e) => {
    const k = e.key;
    if (editing.current) return;
    if (k >= '1' && k <= '4') select(+k - 1), handled(e);
    else if (k === 'e' || k === 'E') setEffectIx((i) => (i + 1) % EFFECTS.length), handled(e);
    else if (k === 'g' || k === 'G') doGlitch(), handled(e);
    else if (k === 'p' || k === 'P') setUserPaused((p) => !p), handled(e);
  };
  const onPad = (e) => {
    if (!e.pressed) return;
    if (e.buttonIndex === 4) select((hero + HEROES.length - 1) % HEROES.length), handled(e);
    else if (e.buttonIndex === 5) select((hero + 1) % HEROES.length), handled(e);
    else if (e.buttonIndex === 3) setEffectIx((i) => (i + 1) % EFFECTS.length), handled(e);
    else if (e.buttonIndex === 2) doGlitch(), handled(e);
  };

  const cardStyle = {
    opacity: cardIn.interpolate({ inputRange: [0, 1], outputRange: [0.2, 1] }),
    transform: [{ scale: cardIn.interpolate({ inputRange: [0, 1], outputRange: [0.86, 1] }) }],
  };

  return (
    <View focusable autoFocus={active} style={styles.root} onKeyDown={onKeyDown} onGamepadButton={onPad}>
      <ShaderView source={NEBULA} uniforms={{ tint, surge }} paused={paused} style={StyleSheet.absoluteFill} />

      <View style={styles.header}>
        <Text style={styles.kicker}>Ranked draft · Pick phase · 0:24</Text>
        <Text style={styles.title}>CHOOSE YOUR CHAMPION</Text>
      </View>

      <View style={styles.left}>
        {HEROES.map((x, i) => (
          <HeroRow key={x.id} hero={x} index={i} selected={i === hero} onSelect={select} />
        ))}
        <View style={styles.leftFoot}>
          <Chip label={userPaused ? 'Resume shaders' : 'Pause shaders'} hint="P" onPress={() => setUserPaused((p) => !p)} />
          <Chip label="Glitch" hint="G" onPress={doGlitch} />
        </View>
      </View>

      <View style={styles.stage} pointerEvents="none">
        <ShaderView source={PORTAL} uniforms={{ tint, surge }} paused={paused} style={styles.portal} />
        <Animated.View style={[styles.cardWrap, cardStyle]}>
          <ShaderView
            source={HOLO}
            images={{ backdrop: h.art, portrait: { source: h.portrait, fit: 'contain' } }}
            uniforms={{ glitch, tint }}
            paused={paused}
            style={styles.card}
          />
          <Image source={h.crest} style={styles.crest} />
        </Animated.View>
        <View style={styles.nameplate}>
          <Text style={styles.nameplateName}>{h.name}</Text>
          <Text style={[styles.nameplateRole, { color: h.tint }]}>{h.role}</Text>
        </View>
      </View>

      <View style={styles.right}>
        <View style={styles.effectRow}>
          {EFFECTS.map((x, i) => (
            <Chip key={x.id} label={x.label} active={i === effectIx} color={h.tint} onPress={() => setEffectIx(i)} hint={i === effectIx ? 'E' : null} />
          ))}
        </View>
        <StatsPanel hero={h} effect={effect} paused={paused} ripple={ripple} center={center} amount={amount} onRipple={onRipple} />
        <LiveShader tint={tint} paused={paused} onEditing={(on) => (editing.current = on)} />
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: { width: 1280, height: 720, overflow: 'hidden', backgroundColor: colors.bg0 },
  header: { position: 'absolute', left: 40, top: 30 },
  kicker: { fontFamily: font, fontSize: 12, fontWeight: '600', letterSpacing: 2, color: colors.textDim, textTransform: 'uppercase' },
  title: { fontFamily: font, fontSize: 30, fontWeight: '700', letterSpacing: 4, color: colors.text, marginTop: 6 },

  left: { position: 'absolute', left: 40, top: 128, width: 270, gap: 10 },
  heroRow: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 12,
    padding: 10,
    borderRadius: 12,
    borderWidth: 1,
    borderColor: colors.line,
    backgroundColor: 'rgba(10, 14, 28, 0.72)',
  },
  heroRowHover: { borderColor: colors.lineStrong, backgroundColor: 'rgba(20, 28, 52, 0.85)' },
  heroThumb: { width: 54, height: 54, borderRadius: 10, borderWidth: 1 },
  heroText: { flex: 1 },
  heroName: { fontFamily: font, fontSize: 17, fontWeight: '700', color: colors.textDim },
  heroRole: { fontFamily: font, fontSize: 12, color: colors.textMute, marginTop: 2 },
  heroKey: { fontFamily: font, fontSize: 12, fontWeight: '700', color: colors.textMute },
  leftFoot: { flexDirection: 'row', flexWrap: 'wrap', gap: 8, marginTop: 8 },

  stage: { position: 'absolute', left: 330, top: 100, width: 440, height: 600, alignItems: 'center' },
  portal: { position: 'absolute', left: -20, top: 20, width: 480, height: 480 },
  cardWrap: { position: 'absolute', left: 95, top: 70, width: 250, height: 380 },
  card: { width: 250, height: 380, borderRadius: 18 },
  crest: { position: 'absolute', left: 16, top: 16, width: 40, height: 40 },
  nameplate: { position: 'absolute', top: 492, alignItems: 'center', width: 440 },
  nameplateName: { fontFamily: font, fontSize: 34, fontWeight: '700', letterSpacing: 6, color: colors.text },
  nameplateRole: { fontFamily: font, fontSize: 13, fontWeight: '600', letterSpacing: 2, textTransform: 'uppercase', marginTop: 4 },

  right: { position: 'absolute', left: 800, top: 40, width: PANEL_W, gap: 12 },
  effectRow: { flexDirection: 'row', gap: 8 },
  panel: { width: PANEL_W, height: PANEL_H, borderRadius: 16 },
  panelInner: {
    flex: 1,
    padding: 20,
    borderRadius: 16,
    borderWidth: 1,
    borderColor: colors.lineStrong,
    backgroundColor: 'rgba(12, 18, 34, 0.94)',
  },
  panelKicker: { fontFamily: font, fontSize: 11, fontWeight: '700', letterSpacing: 1.8, color: colors.textMute, textTransform: 'uppercase' },
  panelName: { fontFamily: font, fontSize: 28, fontWeight: '700', marginTop: 6 },
  panelRole: { fontFamily: font, fontSize: 13, color: colors.textDim, marginBottom: 14 },
  statRow: { flexDirection: 'row', alignItems: 'center', gap: 10, marginTop: 8 },
  statName: { fontFamily: font, fontSize: 13, color: colors.textDim, width: 84 },
  statTrack: { flex: 1, height: 8, borderRadius: 4, backgroundColor: 'rgba(148, 170, 255, 0.12)', overflow: 'hidden' },
  statFill: { height: 8, borderRadius: 4 },
  statValue: { fontFamily: font, fontSize: 13, fontWeight: '700', color: colors.text, width: 28, textAlign: 'right' },
  abilities: { flexDirection: 'row', alignItems: 'center', gap: 10, marginTop: 22 },
  ability: {
    width: 42,
    height: 42,
    borderRadius: 10,
    borderWidth: 1,
    borderColor: colors.lineStrong,
    backgroundColor: colors.panelRaised,
    alignItems: 'center',
    justifyContent: 'center',
  },
  abilityKey: { fontFamily: font, fontSize: 15, fontWeight: '700', color: colors.textDim },
  panelHint: { flex: 1, textAlign: 'right', fontFamily: font, fontSize: 12, color: colors.textMute },

  live: {
    height: 300,
    padding: 14,
    borderRadius: 16,
    borderWidth: 1,
    borderColor: colors.line,
    backgroundColor: 'rgba(8, 12, 24, 0.9)',
  },
  liveHead: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center', marginBottom: 8 },
  liveStatus: { fontFamily: font, fontSize: 12, fontWeight: '700' },
  liveBody: { flex: 1, flexDirection: 'row', gap: 12 },
  code: {
    flex: 1,
    fontFamily: mono,
    fontSize: 10.5,
    lineHeight: 14,
    color: colors.text,
    padding: 8,
    borderRadius: 8,
    borderWidth: 1,
    borderColor: colors.line,
    backgroundColor: 'rgba(0, 0, 0, 0.35)',
    textAlignVertical: 'top',
  },
  badgeCol: { width: 104, gap: 8 },
  badgeWell: { width: 104, height: 104, borderRadius: 12, backgroundColor: 'rgba(0, 0, 0, 0.35)', alignItems: 'center', justifyContent: 'center' },
  badge: { width: 92, height: 92 },
  badgeNote: { fontFamily: font, fontSize: 11, lineHeight: 15, color: colors.textMute },

  chip: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 6,
    paddingHorizontal: 12,
    paddingVertical: 7,
    borderRadius: 999,
    borderWidth: 1,
    borderColor: colors.line,
    backgroundColor: 'rgba(10, 14, 28, 0.7)',
  },
  chipHover: { borderColor: colors.lineStrong },
  chipText: { fontFamily: font, fontSize: 13, fontWeight: '600', color: colors.textDim },
  chipHint: { fontFamily: font, fontSize: 11, fontWeight: '700', color: colors.textMute },
});
