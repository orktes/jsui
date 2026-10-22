// Feature: border radii. Shape-only (no text) so any diff is geometry.
// Row 1: uniform, per-corner, pill clamp, circle, logical corners.
// Row 2: radius + border, uneven border widths, overflow clipping a child,
//        nested clip, tiny radius.
import React from 'react';
import { StyleSheet, View } from 'react-native';

export const meta = { width: 640, height: 360, maxMismatch: 0.5, maxTileMismatch: 3, title: 'Border radius' };

export default function App() {
  return (
    <View style={styles.root}>
      <View style={styles.row}>
        <View style={[styles.box, { borderRadius: 16 }]} />
        <View style={[styles.box, { borderTopLeftRadius: 40, borderTopRightRadius: 4, borderBottomRightRadius: 24, borderBottomLeftRadius: 0 }]} />
        <View style={[styles.box, { height: 44, marginTop: 28, borderRadius: 999, backgroundColor: '#4fd1ff' }]} />
        <View style={[styles.box, { borderRadius: 50, backgroundColor: '#b26bff' }]} />
        <View style={[styles.box, { borderRadius: 8, borderTopStartRadius: 36, borderBottomEndRadius: 36, backgroundColor: '#4ade80' }]} />
      </View>
      <View style={styles.row}>
        <View style={[styles.box, { borderRadius: 20, borderWidth: 6, borderColor: '#ffe29a', backgroundColor: '#a8741f' }]} />
        <View style={[styles.box, { borderRadius: 28, borderTopWidth: 2, borderRightWidth: 10, borderBottomWidth: 18, borderLeftWidth: 4, borderColor: '#ff4d6a', backgroundColor: '#2a3150' }]} />
        <View style={[styles.box, { borderRadius: 30, overflow: 'hidden', backgroundColor: '#2a3150' }]}>
          <View style={{ position: 'absolute', left: -20, top: -20, width: 70, height: 70, backgroundColor: '#ff9a3c' }} />
          <View style={{ position: 'absolute', right: -10, bottom: -10, width: 50, height: 50, backgroundColor: '#4f8cff' }} />
        </View>
        <View style={[styles.box, { borderRadius: 24, overflow: 'hidden', backgroundColor: '#f2c35b', padding: 10 }]}>
          <View style={{ flex: 1, borderRadius: 14, overflow: 'hidden', backgroundColor: '#151d33' }}>
            <View style={{ width: '60%', height: '100%', backgroundColor: '#4fd1ff' }} />
          </View>
        </View>
        <View style={[styles.box, { borderRadius: 2, backgroundColor: '#eef2ff' }]} />
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#11162a', padding: 30, gap: 40 },
  row: { flexDirection: 'row', gap: 26, height: 100 },
  box: { width: 100, height: 100, backgroundColor: '#f2c35b' },
});
