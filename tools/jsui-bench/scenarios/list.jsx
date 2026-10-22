// jsui-bench "list": a static 2000-row list (not virtualized) with one
// counter that changes every frame. Everything but one Text is unchanged, so
// the frame cost should not grow with the row count.
import React, { useEffect, useState } from 'react';
import { AppRegistry, StyleSheet, Text, View } from 'react-native';

const ROWS = 2000;
const rows = [];
for (let i = 0; i < ROWS; i++) rows.push({ id: i, name: `Item ${i}`, level: 1 + (i % 60), gold: (i * 37) % 1000 });

const Row = React.memo(function Row({ row }) {
  return (
    <View style={styles.row}>
      <View style={[styles.icon, { backgroundColor: row.id % 3 ? '#2a3150' : '#6a4aa0' }]} />
      <Text style={styles.name}>{row.name}</Text>
      <Text style={styles.meta}>Lv {row.level}</Text>
      <Text style={styles.gold}>{row.gold} g</Text>
    </View>
  );
});

function Counter() {
  const [n, setN] = useState(0);
  useEffect(() => {
    let raf = 0;
    const step = () => {
      setN((v) => v + 1);
      raf = requestAnimationFrame(step);
    };
    raf = requestAnimationFrame(step);
    return () => cancelAnimationFrame(raf);
  }, []);
  return <Text style={styles.counter}>Frame {n}</Text>;
}

function App() {
  return (
    <View style={styles.root}>
      <View style={styles.header}>
        <Text style={styles.title}>Inventory log</Text>
        <Counter />
      </View>
      <View style={styles.list}>
        {rows.map((r) => (
          <Row key={r.id} row={r} />
        ))}
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#0d1322' },
  header: { flexDirection: 'row', justifyContent: 'space-between', padding: 12, borderBottomWidth: 1, borderColor: '#334' },
  title: { color: '#fff', fontSize: 18, fontWeight: '700' },
  counter: { color: '#f2c35b', fontSize: 16 },
  list: { overflow: 'hidden', flex: 1 },
  row: { flexDirection: 'row', alignItems: 'center', height: 28, paddingHorizontal: 12, gap: 10 },
  icon: { width: 18, height: 18, borderRadius: 4 },
  name: { flex: 1, color: '#dde', fontSize: 13 },
  meta: { width: 50, color: '#99a', fontSize: 12 },
  gold: { width: 60, color: '#f2c35b', fontSize: 12, textAlign: 'right' },
});

AppRegistry.registerComponent('bench', () => App);
