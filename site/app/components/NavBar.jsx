// Fixed top bar: wordmark, page links (horizontally scrollable on phones) and GitHub.
import React from 'react';
import { Pressable, ScrollView, StyleSheet, Text, View } from 'react-native';
import { colors, font, site } from '../theme.js';
import { segments, useRouter } from '../router.js';
import { Button, GITHUB, useLayout } from './ui.jsx';

export const NAV_HEIGHT = 64;

export const LINKS = [
  { to: '/screens', label: 'Screens' },
  { to: '/interactions', label: 'Interactions' },
  { to: '/input', label: 'Input' },
  { to: '/motion', label: 'Motion' },
  { to: '/playground', label: 'Playground' },
  { to: '/embed', label: 'Embed' },
];

export function Logo({ size = 22 }) {
  const box = Math.round(size * 1.35);
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
      <View style={[styles.mark, { width: box, height: box, borderRadius: box * 0.28 }]}>
        <View style={[styles.markInner, { width: box * 0.42, height: box * 0.42, borderRadius: box * 0.1 }]} />
      </View>
      <Text style={[styles.word, { fontSize: size }]}>JSUI</Text>
    </View>
  );
}

function NavLink({ to, label, active }) {
  const { go } = useRouter();
  return (
    <Pressable onPress={() => go(to)} style={{ cursor: 'pointer' }}>
      {({ hovered, focused }) => (
        <View style={[styles.link, (hovered || focused) && styles.linkHot, active && styles.linkActive]}>
          <Text style={[styles.linkText, (hovered || focused || active) && { color: colors.text }]}>{label}</Text>
          {active ? <View style={styles.underline} /> : null}
        </View>
      )}
    </Pressable>
  );
}

export function NavBar() {
  const { path, go } = useRouter();
  const { tablet, phone, gutter } = useLayout();
  const head = '/' + (segments(path)[0] || '');
  return (
    <View style={[styles.bar, { paddingHorizontal: gutter }]}>
      <Pressable onPress={() => go('/')} style={{ cursor: 'pointer' }} focusable={false}>
        <Logo size={phone ? 18 : 20} />
      </Pressable>
      <ScrollView
        horizontal
        showsHorizontalScrollIndicator={false}
        style={{ flex: 1, marginLeft: tablet ? 12 : 36 }}
        contentContainerStyle={{ alignItems: 'center', gap: 2 }}
      >
        {LINKS.map((l) => (
          <NavLink key={l.to} {...l} active={head === l.to} />
        ))}
      </ScrollView>
      {phone ? null : <Button small kind="ghost" label="GitHub" icon="★" href={GITHUB} self="center" />}
    </View>
  );
}

const styles = StyleSheet.create({
  bar: {
    position: 'absolute',
    left: 0,
    right: 0,
    top: 0,
    height: NAV_HEIGHT,
    flexDirection: 'row',
    alignItems: 'center',
    backgroundColor: 'rgba(5, 7, 15, 0.82)',
    borderBottomWidth: 1,
    borderBottomColor: site.stroke,
  },
  mark: {
    alignItems: 'center',
    justifyContent: 'center',
    experimental_backgroundImage: 'linear-gradient(135deg, #ffe29a 0%, #f2c35b 45%, #4fd1ff 100%)',
    boxShadow: '0 6px 20px rgba(242, 195, 91, 0.3)',
  },
  markInner: { backgroundColor: site.bg, transform: [{ rotate: '45deg' }] },
  word: { fontFamily: font, fontWeight: '700', letterSpacing: 3, color: colors.text },
  link: { height: 40, paddingHorizontal: 14, borderRadius: 10, justifyContent: 'center' },
  linkHot: { backgroundColor: 'rgba(255,255,255,0.05)' },
  linkActive: {},
  linkText: { fontFamily: font, fontWeight: '600', fontSize: 14, color: colors.textDim },
  underline: { position: 'absolute', left: 14, right: 14, bottom: 4, height: 2, borderRadius: 1, backgroundColor: colors.gold },
});
