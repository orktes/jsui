// /use-cases: the three jobs JSUI is built for, each with a live example:
// embedded game UI (the combat HUD screen), social UI for games (a friends /
// party / chat panel you can use) and standalone apps (a launcher window).
import React, { useRef } from 'react';
import { Pressable, StyleSheet, Text, View } from 'react-native';
import { colors, font, mono, site, text } from '../theme.js';
import { Button, Column, Eyebrow, Pill, Reveal, Stage, useLayout } from '../components/ui.jsx';
import { screenById } from '../screens/index.js';
import { usePageScroll } from '../router.js';
import { SocialPanel } from './usecases/SocialPanel.jsx';
import { LauncherApp } from './usecases/LauncherApp.jsx';

const Hud = screenById('hud').component;

const CASES = [
  {
    id: 'embedded',
    eyebrow: 'Embedded UIs for games',
    accent: colors.gold,
    title: 'The HUD and menus, inside your engine.',
    lead: 'Your engine draws the world; JSUI draws the UI on top of it, into the engine’s own GPU target, every frame. Write HUDs, menus, inventories and settings in React Native and keep the game loop in charge.',
    points: [
      ['Draws into your frame', 'Skia on the engine’s Metal, Vulkan, D3D12 or GL device: no readback, no extra copy.'],
      ['Input passes through', 'Clicks, keys and pad buttons the UI does not use go straight back to the game.'],
      ['Smooth while the game is busy', 'Animations run natively and React runs on its own thread, so a heavy frame never stutters the HUD.'],
      ['Gamepad-first', 'Spatial D-pad focus, focus scopes and focus-visible rings, built into the runtime.'],
    ],
    hosts: ['Godot', 'Unity', 'Unreal', 'raylib', 'SDL3', 'sokol', 'C API'],
    actions: [
      { label: 'Play the HUD full screen', to: '/examples/hud/full', icon: '▶' },
      { label: 'Embedding guide', to: '/embed', kind: 'ghost' },
    ],
  },
  {
    id: 'social',
    eyebrow: 'Social UIs for games',
    accent: colors.cyan,
    title: 'Friends, party and chat, shared across every game.',
    lead: 'The social layer is the same in every title a studio ships: friends, parties, chat, invites, store and news. Build it once in React Native and drop it into each game, whatever the engine, and onto the web.',
    points: [
      ['One codebase, every engine', 'The same bundle runs in Godot, Unity, Unreal, your own engine and the browser.'],
      ['Wired to your backend', 'Host.postMessage and jsui_post_message carry presence, invites and chat both ways.'],
      ['Ship it as a bundle', 'The UI is a JS bundle the game loads, so it can be updated without rebuilding the engine.'],
      ['Real text', 'A native TextInput with selection, clipboard and undo; HarfBuzz shaping, RTL and fallback fonts.'],
    ],
    hosts: ['Overlays', 'Party & invites', 'Chat', 'Store', 'Companion web'],
    actions: [
      { label: 'Notifications example', to: '/examples/notifications', icon: '▶' },
      { label: 'Scoreboard example', to: '/examples/scoreboard', kind: 'ghost' },
    ],
  },
  {
    id: 'apps',
    eyebrow: 'Standalone apps',
    accent: colors.purple,
    title: 'Launchers, tools and companion apps, no engine needed.',
    lead: 'JSUI also runs on its own: a native desktop window, an iOS or Android app, or a web page. Small, GPU drawn, and with the same components as the in-game UI, so a launcher looks like the game it launches.',
    points: [
      ['Desktop', 'jsui-viewer opens a native window: Skia Graphite on Metal on macOS, OpenGL elsewhere, the UI right up under the title bar.'],
      ['Mobile', 'An iOS xcframework on Metal and an Android library on GLES, with touch and vsync-timed scrolling.'],
      ['The web', 'jsui build --web exports a static site on WebGPU, WebGL or CPU. This website is one.'],
      ['Agent tools', 'Headless jsui screenshot and inspect render the app with no window, for tests and coding agents.'],
    ],
    hosts: ['macOS', 'Windows', 'Linux', 'iOS', 'Android', 'Web'],
    actions: [
      { label: 'Try the playground', to: '/playground', icon: '▶' },
      { label: 'Embedding guide', to: '/embed', kind: 'ghost' },
    ],
  },
];

