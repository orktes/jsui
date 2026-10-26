// The intro that plays once the runtime has loaded: a warp through an SkSL
// nebula (one full-screen <ShaderView>) with game UI flying past the camera,
// the JSUI wordmark, and then the nebula burning away from the center to
// reveal the page.
//
// - Everything moves on the native Animated driver from one progress value
//   `p` (0..1): the shader takes it as a uniform, the flying UI and the
//   wordmark interpolate it. No JS runs per frame.
// - The flying UI is ordinary JSUI views (HUD panels, a loot card, a menu…),
//   not images: transforms on the native driver, painted by Skia.
// - The page decides whether it plays (index.html answers the app's 'ready'
//   with an 'intro' message): not with prefers-reduced-motion, not on the CPU
//   raster, not with ?intro=0. Any key, click, tap or gamepad button skips it.
// - The shader is compiled and its GPU pipelines warmed (precompileShaders)
//   before the timeline starts, so a cold shader cache does not eat the
//   first frames. Until then the overlay is the page's background color,
//   under the HTML splash.
// - Then, still under the splash, the whole intro is drawn once through at
//   WARM_STEPS points of its timeline, one per frame: the flying UI's draws
//   (shadows, gradients, scaled and rotated text, the shader under them)
//   create their GPU pipelines there, not in the frames that play. With a
//   cold browser shader cache (a first visit, a new browser version) those
//   compiles took the intro's first second down to a few frames.
import React, { useEffect, useMemo, useRef, useState } from 'react';
import { Animated, Easing, Host, Pressable, ShaderView, StyleSheet, Text, View, compileShader, precompileShaders, sksl, useHostMessage, useWindowDimensions } from 'react-native';
import { colors, font, rarity, site } from '../theme.js';

const DURATION = 3400;
// p where the reveal starts (the nebula begins to burn away).
const REVEAL = 0.72;
// No answer from the page (another host, an older index.html): give up. The
// page answers once the GPU pipelines are built (index.html, at most ~5 s).
const WAIT_MS = 7000;
// Timeline points drawn under the splash before the intro plays (above).
const WARM_STEPS = 12;

// --------------------------------------------------------------- shader

