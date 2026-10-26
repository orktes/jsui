// /vector: react-native-svg, @shopify/react-native-skia and
// react-native-worklets, each imported by its usual package name (the CLI
// aliases them to JSUI's own implementations) and drawn by native
// VectorViews.
import React, { useEffect, useMemo, useRef, useState } from 'react';
import { Animated, Easing, StyleSheet, Text, View } from 'react-native';
import Svg, { Circle as SvgCircle, Defs, G, LinearGradient as SvgLinearGradient, Path as SvgPath, RadialGradient as SvgRadialGradient, Stop } from 'react-native-svg';
import { BlurMask, Canvas, Circle, Group, Path, RadialGradient, Skia, SweepGradient, vec } from '@shopify/react-native-skia';
import { createSynchronizable, runOnUI } from 'react-native-worklets';
import { ShaderView, sksl } from '@orktes/jsui-react/shader';
import { colors, font, mono, site } from '../theme.js';
import { Column, Reveal, SectionHead, useLayout } from '../components/ui.jsx';
import { DemoButton, DemoCard } from './motion/controls.jsx';

const native = { useNativeDriver: true };

// --- 01: react-native-svg icons -------------------------------------------------

// 24 x 24 icons, each a gradient fill plus a rim.
const ICONS = [
  { name: 'Blade', from: '#ffe29a', to: '#a8741f', d: 'M19 2l3 1-1 3-9.5 9.5-3-3L18 3zM7.6 13.3l3.1 3.1-1.4 1.4-1-1-2.6 2.6.9.9-1.4 1.4-3.7-3.7 1.4-1.4.9.9 2.6-2.6-1-1z' },
  { name: 'Aegis', from: '#4fd1ff', to: '#1477b8', d: 'M12 1.5l8.5 3v6.2c0 5.2-3.5 9.8-8.5 11.8-5-2-8.5-6.6-8.5-11.8V4.5zM12 5l-5 1.8v3.9c0 3.3 2 6.3 5 7.8 3-1.5 5-4.5 5-7.8V6.8z' },
  { name: 'Elixir', from: '#ff7ab0', to: '#9e1730', d: 'M9 2h6v2h-1v4.2l5.2 8.6A3.4 3.4 0 0 1 16.3 22H7.7a3.4 3.4 0 0 1-2.9-5.2L10 8.2V4H9zM8.4 15l-1.9 3.1c-.4.7.1 1.5.9 1.5h9.2c.8 0 1.3-.8.9-1.5L15.6 15z' },
  { name: 'Prism', from: '#d7a6ff', to: '#5b2ea6', d: 'M7 3h10l5 6-10 12L2 9zm1.1 2L5.2 8.5h4.4L11 5zm4.9 0l1.4 3.5h4.4L16 5zM5.4 10.5l5.3 6.4-2.3-6.4zm5.2 0L12 15l1.4-4.5zm5 0l-2.3 6.4 5.3-6.4z' },
  { name: 'Crown', from: '#ffd36b', to: '#c46a12', d: 'M2 7l5 4 5-7 5 7 5-4-2 12H4zm3.3 10h13.4l.9-5.5-3.3 2.6L12 8.2l-4.3 5.9-3.3-2.6z' },
  { name: 'Vital', from: '#7dffb0', to: '#13803f', d: 'M12 21.3l-1.5-1.3C5.4 15.4 2 12.3 2 8.5 2 5.4 4.4 3 7.5 3c1.7 0 3.4.8 4.5 2.1C13.1 3.8 14.8 3 16.5 3 19.6 3 22 5.4 22 8.5c0 3.8-3.4 6.9-8.5 11.5z' },
];

const AnimatedSvgCircle = Animated.createAnimatedComponent(SvgCircle);
const RING_R = 52;
const RING_C = 2 * Math.PI * RING_R;

function Icon({ icon, size }) {
  const id = `g-${icon.name}`;
  return (
    <Svg width={size} height={size} viewBox="0 0 24 24">
      <Defs>
        <SvgLinearGradient id={id} x1="0" y1="0" x2="1" y2="1">
          <Stop offset="0" stopColor={icon.from} />
          <Stop offset="1" stopColor={icon.to} />
        </SvgLinearGradient>
      </Defs>
      <SvgPath d={icon.d} fill={`url(#${id})`} fillRule="evenodd" stroke="rgba(255,255,255,0.35)" strokeWidth={0.4} strokeLinejoin="round" />
    </Svg>
  );
}

