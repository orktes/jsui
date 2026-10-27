// A standalone app, live: a game launcher in a desktop window (with a
// companion phone app beside it on wide layouts). Hover and press work; the
// update's progress bar and the hero's shimmer run on the native driver.
import React, { useEffect, useRef, useState } from 'react';
import { Animated, Easing, Pressable, StyleSheet, Text, View } from 'react-native';
import { colors, font, mono, site } from '../../theme.js';

const native = { useNativeDriver: true };
const NAV = ['Play', 'News', 'Store', 'Friends', 'Settings'];
const NEWS = [
  { title: 'Season 4: The Rift', sub: 'Patch notes', color: colors.purple },
  { title: 'Ashen Vault raid', sub: 'Event · 3 days left', color: colors.orange },
  { title: 'Ranked rewards', sub: 'Season 3 recap', color: colors.cyan },
];

export function LauncherApp({ width }) {
  const phoneBeside = width >= 620;
  const winW = phoneBeside ? width - 132 : width;
  return (
    <View style={{ width, paddingBottom: phoneBeside ? 34 : 0 }}>
      <Window width={winW} inset={phoneBeside ? 28 : 0} />
      {phoneBeside ? (
        <View style={styles.phoneAt}>
          <Phone />
        </View>
      ) : null}
    </View>
  );
}

function Window({ width, inset }) {
  const [tab, setTab] = useState(0);
  const narrow = width < 460;
  return (
    <View style={[styles.window, { width }]}>
      <View style={styles.titlebar}>
        <View style={[styles.light, { backgroundColor: '#ff5f57' }]} />
        <View style={[styles.light, { backgroundColor: '#febc2e' }]} />
        <View style={[styles.light, { backgroundColor: '#28c840' }]} />
        <Text style={styles.winTitle} numberOfLines={1}>
          Aetherfall Launcher
        </Text>
        <Text style={styles.winHost}>jsui-viewer</Text>
      </View>
      <View style={{ flexDirection: 'row' }}>
        {narrow ? null : (
          <View style={styles.sidebar}>
            {NAV.map((n, i) => (
              <Pressable
                key={n}
                onPress={() => setTab(i)}
                style={[styles.navItem, tab === i && styles.navActive]}
                hoveredStyle={styles.navHot}
                pressedStyle={{ transform: [{ scale: 0.96 }] }}
                transition={{ duration: 110, pressed: 40 }}
              >
                <View style={[styles.navDot, tab === i && { backgroundColor: colors.gold }]} />
                <Text style={[styles.navText, tab === i && { color: colors.text }]}>{n}</Text>
              </Pressable>
            ))}
          </View>
        )}
        <View style={[styles.main, inset ? { paddingRight: 14 + inset } : null]}>
          <Hero />
          <View style={styles.newsRow}>
            {NEWS.slice(0, narrow ? 2 : 3).map((n) => (
              <Pressable
                key={n.title}
                style={styles.news}
                hoveredStyle={{ borderColor: n.color + '88', transform: [{ translateY: -2 }] }}
                transition={{ duration: 140 }}
              >
                <View style={[styles.newsArt, { experimental_backgroundImage: `linear-gradient(135deg, ${n.color}66 0%, rgba(10,15,29,0) 100%)` }]} />
                <Text style={styles.newsTitle} numberOfLines={1}>
                  {n.title}
                </Text>
                <Text style={styles.newsSub} numberOfLines={1}>
                  {n.sub}
                </Text>
              </Pressable>
            ))}
          </View>
          <Update />
        </View>
      </View>
    </View>
  );
}

