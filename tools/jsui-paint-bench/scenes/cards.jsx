// Paint bench: a grid of cards with blurred shadows, rounded clips (images
// and gradient bars inside overflow: hidden) scrolling continuously (6 px per
// frame, back and forth) beside a static sidebar. 1280 x 720.
import React, { useEffect, useRef } from 'react';
import { AppRegistry, Image, ScrollView, StyleSheet, Text, View } from 'react-native';

const ART = [
  require('../../../examples/assets/card-ember.png'),
  require('../../../examples/assets/card-tide.png'),
  require('../../../examples/assets/card-verdant.png'),
  require('../../../examples/assets/card-void.png'),
];
const NAMES = ['Ember Warden', 'Tidecaller', 'Verdant Oath', 'Voidwalker', 'Stormbinder', 'Ashen Knight'];
const CARDS = Array.from({ length: 48 }, (_, i) => i);

function Card({ i }) {
  return (
    <View style={styles.card}>
      <Image source={ART[i % ART.length]} style={styles.art} resizeMode="cover" />
      <View style={styles.badge}>
        <Text style={styles.badgeText}>{`#${i + 1}`}</Text>
      </View>
      <View style={styles.body}>
        <Text style={styles.title}>{NAMES[i % NAMES.length]}</Text>
        <Text style={styles.sub}>{`Level ${10 + (i * 7) % 50} · ${['Rare', 'Epic', 'Legendary'][i % 3]}`}</Text>
        <View style={styles.bar}>
          <View style={[styles.fill, { width: `${20 + (i * 13) % 80}%` }]} />
        </View>
      </View>
    </View>
  );
}

function App() {
  const ref = useRef(null);
  useEffect(() => {
    let y = 0;
    let dir = 1;
    let raf = 0;
    const step = () => {
      y += 6 * dir;
      if (y > 1800 || y < 0) dir = -dir;
      if (ref.current) ref.current.scrollTo({ y, animated: false });
      raf = requestAnimationFrame(step);
    };
    raf = requestAnimationFrame(step);
    return () => cancelAnimationFrame(raf);
  }, []);
  return (
    <View style={styles.root}>
      <View style={styles.side}>
        <Text style={styles.sideTitle}>Collection</Text>
        {['All cards', 'Heroes', 'Spells', 'Relics', 'Favourites', 'Recently added'].map((l, i) => (
          <View key={l} style={[styles.sideItem, i === 0 && styles.sideItemOn]}>
            <Text style={styles.sideText}>{l}</Text>
          </View>
        ))}
      </View>
      <ScrollView ref={ref} style={styles.scroll} contentContainerStyle={styles.grid}>
        {CARDS.map((i) => (
          <Card key={i} i={i} />
        ))}
      </ScrollView>
    </View>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, flexDirection: 'row', backgroundColor: '#0b1020' },
  side: { width: 240, padding: 20, gap: 8, backgroundColor: '#111830', borderRightWidth: 1, borderRightColor: 'rgba(255,255,255,0.08)' },
  sideTitle: { color: '#fff', fontSize: 22, fontWeight: '700', marginBottom: 12 },
  sideItem: { paddingVertical: 10, paddingHorizontal: 14, borderRadius: 10 },
  sideItemOn: { backgroundColor: 'rgba(120,140,255,0.18)', borderWidth: 1, borderColor: 'rgba(120,140,255,0.4)' },
  sideText: { color: '#cfd6ff', fontSize: 15 },
  scroll: { flex: 1 },
  grid: { flexDirection: 'row', flexWrap: 'wrap', padding: 24, gap: 24 },
  card: {
    width: 230,
    height: 300,
    borderRadius: 18,
    overflow: 'hidden',
    backgroundColor: '#182041',
    borderWidth: 1,
    borderColor: 'rgba(255,255,255,0.12)',
    boxShadow: '0 12px 28px rgba(0,0,0,0.55)',
  },
  art: { width: '100%', height: 170 },
  badge: { position: 'absolute', top: 10, right: 10, paddingHorizontal: 8, paddingVertical: 3, borderRadius: 10, backgroundColor: 'rgba(0,0,0,0.6)' },
  badgeText: { color: '#f2c35b', fontSize: 12, fontWeight: '700' },
  body: { padding: 14, gap: 6 },
  title: { color: '#fff', fontSize: 17, fontWeight: '700' },
  sub: { color: '#9aa3c7', fontSize: 13 },
  bar: { height: 8, borderRadius: 4, overflow: 'hidden', backgroundColor: 'rgba(255,255,255,0.1)', marginTop: 6 },
  fill: { height: '100%', borderRadius: 4, experimental_backgroundImage: 'linear-gradient(90deg, #ff4d6a, #f2c35b)' },
});

AppRegistry.registerComponent('Cards', () => App);
export default App;