function QuestRing() {
  const p = useRef(new Animated.Value(0)).current;
  useEffect(() => {
    const loop = Animated.loop(
      Animated.sequence([
        Animated.timing(p, { toValue: 1, duration: 2200, easing: Easing.inOut(Easing.cubic), ...native }),
        Animated.delay(700),
        Animated.timing(p, { toValue: 0, duration: 900, easing: Easing.in(Easing.quad), ...native }),
        Animated.delay(300),
      ]),
    );
    loop.start();
    return () => loop.stop();
  }, [p]);
  const offset = p.interpolate({ inputRange: [0, 1], outputRange: [RING_C, RING_C * 0.12] });
  return (
    <Svg width={132} height={132} viewBox="0 0 132 132">
      <Defs>
        <SvgLinearGradient id="ring" x1="0" y1="0" x2="1" y2="1">
          <Stop offset="0" stopColor={colors.goldLight} />
          <Stop offset="0.55" stopColor={colors.gold} />
          <Stop offset="1" stopColor={colors.orange} />
        </SvgLinearGradient>
        <SvgRadialGradient id="core" cx="0.5" cy="0.45" r="0.55">
          <Stop offset="0" stopColor="#24305a" />
          <Stop offset="1" stopColor="#0b1020" />
        </SvgRadialGradient>
      </Defs>
      <SvgCircle cx={66} cy={66} r={44} fill="url(#core)" />
      <SvgCircle cx={66} cy={66} r={RING_R} fill="none" stroke="rgba(148,170,255,0.14)" strokeWidth={10} />
      <G rotation={-90} origin="66, 66">
        <AnimatedSvgCircle cx={66} cy={66} r={RING_R} fill="none" stroke="url(#ring)" strokeWidth={10} strokeLinecap="round" strokeDasharray={`${RING_C} ${RING_C}`} strokeDashoffset={offset} />
      </G>
      <SvgPath d="M66 46l5.9 12 13.1 1.9-9.5 9.3 2.2 13.1L66 76.1l-11.7 6.2 2.2-13.1-9.5-9.3 13.1-1.9z" fill={colors.gold} opacity={0.92} />
    </Svg>
  );
}

function SvgDemo() {
  const { phone } = useLayout();
  return (
    <DemoCard
      index={1}
      title="react-native-svg"
      blurb="Import from 'react-native-svg' as usual. Paths, gradients, clip paths, masks, <Use> and SvgXml are drawn by Skia in C++, crisp at any scale. The quest ring is a strokeDashoffset driven by the native Animated driver, so it never re-renders React."
      tags={[['react-native-svg', colors.cyan], ['gradients', colors.purple], ['native driver', colors.green]]}
      codeTitle="QuestRing.jsx"
      code={`
import Svg, { Circle, Defs, LinearGradient, Stop } from 'react-native-svg';

const AnimatedCircle = Animated.createAnimatedComponent(Circle);
const C = 2 * Math.PI * 52;

const offset = progress.interpolate({
  inputRange: [0, 1], outputRange: [C, 0],
});

<Svg width={132} height={132} viewBox="0 0 132 132">
  <Defs>
    <LinearGradient id="ring" x1="0" y1="0" x2="1" y2="1">
      <Stop offset="0" stopColor="#ffe29a" />
      <Stop offset="1" stopColor="#ff9a3c" />
    </LinearGradient>
  </Defs>
  <AnimatedCircle cx={66} cy={66} r={52}
    fill="none" stroke="url(#ring)" strokeWidth={10}
    strokeLinecap="round" strokeDasharray={[C, C]}
    strokeDashoffset={offset} />
</Svg>`}
    >
      <View style={[styles.svgStage, phone && { flexDirection: 'column' }]}>
        <View style={styles.ringBox}>
          <QuestRing />
          <Text style={styles.caption}>Quest progress</Text>
        </View>
        <View style={styles.iconGrid}>
          {ICONS.map((icon) => (
            <View key={icon.name} style={styles.iconTile}>
              <Icon icon={icon} size={44} />
              <Text style={styles.iconLabel}>{icon.name}</Text>
            </View>
          ))}
        </View>
      </View>
    </DemoCard>
  );
}

// --- 02: Skia canvas --------------------------------------------------------------

