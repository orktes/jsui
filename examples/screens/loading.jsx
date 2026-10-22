// Loading screen: full-bleed art, zone title and lore, gameplay tip card,
// segmented progress bar and a border-drawn spinner.
import React from 'react';
import { ImageBackground, StyleSheet, Text, View } from 'react-native';
import { colors, font } from '../theme.js';

export const meta = { width: 1280, height: 720, maxMismatch: 1.5, title: 'Loading screen' };

export default function App() {
  return (
    <ImageBackground source={require('../assets/bg-loading.jpg')} style={styles.root} resizeMode="cover">
      <View style={styles.grade} />

      <View style={styles.logo}>
        <Text style={styles.logoText}>AETHERFALL</Text>
        <Text style={styles.logoSub}>Chronicles of the Shattered Sky</Text>
      </View>

      <View style={styles.info}>
        <Text style={styles.chapter}>Chapter III</Text>
        <Text style={styles.zone}>The Sunken Causeway</Text>
        <View style={styles.zoneRule} />
        <Text style={styles.lore}>
          When the Old King fell, the sea climbed the causeway stone by stone. The lanterns still burn beneath the tide, and those who follow their light rarely
          return the same.
        </Text>
        <View style={styles.tip}>
          <View style={styles.tipBadge}>
            <Text style={styles.tipBadgeText}>Tip</Text>
          </View>
          <Text style={styles.tipText}>
            Hold <Text style={styles.key}> LB </Text> while aiming to slow time briefly. Each use drains one segment of focus.
          </Text>
        </View>
      </View>

      <View style={styles.bottom}>
        <View style={styles.spinnerWrap}>
          <View style={styles.spinnerFrame} />
          <View style={styles.spinner} />
          <View style={styles.spinnerCore} />
        </View>
        <View style={{ flex: 1 }}>
          <View style={styles.statusRow}>
            <Text style={styles.status}>Compiling shaders</Text>
            <Text style={styles.statusCount}>412 / 860</Text>
            <View style={{ flex: 1 }} />
            <Text style={styles.pct}>68%</Text>
          </View>
          <View style={styles.track}>
            {Array.from({ length: 40 }, (_, i) => (
              <View key={i} style={[styles.seg, i < 27 && styles.segOn, i === 27 && styles.segHead]} />
            ))}
          </View>
        </View>
      </View>
    </ImageBackground>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#0b1a20' },
  grade: {
    ...StyleSheet.absoluteFillObject,
    experimental_backgroundImage:
      'linear-gradient(0deg, rgba(3,8,12,0.95) 0%, rgba(3,8,12,0.55) 35%, rgba(3,8,12,0) 60%), linear-gradient(90deg, rgba(3,8,12,0.6) 0%, rgba(3,8,12,0) 50%)',
  },
  logo: { position: 'absolute', right: 56, top: 44, alignItems: 'flex-end' },
  logoText: { fontFamily: font, fontWeight: '700', fontSize: 26, letterSpacing: 7, color: '#fff6dc', textShadowColor: 'rgba(242,195,91,0.6)', textShadowOffset: { width: 0, height: 0 }, textShadowRadius: 14 },
  logoSub: { fontFamily: font, fontStyle: 'italic', fontSize: 12, color: colors.textDim, marginTop: 4 },

  info: { position: 'absolute', left: 64, bottom: 150, width: 560 },
  chapter: { fontFamily: font, fontWeight: '600', fontSize: 13, letterSpacing: 4, textTransform: 'uppercase', color: '#f2c27a' },
  zone: { fontFamily: font, fontWeight: '700', fontSize: 48, color: '#fff', marginTop: 6, letterSpacing: -0.5, textShadowColor: 'rgba(0,0,0,0.6)', textShadowOffset: { width: 0, height: 2 }, textShadowRadius: 10 },
  zoneRule: { width: 80, height: 3, borderRadius: 2, backgroundColor: '#f2c27a', marginTop: 14, boxShadow: '0 0 10px rgba(242,194,122,0.8)' },
  lore: { fontFamily: font, fontSize: 16, lineHeight: 26, color: '#c9d4dc', marginTop: 16 },
  tip: {
    flexDirection: 'row',
    alignItems: 'flex-start',
    gap: 12,
    marginTop: 22,
    padding: 14,
    borderRadius: 10,
    backgroundColor: 'rgba(8,20,26,0.75)',
    borderWidth: 1,
    borderColor: 'rgba(120,200,220,0.2)',
  },
  tipBadge: { paddingHorizontal: 8, paddingVertical: 3, borderRadius: 4, backgroundColor: '#3ad0ff' },
  tipBadgeText: { fontFamily: font, fontWeight: '700', fontSize: 11, letterSpacing: 1.2, textTransform: 'uppercase', color: '#03141c' },
  tipText: { flex: 1, fontFamily: font, fontSize: 14, lineHeight: 21, color: colors.text },
  key: { fontWeight: '700', color: '#03141c', backgroundColor: '#e8f4ff' },

  bottom: { position: 'absolute', left: 64, right: 64, bottom: 48, flexDirection: 'row', alignItems: 'center', gap: 22 },
  spinnerWrap: { width: 48, height: 48, alignItems: 'center', justifyContent: 'center' },
  spinnerFrame: { position: 'absolute', width: 34, height: 34, borderWidth: 1, borderColor: 'rgba(242,194,122,0.35)', transform: [{ rotate: '45deg' }] },
  spinner: {
    position: 'absolute',
    width: 44,
    height: 44,
    borderRadius: 22,
    borderWidth: 3,
    borderTopColor: '#f2c27a',
    borderRightColor: 'rgba(242,194,122,0.5)',
    borderBottomColor: 'rgba(242,194,122,0.12)',
    borderLeftColor: 'rgba(242,194,122,0.12)',
    transform: [{ rotate: '30deg' }],
  },
  spinnerCore: { width: 8, height: 8, borderRadius: 4, backgroundColor: '#fff3d6', boxShadow: '0 0 12px rgba(255,220,160,0.9)' },
  statusRow: { flexDirection: 'row', alignItems: 'baseline', gap: 10, marginBottom: 10 },
  status: { fontFamily: font, fontWeight: '600', fontSize: 14, color: colors.text },
  statusCount: { fontFamily: font, fontWeight: '500', fontSize: 12, color: colors.textMute },
  pct: { fontFamily: font, fontWeight: '700', fontSize: 22, color: '#f2c27a' },
  track: { flexDirection: 'row', gap: 3, height: 8 },
  seg: { flex: 1, borderRadius: 1, backgroundColor: 'rgba(255,255,255,0.10)' },
  segOn: { backgroundColor: '#e0a85c' },
  segHead: { backgroundColor: '#fff3d6', boxShadow: '0 0 10px rgba(255,220,160,0.9)' },
});
