// Feature grid: each card is a link into the page that demonstrates it (`to`)
// or to its docs (`href`), with a spring lift on hover / focus (works with
// mouse, keyboard and gamepad).
import React, { useRef } from 'react';
import { Animated, Linking, Pressable, StyleSheet, Text, View } from 'react-native';
import { colors, font, mono, site, text } from '../../theme.js';
import { useRouter } from '../../router.js';
import { Reveal, useLayout } from '../../components/ui.jsx';

export const FEATURES = [
  { glyph: '</>', title: 'React Native API', body: "View, Text, Image, ScrollView, FlatList, TextInput, Modal, Pressable, StyleSheet. Import from 'react-native' and it just works.", color: colors.cyan, to: '/playground', cta: 'Try it live' },
  { glyph: 'A', title: 'Gamepad-first focus', body: 'Spatial D-pad and stick navigation, focus scopes for modals, focus-visible rings, Tab order and hold-to-repeat, built into the runtime.', color: colors.gold, to: '/features/input', cta: 'Grab a controller' },
  { glyph: '◆', title: 'Native Animated driver', body: 'useNativeDriver: true moves springs, timings, decays and interpolations into C++. They keep running while your JS is busy.', color: colors.purple, to: '/features/motion', cta: 'Open the motion lab' },
  { glyph: '✓', title: 'Chrome-exact rendering', body: 'Every example screen is pixel-compared with the same code rendered by react-native-web in headless Chrome, in CI.', color: colors.green, to: '/examples', cta: 'See the examples' },
  { glyph: '▲', title: 'Incremental repaint', body: 'Only damaged rectangles are repainted on the CPU path; a blinking caret costs a few pixels. Or draw straight into your GL target.', color: colors.orange, to: '/embed', cta: 'Rendering options' },
  { glyph: 'F5', title: 'Dev mode live reload', body: 'Point any host at jsui dev and the bundle reloads on save, keeping the last good frame on screen and an error overlay on top.', color: colors.blue, to: '/embed', cta: 'Set up a host' },
  { glyph: 'Aa', title: 'Text editing and i18n', body: 'skparagraph shaping with HarfBuzz and ICU, bidi text, a native TextInput with selection, word jumps, clipboard and undo.', color: colors.red, to: '/features/interactions', cta: 'Type something' },
  { glyph: '≡', title: 'Virtualized lists', body: "FlatList and SectionList window their cells like React Native, keep the focused row mounted and glide it into view.", color: colors.goldLight, to: '/features/interactions', cta: 'Fling a list' },
];

function FeatureCard({ f, width, delay }) {
  const { go } = useRouter();
  const lift = useRef(new Animated.Value(0)).current;
  const to = (v) => Animated.spring(lift, { toValue: v, stiffness: 320, damping: 20, useNativeDriver: true }).start();
  return (
    <Reveal delay={delay} style={{ width }}>
      <Pressable
        onPress={() => (f.href ? Linking.openURL(f.href) : go(f.to))}
        onHoverIn={() => to(1)}
        onHoverOut={() => to(0)}
        onFocus={() => to(1)}
        onBlur={() => to(0)}
        style={{ flex: 1, cursor: 'pointer' }}
      >
        {({ hovered, focused, pressed }) => (
          <Animated.View
            style={[
              styles.card,
              (hovered || focused) && { borderColor: f.color + '77' },
              focused && { boxShadow: `0 0 0 2px ${site.bg}, 0 0 0 4px ${colors.cyan}, 0 24px 60px rgba(0,0,0,0.45)` },
              { transform: [{ translateY: lift.interpolate({ inputRange: [0, 1], outputRange: [0, -6] }) }, { scale: pressed ? 0.98 : 1 }] },
            ]}
          >
            <Animated.View pointerEvents="none" style={[StyleSheet.absoluteFill, styles.cardGlow, { opacity: lift, experimental_backgroundImage: `radial-gradient(circle at 0% 0%, ${f.color}26 0%, rgba(0,0,0,0) 60%)` }]} />
            <View style={[styles.icon, { borderColor: f.color + '66', backgroundColor: f.color + '18' }]}>
              <Text style={[styles.iconText, { color: f.color }]}>{f.glyph}</Text>
            </View>
            <Text style={text.h3}>{f.title}</Text>
            <Text style={[text.body, { fontSize: 14, lineHeight: 22, flex: 1 }]}>{f.body}</Text>
            <View style={styles.ctaRow}>
              <Text style={[styles.cta, { color: f.color }]}>{f.cta}</Text>
              <Animated.Text style={[styles.cta, { color: f.color, transform: [{ translateX: lift.interpolate({ inputRange: [0, 1], outputRange: [0, 5] }) }] }]}>→</Animated.Text>
            </View>
          </Animated.View>
        )}
      </Pressable>
    </Reveal>
  );
}

export function Features({ items = FEATURES }) {
  const { width, gutter, phone } = useLayout();
  const inner = Math.min(width - gutter * 2, 1240);
  const cols = inner > 1000 ? 4 : inner > 560 ? 2 : 1;
  const gap = phone ? 12 : 16;
  const w = Math.floor((inner - gap * (cols - 1)) / cols);
  return (
    <View style={[styles.grid, { gap }]}>
      {items.map((f, i) => (
        <FeatureCard key={f.title} f={f} width={w} delay={60 * (i % cols)} />
      ))}
    </View>
  );
}

const styles = StyleSheet.create({
  grid: { flexDirection: 'row', flexWrap: 'wrap' },
  card: {
    flex: 1,
    gap: 10,
    padding: 22,
    borderRadius: 18,
    borderWidth: 1,
    borderColor: site.stroke,
    backgroundColor: site.card,
    overflow: 'hidden',
    boxShadow: '0 24px 60px rgba(0, 0, 0, 0.45), inset 0 1px 0 rgba(255, 255, 255, 0.05)',
  },
  cardGlow: { borderRadius: 18 },
  icon: { width: 42, height: 42, borderRadius: 12, borderWidth: 1, alignItems: 'center', justifyContent: 'center', marginBottom: 4 },
  iconText: { fontFamily: mono, fontWeight: '700', fontSize: 15 },
  ctaRow: { flexDirection: 'row', alignItems: 'center', gap: 6, marginTop: 4 },
  cta: { fontFamily: font, fontWeight: '700', fontSize: 13 },
});
