// /: the landing page.
import React, { useEffect, useRef, useState } from 'react';
import { Animated, Easing, StyleSheet, Text, View, bridgeStats } from 'react-native';
import { colors, font, mono, site, text } from '../theme.js';
import { Button, Card, Column, Eyebrow, Key, Pill, Reveal, SectionHead, Stage, useLayout } from '../components/ui.jsx';
import { SCREENS } from '../screens/index.js';
import { Features } from './home/Features.jsx';
import { Pipeline } from './home/Pipeline.jsx';
import { CodeTabs } from './home/CodeTabs.jsx';
import { Highlights, SkillInstall } from './home/Highlights.jsx';
import { RunsIn } from './home/RunsIn.jsx';

const HeroScreen = SCREENS[0].component;

/** Live count of host nodes React has mounted on this page right now. */
function useNodeCount() {
  const [n, setN] = useState(() => bridgeStats().nodes);
  useEffect(() => {
    const id = setInterval(() => setN(bridgeStats().nodes), 1500);
    return () => clearInterval(id);
  }, []);
  return n;
}

function LiveBadge({ label = 'Live' }) {
  const p = useRef(new Animated.Value(0)).current;
  useEffect(() => {
    const loop = Animated.loop(
      Animated.sequence([
        Animated.timing(p, { toValue: 1, duration: 900, easing: Easing.inOut(Easing.sin), useNativeDriver: true }),
        Animated.timing(p, { toValue: 0, duration: 900, easing: Easing.inOut(Easing.sin), useNativeDriver: true }),
      ]),
    );
    loop.start();
    return () => loop.stop();
  }, [p]);
  return (
    <View style={styles.badge}>
      <Animated.View style={[styles.badgeDot, { opacity: p.interpolate({ inputRange: [0, 1], outputRange: [0.4, 1] }), transform: [{ scale: p.interpolate({ inputRange: [0, 1], outputRange: [0.8, 1.2] }) }] }]} />
      <Text style={styles.badgeText}>{label}</Text>
    </View>
  );
}

function HeroVisual({ width }) {
  const float = useRef(new Animated.Value(0)).current;
  useEffect(() => {
    const loop = Animated.loop(
      Animated.sequence([
        Animated.timing(float, { toValue: 1, duration: 3200, easing: Easing.inOut(Easing.sin), useNativeDriver: true }),
        Animated.timing(float, { toValue: 0, duration: 3200, easing: Easing.inOut(Easing.sin), useNativeDriver: true }),
      ]),
    );
    loop.start();
    return () => loop.stop();
  }, [float]);
  return (
    <View style={{ width }}>
      <Animated.View
        pointerEvents="none"
        style={[styles.heroGlow, { opacity: float.interpolate({ inputRange: [0, 1], outputRange: [0.55, 0.95] }) }]}
      />
      <Stage width={width}>
        <HeroScreen active />
      </Stage>
      <View style={styles.heroTag} pointerEvents="none">
        <LiveBadge label="Live game screen · not a video" />
      </View>
      <Animated.View
        pointerEvents="none"
        style={[styles.floatChip, { right: -8, bottom: -18, transform: [{ translateY: float.interpolate({ inputRange: [0, 1], outputRange: [0, -8] }) }] }]}
      >
        <Text style={styles.floatMono}>{'<View style={styles.menu}>'}</Text>
      </Animated.View>
    </View>
  );
}

function Stat({ value, label, color = colors.text }) {
  return (
    <View style={styles.stat}>
      <Text style={[styles.statValue, { color }]}>{value}</Text>
      <Text style={styles.statLabel}>{label}</Text>
    </View>
  );
}