export const INTRO_SHADER = sksl`uniform float2 resolution;
uniform float time;
uniform float pixelRatio;
uniform float p;

// Dave Hoskins' hash (no sin(): stable at large coordinates on every backend).
float hash(float2 q) {
  float3 p3 = fract(float3(q.xyx) * 0.1031);
  p3 += dot(p3, p3.yzx + 33.33);
  return fract((p3.x + p3.y) * p3.z);
}
float noise(float2 q) {
  float2 i = floor(q);
  float2 f = fract(q);
  float2 u = f * f * (3.0 - 2.0 * f);
  return mix(mix(hash(i), hash(i + float2(1, 0)), u.x), mix(hash(i + float2(0, 1)), hash(i + float2(1, 1)), u.x), u.y);
}
float fbm(float2 q) {
  float v = 0.0;
  float a = 0.5;
  for (int i = 0; i < 4; i++) {
    v += a * noise(q);
    q = q * 2.07 + float2(1.7, 9.2);
    a *= 0.5;
  }
  return v;
}

// Stars rushing past: in polar coordinates, one star per angular bin and
// layer at a radius that accelerates outward with travel, stretched along
// the radius into a streak while the warp is on (a streak never crosses a
// bin edge along its length, so nothing gets clipped).
float streaks(float2 uv, float travel, float warp) {
  float r = length(uv);
  float a = atan(uv.y, uv.x) / 6.2831853;
  float acc = 0.0;
  for (int i = 0; i < 3; i++) {
    float fi = float(i);
    float bins = 110.0 + fi * 80.0;
    float x = a * bins + fi * 0.37;
    float id = floor(x);
    float h = hash(float2(id, fi * 13.1 + 1.0));
    float h2 = hash(float2(id + 3.7, fi + 5.0));
    float d = fract(h + travel * (0.5 + 0.9 * h2));  // 0: far, 1: past the camera
    float rs = 0.02 + d * d * 1.25;
    float len = 0.003 + warp * (0.015 + 0.3 * d * d);
    float across = abs(fract(x) - 0.5) * r * 6.2831853 / bins;
    float w = 0.0012 + 0.0035 * d;
    float tail = clamp((r - rs + len) / len, 0.0, 1.0) * step(r, rs);
    float head = smoothstep(0.004 + 0.01 * d, 0.0, abs(r - rs));
    float body = max(tail * tail, head) * smoothstep(w, w * 0.2, across);
    acc += body * smoothstep(0.0, 0.35, d) * step(0.35, h2) * (0.45 + 0.55 * h);
  }
  return acc;
}

// Distant stars that stay put and twinkle (cells of 4 logical px).
float field(float2 fc, float time) {
  float2 g = fc / (4.0 * pixelRatio);
  float h = hash(floor(g));
  float dot = smoothstep(0.42, 0.1, length(fract(g) - 0.5));
  return step(0.996, h) * dot * (0.55 + 0.45 * sin(time * 2.0 + h * 300.0));
}

half4 main(float2 fc) {
  float2 uv = (fc - 0.5 * resolution) / resolution.y;
  float r = length(uv);
  float warp = smoothstep(0.06, 0.4, p) * (1.0 - smoothstep(0.48, 0.7, p));
  float t = time * 0.035;

  // The nebula: domain-warped fbm, zooming in as we fly.
  float2 w = uv / (1.0 + 1.4 * p * p);
  float2 q = float2(fbm(w * 1.9 + t), fbm(w * 1.9 - t + 3.1));
  float n = fbm(w * 2.6 + q * 1.9 + float2(t * 1.6, -t));
  float lanes = fbm(w * 4.4 - q * 1.3 + 7.7);

  half3 col = half3(0.02, 0.027, 0.059);
  col += half3(0.07, 0.13, 0.42) * half(smoothstep(0.25, 0.85, q.y));
  col += half3(0.36, 0.13, 0.55) * half(smoothstep(0.42, 0.82, n) * 0.9);
  col += half3(0.18, 0.62, 0.95) * half(smoothstep(0.55, 0.95, q.x * n * 1.9) * 0.55);
  col += half3(1.0, 0.72, 0.32) * half(pow(smoothstep(0.5, 0.95, n), 3.0) * 0.95);
  col *= half(0.45 + 0.55 * smoothstep(0.18, 0.62, lanes));
  // A hot core ahead of us while the warp is on.
  col += half3(1.0, 0.82, 0.5) * half(exp(-r * r * 9.0) * (0.15 + 0.7 * warp));
  col += half3(0.78, 0.86, 1.0) * half(streaks(uv, time * 0.03 + p * p * 2.2, warp) * (0.8 + 0.6 * warp));
  col += half3(field(fc, time) * (1.0 - warp));
  col *= half(1.0 - 0.55 * dot(uv, uv));

  // Fade in from the page background.
  col = mix(half3(0.02, 0.027, 0.059), col, half(smoothstep(0.0, 0.1, p)));

  // The reveal: a ragged front from the center out, glowing gold and cyan.
  float rv = smoothstep(${REVEAL}, 1.0, p);
  rv = rv * rv * (3.0 - 2.0 * rv);
  float front = rv * 1.45 - 0.08;
  float dd = r + (n - 0.5) * 0.34 + (lanes - 0.5) * 0.12;
  float keep = smoothstep(front - 0.015, front + 0.07, dd);
  float edge = exp(-abs(dd - front) * 38.0) * step(0.001, rv) * (1.0 - smoothstep(0.85, 1.0, p));
  half3 glow = mix(half3(0.31, 0.82, 1.0), half3(1.0, 0.78, 0.36), half(smoothstep(-0.05, 0.05, n - 0.5))) * half(edge * 1.6);
  return half4(col * half(keep) + glow, half(max(keep, edge * 0.6)));
}`;

// --------------------------------------------------------------- flying UI
// Small, self-contained pieces that look like they came out of real games.

const Bar = ({ value, color, height = 6, track = 'rgba(255,255,255,0.08)' }) => (
  <View style={{ height, borderRadius: height / 2, backgroundColor: track, overflow: 'hidden' }}>
    <View style={{ width: `${value}%`, height: '100%', borderRadius: height / 2, backgroundColor: color, boxShadow: `0 0 8px ${color}` }} />
  </View>
);

function Health() {
  return (
    <View style={[s.panel, { width: 250, flexDirection: 'row', gap: 12, alignItems: 'center' }]}>
      <View style={s.portrait}><Text style={s.portraitText}>V</Text></View>
      <View style={{ flex: 1, gap: 6 }}>
        <Text style={s.label}>VALKYRIE · LV 42</Text>
        <Bar value={78} color={colors.red} height={8} />
        <Bar value={55} color={colors.cyan} height={5} />
      </View>
    </View>
  );
}

