// Feature: borders. Shape-only.
// Row 1: uniform, per-side widths, per-side colours, per-side colours + radius,
//        translucent border over a background.
// Row 2: hairline 1px, thick inner-radius clamp, per-side colours with uneven
//        widths, border on transparent background, borderStyle solid default.
import React from 'react';
import { StyleSheet, View } from 'react-native';

export const meta = { width: 640, height: 360, maxMismatch: 0.5, maxTileMismatch: 3, title: 'Borders' };

const sides = { borderTopColor: '#ff4d6a', borderRightColor: '#f2c35b', borderBottomColor: '#4ade80', borderLeftColor: '#4f8cff' };

export default function App() {
  return (
    <View style={styles.root}>
      <View style={styles.row}>
        <View style={[styles.box, { borderWidth: 4, borderColor: '#eef2ff' }]} />
        <View style={[styles.box, { borderTopWidth: 2, borderRightWidth: 8, borderBottomWidth: 14, borderLeftWidth: 20, borderColor: '#4fd1ff' }]} />
        <View style={[styles.box, { borderWidth: 10, ...sides }]} />
        <View style={[styles.box, { borderWidth: 10, borderRadius: 24, ...sides }]} />
        <View style={[styles.box, { borderWidth: 12, borderColor: 'rgba(255,255,255,0.35)', backgroundColor: '#b26bff' }]} />
      </View>
      <View style={styles.row}>
        <View style={[styles.box, { borderWidth: 1, borderColor: '#a3aecb' }]} />
        <View style={[styles.box, { borderWidth: 30, borderRadius: 20, borderColor: '#f2c35b', backgroundColor: '#ff4d6a' }]} />
        <View style={[styles.box, { borderTopWidth: 4, borderRightWidth: 16, borderBottomWidth: 8, borderLeftWidth: 24, borderRadius: 16, ...sides }]} />
        <View style={[styles.box, { borderWidth: 3, borderColor: '#4ade80', backgroundColor: 'transparent' }]} />
        <View style={[styles.box, { borderBottomWidth: 6, borderBottomColor: '#f2c35b', borderRadius: 8, backgroundColor: '#2a3150' }]} />
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#11162a', padding: 30, gap: 40 },
  row: { flexDirection: 'row', gap: 26 },
  box: { width: 100, height: 100, backgroundColor: '#2a3150' },
});
