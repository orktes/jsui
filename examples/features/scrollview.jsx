// Feature: ScrollView. Initial content offsets (vertical with padding and a
// rounded clip, horizontal card strip), partially visible rows clipped at
// the viewport, sticky headers and a list that is not scrolled.
// Indicators are hidden: overlay scrollbars differ from Chrome's.
import React, { useLayoutEffect, useRef } from 'react';
import { Platform, ScrollView, StyleSheet, Text, View } from 'react-native';

export const meta = { width: 640, height: 360, maxMismatch: 1.0, title: 'ScrollView' };

const C = ['#ff4d6a', '#f2c35b', '#4ade80', '#4fd1ff', '#b26bff'];
const rows = Array.from({ length: 12 }, (_, i) => i);
const hidden = { showsVerticalScrollIndicator: false, showsHorizontalScrollIndicator: false };

// React Native applies contentOffset natively; react-native-web ignores it,
// so the browser reference scrolls through the ref instead.
function useInitialOffset(offset) {
  const ref = useRef(null);
  useLayoutEffect(() => {
    if (Platform.OS === 'web' && ref.current) ref.current.scrollTo({ ...offset, animated: false });
  }, []);
  return ref;
}

export default function App() {
  const listOffset = { x: 0, y: 130 };
  const stripOffset = { x: 150, y: 0 };
  const stickyOffset = { x: 0, y: 60 };
  const list = useInitialOffset(listOffset);
  const strip = useInitialOffset(stripOffset);
  const sticky = useInitialOffset(stickyOffset);
  return (
    <View style={styles.root}>
      <View style={styles.col}>
        <Text style={styles.label}>contentOffset y 130</Text>
        <ScrollView ref={list} {...hidden} contentOffset={listOffset} style={styles.list} contentContainerStyle={styles.listContent}>
          {rows.map((i) => (
            <View key={i} style={[styles.row, { borderLeftColor: C[i % C.length] }]}>
              <Text style={styles.rowText}>Quest {i + 1}</Text>
              <Text style={styles.rowMeta}>{(i + 1) * 120} XP</Text>
            </View>
          ))}
        </ScrollView>
      </View>
      <View style={styles.col}>
        <Text style={styles.label}>horizontal, x 150</Text>
        <ScrollView ref={strip} {...hidden} horizontal contentOffset={stripOffset} style={styles.strip} contentContainerStyle={styles.stripContent}>
          {rows.slice(0, 8).map((i) => (
            <View key={i} style={[styles.card, { backgroundColor: C[i % C.length] }]}>
              <Text style={styles.cardText}>{i + 1}</Text>
            </View>
          ))}
        </ScrollView>
        <Text style={styles.label}>not scrolled</Text>
        <ScrollView {...hidden} style={styles.small}>
          {rows.slice(0, 5).map((i) => (
            <View key={i} style={[styles.thin, { backgroundColor: C[i % C.length] }]} />
          ))}
        </ScrollView>
      </View>
      <View style={styles.col}>
        <Text style={styles.label}>sticky headers, y 60</Text>
        <ScrollView ref={sticky} {...hidden} contentOffset={stickyOffset} stickyHeaderIndices={[0, 3]} style={styles.sticky}>
          <View style={styles.header}>
            <Text style={styles.headerText}>Weapons</Text>
          </View>
          <View style={styles.item} />
          <View style={[styles.item, { backgroundColor: '#2f3a63' }]} />
          <View style={styles.header}>
            <Text style={styles.headerText}>Armor</Text>
          </View>
          <View style={styles.item} />
          <View style={[styles.item, { backgroundColor: '#2f3a63' }]} />
          <View style={styles.item} />
        </ScrollView>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, flexDirection: 'row', backgroundColor: '#11162a', padding: 24, gap: 24 },
  col: { gap: 8 },
  label: { color: '#a3aecb', fontSize: 12, fontFamily: 'Inter' },
  list: { width: 180, height: 260, flexGrow: 0, borderRadius: 12, backgroundColor: '#1d2442' },
  listContent: { padding: 10, gap: 6 },
  row: { height: 40, borderLeftWidth: 4, paddingHorizontal: 10, backgroundColor: '#2a3150', flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between' },
  rowText: { color: '#ffffff', fontSize: 14, fontWeight: '600', fontFamily: 'Inter' },
  rowMeta: { color: '#f2c35b', fontSize: 12, fontFamily: 'Inter' },
  strip: { width: 200, height: 100, flexGrow: 0, backgroundColor: '#1d2442' },
  stripContent: { padding: 10, gap: 10 },
  card: { width: 80, height: 80, alignItems: 'center', justifyContent: 'center' },
  cardText: { color: '#11162a', fontSize: 28, fontWeight: '700', fontFamily: 'Inter' },
  small: { width: 200, height: 90, flexGrow: 0, backgroundColor: '#1d2442' },
  thin: { height: 24, marginBottom: 4 },
  sticky: { width: 140, height: 200, flexGrow: 0, backgroundColor: '#1d2442' },
  header: { height: 30, backgroundColor: '#4fd1ff', justifyContent: 'center', paddingHorizontal: 8 },
  headerText: { color: '#11162a', fontSize: 13, fontWeight: '700', fontFamily: 'Inter' },
  item: { height: 50, backgroundColor: '#2a3150' },
});