function Abilities() {
  const slots = [['Q', colors.cyan, 0], ['E', colors.purple, 0.6], ['R', colors.gold, 0], ['F', colors.green, 0]];
  return (
    <View style={[s.panel, { flexDirection: 'row', gap: 8, padding: 10 }]}>
      {slots.map(([k, c, cd]) => (
        <View key={k} style={[s.slot, { borderColor: c, boxShadow: `0 0 12px ${c}66` }]}>
          <View style={[s.slotGem, { backgroundColor: c }]} />
          {cd ? <View style={[s.cooldown, { height: `${cd * 100}%` }]}><Text style={s.cdText}>3.2</Text></View> : null}
          <Text style={s.slotKey}>{k}</Text>
        </View>
      ))}
    </View>
  );
}

function Loot() {
  const c = rarity.legendary.color;
  return (
    <View style={[s.panel, { width: 200, gap: 8, borderColor: c, boxShadow: `0 0 28px ${rarity.legendary.glow}` }]}>
      <View style={[s.lootArt, { experimental_backgroundImage: `radial-gradient(circle at 50% 40%, ${c}aa 0%, rgba(20,14,4,0.9) 70%)` }]}>
        <View style={s.blade} />
      </View>
      <Text style={[s.eyebrow, { color: c }]}>LEGENDARY</Text>
      <Text style={s.title}>Aetherfang</Text>
      <View style={{ flexDirection: 'row', justifyContent: 'space-between' }}>
        <Text style={s.stat}>DMG <Text style={{ color: colors.text }}>412</Text></Text>
        <Text style={s.stat}>SPD <Text style={{ color: colors.text }}>1.3</Text></Text>
      </View>
    </View>
  );
}

function Minimap() {
  return (
    <View style={s.minimap}>
      <View style={[s.ring, { width: 90, height: 90, borderRadius: 45 }]} />
      <View style={[s.ring, { width: 50, height: 50, borderRadius: 25 }]} />
      <View style={[s.blip, { left: 30, top: 34, backgroundColor: colors.red }]} />
      <View style={[s.blip, { left: 92, top: 50, backgroundColor: colors.red }]} />
      <View style={[s.blip, { left: 74, top: 96, backgroundColor: colors.gold }]} />
      <View style={s.player} />
      <Text style={s.north}>N</Text>
    </View>
  );
}

function Quest() {
  return (
    <View style={[s.panel, { width: 240, gap: 7 }]}>
      <Text style={[s.eyebrow, { color: colors.gold }]}>MAIN QUEST</Text>
      <Text style={s.title}>Seal the Rift</Text>
      {[['Reach the Sunken Gate', true], ['Defeat the Warden  2/3', false]].map(([t, done]) => (
        <View key={t} style={{ flexDirection: 'row', gap: 8, alignItems: 'center' }}>
          <View style={[s.check, done && { backgroundColor: colors.green, borderColor: colors.green }]} />
          <Text style={[s.small, done && { color: colors.textMute, textDecorationLine: 'line-through' }]}>{t}</Text>
        </View>
      ))}
    </View>
  );
}

function Menu() {
  return (
    <View style={{ width: 210, gap: 8 }}>
      {['CONTINUE', 'NEW GAME', 'SETTINGS'].map((l, i) => (
        <View key={l} style={[s.menuBtn, i === 0 && s.menuBtnHot]}>
          <Text style={[s.menuText, i === 0 && { color: '#1a1205' }]}>{l}</Text>
        </View>
      ))}
    </View>
  );
}

function Damage() {
  return (
    <View style={{ alignItems: 'center' }}>
      <Text style={[s.eyebrow, { color: colors.goldLight, fontSize: 14 }]}>CRITICAL!</Text>
      <Text style={s.damage}>-2,481</Text>
    </View>
  );
}

function KillFeed() {
  return (
    <View style={{ gap: 6, width: 230 }}>
      {[['Nyx', 'Orrin', colors.cyan], ['YOU', 'Vex', colors.gold]].map(([a, b, c]) => (
        <View key={a} style={s.feedRow}>
          <Text style={[s.feedName, { color: c }]}>{a}</Text>
          <View style={s.feedIcon} />
          <Text style={[s.feedName, { color: colors.red }]}>{b}</Text>
          {a === 'YOU' ? <Text style={[s.eyebrow, { color: colors.gold, marginLeft: 'auto' }]}>HEADSHOT</Text> : null}
        </View>
      ))}
    </View>
  );
}

