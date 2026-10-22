// Title screen: backdrop art, logo lockup, focused menu stack, news card,
// player chip and controller hints.
import React from 'react';
import { Image, ImageBackground, StyleSheet, Text, View } from 'react-native';
import { colors, font, type } from '../theme.js';

export const meta = { width: 1280, height: 720, maxMismatch: 1.5, title: 'Main menu' };

const ITEMS = [
  { label: 'Continue', hint: 'Act III · The Ember Gate', focused: true },
  { label: 'New Journey' },
  { label: 'Multiplayer', badge: '3 invites' },
  { label: 'Armory' },
  { label: 'Settings' },
  { label: 'Quit to Desktop', muted: true },
];

function MenuButton({ label, hint, focused, badge, muted }) {
  return (
    <View style={[styles.button, focused && styles.buttonFocused]}>
      {focused ? <View style={styles.focusBar} /> : null}
      <View style={{ flex: 1 }}>
        <Text style={[styles.buttonText, focused && styles.buttonTextFocused, muted && { color: colors.textMute }]}>{label}</Text>
        {hint ? <Text style={styles.buttonHint}>{hint}</Text> : null}
      </View>
      {badge ? (
        <View style={styles.badge}>
          <Text style={styles.badgeText}>{badge}</Text>
        </View>
      ) : null}
      {focused ? <Text style={styles.chevron}>›</Text> : null}
    </View>
  );
}

function KeyHint({ glyph, label, color }) {
  return (
    <View style={styles.keyHint}>
      <View style={[styles.keyGlyph, { borderColor: color }]}>
        <Text style={[styles.keyGlyphText, { color }]}>{glyph}</Text>
      </View>
      <Text style={styles.keyLabel}>{label}</Text>
    </View>
  );
}

