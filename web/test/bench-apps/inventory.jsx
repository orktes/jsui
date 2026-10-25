// Self-driving inventory (same React work in JSUI and react-native-web): 500
// memo slots in React state; every frame one slot's count changes.
// docs/PERFORMANCE.md "Web":
//   node web/test/frame-bench.mjs --real-gpu --renderers webgpu,chrome --settle 1500 web/test/bench-apps/inventory.jsx
import React, { useEffect, useState } from 'react';
import { AppRegistry, StyleSheet, Text, View } from 'react-native';

const N = 500;
const RARITY = ['#9aa3b8', '#4ade80', '#4f8cff', '#b26bff', '#ff9a3c'];
const initial = [];
for (let i = 0; i < N; i++) initial.push({ item: `item-${i % 40}`, count: (i * 7) % 20, rarity: i % 5 });
const Slot = React.memo(function Slot({ slot }) {
  return (
    <View style={[styles.slot, { borderColor: RARITY[slot.rarity] }]}>
      <View style={[styles.icon, { backgroundColor: RARITY[slot.rarity] }]} />
      {slot.count > 0 ? <Text style={styles.count}>{slot.count}</Text> : null}
    </View>
  );
});
export default function App() {
  const [slots, setSlots] = useState(initial);
  useEffect(() => {
    let f = 0;
    let raf = requestAnimationFrame(function step() {
      f++;
      setSlots((s) => { const n = s.slice(); const i = (f * 37) % N; n[i] = { ...n[i], count: (n[i].count + 1) % 20 }; return n; });
      raf = requestAnimationFrame(step);
    });
    return () => cancelAnimationFrame(raf);
  }, []);
  return (
    <View style={styles.root}>
      <Text style={styles.title}>Bag (500)</Text>
      <View style={styles.grid}>{slots.map((s, i) => <Slot key={i} slot={s} />)}</View>
    </View>
  );
}
AppRegistry.registerComponent('Inventory', () => App);
const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#0d1322', padding: 16 },
  title: { color: '#fff', fontSize: 18, fontWeight: '700', marginBottom: 8 },
  grid: { flexDirection: 'row', flexWrap: 'wrap', gap: 4 },
  slot: { width: 44, height: 44, borderRadius: 6, borderWidth: 1, backgroundColor: '#151b33' },
  icon: { position: 'absolute', left: 8, top: 8, width: 26, height: 26, borderRadius: 4, opacity: 0.8 },
  count: { position: 'absolute', right: 3, bottom: 1, color: '#fff', fontSize: 11, fontWeight: '700' },
});