function ThisPage() {
  const nodes = useNodeCount();
  const { phone, tablet } = useLayout();
  return (
    <Card style={[styles.thisPage, tablet && { flexDirection: 'column' }, phone && { padding: 20 }]} glow="rgba(242,195,91,0.35)">
      <View pointerEvents="none" style={styles.thisGlow} />
      <View style={{ flex: tablet ? 0 : 1.1, gap: 14 }}>
        <Eyebrow>You are looking at it</Eyebrow>
        <Text style={[phone ? text.h2 : text.h1, { maxWidth: 620 }]}>This page is not HTML. It is JSUI, in one canvas.</Text>
        <Text style={[text.body, { maxWidth: 620 }]}>
          Every word, gradient, shadow and focus ring on this site is drawn by the WebAssembly build of JSUI into a single {'<canvas>'}. The site is an ordinary React Native app: QuickJS runs it, Yoga lays it out, Skia paints it. The same runtime ships inside a game as a static library.
        </Text>
        <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 8, alignItems: 'center' }}>
          <Text style={text.small}>Try</Text>
          <Key>Tab</Key>
          <Text style={text.small}>or a gamepad D-pad to move focus,</Text>
          <Key>Space</Key>
          <Text style={text.small}>to scroll.</Text>
        </View>
      </View>
      <View style={[styles.stats, { flex: tablet ? 0 : 1 }]}>
        <Stat value="1" label="<canvas> element" color={colors.goldLight} />
        <Stat value="0" label="DOM nodes in the UI" color={colors.cyan} />
        <Stat value={String(nodes)} label="JSUI nodes live on this page" color={colors.green} />
        <Stat value="≈9 MB" label="runtime as .wasm (CPU build)" color={colors.purple} />
      </View>
    </Card>
  );
}

