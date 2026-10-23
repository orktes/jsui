// jsui-bench "inventory": a 500-slot grid fed from host state. The bench
// calls jsui_patch_state('inventory', {"slots": {"<i>": {"count": n}}}) every
// frame, so one slot changes per frame; unchanged slots keep their identity
// (useHostState) and React.memo skips them.
import React from 'react';
import { AppRegistry, StyleSheet, Text, View } from 'react-native';
import { useHostState } from '@orktes/jsui-react';

const N = 500;
const initial = { slots: {} };
for (let i = 0; i < N; i++) initial.slots[i] = { item: `item-${i % 40}`, count: (i * 7) % 20, rarity: i % 5 };
const RARITY = ['#9aa3b8', '#4ade80', '#4f8cff', '#b26bff', '#ff9a3c'];

const Slot = React.memo(function Slot({ slot }) {
  return (
    <View style={[styles.slot, { borderColor: RARITY[slot.rarity] }]}>
      <View style={[styles.icon, { backgroundColor: RARITY[slot.rarity] }]} />
      {slot.count > 0 ? <Text style={styles.count}>{slot.count}</Text> : null}
    </View>
  );
});

function App() {
  const inv = useHostState('inventory', initial);
  const cells = [];
  for (let i = 0; i < N; i++) {
    const s = inv.slots[i] || initial.slots[i];
    cells.push(<Slot key={i} slot={s} />);
  }
  return (
    <View style={styles.root}>
      <Text style={styles.title}>Bag (500)</Text>
      <View style={styles.grid}>{cells}</View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#0d1322', padding: 16 },
  title: { color: '#fff', fontSize: 18, fontWeight: '700', marginBottom: 8 },
  grid: { flexDirection: 'row', flexWrap: 'wrap', gap: 4 },
  slot: { width: 44, height: 44, borderRadius: 6, borderWidth: 1, backgroundColor: '#151b33' },
  icon: { position: 'absolute', left: 8, top: 8, width: 26, height: 26, borderRadius: 4, opacity: 0.8 },
  count: { position: 'absolute', right: 3, bottom: 1, color: '#fff', fontSize: 11, fontWeight: '700' },
});

AppRegistry.registerComponent('bench', () => App);
