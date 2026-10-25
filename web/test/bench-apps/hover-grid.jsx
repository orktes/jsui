// Hover benchmark (web/test/hover-bench.mjs): a grid of cards whose hover
// feedback is all native state styles (no JS on hover). The same grid as
// plain HTML + CSS :hover is in hover-grid.html, for Chrome itself.
import React from 'react';
import { AppRegistry, StyleSheet, Text, View } from 'react-native';

const COLORS = ['#9aa3b8', '#4ade80', '#4f8cff', '#b26bff', '#ff9a3c'];
const cards = [];
for (let i = 0; i < 24; i++) cards.push({ id: i, color: COLORS[i % 5], name: `Item ${i + 1}` });

function Card({ card }) {
  return (
    <View
      group
      style={[styles.card, { borderColor: card.color }]}
      hoveredStyle={styles.cardHover}
      transition={{ duration: 120 }}
    >
      <View style={[styles.icon, { backgroundColor: card.color }]} />
      <Text style={styles.name} groupHoveredStyle={styles.nameHover} transition={{ duration: 120 }}>
        {card.name}
      </Text>
      <Text style={styles.sub}>Rare item</Text>
    </View>
  );
}

export default function App() {
  return (
    <View style={styles.root}>
      <View style={styles.grid}>
        {cards.map((c) => (
          <Card key={c.id} card={c} />
        ))}
      </View>
    </View>
  );
}
AppRegistry.registerComponent('HoverGrid', () => App);

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#0d1322', padding: 40 },
  grid: { flexDirection: 'row', flexWrap: 'wrap', gap: 18, width: 6 * 150 + 5 * 18 },
  card: {
    width: 150,
    height: 170,
    borderRadius: 12,
    borderWidth: 1.5,
    backgroundColor: '#141b2e',
    alignItems: 'center',
    paddingTop: 22,
    boxShadow: '0 0 8px rgba(79,140,255,0.35)',
  },
  cardHover: {
    transform: [{ scale: 1.06 }],
    borderColor: '#ffb23f',
    boxShadow: '0 0 0 3px rgba(255,178,63,0.35), 0 0 24px rgba(255,178,63,0.75)',
  },
  icon: { width: 64, height: 64, borderRadius: 10 },
  name: { marginTop: 16, fontSize: 15, fontWeight: '700', color: '#c9d2e8' },
  nameHover: { color: '#ffffff' },
  sub: { marginTop: 4, fontSize: 12, color: '#7d88a6' },
});
