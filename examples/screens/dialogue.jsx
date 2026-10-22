// Dialogue: cinematic letterbox, speaker portrait overlapping the text box,
// rich inline text spans, and a list of response choices with skill checks.
import React from 'react';
import { Image, ImageBackground, StyleSheet, Text, View } from 'react-native';
import { colors, font } from '../theme.js';

export const meta = { width: 1280, height: 720, maxMismatch: 1.5, title: 'Dialogue' };

const CHOICES = [
  { n: 1, text: 'Tell me what happened at the Cinder Gate.' },
  { n: 2, text: 'The Wardens will not hold the bridge without your fire.', check: 'Persuade 14', focused: true },
  { n: 3, text: 'Hand over the Ember Key, or I take it.', check: 'Intimidate 18', locked: true },
  { n: 4, text: '[Leave] We will speak again, Seren.', muted: true },
];

export default function App() {
  return (
    <ImageBackground source={require('../assets/bg-menu.jpg')} style={styles.root} resizeMode="cover">
      <View style={styles.dim} />
      <View style={[styles.letterbox, { top: 0 }]} />
      <View style={[styles.letterbox, { bottom: 0 }]} />

      <View style={styles.listener}>
        <Image source={require('../assets/portrait-rogue.png')} style={styles.listenerImg} />
        <Text style={styles.listenerName}>You</Text>
      </View>

      <View style={styles.choices}>
        {CHOICES.map((c) => (
          <View key={c.n} style={[styles.choice, c.focused && styles.choiceFocused, c.locked && { opacity: 0.5 }]}>
            <View style={[styles.choiceNum, c.focused && styles.choiceNumFocused]}>
              <Text style={[styles.choiceNumText, c.focused && { color: '#1a1206' }]}>{c.n}</Text>
            </View>
            <Text style={[styles.choiceText, c.muted && { color: colors.textMute }, c.focused && { color: '#fff' }]} numberOfLines={1}>
              {c.check ? <Text style={[styles.check, c.locked && { color: colors.red }]}>[{c.check}] </Text> : null}
              {c.text}
            </Text>
          </View>
        ))}
      </View>

      <View style={styles.box}>
        <View style={styles.boxGlow} />
        <View style={styles.portraitFrame}>
          <Image source={require('../assets/portrait-mage.png')} style={styles.portrait} />
        </View>
        <View style={styles.textCol}>
          <View style={styles.namePlate}>
            <Text style={styles.name}>Seren Ashvale</Text>
            <Text style={styles.title}>Stormcaller of the Ninth Spire</Text>
          </View>
          <Text style={styles.line}>
            The gate did not fall, traveller. It was <Text style={styles.em}>opened</Text>. Someone carried the{' '}
            <Text style={styles.keyword}>Ember Key</Text> past three Wardens and not one of them remembers a face. If the{' '}
            <Text style={styles.place}>Old King's</Text> fire wakes before the tide turns, there will be no bridge left to defend.
          </Text>
          <View style={styles.footerRow}>
            <View style={styles.affinity}>
              <Text style={styles.affinityLabel}>Trust</Text>
              <View style={styles.affinityTrack}>
                <View style={styles.affinityFill} />
              </View>
            </View>
            <View style={{ flex: 1 }} />
            <Text style={styles.skip}>Hold X to skip</Text>
            <View style={styles.next} />
          </View>
        </View>
      </View>
    </ImageBackground>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#05070f' },
  dim: { ...StyleSheet.absoluteFillObject, backgroundColor: 'rgba(4,6,14,0.35)' },
  letterbox: { position: 'absolute', left: 0, right: 0, height: 56, backgroundColor: '#000' },

  listener: { position: 'absolute', right: 80, top: 90, alignItems: 'center', opacity: 0.8 },
  listenerImg: { width: 96, height: 96, borderRadius: 48, borderWidth: 2, borderColor: 'rgba(255,255,255,0.25)' },
  listenerName: { fontFamily: font, fontWeight: '600', fontSize: 12, letterSpacing: 1.6, textTransform: 'uppercase', color: colors.textDim, marginTop: 8 },

  choices: { position: 'absolute', right: 80, top: 236, width: 560, gap: 8 },
  choice: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 12,
    height: 44,
    paddingHorizontal: 12,
    borderRadius: 8,
    backgroundColor: 'rgba(8,10,22,0.72)',
    borderWidth: 1,
    borderColor: 'rgba(255,255,255,0.06)',
  },
  choiceFocused: {
    borderColor: 'rgba(242,195,91,0.8)',
    experimental_backgroundImage: 'linear-gradient(90deg, rgba(242,195,91,0.28), rgba(8,10,22,0.8) 70%)',
    boxShadow: '0 0 20px rgba(242,195,91,0.25)',
    transform: [{ translateX: -12 }],
  },
  choiceNum: { width: 24, height: 24, borderRadius: 6, borderWidth: 1, borderColor: colors.lineStrong, alignItems: 'center', justifyContent: 'center' },
  choiceNumFocused: { backgroundColor: colors.gold, borderColor: colors.gold },
  choiceNumText: { fontFamily: font, fontWeight: '700', fontSize: 12, color: colors.textDim },
  choiceText: { flex: 1, fontFamily: font, fontWeight: '500', fontSize: 15, color: colors.text },
  check: { fontWeight: '700', color: colors.cyan },

  box: {
    position: 'absolute',
    left: 60,
    right: 60,
    bottom: 76,
    height: 190,
    flexDirection: 'row',
    borderRadius: 18,
    backgroundColor: 'rgba(9,12,24,0.94)',
    borderWidth: 1,
    borderColor: 'rgba(148,170,255,0.22)',
    boxShadow: '0 30px 60px rgba(0,0,0,0.7), inset 0 1px 0 rgba(255,255,255,0.08)',
  },
  boxGlow: {
    position: 'absolute',
    left: 0,
    right: 0,
    top: 0,
    height: 2,
    borderTopLeftRadius: 18,
    borderTopRightRadius: 18,
    experimental_backgroundImage: 'linear-gradient(90deg, rgba(79,209,255,0) 0%, rgba(79,209,255,0.9) 30%, rgba(242,195,91,0.9) 70%, rgba(242,195,91,0) 100%)',
  },
  portraitFrame: {
    width: 190,
    height: 230,
    marginTop: -64,
    marginLeft: 26,
    borderRadius: 16,
    padding: 4,
    experimental_backgroundImage: 'linear-gradient(180deg, #7fe3ff 0%, #2a3f80 60%, #151b38 100%)',
    boxShadow: '0 18px 40px rgba(0,0,0,0.7), 0 0 26px rgba(79,209,255,0.35)',
    transform: [{ rotate: '-2deg' }],
  },
  portrait: { flex: 1, borderRadius: 12 },
  textCol: { flex: 1, paddingVertical: 22, paddingLeft: 28, paddingRight: 30 },
  namePlate: { flexDirection: 'row', alignItems: 'baseline', gap: 12 },
  name: {
    fontFamily: font,
    fontWeight: '700',
    fontSize: 22,
    color: colors.cyan,
    textShadowColor: 'rgba(79,209,255,0.6)',
    textShadowOffset: { width: 0, height: 0 },
    textShadowRadius: 12,
  },
  title: { fontFamily: font, fontStyle: 'italic', fontSize: 13, color: colors.textMute },
  line: { fontFamily: font, fontSize: 18, lineHeight: 28, color: '#dfe5f7', marginTop: 10 },
  em: { fontStyle: 'italic', color: '#fff' },
  keyword: { fontWeight: '700', color: colors.gold },
  place: { fontWeight: '600', color: colors.orange },
  footerRow: { flexDirection: 'row', alignItems: 'center', marginTop: 'auto', gap: 12 },
  affinity: { flexDirection: 'row', alignItems: 'center', gap: 10 },
  affinityLabel: { fontFamily: font, fontWeight: '600', fontSize: 11, letterSpacing: 1.4, textTransform: 'uppercase', color: colors.textMute },
  affinityTrack: { width: 120, height: 4, borderRadius: 2, backgroundColor: 'rgba(255,255,255,0.1)' },
  affinityFill: { width: '70%', height: '100%', borderRadius: 2, backgroundColor: colors.cyan },
  skip: { fontFamily: font, fontSize: 12, color: colors.textMute },
  next: { width: 10, height: 10, borderRightWidth: 2, borderBottomWidth: 2, borderColor: colors.gold, transform: [{ rotate: '45deg' }], marginTop: -4 },
});