const NEBULA = sksl`
uniform float2 resolution;
uniform float time;
float hash(float2 p) { return fract(sin(dot(p, float2(41.3, 289.1))) * 43758.5453); }
float noise(float2 p) {
  float2 i = floor(p), f = fract(p);
  float2 u = f * f * (3.0 - 2.0 * f);
  return mix(mix(hash(i), hash(i + float2(1, 0)), u.x), mix(hash(i + float2(0, 1)), hash(i + float2(1, 1)), u.x), u.y);
}
half4 main(float2 p) {
  float2 uv = (p - 0.5 * resolution) / resolution.y;
  float n = 0.0, a = 0.5;
  float2 q = uv * 3.0 + float2(time * 0.05, -time * 0.03);
  for (int i = 0; i < 5; i++) { n += a * noise(q); q *= 2.03; a *= 0.5; }
  float glow = exp(-3.2 * length(uv));
  half3 c = mix(half3(0.02, 0.03, 0.08), half3(0.25, 0.10, 0.45), half(n * 1.2));
  c += half3(0.95, 0.65, 0.25) * half(glow * 0.55 * (0.7 + 0.3 * n));
  return half4(c, 1.0);
}`;

// A six-point star path (Skia.Path, the imperative API).
function starPath(cx, cy, outer, inner, points = 6) {
  const path = Skia.Path.Make();
  for (let i = 0; i < points * 2; i++) {
    const r = i % 2 ? inner : outer;
    const a = (Math.PI * i) / points - Math.PI / 2;
    const x = cx + r * Math.cos(a);
    const y = cy + r * Math.sin(a);
    if (i === 0) path.moveTo(x, y);
    else path.lineTo(x, y);
  }
  path.close();
  return path;
}

function SkiaScene({ size }) {
  const c = size / 2;
  const spin = useRef(new Animated.Value(0)).current;
  const charge = useRef(new Animated.Value(0)).current;
  const pulse = useRef(new Animated.Value(0)).current;
  useEffect(() => {
    const anims = [
      Animated.loop(Animated.timing(spin, { toValue: 1, duration: 9000, easing: Easing.linear, ...native })),
      Animated.loop(
        Animated.sequence([
          Animated.timing(charge, { toValue: 1, duration: 2600, easing: Easing.inOut(Easing.cubic), ...native }),
          Animated.delay(500),
          Animated.timing(charge, { toValue: 0, duration: 1200, easing: Easing.inOut(Easing.quad), ...native }),
        ]),
      ),
      Animated.loop(
        Animated.sequence([
          Animated.timing(pulse, { toValue: 1, duration: 1300, easing: Easing.inOut(Easing.sin), ...native }),
          Animated.timing(pulse, { toValue: 0, duration: 1300, easing: Easing.inOut(Easing.sin), ...native }),
        ]),
      ),
    ];
    anims.forEach((a) => a.start());
    return () => anims.forEach((a) => a.stop());
  }, [spin, charge, pulse]);
  const rotate = spin.interpolate({ inputRange: [0, 1], outputRange: [0, Math.PI * 2] });
  const counter = spin.interpolate({ inputRange: [0, 1], outputRange: [0, -Math.PI * 4] });
  const glowR = pulse.interpolate({ inputRange: [0, 1], outputRange: [size * 0.16, size * 0.21] });
  const star = useMemo(() => starPath(c, c, size * 0.15, size * 0.065), [c, size]);
  const arc = useMemo(() => {
    const p = Skia.Path.Make();
    p.addCircle(c, c, size * 0.36);
    return p;
  }, [c, size]);
  const ticks = useMemo(() => {
    const p = Skia.Path.Make();
    for (let i = 0; i < 48; i++) {
      const a = (Math.PI * 2 * i) / 48;
      const r0 = size * (i % 4 ? 0.425 : 0.405);
      const r1 = size * 0.445;
      p.moveTo(c + r0 * Math.cos(a), c + r0 * Math.sin(a));
      p.lineTo(c + r1 * Math.cos(a), c + r1 * Math.sin(a));
    }
    return p;
  }, [c, size]);
  return (
    <View style={{ width: size, height: size, borderRadius: 20, overflow: 'hidden' }}>
      <ShaderView source={NEBULA} style={StyleSheet.absoluteFill} />
      <Canvas style={StyleSheet.absoluteFill}>
        <Group transform={[{ rotate }]} origin={vec(c, c)}>
          <Path path={ticks} style="stroke" strokeWidth={2} strokeCap="round" color="rgba(164,186,255,0.45)" />
        </Group>
        <Path path={arc} style="stroke" strokeWidth={10} color="rgba(148,170,255,0.12)" />
        <Group transform={[{ rotate: -Math.PI / 2 }]} origin={vec(c, c)}>
          <Path path={arc} style="stroke" strokeWidth={10} strokeCap="round" end={charge}>
            <SweepGradient c={vec(c, c)} colors={[colors.cyan, colors.purple, colors.gold, colors.cyan]} />
            <BlurMask blur={2} style="solid" />
          </Path>
        </Group>
        <Circle cx={c} cy={c} r={glowR}>
          <RadialGradient c={vec(c, c)} r={size * 0.22} colors={['rgba(255,214,120,0.85)', 'rgba(255,154,60,0.25)', 'rgba(255,154,60,0)']} />
        </Circle>
        <Group transform={[{ rotate: counter }]} origin={vec(c, c)}>
          <Path path={star} color={colors.goldLight}>
            <BlurMask blur={6} style="outer" />
          </Path>
          <Path path={star}>
            <RadialGradient c={vec(c, c - size * 0.04)} r={size * 0.16} colors={['#fff7d6', colors.gold, colors.goldDeep]} />
          </Path>
        </Group>
      </Canvas>
    </View>
  );
}

