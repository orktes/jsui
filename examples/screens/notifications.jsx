// Notifications: stacked toasts (achievement, legendary drop, party invite,
// system warning), an "area discovered" banner and a kill feed.
import React from 'react';
import { Image, ImageBackground, StyleSheet, Text, View } from 'react-native';
import { colors, font, rarity } from '../theme.js';

export const meta = { width: 1280, height: 720, maxMismatch: 1.5, title: 'Notifications' };

function Toast({ accent, children, style }) {
  return (
    <View style={[styles.toast, { borderLeftColor: accent }, style]}>
      {children}
    </View>
  );
}

const FEED = [
  { a: 'Valkyrie_07', b: 'Ashlight', weapon: require('../assets/item-bow.png'), mine: true },
  { a: 'Mireille', b: 'nightjar', weapon: require('../assets/item-sword.png') },
  { a: 'Okonkwo', b: 'GRIMWALD', weapon: require('../assets/item-axe.png'), headshot: true },
];

export default function App() {
  return (
    <ImageBackground source={require('../assets/bg-loading.jpg')} style={styles.root} resizeMode="cover">
      <View style={styles.shade} />

      <View style={styles.feed}>
        {FEED.map((f, i) => (
          <View key={i} style={[styles.feedRow, f.mine && styles.feedMine, { opacity: 1 - i * 0.18 }]}>
            <Text style={[styles.feedName, { color: '#7fb4ff' }]}>{f.a}</Text>
            <Image source={f.weapon} style={styles.feedIcon} />
            {f.headshot ? <View style={styles.headshot} /> : null}
            <Text style={[styles.feedName, { color: '#ff7f8f' }]}>{f.b}</Text>
          </View>
        ))}
      </View>

      <View style={styles.banner}>
        <View style={styles.bannerRule}>
          <View style={[styles.rule, { experimental_backgroundImage: 'linear-gradient(90deg, rgba(255,226,154,0), rgba(255,226,154,0.9))' }]} />
          <View style={styles.diamond} />
          <View style={[styles.rule, { experimental_backgroundImage: 'linear-gradient(90deg, rgba(255,226,154,0.9), rgba(255,226,154,0))' }]} />
        </View>
        <Text style={styles.bannerKicker}>Area Discovered</Text>
        <Text style={styles.bannerTitle}>The Drowned Observatory</Text>
        <Text style={styles.bannerSub}>Recommended level 44 – 48</Text>
      </View>

      <View style={styles.stack}>
        <Toast accent={colors.gold} style={styles.achievement}>
          <View style={styles.trophy}>
            <Image source={require('../assets/item-gem.png')} style={{ width: 40, height: 40 }} />
          </View>
          <View style={{ flex: 1 }}>
            <Text style={[styles.kicker, { color: colors.gold }]}>Achievement Unlocked</Text>
            <Text style={styles.toastTitle}>Starfall Cartographer</Text>
            <Text style={styles.toastBody}>Discover every observatory in Ashen Reach.</Text>
          </View>
          <Text style={styles.points}>+50</Text>
        </Toast>

        <Toast accent={rarity.legendary.color} style={styles.loot}>
          <View style={styles.lootIcon}>
            <Image source={require('../assets/item-amulet.png')} style={{ width: 40, height: 40 }} />
          </View>
          <View style={{ flex: 1 }}>
            <Text style={[styles.kicker, { color: rarity.legendary.color }]}>Legendary Drop</Text>
            <Text style={[styles.toastTitle, { color: rarity.legendary.color }]} numberOfLines={1}>Heart of the Drowned Star</Text>
            <Text style={styles.toastBody}>Amulet · Item level 480</Text>
          </View>
        </Toast>

        <Toast accent={colors.cyan}>
          <Image source={require('../assets/crest-moon.png')} style={styles.avatar} />
          <View style={{ flex: 1 }}>
            <Text style={styles.toastTitle}>
              <Text style={{ color: colors.cyan }}>nightjar</Text> invited you to a party
            </Text>
            <View style={styles.actions}>
              <View style={[styles.action, styles.accept]}>
                <Text style={styles.acceptText}>Accept</Text>
              </View>
              <View style={styles.action}>
                <Text style={styles.declineText}>Decline</Text>
              </View>
              <Text style={styles.timeout}>0:24</Text>
            </View>
          </View>
        </Toast>

        <Toast accent={colors.orange} style={styles.system}>
          <View style={styles.warnIcon}>
            <Text style={styles.warnGlyph}>!</Text>
          </View>
          <View style={{ flex: 1 }}>
            <Text style={styles.toastTitle}>Server restart in 5 minutes</Text>
            <Text style={styles.toastBody} numberOfLines={1}>Finish your encounter — progress is saved automatically at the next checkpoint.</Text>
          </View>
        </Toast>

        <View style={styles.mini}>
          <View style={styles.onlineDot} />
          <Text style={styles.miniText}>
            <Text style={{ fontWeight: '700', color: colors.text }}>pyrite</Text> is now online
          </Text>
        </View>
      </View>
    </ImageBackground>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#0b1a20' },
  shade: { ...StyleSheet.absoluteFillObject, backgroundColor: 'rgba(3,8,12,0.35)' },

  feed: { position: 'absolute', left: 28, top: 28, gap: 6, alignItems: 'flex-start' },
  feedRow: { flexDirection: 'row', alignItems: 'center', gap: 8, height: 30, paddingHorizontal: 12, borderRadius: 6, backgroundColor: 'rgba(0,0,0,0.5)' },
  feedMine: { borderWidth: 1, borderColor: 'rgba(255,255,255,0.55)', backgroundColor: 'rgba(79,140,255,0.25)' },
  feedName: { fontFamily: font, fontWeight: '700', fontSize: 13 },
  feedIcon: { width: 22, height: 22, transform: [{ scaleX: -1 }] },
  headshot: { width: 8, height: 8, borderRadius: 4, backgroundColor: colors.red, borderWidth: 1, borderColor: '#fff' },

  banner: { position: 'absolute', top: 450, left: 0, right: 0, alignItems: 'center' },
  bannerRule: { flexDirection: 'row', alignItems: 'center', gap: 12, width: 420 },
  rule: { flex: 1, height: 1 },
  diamond: { width: 8, height: 8, backgroundColor: colors.goldLight, transform: [{ rotate: '45deg' }] },
  bannerKicker: { fontFamily: font, fontWeight: '600', fontSize: 13, letterSpacing: 5, textTransform: 'uppercase', color: colors.goldLight, marginTop: 14 },
  bannerTitle: {
    fontFamily: font,
    fontWeight: '700',
    fontSize: 44,
    letterSpacing: 1,
    color: '#fff',
    marginTop: 6,
    textShadowColor: 'rgba(0,0,0,0.75)',
    textShadowOffset: { width: 0, height: 3 },
    textShadowRadius: 14,
  },
  bannerSub: { fontFamily: font, fontStyle: 'italic', fontSize: 15, color: colors.textDim, marginTop: 6 },

  stack: { position: 'absolute', right: 24, top: 24, width: 380, gap: 10, alignItems: 'stretch' },
  toast: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 12,
    padding: 12,
    borderRadius: 10,
    borderLeftWidth: 4,
    backgroundColor: 'rgba(10,14,26,0.92)',
    boxShadow: '0 12px 30px rgba(0,0,0,0.55)',
  },
  achievement: {
    experimental_backgroundImage: 'linear-gradient(100deg, rgba(242,195,91,0.22) 0%, rgba(10,14,26,0.92) 55%)',
    boxShadow: '0 12px 30px rgba(0,0,0,0.55), 0 0 0 1px rgba(242,195,91,0.35)',
  },
  trophy: {
    width: 52,
    height: 52,
    borderRadius: 26,
    alignItems: 'center',
    justifyContent: 'center',
    borderWidth: 2,
    borderColor: colors.gold,
    experimental_backgroundImage: 'radial-gradient(circle, rgba(242,195,91,0.45) 0%, rgba(40,26,6,0.95) 70%)',
    boxShadow: '0 0 16px rgba(242,195,91,0.5)',
  },
  points: { fontFamily: font, fontWeight: '700', fontSize: 18, color: colors.goldLight },
  loot: { boxShadow: '0 12px 30px rgba(0,0,0,0.55), 0 0 24px rgba(255,178,63,0.35)', borderWidth: 1, borderColor: 'rgba(255,178,63,0.5)', borderLeftWidth: 4 },
  lootIcon: {
    width: 52,
    height: 52,
    borderRadius: 10,
    alignItems: 'center',
    justifyContent: 'center',
    borderWidth: 1.5,
    borderColor: rarity.legendary.color,
    experimental_backgroundImage: 'radial-gradient(circle at 50% 40%, rgba(255,178,63,0.5) 0%, rgba(40,20,4,0.9) 75%)',
  },
  kicker: { fontFamily: font, fontWeight: '700', fontSize: 10, letterSpacing: 1.6, textTransform: 'uppercase' },
  toastTitle: { fontFamily: font, fontWeight: '600', fontSize: 15, color: colors.text, marginTop: 2 },
  toastBody: { fontFamily: font, fontSize: 12, color: colors.textDim, marginTop: 2 },
  avatar: { width: 44, height: 44, borderRadius: 22, borderWidth: 2, borderColor: colors.cyan },
  actions: { flexDirection: 'row', alignItems: 'center', gap: 8, marginTop: 8 },
  action: { height: 28, paddingHorizontal: 14, borderRadius: 6, justifyContent: 'center', borderWidth: 1, borderColor: colors.lineStrong },
  accept: { backgroundColor: colors.cyan, borderColor: colors.cyan },
  acceptText: { fontFamily: font, fontWeight: '700', fontSize: 12, color: '#04121c' },
  declineText: { fontFamily: font, fontWeight: '600', fontSize: 12, color: colors.textDim },
  timeout: { marginLeft: 'auto', fontFamily: font, fontWeight: '600', fontSize: 12, color: colors.textMute },
  system: { backgroundColor: 'rgba(30,18,6,0.92)' },
  warnIcon: { width: 32, height: 32, borderRadius: 6, backgroundColor: colors.orange, alignItems: 'center', justifyContent: 'center', transform: [{ rotate: '45deg' }], marginHorizontal: 4 },
  warnGlyph: { fontFamily: font, fontWeight: '700', fontSize: 18, color: '#2a1300', transform: [{ rotate: '-45deg' }] },
  mini: { flexDirection: 'row', alignItems: 'center', gap: 8, alignSelf: 'flex-end', paddingHorizontal: 12, paddingVertical: 7, borderRadius: 16, backgroundColor: 'rgba(10,14,26,0.8)', opacity: 0.85 },
  onlineDot: { width: 8, height: 8, borderRadius: 4, backgroundColor: colors.green, boxShadow: '0 0 8px rgba(74,222,128,0.9)' },
  miniText: { fontFamily: font, fontSize: 12, color: colors.textDim },
});