function Hero() {
  const shine = useRef(new Animated.Value(0)).current;
  useEffect(() => {
    const loop = Animated.loop(
      Animated.sequence([Animated.timing(shine, { toValue: 1, duration: 2400, easing: Easing.inOut(Easing.cubic), ...native }), Animated.delay(2600)]),
    );
    loop.start();
    return () => loop.stop();
  }, [shine]);
  return (
    <View style={styles.hero}>
      <Animated.View
        pointerEvents="none"
        style={[styles.shine, { transform: [{ translateX: shine.interpolate({ inputRange: [0, 1], outputRange: [-200, 700] }) }, { rotate: '18deg' }] }]}
      />
      <Text style={styles.heroEyebrow}>Season 4 · now live</Text>
      <Text style={styles.heroTitle}>AETHERFALL</Text>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10, marginTop: 8 }}>
        <Pressable
          style={styles.play}
          hoveredStyle={styles.playHot}
          focusVisibleStyle={{ borderColor: colors.text }}
          pressedStyle={{ transform: [{ scale: 0.95 }] }}
          transition={{ duration: 120, pressed: 40 }}
        >
          <Text style={styles.playText}>▶  Play</Text>
        </Pressable>
        <Text style={styles.heroMeta}>v4.0.2 · 3 friends online</Text>
      </View>
    </View>
  );
}

// An update downloading, forever: one native timing loop.
function Update() {
  const p = useRef(new Animated.Value(0)).current;
  useEffect(() => {
    const loop = Animated.loop(
      Animated.sequence([Animated.timing(p, { toValue: 1, duration: 6000, easing: Easing.inOut(Easing.quad), ...native }), Animated.delay(900)]),
    );
    loop.start();
    return () => loop.stop();
  }, [p]);
  return (
    <View style={styles.update}>
      <Text style={styles.updateText}>Downloading hotfix 4.0.3</Text>
      <View style={styles.track}>
        <Animated.View style={[styles.fill, { transform: [{ scaleX: p }] }]} />
      </View>
      <Text style={styles.updateMono}>84 MB</Text>
    </View>
  );
}