export function HomePage() {
  const { width, gutter, phone, tablet } = useLayout();
  const inner = Math.min(width - gutter * 2, 1240);
  const stackHero = width < 1100;
  // Side by side: the hero copy column keeps room for two columns of highlights.
  const heroW = stackHero ? inner : Math.min(Math.round(inner * 0.6), inner - 448 - 48);
  const copyW = stackHero ? inner : inner - heroW - 48;  // the hero copy column
  return (
    <View>
      <Column style={{ paddingTop: phone ? 32 : 60 }}>
        <Reveal>
          <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 8 }}>
            <Pill color={colors.gold}>QuickJS + Yoga + Skia</Pill>
            <Pill color={colors.cyan}>C API · native + WASM</Pill>
          </View>
        </Reveal>
        <Reveal delay={60} style={{ marginTop: phone ? 16 : 22 }}>
          <Text style={[text.hero, { fontSize: 72, lineHeight: 78, letterSpacing: -2 }, tablet && { fontSize: 56, lineHeight: 62, letterSpacing: -1.4 }, phone && { fontSize: 40, lineHeight: 45, letterSpacing: -1 }]}>
            Game UI in React Native.{'\n'}
            <Text style={{ color: colors.gold }}>Rendered everywhere.</Text>
          </Text>
        </Reveal>
        <View style={[styles.hero, stackHero && { flexDirection: 'column', alignItems: 'stretch' }, { marginTop: phone ? 18 : 32 }]}>
          <View style={[{ flex: 1, gap: phone ? 18 : 24 }]}>
            <Reveal delay={120}>
              <Text style={[text.lead, phone && { fontSize: 16, lineHeight: 25 }]}>
                JSUI is an embeddable UI runtime. Write ordinary React Native, then ship it inside your engine through a small C API: Godot, your own C++ renderer, or the browser via WebAssembly. CPU or GPU renderer, about 9 MB.
              </Text>
            </Reveal>
            <Reveal delay={180}>
              <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 12 }}>
                <Button label="Explore the examples" to="/examples" icon="▶" />
                <Button label="Open the playground" kind="ghost" to="/playground" />
              </View>
            </Reveal>
            <View style={{ gap: 10 }}>
              <Highlights width={copyW} delay={220} />
              <Reveal delay={300} style={{ maxWidth: 520 }}>
                <SkillInstall />
              </Reveal>
            </View>
            <Reveal delay={340}>
              <View style={styles.proof}>
                <View style={styles.proofDot} />
                <Text style={styles.proofText}>This entire page is rendered by JSUI's WASM build into one canvas. No DOM, no CSS.</Text>
              </View>
            </Reveal>
          </View>
          <View style={[{ width: heroW, gap: phone ? 28 : 44 }, stackHero && { marginTop: 12 }]}>
            <Reveal delay={200} distance={28}>
              <HeroVisual width={heroW} />
            </Reveal>
            <Reveal delay={380}>
              <RunsIn />
            </Reveal>
          </View>
        </View>
      </Column>

      <Column style={{ marginTop: phone ? 72 : 120 }}>
        <Reveal>
          <ThisPage />
        </Reveal>
      </Column>

      <Column style={{ marginTop: phone ? 72 : 120, gap: phone ? 24 : 36 }}>
        <SectionHead
          eyebrow="Why JSUI"
          title="Everything a game UI needs. Nothing a browser drags along."
          lead="A browser-shaped stack (JS, layout, 2D paint, resources, input) as one static library. You keep React Native's API and tooling; your game keeps control of the frame."
        />
        <Features />
      </Column>

      <Column style={{ marginTop: phone ? 72 : 120, gap: phone ? 24 : 36 }}>
        <SectionHead
          eyebrow="How it works"
          title="From JSX to a texture in your engine"
          lead="@orktes/jsui-cli bundles your app with esbuild, aliasing react-native to @orktes/jsui-react. Inside the runtime QuickJS runs React, the reconciler drives a native node tree, Yoga lays it out and Skia paints it into pixels you own."
        />
        <Reveal>
          <Pipeline />
        </Reveal>
      </Column>

      <Column style={{ marginTop: phone ? 72 : 120 }}>
        {/* Stacked: stretch both halves to the column width, or the code block would overflow the
            screen at its natural width. */}
        <View style={[styles.codeSplit, tablet && { flexDirection: 'column', alignItems: 'stretch' }]}>
          <View style={{ flex: tablet ? 0 : 0.85, gap: 18 }}>
            <SectionHead eyebrow="Write it once" title="It is just React Native" lead="Same components, same StyleSheet, same Pressable states. The host side is a dozen lines: feed input, tick, take pixels." />
            <View style={{ gap: 10 }}>
              {[
                ['Focus, hover and press states', 'Pressable style functions receive { focused, hovered, pressed }.'],
                ['autoFocus and spatial navigation', 'The D-pad finds the nearest focusable neighbour; no focus maps to maintain.'],
                ['Messages both ways', 'Host.postMessage from JS, jsui_post_message from your engine.'],
              ].map(([t, b]) => (
                <View key={t} style={styles.point}>
                  <Text style={styles.pointCheck}>✓</Text>
                  <View style={{ flex: 1 }}>
                    <Text style={[text.h3, { fontSize: 15 }]}>{t}</Text>
                    <Text style={[text.body, { fontSize: 14 }]}>{b}</Text>
                  </View>
                </View>
              ))}
            </View>
          </View>
          <View style={{ flex: tablet ? 0 : 1.15 }}>
            <CodeTabs />
          </View>
        </View>
      </Column>

      <Column style={{ marginTop: phone ? 72 : 120 }}>
        <Reveal>
          <View style={[styles.cta, phone && { padding: 24 }]}>
            <View pointerEvents="none" style={styles.ctaGlow} />
            <Eyebrow style={{ textAlign: 'center' }}>Keep exploring</Eyebrow>
            <Text style={[phone ? text.h2 : text.h1, { textAlign: 'center', maxWidth: 720 }]}>Poke at it. Everything here is live.</Text>
            <Text style={[text.lead, { textAlign: 'center', maxWidth: 640 }, phone && { fontSize: 16, lineHeight: 25 }]}>
              Twelve interactive game screens, the use cases JSUI is built for, a live code playground and a guide to embedding the runtime in your engine.
            </Text>
            <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 12, justifyContent: 'center', marginTop: 8 }}>
              <Button label="Example screens" to="/examples" icon="▶" />
              <Button label="Use cases" kind="ghost" to="/use-cases" />
              <Button label="Playground" kind="ghost" to="/playground" />
              <Button label="Embedding guide" kind="ghost" to="/embed" />
            </View>
          </View>
        </Reveal>
      </Column>
    </View>
  );
}