function Xp() {
  return (
    <View style={[s.panel, { width: 260, gap: 8 }]}>
      <View style={{ flexDirection: 'row', justifyContent: 'space-between' }}>
        <Text style={s.label}>LEVEL 42</Text>
        <Text style={[s.label, { color: colors.gold }]}>+1,250 XP</Text>
      </View>
      <Bar value={64} color={colors.gold} height={7} />
    </View>
  );
}

function Victory() {
  return (
    <View style={{ alignItems: 'center', gap: 4 }}>
      <View style={s.victoryLine} />
      <Text style={s.victory}>VICTORY</Text>
      <Text style={s.small}>Rank #1 · 14 eliminations</Text>
      <View style={s.victoryLine} />
    </View>
  );
}

function Tier() {
  const c = rarity.epic.color;
  return (
    <View style={[s.panel, { width: 130, alignItems: 'center', gap: 8, borderColor: c, boxShadow: `0 0 24px ${rarity.epic.glow}` }]}>
      <Text style={[s.eyebrow, { color: c }]}>TIER 37</Text>
      <View style={[s.chest, { borderColor: c, experimental_backgroundImage: `linear-gradient(160deg, ${c} 0%, #2a1050 100%)` }]} />
      <Text style={s.small}>Void Wings</Text>
    </View>
  );
}

function Achievement() {
  return (
    <View style={[s.panel, { flexDirection: 'row', gap: 12, alignItems: 'center', width: 260, borderColor: colors.gold + '88' }]}>
      <View style={s.trophy}><View style={s.trophyCup} /></View>
      <View style={{ gap: 2 }}>
        <Text style={[s.eyebrow, { color: colors.gold }]}>ACHIEVEMENT UNLOCKED</Text>
        <Text style={s.title}>Speed Demon</Text>
      </View>
    </View>
  );
}

function Slider() {
  return (
    <View style={[s.panel, { width: 250, gap: 10 }]}>
      <View style={{ flexDirection: 'row', justifyContent: 'space-between' }}>
        <Text style={s.small}>Field of View</Text>
        <Text style={[s.small, { color: colors.cyan }]}>103</Text>
      </View>
      <View>
        <Bar value={68} color={colors.cyan} height={4} />
        <View style={s.knob} />
      </View>
    </View>
  );
}

function Dialogue() {
  return (
    <View style={[s.panel, { width: 280, gap: 4 }]}>
      <Text style={[s.eyebrow, { color: colors.cyan }]}>ELARA</Text>
      <Text style={s.small}>The rift is opening. We have one shot at this. Hurry!</Text>
    </View>
  );
}

const PIECES = [Health, Loot, Abilities, Minimap, Quest, Damage, Menu, Achievement, KillFeed, Tier, Xp, Dialogue, Slider, Victory];

// Each piece flies from the vanishing point past the camera: depth z goes
// from far to behind us, the view scales as 1 / z and moves out along its
// direction as 1 / z. Sampled into an interpolation (native driver).
const SAMPLES = 8;
const Z0 = 5.5;
const Z1 = 0.3;
const FLIGHT = 0.28;  // of the timeline

function flight(p, i, n, reach) {
  const start = 0.05 + (i * (0.38 - 0.05)) / (n - 1);
  const angle = i * 2.39996 + 0.6;  // golden angle: spread around the screen
  const dx = Math.cos(angle);
  const dy = Math.sin(angle) * 0.72;
  const input = [];
  const sc = [];
  const tx = [];
  const ty = [];
  for (let k = 0; k <= SAMPLES; k++) {
    const u = k / SAMPLES;
    const z = Z0 + (Z1 - Z0) * u;
    input.push(start + FLIGHT * u);
    sc.push(1 / z);
    tx.push((dx * reach * (0.35 + 1 / z)) / 1.6);
    ty.push((dy * reach * (0.35 + 1 / z)) / 1.6);
  }
  const range = (outputRange) => p.interpolate({ inputRange: input, outputRange, extrapolate: 'clamp' });
  const tilt = (i % 2 ? 1 : -1) * (6 + (i % 3) * 4);
  return {
    opacity: p.interpolate({
      inputRange: [start, start + FLIGHT * 0.12, start + FLIGHT * 0.7, start + FLIGHT],
      outputRange: [0, 1, 1, 0],
      extrapolate: 'clamp',
    }),
    transform: [
      { translateX: range(tx) },
      { translateY: range(ty) },
      { scale: range(sc) },
      { rotate: p.interpolate({ inputRange: [start, start + FLIGHT], outputRange: [`${tilt}deg`, `${-tilt * 0.4}deg`], extrapolate: 'clamp' }) },
    ],
  };
}