// The companion app: the same components on a phone.
function Phone() {
  return (
    <View style={styles.phone}>
      <View style={styles.notch} />
      <Text style={styles.phoneTitle}>Squad</Text>
      {[
        ['Nyx', colors.green, 'Lobby'],
        ['Kade', colors.cyan, 'Ranked'],
        ['Mira', colors.green, 'Online'],
      ].map(([n, c, s]) => (
        <View key={n} style={styles.phoneRow}>
          <View style={[styles.phoneDot, { backgroundColor: c }]} />
          <Text style={styles.phoneName}>{n}</Text>
          <Text style={styles.phoneSub}>{s}</Text>
        </View>
      ))}
      <View style={styles.phoneCard}>
        <Text style={styles.phoneCardTitle}>Daily reward</Text>
        <Text style={styles.phoneCardSub}>Claim 250 shards</Text>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  window: {
    borderRadius: 14,
    borderWidth: 1,
    borderColor: 'rgba(178, 107, 255, 0.32)',
    backgroundColor: '#080b16',
    overflow: 'hidden',
    boxShadow: '0 30px 80px rgba(0,0,0,0.6)',
  },
  titlebar: { height: 34, flexDirection: 'row', alignItems: 'center', gap: 7, paddingHorizontal: 12, backgroundColor: '#0d1222', borderBottomWidth: 1, borderBottomColor: site.stroke },
  light: { width: 11, height: 11, borderRadius: 6 },
  winTitle: { flex: 1, textAlign: 'center', fontFamily: font, fontWeight: '600', fontSize: 12, color: colors.textDim, marginRight: 4 },
  winHost: { fontFamily: mono, fontSize: 10.5, color: colors.textMute },
  sidebar: { width: 120, paddingVertical: 12, paddingHorizontal: 8, gap: 2, borderRightWidth: 1, borderRightColor: site.stroke },
  navItem: { height: 34, flexDirection: 'row', alignItems: 'center', gap: 9, paddingHorizontal: 10, borderRadius: 9, cursor: 'pointer' },
  navActive: { backgroundColor: 'rgba(242,195,91,0.08)' },
  navHot: { backgroundColor: 'rgba(255,255,255,0.05)' },
  navDot: { width: 6, height: 6, borderRadius: 3, backgroundColor: 'rgba(148,170,255,0.3)' },
  navText: { fontFamily: font, fontWeight: '600', fontSize: 13, color: colors.textDim },
  main: { flex: 1, padding: 14, gap: 12 },
  hero: {
    height: 170,
    borderRadius: 12,
    padding: 18,
    justifyContent: 'flex-end',
    overflow: 'hidden',
    experimental_backgroundImage:
      'radial-gradient(ellipse at 80% 20%, rgba(178,107,255,0.55) 0%, rgba(178,107,255,0) 55%), radial-gradient(ellipse at 20% 100%, rgba(255,154,60,0.35) 0%, rgba(255,154,60,0) 60%), linear-gradient(160deg, #1a1236 0%, #0a0f1d 100%)',
  },
  shine: { position: 'absolute', top: -60, width: 60, height: 320, backgroundColor: 'rgba(255,255,255,0.07)' },
  heroEyebrow: { fontFamily: font, fontWeight: '700', fontSize: 11, letterSpacing: 2, textTransform: 'uppercase', color: colors.goldLight },
  heroTitle: { fontFamily: font, fontWeight: '700', fontSize: 30, letterSpacing: 5, color: colors.text },
  play: { height: 36, paddingHorizontal: 20, borderRadius: 10, justifyContent: 'center', backgroundColor: colors.gold, borderWidth: 1, borderColor: 'transparent', cursor: 'pointer' },
  playHot: { backgroundColor: colors.goldLight },
  playText: { fontFamily: font, fontWeight: '700', fontSize: 14, color: '#1a1206' },
  heroMeta: { fontFamily: font, fontSize: 12, color: colors.textDim },
  newsRow: { flexDirection: 'row', gap: 10 },
  news: { flex: 1, padding: 10, gap: 3, borderRadius: 10, borderWidth: 1, borderColor: site.stroke, backgroundColor: 'rgba(255,255,255,0.02)', cursor: 'pointer' },
  newsArt: { height: 44, borderRadius: 7, marginBottom: 5 },
  newsTitle: { fontFamily: font, fontWeight: '700', fontSize: 12.5, color: colors.text },
  newsSub: { fontFamily: font, fontSize: 11, color: colors.textMute },
  update: { flexDirection: 'row', alignItems: 'center', gap: 10 },
  updateText: { fontFamily: font, fontSize: 12, color: colors.textDim },
  track: { flex: 1, height: 4, borderRadius: 2, backgroundColor: 'rgba(255,255,255,0.07)', overflow: 'hidden' },
  fill: { height: 4, width: '100%', backgroundColor: colors.purple, transformOrigin: 'left' },
  updateMono: { fontFamily: mono, fontSize: 11, color: colors.textMute },
  phoneAt: { position: 'absolute', right: 0, bottom: 0 },
  phone: {
    width: 150,
    height: 290,
    borderRadius: 26,
    borderWidth: 5,
    borderColor: '#1a2036',
    backgroundColor: '#070a14',
    padding: 12,
    paddingTop: 26,
    gap: 8,
    boxShadow: '0 30px 70px rgba(0,0,0,0.7)',
  },
  notch: { position: 'absolute', top: 8, alignSelf: 'center', width: 46, height: 10, borderRadius: 5, backgroundColor: '#1a2036' },
  phoneTitle: { fontFamily: font, fontWeight: '700', fontSize: 15, color: colors.text, marginBottom: 2 },
  phoneRow: { flexDirection: 'row', alignItems: 'center', gap: 7, height: 28, paddingHorizontal: 8, borderRadius: 8, backgroundColor: 'rgba(255,255,255,0.03)' },
  phoneDot: { width: 7, height: 7, borderRadius: 4 },
  phoneName: { flex: 1, fontFamily: font, fontWeight: '600', fontSize: 12, color: colors.text },
  phoneSub: { fontFamily: font, fontSize: 10.5, color: colors.textMute },
  phoneCard: {
    marginTop: 'auto',
    padding: 10,
    borderRadius: 10,
    experimental_backgroundImage: 'linear-gradient(135deg, rgba(242,195,91,0.30) 0%, rgba(178,107,255,0.18) 100%)',
    borderWidth: 1,
    borderColor: 'rgba(242,195,91,0.4)',
  },
  phoneCardTitle: { fontFamily: font, fontWeight: '700', fontSize: 12, color: colors.text },
  phoneCardSub: { fontFamily: font, fontSize: 10.5, color: colors.goldLight },
});