export function UseCasesPage() {
  const L = useLayout();
  const inner = Math.min(L.width - L.gutter * 2, 1240);
  const wide = !L.tablet;
  const copyW = wide ? 440 : inner;
  const visW = wide ? inner - copyW - 56 : inner;
  // Where each case starts in the page (onLayout), for the index at the top.
  const scroll = usePageScroll();
  const tops = useRef({}).current;
  return (
    <View style={{ paddingBottom: 40 }}>
      <Column style={{ paddingTop: L.phone ? 32 : 60 }}>
        <Reveal>
          <View style={{ gap: 12, maxWidth: 820 }}>
            <Eyebrow>Use cases</Eyebrow>
            <Text style={[L.phone ? text.h2 : text.h1]}>One UI runtime. Three jobs.</Text>
            <Text style={[text.lead, L.phone && { fontSize: 16, lineHeight: 25 }]}>
              JSUI is React Native for places a browser does not fit: inside a game engine, as the social layer shared by a studio’s games, and as a
              small standalone app. Everything below is live.
            </Text>
          </View>
        </Reveal>
        <Reveal delay={100} style={{ marginTop: L.phone ? 22 : 32 }}>
          <View style={[styles.index, L.phone && { flexDirection: 'column' }]}>
            {CASES.map((c, i) => (
              <Pressable
                key={c.id}
                onPress={() => tops[c.id] != null && scroll.to(tops[c.id] - 24)}
                accessibilityRole="link"
                group
                style={[styles.indexItem, { borderColor: c.accent + '40' }]}
                hoveredStyle={{ borderColor: c.accent + 'aa', backgroundColor: 'rgba(22, 30, 54, 0.92)' }}
                focusVisibleStyle={{ borderColor: c.accent }}
                pressedStyle={{ transform: [{ scale: 0.98 }] }}
                transition={{ duration: 130, pressed: 40 }}
              >
                <Text style={[styles.indexNum, { color: c.accent }]}>{String(i + 1).padStart(2, '0')}</Text>
                <Text style={styles.indexTitle}>{c.eyebrow}</Text>
                <Text style={[styles.indexArrow, { color: c.accent }]} groupHoveredStyle={{ transform: [{ translateY: 3 }] }} transition={140}>
                  ↓
                </Text>
              </Pressable>
            ))}
          </View>
        </Reveal>
      </Column>
      {CASES.map((c, i) => (
        <Column key={c.id} style={{ marginTop: L.phone ? 64 : 112 }} onLayout={(e) => (tops[c.id] = e.nativeEvent.layout.y)}>
          <Case c={c} index={i} wide={wide} copyW={copyW} visW={visW} flip={wide && i % 2 === 1} />
        </Column>
      ))}
    </View>
  );
}

function Case({ c, index, wide, copyW, visW, flip }) {
  const { phone } = useLayout();
  const visual =
    c.id === 'embedded' ? <EmbeddedVisual width={visW} accent={c.accent} /> : c.id === 'social' ? <SocialPanel width={visW} /> : <LauncherApp width={visW} />;
  return (
    <View style={[styles.case, !wide && { flexDirection: 'column', alignItems: 'stretch' }, flip && { flexDirection: 'row-reverse' }]}>
      <Reveal style={{ width: copyW, gap: 16 }}>
        <View style={styles.caseHead}>
          <Text style={[styles.caseNum, { color: c.accent, borderColor: c.accent + '55', backgroundColor: c.accent + '14' }]}>{String(index + 1).padStart(2, '0')}</Text>
          <Eyebrow color={c.accent}>{c.eyebrow}</Eyebrow>
        </View>
        <Text style={[phone ? text.h2 : text.h1, !phone && { fontSize: 34, lineHeight: 40 }]}>{c.title}</Text>
        <Text style={[text.body, { fontSize: 16, lineHeight: 26 }]}>{c.lead}</Text>
        <View style={{ gap: 12, marginTop: 4 }}>
          {c.points.map(([t, b]) => (
            <View key={t} style={styles.point}>
              <View style={[styles.bullet, { backgroundColor: c.accent }]} />
              <View style={{ flex: 1, gap: 2 }}>
                <Text style={styles.pointTitle}>{t}</Text>
                <Text style={styles.pointBody}>{b}</Text>
              </View>
            </View>
          ))}
        </View>
        <View style={styles.hosts}>
          {c.hosts.map((h) => (
            <Pill key={h} color={c.accent}>
              {h}
            </Pill>
          ))}
        </View>
        <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 12, marginTop: 6 }}>
          {c.actions.map((a) => (
            <Button key={a.label} small={phone} {...a} />
          ))}
        </View>
      </Reveal>
      <Reveal delay={120} distance={26} style={{ width: visW }}>
        {visual}
      </Reveal>
    </View>
  );
}