const FlyingUI = React.memo(function FlyingUI({ p, reach }) {
  const styles = useMemo(() => PIECES.map((_, i) => flight(p, i, PIECES.length, reach)), [p, reach]);
  return (
    <View pointerEvents="none" style={s.center}>
      {PIECES.map((Piece, i) => (
        <Animated.View key={i} style={[s.flyer, styles[i]]}>
          <Piece />
        </Animated.View>
      ))}
    </View>
  );
});

function Wordmark({ p, phone }) {
  const style = {
    opacity: p.interpolate({ inputRange: [0.52, 0.62, REVEAL + 0.02, 0.86], outputRange: [0, 1, 1, 0], extrapolate: 'clamp' }),
    transform: [{ scale: p.interpolate({ inputRange: [0.52, 0.66, REVEAL, 0.9], outputRange: [0.82, 1, 1.03, 1.3], extrapolate: 'clamp' }) }],
  };
  return (
    <View pointerEvents="none" style={s.center}>
      <Animated.View style={[{ alignItems: 'center', gap: phone ? 10 : 14 }, style]}>
        <View style={[s.scrim, phone && { width: 420, height: 260, marginLeft: -210, marginTop: -130 }]} />
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: phone ? 14 : 20 }}>
          <View style={[s.mark, phone && { width: 46, height: 46, borderRadius: 13 }]}>
            <View style={[s.markCut, phone && { width: 18, height: 18 }]} />
          </View>
          <Text style={[s.word, phone && { fontSize: 64, letterSpacing: 4 }]}>JSUI</Text>
        </View>
        <Text style={[s.tagline, phone && { fontSize: 11, letterSpacing: 3 }]}>GAME UI · REACT NATIVE · EVERYWHERE</Text>
      </Animated.View>
    </View>
  );
}

// --------------------------------------------------------------- the intro

/**
 * Covers the app until the page says whether to play, then plays (or not)
 * and unmounts. `p` is the timeline, shared with the app so the page can
 * settle into place during the reveal; it ends at 1 either way.
 */
export function Intro({ p }) {
  const [phase, setPhase] = useState('wait');  // wait -> warm -> play -> done
  const { width, height } = useWindowDimensions();
  const ended = useRef(false);
  const answered = useRef(false);

  const finish = () => {
    if (ended.current) return;
    ended.current = true;
    p.setValue(1);
    setPhase('done');
    Host.postMessage('intro', { state: 'done' });
  };

  useHostMessage('intro', (data) => {
    if (phase !== 'wait' || ended.current || answered.current) return;
    answered.current = true;
    if (!data || !data.play) return finish();
    precompileShaders(INTRO_SHADER).then(() => {
      if (ended.current) return;
      const res = compileShader(INTRO_SHADER);
      if (res.error) return finish();
      setPhase('warm');
      // One timeline point per frame (requestAnimationFrame callbacks run
      // before the frame paints).
      let step = 0;
      const next = () => {
        if (ended.current) return;
        if (step < WARM_STEPS) {
          p.setValue(0.04 + (0.9 * step++) / (WARM_STEPS - 1));
          return requestAnimationFrame(next);
        }
        p.setValue(0);
        play(data);
      };
      requestAnimationFrame(next);
    });
  });

  const play = (data) => {
    setPhase('play');
    Host.postMessage('intro', { state: 'play' });
    // ?intro=<p> (a number): hold the timeline there (screenshots, tuning).
    if (typeof data.hold === 'number') return p.setValue(data.hold);
    Animated.timing(p, { toValue: 1, duration: DURATION, easing: Easing.linear, useNativeDriver: true }).start(({ finished }) => finished && finish());
  };

  useEffect(() => {
    const t = setTimeout(() => !answered.current && finish(), WAIT_MS);
    return () => clearTimeout(t);
  }, []);

  // Any input: punch through the rest of the warp and reveal the page.
  const skipping = useRef(false);
  const skip = (e) => {
    if (e && e.preventDefault) e.preventDefault();
    if (phase !== 'play' || skipping.current) return;
    skipping.current = true;
    p.stopAnimation((v) => {
      const left = Math.max(0, 1 - v);
      Animated.timing(p, { toValue: 1, duration: 300 + 650 * left, easing: Easing.inOut(Easing.quad), useNativeDriver: true }).start(() => finish());
    });
  };

  if (phase === 'done') return null;
  const phone = width < 640;
  return (
    <Pressable
      focusable
      autoFocus
      accessibilityLabel="Intro. Press any key to skip."
      onPressIn={skip}
      onKeyDown={skip}
      onGamepadButton={(e) => e.pressed && skip(e)}
      style={[StyleSheet.absoluteFill, phase === 'wait' && { backgroundColor: site.bg }]}
    >
      {phase === 'warm' || phase === 'play' ? (
        <>
          <ShaderView source={INTRO_SHADER} uniforms={{ p }} style={StyleSheet.absoluteFill} />
          <FlyingUI p={p} reach={Math.min(width, height * 1.4) * 0.5} />
          <Wordmark p={p} phone={phone} />
        </>
      ) : null}
    </Pressable>
  );
}