const styles = StyleSheet.create({
  hero: { flexDirection: 'row', alignItems: 'flex-start', gap: 48 },
  heroGlow: {
    position: 'absolute',
    left: -60,
    right: -60,
    top: -60,
    bottom: -60,
    experimental_backgroundImage: 'radial-gradient(ellipse at 50% 50%, rgba(242,195,91,0.20) 0%, rgba(79,140,255,0.10) 40%, rgba(5,7,15,0) 70%)',
  },
  heroTag: { position: 'absolute', left: 14, top: -14 },
  badge: { flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 12, height: 28, borderRadius: 14, backgroundColor: 'rgba(5,7,15,0.92)', borderWidth: 1, borderColor: 'rgba(74,222,128,0.45)' },
  badgeDot: { width: 8, height: 8, borderRadius: 4, backgroundColor: colors.green, boxShadow: '0 0 10px rgba(74,222,128,0.9)' },
  badgeText: { fontFamily: font, fontWeight: '700', fontSize: 12, letterSpacing: 0.4, color: colors.text },
  floatChip: { position: 'absolute', paddingHorizontal: 12, paddingVertical: 8, borderRadius: 10, backgroundColor: 'rgba(11,16,32,0.95)', borderWidth: 1, borderColor: site.strokeStrong, boxShadow: '0 12px 30px rgba(0,0,0,0.6)' },
  floatMono: { fontFamily: mono, fontSize: 12, color: colors.cyan },
  proof: { flexDirection: 'row', alignItems: 'center', gap: 10 },
  proofDot: { width: 6, height: 6, borderRadius: 3, backgroundColor: colors.gold, boxShadow: '0 0 8px rgba(242,195,91,0.9)' },
  proofText: { fontFamily: font, fontWeight: '500', fontSize: 14, color: colors.textDim, flexShrink: 1 },
  thisPage: { flexDirection: 'row', gap: 36, padding: 36, overflow: 'hidden' },
  thisGlow: { ...StyleSheet.absoluteFillObject, experimental_backgroundImage: 'radial-gradient(ellipse at 100% 0%, rgba(242,195,91,0.12) 0%, rgba(5,7,15,0) 55%)' },
  stats: { flexDirection: 'row', flexWrap: 'wrap', gap: 12, alignContent: 'center' },
  stat: { flexGrow: 1, flexBasis: 150, padding: 18, gap: 4, borderRadius: 14, borderWidth: 1, borderColor: site.stroke, backgroundColor: 'rgba(5,8,18,0.6)' },
  statValue: { fontFamily: font, fontWeight: '700', fontSize: 34, letterSpacing: -0.8 },
  statLabel: { fontFamily: font, fontWeight: '500', fontSize: 13, color: colors.textMute },
  codeSplit: { flexDirection: 'row', gap: 48, alignItems: 'flex-start' },
  point: { flexDirection: 'row', gap: 12, alignItems: 'flex-start' },
  pointCheck: { fontFamily: font, fontWeight: '700', fontSize: 15, color: colors.green, marginTop: 1 },
  cta: { alignItems: 'center', gap: 14, padding: 56, borderRadius: 24, borderWidth: 1, borderColor: site.strokeStrong, backgroundColor: site.bgRaised, overflow: 'hidden' },
  ctaGlow: { ...StyleSheet.absoluteFillObject, experimental_backgroundImage: 'radial-gradient(ellipse at 50% 120%, rgba(242,195,91,0.22) 0%, rgba(79,140,255,0.08) 45%, rgba(5,7,15,0) 75%)' },
});
