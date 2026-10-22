// Feature: shadows. Shape-only.
// Row 1 (boxShadow strings): offset blur, spread, multiple, coloured glow,
//        on a rounded box.
// Row 2: inset, inset + spread + radius, outer + inset combined, RN object
//        array syntax, legacy shadowColor/Offset/Opacity/Radius.
import React from 'react';
import { StyleSheet, View } from 'react-native';

export const meta = { width: 680, height: 380, maxMismatch: 0.5, title: 'Shadows' };

export default function App() {
  return (
    <View style={styles.root}>
      <View style={styles.row}>
        <View style={[styles.box, { boxShadow: '6px 8px 12px rgba(0,0,0,0.6)' }]} />
        <View style={[styles.box, { boxShadow: '0 0 0 8px #4f8cff' }]} />
        <View style={[styles.box, { boxShadow: '-10px -10px 0 #ff4d6a, 10px 10px 0 #4ade80, 0 0 30px rgba(255,255,255,0.4)' }]} />
        <View style={[styles.box, { backgroundColor: '#f2c35b', boxShadow: '0 0 24px 4px rgba(242,195,91,0.8)' }]} />
        <View style={[styles.box, { borderRadius: 24, boxShadow: '0 16px 24px -6px rgba(0,0,0,0.9)' }]} />
      </View>
      <View style={styles.row}>
        <View style={[styles.box, { boxShadow: 'inset 0 0 18px rgba(79,209,255,0.9)' }]} />
        <View style={[styles.box, { borderRadius: 20, boxShadow: 'inset 6px 6px 0 6px rgba(255,77,106,0.8)' }]} />
        <View style={[styles.box, { borderRadius: 12, boxShadow: '0 10px 20px rgba(0,0,0,0.7), inset 0 2px 0 rgba(255,255,255,0.5), inset 0 -16px 20px rgba(0,0,0,0.45)' }]} />
        <View
          style={[
            styles.box,
            {
              borderRadius: 50,
              boxShadow: [
                { offsetX: 0, offsetY: 0, blurRadius: 20, spreadDistance: 2, color: 'rgba(178,107,255,0.9)' },
                { offsetX: 0, offsetY: 0, blurRadius: 6, color: '#ffffff', inset: true },
              ],
            },
          ]}
        />
        <View style={[styles.box, { borderRadius: 10, shadowColor: '#000', shadowOffset: { width: 0, height: 10 }, shadowOpacity: 0.8, shadowRadius: 14 }]} />
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#2a3150', padding: 40, gap: 60 },
  row: { flexDirection: 'row', gap: 36 },
  box: { width: 96, height: 96, backgroundColor: '#5b6a96' },
});