// --------------------------------------------------------------- styles

const s = StyleSheet.create({
  center: { ...StyleSheet.absoluteFillObject, alignItems: 'center', justifyContent: 'center' },
  flyer: { position: 'absolute' },
  panel: {
    padding: 14,
    borderRadius: 12,
    borderWidth: 1,
    borderColor: colors.lineStrong,
    backgroundColor: 'rgba(10, 15, 30, 0.88)',
    boxShadow: '0 16px 40px rgba(0,0,0,0.55), inset 0 1px 0 rgba(255,255,255,0.06)',
  },
  label: { fontFamily: font, fontWeight: '700', fontSize: 12, letterSpacing: 1.2, color: colors.text },
  eyebrow: { fontFamily: font, fontWeight: '700', fontSize: 10, letterSpacing: 2, color: colors.textDim },
  title: { fontFamily: font, fontWeight: '700', fontSize: 17, color: colors.text },
  small: { fontFamily: font, fontWeight: '500', fontSize: 12, lineHeight: 17, color: colors.textDim, flexShrink: 1 },
  stat: { fontFamily: font, fontWeight: '600', fontSize: 11, letterSpacing: 1, color: colors.textMute },
  portrait: { width: 44, height: 44, borderRadius: 22, borderWidth: 2, borderColor: colors.gold, alignItems: 'center', justifyContent: 'center', experimental_backgroundImage: 'linear-gradient(160deg, #3b4a7a 0%, #141a2e 100%)' },
  portraitText: { fontFamily: font, fontWeight: '700', fontSize: 18, color: colors.goldLight },
  slot: { width: 46, height: 46, borderRadius: 10, borderWidth: 1.5, backgroundColor: 'rgba(5,8,18,0.9)', alignItems: 'center', justifyContent: 'center', overflow: 'hidden' },
  slotGem: { width: 16, height: 16, borderRadius: 3, transform: [{ rotate: '45deg' }] },
  cooldown: { position: 'absolute', left: 0, right: 0, bottom: 0, backgroundColor: 'rgba(0,0,0,0.65)', alignItems: 'center', justifyContent: 'center' },
  cdText: { fontFamily: font, fontWeight: '700', fontSize: 12, color: colors.text },
  slotKey: { position: 'absolute', right: 4, bottom: 2, fontFamily: font, fontWeight: '700', fontSize: 9, color: colors.textDim },
  lootArt: { height: 92, borderRadius: 8, alignItems: 'center', justifyContent: 'center' },
  blade: { width: 8, height: 70, borderRadius: 4, backgroundColor: colors.goldLight, transform: [{ rotate: '35deg' }], boxShadow: '0 0 18px rgba(255,226,154,0.9)' },
  minimap: { width: 130, height: 130, borderRadius: 65, borderWidth: 2, borderColor: colors.lineStrong, backgroundColor: 'rgba(8,20,30,0.85)', alignItems: 'center', justifyContent: 'center', boxShadow: '0 0 30px rgba(79,209,255,0.25)' },
  ring: { position: 'absolute', borderWidth: 1, borderColor: 'rgba(79,209,255,0.25)' },
  blip: { position: 'absolute', width: 8, height: 8, borderRadius: 4 },
  player: { width: 0, height: 0, borderLeftWidth: 7, borderRightWidth: 7, borderBottomWidth: 14, borderLeftColor: 'transparent', borderRightColor: 'transparent', borderBottomColor: colors.cyan },
  north: { position: 'absolute', top: 6, fontFamily: font, fontWeight: '700', fontSize: 11, color: colors.gold },
  check: { width: 12, height: 12, borderRadius: 3, borderWidth: 1.5, borderColor: colors.textDim },
  menuBtn: { height: 44, borderRadius: 8, borderWidth: 1, borderColor: colors.lineStrong, backgroundColor: 'rgba(10,15,30,0.85)', justifyContent: 'center', paddingHorizontal: 18 },
  menuBtnHot: { borderColor: colors.goldLight, experimental_backgroundImage: 'linear-gradient(180deg, #ffe29a 0%, #f2c35b 55%, #d99a32 100%)', boxShadow: '0 0 26px rgba(242,195,91,0.55)' },
  menuText: { fontFamily: font, fontWeight: '700', fontSize: 14, letterSpacing: 2.4, color: colors.text },
  damage: { fontFamily: font, fontWeight: '700', fontSize: 54, letterSpacing: -1, color: colors.orange, textShadowColor: 'rgba(255,120,40,0.85)', textShadowRadius: 18 },
  feedRow: { flexDirection: 'row', alignItems: 'center', gap: 10, paddingHorizontal: 12, height: 34, borderRadius: 6, backgroundColor: 'rgba(5,8,18,0.8)', borderLeftWidth: 3, borderLeftColor: colors.gold },
  feedName: { fontFamily: font, fontWeight: '700', fontSize: 13 },
  feedIcon: { width: 14, height: 4, borderRadius: 2, backgroundColor: colors.text },
  victory: { fontFamily: font, fontWeight: '700', fontSize: 56, letterSpacing: 10, color: colors.goldLight, textShadowColor: 'rgba(242,195,91,0.9)', textShadowRadius: 24 },
  victoryLine: { width: 320, height: 2, experimental_backgroundImage: 'linear-gradient(90deg, rgba(242,195,91,0) 0%, #f2c35b 50%, rgba(242,195,91,0) 100%)' },
  chest: { width: 64, height: 52, borderRadius: 8, borderWidth: 1.5 },
  trophy: { width: 44, height: 44, borderRadius: 22, backgroundColor: 'rgba(242,195,91,0.18)', borderWidth: 1.5, borderColor: colors.gold, alignItems: 'center', justifyContent: 'center' },
  trophyCup: { width: 18, height: 16, borderBottomLeftRadius: 9, borderBottomRightRadius: 9, backgroundColor: colors.gold },
  knob: { position: 'absolute', left: '68%', top: -6, marginLeft: -8, width: 16, height: 16, borderRadius: 8, backgroundColor: colors.text, boxShadow: '0 0 10px rgba(79,209,255,0.9)' },
  scrim: {
    position: 'absolute',
    left: '50%',
    top: '50%',
    width: 900,
    height: 420,
    marginLeft: -450,
    marginTop: -210,
    experimental_backgroundImage: 'radial-gradient(ellipse at 50% 50%, rgba(5,7,15,0.62) 0%, rgba(5,7,15,0.35) 45%, rgba(5,7,15,0) 70%)',
  },
  mark: { width: 64, height: 64, borderRadius: 18, alignItems: 'center', justifyContent: 'center', experimental_backgroundImage: 'linear-gradient(135deg, #ffe29a 0%, #f2c35b 45%, #4fd1ff 100%)', boxShadow: '0 0 40px rgba(242,195,91,0.55)' },
  markCut: { width: 26, height: 26, borderRadius: 6, backgroundColor: site.bg, transform: [{ rotate: '45deg' }] },
  word: { fontFamily: font, fontWeight: '700', fontSize: 104, letterSpacing: 8, color: '#fff7e0', textShadowColor: 'rgba(242,195,91,0.75)', textShadowRadius: 30 },
  tagline: { fontFamily: font, fontWeight: '700', fontSize: 14, letterSpacing: 5, color: colors.textDim },
});
