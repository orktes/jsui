// Feature: virtualized lists. A 10,000 row FlatList opened in the middle
// (getItemLayout + initialScrollIndex), a 3-column FlatList grid, and a
// SectionList with headers and separators. Only a window of each list is
// mounted; the frame must still match react-native-web pixel for pixel.
import React from 'react';
import { FlatList, SectionList, StyleSheet, Text, View } from 'react-native';

export const meta = { width: 720, height: 400, maxMismatch: 1.5, title: 'Virtualized lists' };

const ROW = 36;
const rows = Array.from({ length: 10000 }, (_, i) => ({ id: 'r' + i, n: i, score: (i * 7919) % 10000 }));
const tiles = Array.from({ length: 600 }, (_, i) => ({ id: 't' + i, n: i }));
const TILE_COLORS = ['#ff4d6a', '#f2c35b', '#4ade80', '#4fd1ff', '#b26bff'];
const sections = Array.from({ length: 40 }, (_, s) => ({
  title: 'Chapter ' + (s + 1),
  data: Array.from({ length: 6 }, (_, i) => ({ id: s + '-' + i, label: 'Quest ' + (s + 1) + '.' + (i + 1) })),
}));

const getItemLayout = (_data, index) => ({ length: ROW, offset: ROW * index, index });

function Row({ item }) {
  return (
    <View style={[styles.row, item.n % 2 ? styles.rowOdd : null]}>
      <Text style={styles.rank}>#{item.n + 1}</Text>
      <Text style={styles.name}>Player {item.n}</Text>
      <Text style={styles.score}>{item.score}</Text>
    </View>
  );
}

export default function App() {
  return (
    <View style={styles.root}>
      <View style={styles.panel}>
        <Text style={styles.title}>Leaderboard</Text>
        <FlatList
          style={styles.list}
          data={rows}
          renderItem={({ item }) => <Row item={item} />}
          keyExtractor={(item) => item.id}
          getItemLayout={getItemLayout}
          initialScrollIndex={4995}
          showsVerticalScrollIndicator={false}
        />
      </View>
      <View style={styles.panel}>
        <Text style={styles.title}>Inventory</Text>
        <FlatList
          style={styles.list}
          data={tiles}
          numColumns={3}
          columnWrapperStyle={styles.tileRow}
          renderItem={({ item }) => (
            <View style={[styles.tile, { backgroundColor: TILE_COLORS[item.n % TILE_COLORS.length] }]}>
              <Text style={styles.tileText}>{item.n}</Text>
            </View>
          )}
          ItemSeparatorComponent={() => <View style={styles.gap} />}
          showsVerticalScrollIndicator={false}
        />
      </View>
      <View style={styles.panel}>
        <Text style={styles.title}>Quests</Text>
        <SectionList
          style={styles.list}
          sections={sections}
          keyExtractor={(item) => item.id}
          renderSectionHeader={({ section }) => <Text style={styles.header}>{section.title}</Text>}
          renderItem={({ item }) => <Text style={styles.quest}>{item.label}</Text>}
          ItemSeparatorComponent={() => <View style={styles.separator} />}
          showsVerticalScrollIndicator={false}
        />
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, flexDirection: 'row', padding: 16, gap: 16, backgroundColor: '#0d1020' },
  panel: { flex: 1, backgroundColor: '#161b33', borderRadius: 12, padding: 12, overflow: 'hidden' },
  title: { color: '#f2c35b', fontSize: 16, fontWeight: '700', marginBottom: 8 },
  list: { flex: 1 },
  row: { height: ROW, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 8, backgroundColor: '#1d2342' },
  rowOdd: { backgroundColor: '#232a50' },
  rank: { width: 56, color: '#8b93c9', fontSize: 12 },
  name: { flex: 1, color: '#e8ebff', fontSize: 13 },
  score: { color: '#4ade80', fontSize: 13, fontWeight: '600' },
  tileRow: { gap: 8 },
  tile: { flex: 1, height: 48, borderRadius: 8, alignItems: 'center', justifyContent: 'center' },
  tileText: { color: '#0d1020', fontSize: 14, fontWeight: '700' },
  gap: { height: 8 },
  header: { color: '#4fd1ff', fontSize: 13, fontWeight: '700', paddingVertical: 6, backgroundColor: '#161b33' },
  quest: { color: '#e8ebff', fontSize: 13, paddingVertical: 8, paddingLeft: 8 },
  separator: { height: 1, backgroundColor: 'rgba(255,255,255,0.08)' },
});