function SkiaDemo() {
  const { width } = useLayout();
  const size = width < 640 ? Math.min(300, width - 64) : 340;
  return (
    <DemoCard
      index={2}
      title="Skia Canvas"
      blurb="The @shopify/react-native-skia API: declarative Groups, Paths, gradients, blur masks and SkSL shaders, plus the imperative Skia object. Animated values plug straight into drawing props (rotation, trim, radius), and the native driver runs them with no React render and no JS per frame."
      tags={[['react-native-skia', colors.gold], ['SkSL', colors.purple], ['trim paths', colors.cyan]]}
      codeTitle="ChargeDial.jsx"
      code={`
import { Canvas, Group, Path, SweepGradient,
  BlurMask, Skia, vec } from '@shopify/react-native-skia';

const arc = Skia.Path.Make();
arc.addCircle(170, 170, 122);

// charge: an Animated.Value (native driver)
<Canvas style={{ width: 340, height: 340 }}>
  <Group transform={[{ rotate: -Math.PI / 2 }]}
         origin={vec(170, 170)}>
    <Path path={arc} style="stroke" strokeWidth={10}
          strokeCap="round" end={charge}>
      <SweepGradient c={vec(170, 170)}
        colors={['#4fd1ff', '#b26bff', '#f2c35b', '#4fd1ff']} />
      <BlurMask blur={2} style="solid" />
    </Path>
  </Group>
</Canvas>`}
    >
      <View style={styles.center}>
        <SkiaScene size={size} />
      </View>
    </DemoCard>
  );
}

// --- 03: worklets -----------------------------------------------------------------

const SWARM = 28;