export default function App() {
  return (
    <ImageBackground source={require('../assets/bg-menu.jpg')} style={styles.root} resizeMode="cover">
      <View style={styles.scrim} />

      <View style={styles.topBar}>
        <View style={styles.profile}>
          <Image source={require('../assets/crest-wolf.png')} style={styles.crest} />
          <View>
            <Text style={styles.profileName}>Valkyrie_07</Text>
            <Text style={styles.profileLevel}>Level 42 · Vanguard</Text>
          </View>
          <View style={styles.divider} />
          <View style={styles.currency}>
            <View style={[styles.coin, { experimental_backgroundImage: 'radial-gradient(circle at 35% 35%, #fff2c2 0%, #f2c35b 45%, #a8741f 100%)' }]} />
            <Text style={styles.currencyText}>12,480</Text>
          </View>
          <View style={styles.currency}>
            <View style={[styles.coin, { borderRadius: 3, transform: [{ rotate: '45deg' }], experimental_backgroundImage: 'linear-gradient(135deg, #d6b8ff 0%, #8a4dff 100%)' }]} />
            <Text style={styles.currencyText}>860</Text>
          </View>
        </View>
      </View>

      <View style={styles.left}>
        <Text style={styles.season}>Season 4 — Emberfall</Text>
        <Text style={styles.title}>AETHERFALL</Text>
        <View style={styles.titleRule}>
          <View style={styles.ruleLine} />
          <View style={styles.ruleDiamond} />
          <View style={[styles.ruleLine, { experimental_backgroundImage: 'linear-gradient(90deg, rgba(242,195,91,0.9), rgba(242,195,91,0))' }]} />
        </View>
        <Text style={styles.subtitle}>Chronicles of the Shattered Sky</Text>

        <View style={styles.menu}>
          {ITEMS.map((it) => (
            <MenuButton key={it.label} {...it} />
          ))}
        </View>
      </View>

      <View style={styles.news}>
        <Image source={require('../assets/card-ember.png')} style={styles.newsArt} resizeMode="cover" />
        <View style={styles.newsTag}>
          <Text style={styles.newsTagText}>New</Text>
        </View>
        <View style={styles.newsBody}>
          <Text style={styles.newsKicker}>Patch 4.2 · Live now</Text>
          <Text style={styles.newsTitle}>The Ember Tide rises</Text>
          <Text style={styles.newsText} numberOfLines={2}>
            Three new rifts, the Cinderforged armor set and a reworked talent tree for every class. Log in before the tide recedes to claim the Founder's Brazier.
          </Text>
          <View style={styles.pips}>
            <View style={[styles.pip, styles.pipActive]} />
            <View style={styles.pip} />
            <View style={styles.pip} />
          </View>
        </View>
      </View>

      <View style={styles.bottomBar}>
        <KeyHint glyph="A" label="Select" color="#5fe08a" />
        <KeyHint glyph="B" label="Back" color="#ff6b81" />
        <KeyHint glyph="Y" label="Patch Notes" color="#ffd36b" />
        <View style={{ flex: 1 }} />
        <Text style={styles.version}>v4.2.0 · EU-West · 38 ms</Text>
      </View>
    </ImageBackground>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: colors.bg0 },
  scrim: {
    ...StyleSheet.absoluteFillObject,
    experimental_backgroundImage:
      'linear-gradient(90deg, rgba(5,7,16,0.92) 0%, rgba(5,7,16,0.7) 38%, rgba(5,7,16,0) 70%), linear-gradient(180deg, rgba(5,7,16,0) 70%, rgba(5,7,16,0.85) 100%)',
  },
  topBar: { position: 'absolute', top: 28, right: 32, flexDirection: 'row' },
  profile: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 12,
    paddingVertical: 8,
    paddingLeft: 8,
    paddingRight: 18,
    borderRadius: 30,
    backgroundColor: 'rgba(10, 14, 28, 0.72)',
    borderWidth: 1,
    borderColor: colors.line,
    boxShadow: '0 8px 24px rgba(0,0,0,0.45)',
  },
  crest: { width: 40, height: 40, borderRadius: 20, borderWidth: 2, borderColor: colors.gold },
  profileName: { fontFamily: font, fontWeight: '600', fontSize: 15, color: colors.text },
  profileLevel: { fontFamily: font, fontWeight: '500', fontSize: 12, color: colors.textDim, marginTop: 1 },
  divider: { width: 1, height: 28, backgroundColor: colors.line, marginHorizontal: 4 },
  currency: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  coin: { width: 16, height: 16, borderRadius: 8, boxShadow: '0 0 8px rgba(242,195,91,0.5)' },
  currencyText: { fontFamily: font, fontWeight: '700', fontSize: 15, color: colors.text },

  left: { position: 'absolute', left: 88, top: 64, width: 600 },
  season: { ...type.label, color: colors.gold, letterSpacing: 3 },
  title: {
    fontFamily: font,
    fontWeight: '700',
    fontSize: 64,
    letterSpacing: 7,
    color: '#fff6dc',
    marginTop: 10,
    textShadowColor: 'rgba(242, 195, 91, 0.65)',
    textShadowOffset: { width: 0, height: 0 },
    textShadowRadius: 24,
  },
  titleRule: { flexDirection: 'row', alignItems: 'center', gap: 10, marginTop: 6, width: 380 },
  ruleLine: { flex: 1, height: 1, backgroundColor: 'rgba(242,195,91,0.9)' },
  ruleDiamond: { width: 9, height: 9, backgroundColor: colors.gold, transform: [{ rotate: '45deg' }], boxShadow: '0 0 10px rgba(242,195,91,0.9)' },
  subtitle: { fontFamily: font, fontStyle: 'italic', fontSize: 17, color: colors.textDim, marginTop: 12, letterSpacing: 0.5 },

  menu: { marginTop: 32, gap: 8, width: 360 },
  button: {
    height: 48,
    flexDirection: 'row',
    alignItems: 'center',
    paddingHorizontal: 22,
    borderRadius: 8,
    backgroundColor: 'rgba(14, 20, 38, 0.55)',
    borderWidth: 1,
    borderColor: 'rgba(148, 170, 255, 0.10)',
  },
  buttonFocused: {
    height: 64,
    borderColor: 'rgba(242, 195, 91, 0.85)',
    experimental_backgroundImage: 'linear-gradient(90deg, rgba(242,195,91,0.32) 0%, rgba(242,195,91,0.06) 100%)',
    boxShadow: '0 0 0 1px rgba(242,195,91,0.25), 0 0 28px rgba(242,195,91,0.35), inset 0 0 18px rgba(242,195,91,0.18)',
    transform: [{ translateX: 10 }],
  },
  focusBar: {
    position: 'absolute',
    left: -1,
    top: 10,
    bottom: 10,
    width: 4,
    borderTopRightRadius: 3,
    borderBottomRightRadius: 3,
    backgroundColor: colors.goldLight,
    boxShadow: '0 0 12px rgba(255,226,154,0.9)',
  },
  buttonText: { fontFamily: font, fontWeight: '600', fontSize: 17, letterSpacing: 1.5, textTransform: 'uppercase', color: '#c9d2ea' },
  buttonTextFocused: { color: '#fff6dc', fontWeight: '700' },
  buttonHint: { fontFamily: font, fontWeight: '500', fontSize: 12, color: colors.gold, marginTop: 3, letterSpacing: 0.4 },
  chevron: { fontFamily: font, fontWeight: '400', fontSize: 30, color: colors.goldLight, marginTop: -4 },
  badge: { paddingHorizontal: 8, paddingVertical: 3, borderRadius: 10, backgroundColor: colors.red, boxShadow: '0 0 10px rgba(255,77,106,0.6)' },
  badgeText: { fontFamily: font, fontWeight: '700', fontSize: 11, color: '#fff' },

  news: {
    position: 'absolute',
    right: 32,
    bottom: 84,
    width: 340,
    borderRadius: 14,
    overflow: 'hidden',
    backgroundColor: colors.panelSolid,
    borderWidth: 1,
    borderColor: colors.line,
    boxShadow: '0 20px 50px rgba(0,0,0,0.6)',
  },
  newsArt: { width: '100%', height: 120 },
  newsTag: {
    position: 'absolute',
    top: 12,
    left: 12,
    paddingHorizontal: 9,
    paddingVertical: 4,
    borderRadius: 6,
    backgroundColor: colors.orange,
  },
  newsTagText: { fontFamily: font, fontWeight: '700', fontSize: 11, letterSpacing: 1.2, textTransform: 'uppercase', color: '#2a1300' },
  newsBody: { padding: 16, paddingTop: 14 },
  newsKicker: { ...type.label, fontSize: 11, color: colors.orange },
  newsTitle: { fontFamily: font, fontWeight: '700', fontSize: 19, color: colors.text, marginTop: 6 },
  newsText: { fontFamily: font, fontSize: 13, lineHeight: 19, color: colors.textDim, marginTop: 6 },
  pips: { flexDirection: 'row', gap: 6, marginTop: 14 },
  pip: { width: 8, height: 4, borderRadius: 2, backgroundColor: colors.lineStrong },
  pipActive: { width: 22, backgroundColor: colors.orange },

  bottomBar: {
    position: 'absolute',
    left: 0,
    right: 0,
    bottom: 0,
    height: 56,
    paddingHorizontal: 88,
    flexDirection: 'row',
    alignItems: 'center',
    gap: 28,
    borderTopWidth: 1,
    borderTopColor: 'rgba(148,170,255,0.10)',
    backgroundColor: 'rgba(5,7,16,0.6)',
  },
  keyHint: { flexDirection: 'row', alignItems: 'center', gap: 10 },
  keyGlyph: { width: 24, height: 24, borderRadius: 12, borderWidth: 2, alignItems: 'center', justifyContent: 'center' },
  keyGlyphText: { fontFamily: font, fontWeight: '700', fontSize: 12 },
  keyLabel: { fontFamily: font, fontWeight: '500', fontSize: 14, color: colors.textDim },
  version: { fontFamily: font, fontSize: 12, color: colors.textMute, letterSpacing: 0.5 },
});

