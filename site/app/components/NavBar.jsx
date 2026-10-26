// Fixed top bar: wordmark, page links (horizontally scrollable on phones),
// the sound switch and GitHub.
import React from 'react';
import { Pressable, ScrollView, StyleSheet, Text, View, useHostState } from 'react-native';
import { colors, font, site } from '../theme.js';
import { segments, useRouter } from '../router.js';
import { setSoundMuted, useAudioState } from '../sound.js';
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

// Sound on / off (the page mutes its Web Audio output and remembers it).
// Until the first click or key press the browser keeps audio locked: the
// switch says so ("Enable sound"), and pressing it is that gesture.
const EQ = [0.45, 0.85, 0.6, 1];
function SoundSwitch({ compact }) {
  const { unlocked, muted } = useAudioState();
  const on = unlocked && !muted;
  const label = muted ? 'Sound off' : unlocked ? 'Sound on' : 'Enable sound';
  return (
    <Pressable
      onPress={() => setSoundMuted(unlocked ? !muted : false)}
      accessibilityRole="switch"
      accessibilityLabel={label}
      accessibilityState={{ checked: on }}
      style={{ cursor: 'pointer', alignSelf: 'center', marginRight: compact ? 0 : 8 }}
    >
      {({ hovered, focused }) => (
        <View style={[styles.sound, (hovered || focused) && styles.linkHot]}>
          <View style={styles.eq}>
            {EQ.map((h, i) => (
              <View key={i} style={[styles.eqBar, { height: on ? 4 + h * 12 : 3, backgroundColor: on ? colors.gold : colors.textDim }]} />
            ))}
            {muted ? <View style={styles.eqSlash} /> : null}
          </View>
          {compact ? null : (
            <Text style={[styles.linkText, { fontSize: 13 }, (hovered || focused) && { color: colors.text }, !unlocked && !muted && { color: colors.gold }]}>
              {label}
            </Text>
          )}
        </View>
      )}
    </Pressable>
  );
}

export function NavBar() {
  const { path, go } = useRouter();
  const { tablet, phone, gutter } = useLayout();
  const head = '/' + (segments(path)[0] || '');
  // A native window whose buttons sit in this bar (jsui-viewer
  // --titlebar-overlay): the wordmark starts after them.
  const controls = useHostState('jsui.windowControls', null);
  const left = controls ? Math.max(gutter, controls.x + controls.width + 20) : gutter;
  return (
    <View style={[styles.bar, { paddingHorizontal: gutter, paddingLeft: left }]}>
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
      <SoundSwitch compact={tablet} />
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
  sound: { height: 40, paddingHorizontal: 10, borderRadius: 10, flexDirection: 'row', alignItems: 'center', gap: 8 },
  eq: { width: 18, height: 16, flexDirection: 'row', alignItems: 'flex-end', justifyContent: 'space-between' },
  eqBar: { width: 3, borderRadius: 1.5 },
  eqSlash: { position: 'absolute', left: 8, top: -2, width: 2, height: 20, borderRadius: 1, backgroundColor: colors.textDim, transform: [{ rotate: '45deg' }] },
  underline: { position: 'absolute', left: 14, right: 14, bottom: 4, height: 2, borderRadius: 1, backgroundColor: colors.gold },
});