function WorkletDemo() {
  const { width } = useLayout();
  const w = width < 640 ? Math.min(320, width - 64) : 420;
  const h = 260;
  const xs = useMemo(() => Array.from({ length: SWARM }, () => new Animated.Value(w / 2)), [w]);
  const ys = useMemo(() => Array.from({ length: SWARM }, () => new Animated.Value(h / 2)), [w]);
  const running = useMemo(() => createSynchronizable(true), []);
  const [on, setOn] = useState(true);
  const renders = useRef(0);
  renders.current++;

  useEffect(() => {
    running.setBlocking(on);
    if (!on) return undefined;
    const W = w;
    const H = h;
    const n = SWARM;
    // Runs on the UI runtime: every frame it moves the swarm along a
    // drifting rose curve and writes the Animated values directly.
    runOnUI(() => {
      'worklet';
      const t0 = performance.now();
      const frame = (now) => {
        if (!running.getDirty()) return;
        const t = (now - t0) / 1000;
        for (let i = 0; i < n; i++) {
          const k = i / n;
          const a = k * Math.PI * 2 + t * 0.6;
          const r = Math.cos(3 * a + t * 0.9) * 0.36 + 0.06 * Math.sin(t * 2.1 + i);
          xs[i].setValue(W / 2 + r * W * Math.cos(a));
          ys[i].setValue(H / 2 + r * H * 1.2 * Math.sin(a));
        }
        requestAnimationFrame(frame);
      };
      requestAnimationFrame(frame);
    })();
    return () => running.setBlocking(false);
  }, [on, w, xs, ys, running]);

  return (
    <DemoCard
      index={3}
      title="Worklets"
      blurb="react-native-worklets: functions marked 'worklet' run on the UI runtime, a separate small JS runtime that the render loop ticks. This swarm is one requestAnimationFrame loop there, writing Animated values that feed Skia circles. The React tree rendered once; the counter below proves it."
      tags={[['react-native-worklets', colors.green], ['runOnUI', colors.cyan], ['UI runtime', colors.purple]]}
      codeTitle="Swarm.jsx"
      code={`
import { runOnUI, createSynchronizable } from 'react-native-worklets';

const running = createSynchronizable(true);

runOnUI(() => {
  'worklet';
  const t0 = performance.now();
  const frame = (now) => {
    if (!running.getDirty()) return;
    const t = (now - t0) / 1000;
    for (let i = 0; i < n; i++) {
      const a = (i / n) * 2 * Math.PI + t * 0.6;
      const r = Math.cos(3 * a + t * 0.9) * 0.36;
      xs[i].setValue(W / 2 + r * W * Math.cos(a));
      ys[i].setValue(H / 2 + r * H * Math.sin(a));
    }
    requestAnimationFrame(frame);
  };
  requestAnimationFrame(frame);
})();

// xs / ys: Animated.Values, read by <Circle cx={xs[i]} cy={ys[i]} />`}
    >
      <View style={[styles.center, { gap: 14 }]}>
        <View style={[styles.swarm, { width: w, height: h }]}>
          <Canvas style={StyleSheet.absoluteFill}>
            {xs.map((x, i) => (
              <Circle key={i} cx={x} cy={ys[i]} r={i % 7 === 0 ? 7 : 4.5} color={[colors.cyan, colors.purple, colors.gold, colors.green][i % 4]}>
                <BlurMask blur={i % 7 === 0 ? 5 : 2} style="solid" />
              </Circle>
            ))}
          </Canvas>
        </View>
        <View style={styles.swarmBar}>
          <DemoButton label={on ? 'Pause worklet' : 'Run worklet'} primary={!on} onPress={() => setOn((v) => !v)} />
          <Text style={styles.stat}>React renders: {renders.current}</Text>
        </View>
      </View>
    </DemoCard>
  );
}

export function VectorPage() {
  const { phone } = useLayout();
  return (
    <Column style={{ paddingTop: phone ? 36 : 64, paddingBottom: 40 }}>
      <Reveal>
        <SectionHead
          eyebrow="Vector graphics"
          title="SVG, Skia and worklets. The React Native packages, natively."
          lead="Code written for react-native-svg, @shopify/react-native-skia and react-native-worklets runs on JSUI unchanged. Each <Svg> and <Canvas> is one native view painted by Skia on the GPU, on every host: the browser, Godot, Unity, Unreal and the rest."
        />
      </Reveal>
      <View style={{ gap: phone ? 18 : 28, marginTop: phone ? 32 : 56 }}>
        <SvgDemo />
        <SkiaDemo />
        <WorkletDemo />
      </View>
    </Column>
  );
}

const styles = StyleSheet.create({
  center: { alignItems: 'center', justifyContent: 'center', paddingVertical: 8 },
  svgStage: { flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: 28, paddingVertical: 12 },
  ringBox: { alignItems: 'center', gap: 10 },
  caption: { fontFamily: font, fontWeight: '600', fontSize: 12, letterSpacing: 1.6, textTransform: 'uppercase', color: colors.textMute },
  iconGrid: { flexDirection: 'row', flexWrap: 'wrap', gap: 12, maxWidth: 300, justifyContent: 'center' },
  iconTile: {
    width: 88,
    alignItems: 'center',
    gap: 8,
    paddingVertical: 14,
    borderRadius: 14,
    borderWidth: 1,
    borderColor: site.stroke,
    backgroundColor: 'rgba(255,255,255,0.025)',
  },
  iconLabel: { fontFamily: font, fontWeight: '600', fontSize: 12, color: colors.textDim },
  swarm: { borderRadius: 18, overflow: 'hidden', borderWidth: 1, borderColor: site.stroke, backgroundColor: '#070b18' },
  swarmBar: { flexDirection: 'row', alignItems: 'center', gap: 16 },
  stat: { fontFamily: mono, fontSize: 13, color: colors.textDim },
});