// The combat HUD screen, live, framed as the layer over the game's own frame.
function EmbeddedVisual({ width, accent }) {
  return (
    <View style={{ gap: 12 }}>
      <View>
        <View pointerEvents="none" style={styles.hudGlow} />
        <Stage width={width}>
          <Hud active />
        </Stage>
        <View pointerEvents="none" style={styles.layerTag}>
          <View style={[styles.layerDot, { backgroundColor: accent }]} />
          <Text style={styles.layerText}>JSUI layer · live</Text>
        </View>
      </View>
      <View style={styles.stack}>
        <Layer label="Your engine" sub="world, 3D, post" color={colors.textMute} />
        <Text style={styles.plus}>+</Text>
        <Layer label="JSUI" sub="HUD, menus, text, input" color={accent} />
        <Text style={styles.plus}>=</Text>
        <Layer label="One frame" sub="one GPU target" color={colors.green} />
      </View>
    </View>
  );
}

function Layer({ label, sub, color }) {
  return (
    <View style={[styles.layer, { borderColor: color + '55' }]}>
      <Text style={[styles.layerLabel, { color }]}>{label}</Text>
      <Text style={styles.layerSub} numberOfLines={1}>
        {sub}
      </Text>
    </View>
  );
}

const styles = StyleSheet.create({
  index: { flexDirection: 'row', gap: 12 },
  indexItem: { flex: 1, cursor: 'pointer', flexDirection: 'row', alignItems: 'center', gap: 12, paddingHorizontal: 16, height: 52, borderRadius: 14, borderWidth: 1, backgroundColor: site.card },
  indexNum: { fontFamily: mono, fontWeight: '700', fontSize: 13 },
  indexTitle: { flex: 1, fontFamily: font, fontWeight: '600', fontSize: 15, color: colors.text },
  indexArrow: { fontFamily: font, fontWeight: '700', fontSize: 16 },
  case: { flexDirection: 'row', alignItems: 'center', gap: 56 },
  caseHead: { flexDirection: 'row', alignItems: 'center', gap: 12 },
  caseNum: { fontFamily: mono, fontWeight: '700', fontSize: 12, paddingHorizontal: 8, paddingVertical: 4, borderRadius: 8, borderWidth: 1, overflow: 'hidden' },
  point: { flexDirection: 'row', gap: 12, alignItems: 'flex-start' },
  bullet: { width: 6, height: 6, borderRadius: 1, marginTop: 8, transform: [{ rotate: '45deg' }] },
  pointTitle: { fontFamily: font, fontWeight: '700', fontSize: 14.5, color: colors.text },
  pointBody: { fontFamily: font, fontSize: 14, lineHeight: 21, color: colors.textDim },
  hosts: { flexDirection: 'row', flexWrap: 'wrap', gap: 8, marginTop: 4 },
  hudGlow: {
    position: 'absolute',
    left: -50,
    right: -50,
    top: -50,
    bottom: -50,
    experimental_backgroundImage: 'radial-gradient(ellipse at 50% 50%, rgba(255,77,106,0.16) 0%, rgba(242,195,91,0.08) 40%, rgba(5,7,15,0) 70%)',
  },
  layerTag: {
    position: 'absolute',
    left: 14,
    top: -13,
    flexDirection: 'row',
    alignItems: 'center',
    gap: 8,
    paddingHorizontal: 11,
    height: 26,
    borderRadius: 13,
    backgroundColor: 'rgba(5,7,15,0.94)',
    borderWidth: 1,
    borderColor: site.strokeStrong,
  },
  layerDot: { width: 7, height: 7, borderRadius: 4 },
  layerText: { fontFamily: font, fontWeight: '700', fontSize: 12, color: colors.text },
  stack: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  layer: { flex: 1, paddingHorizontal: 12, paddingVertical: 9, borderRadius: 12, borderWidth: 1, backgroundColor: 'rgba(255,255,255,0.02)', gap: 2 },
  layerLabel: { fontFamily: font, fontWeight: '700', fontSize: 13 },
  layerSub: { fontFamily: font, fontSize: 11.5, color: colors.textMute },
  plus: { fontFamily: font, fontWeight: '700', fontSize: 16, color: colors.textMute },
});
