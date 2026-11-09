// Paint bench: a tablet shell's app launcher opening and closing over and
// over: a full-screen wallpaper under a dimming scrim that fades, a search
// field with a shadow, and a grid of 24 app tiles (icon in a rounded square,
// two-line label) fading and scaling 0.94 -> 1 as one layer, 12 frames each
// way with 12 still frames between. Meant for 1143 x 686 @ 1.75 (2000 x 1200).
import React, { useEffect, useRef } from 'react';
import { Animated, AppRegistry, Image, StyleSheet, Text, View } from 'react-native';

const ICONS = [
  require('../../../examples/assets/item-amulet.png'),
  require('../../../examples/assets/item-axe.png'),
  require('../../../examples/assets/item-boots.png'),
  require('../../../examples/assets/item-bow.png'),
  require('../../../examples/assets/item-gem.png'),
  require('../../../examples/assets/item-helmet.png'),
  require('../../../examples/assets/item-potion-health.png'),
  require('../../../examples/assets/item-potion-mana.png'),
  require('../../../examples/assets/item-ring.png'),
  require('../../../examples/assets/item-scroll.png'),
  require('../../../examples/assets/item-shield.png'),
  require('../../../examples/assets/item-sword.png'),
];
const WALL = require('../../../examples/assets/bg-menu.jpg');
const NAMES = ['Terminal', 'Firefox', 'Files', 'Settings', 'Claude', 'Camera', 'Gallery', 'Music', 'Clock', 'Notes',
  'Calendar', 'Weather'];
const TINTS = ['#3b82f6', '#f97316', '#10b981', '#8b5cf6', '#ef4444', '#eab308'];
const APPS = Array.from({ length: 24 }, (_, i) => i);

function Tile({ i }) {
  return (
    <View style={styles.tile}>
      <View style={[styles.icon, { backgroundColor: TINTS[i % TINTS.length] }]}>
        <Image source={ICONS[i % ICONS.length]} style={styles.glyph} />
      </View>
      <Text numberOfLines={2} style={styles.name}>{`${NAMES[i % NAMES.length]}${i >= 12 ? ' 2' : ''}`}</Text>
    </View>
  );
}

function App() {
  const v = useRef(new Animated.Value(0)).current;
  const sc = useRef(new Animated.Value(0.94)).current;
  useEffect(() => {
    let f = 0;
    let raf = 0;
    const ease = (x) => 1 - Math.pow(1 - x, 3);
    const step = () => {
      const p = f % 48;
      const k = p < 12 ? ease(p / 12) : p < 24 ? 1 : p < 36 ? 1 - ease((p - 24) / 12) : 0;
      v.setValue(k);
      sc.setValue(0.94 + 0.06 * k);
      f++;
      raf = requestAnimationFrame(step);
    };
    raf = requestAnimationFrame(step);
    return () => cancelAnimationFrame(raf);
  }, [v, sc]);
  return (
    <View style={styles.root}>
      <Image source={WALL} style={StyleSheet.absoluteFill} resizeMode="cover" />
      <Animated.View style={[StyleSheet.absoluteFill, { opacity: v }]}>
        <View style={[StyleSheet.absoluteFill, { backgroundColor: 'rgba(6,8,20,0.55)' }]} />
      </Animated.View>
      <Animated.View style={[styles.launcher, { opacity: v, transform: [{ scale: sc }] }]}>
        <View style={styles.search}>
          <Text style={styles.searchText}>Search apps</Text>
        </View>
        <View style={styles.grid}>
          {APPS.map((i) => (
            <Tile key={i} i={i} />
          ))}
        </View>
      </Animated.View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#000' },
  launcher: { position: 'absolute', left: 0, right: 0, top: 60, bottom: 40, alignItems: 'center' },
  search: {
    width: 868, height: 56, borderRadius: 28, paddingHorizontal: 24, justifyContent: 'center',
    backgroundColor: 'rgba(30,34,52,0.92)', borderWidth: 1, borderColor: 'rgba(255,255,255,0.12)',
    boxShadow: '0px 12px 36px rgba(0,0,0,0.45)',
  },
  searchText: { color: 'rgba(255,255,255,0.5)', fontSize: 20 },
  grid: { width: 900, marginTop: 28, flexDirection: 'row', flexWrap: 'wrap' },
  tile: { width: 150, height: 150, alignItems: 'center', paddingTop: 12, borderRadius: 18 },
  icon: { width: 72, height: 72, borderRadius: 20, alignItems: 'center', justifyContent: 'center' },
  glyph: { width: 52, height: 52 },
  name: { marginTop: 8, color: '#fff', fontSize: 15, textAlign: 'center', width: 130 },
});

AppRegistry.registerComponent('App', () => App);
