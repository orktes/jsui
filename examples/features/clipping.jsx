// Feature: overflow clipping, zIndex and stacking. Shape-only.
// Row 1: overflow hidden (rect), with radius around a rotated child, around an
//        image, nested clips with different radii, overflow visible (default).
// Row 2: zIndex reorders siblings, negative zIndex, absolute child escaping
//        an unclipped parent, clipped scrolling-style content strip.
import React from 'react';
import { Image, StyleSheet, View } from 'react-native';

export const meta = { width: 640, height: 360, maxMismatch: 0.5, title: 'Clipping & stacking' };

const C = ['#ff4d6a', '#f2c35b', '#4ade80', '#4fd1ff', '#b26bff'];

export default function App() {
  return (
    <View style={styles.root}>
      <View style={styles.row}>
        <View style={[styles.box, { overflow: 'hidden' }]}>
          <View style={{ position: 'absolute', left: 50, top: 50, width: 100, height: 100, backgroundColor: C[0] }} />
        </View>
        <View style={[styles.box, { overflow: 'hidden', borderRadius: 50 }]}>
          <View style={{ position: 'absolute', left: 20, top: -20, width: 60, height: 140, backgroundColor: C[1], transform: [{ rotate: '35deg' }] }} />
        </View>
        <View style={[styles.box, { overflow: 'hidden', borderTopLeftRadius: 40, borderBottomRightRadius: 40 }]}>
          <Image source={require('../assets/card-tide.png')} style={{ width: 180, height: 100, marginLeft: -40 }} />
        </View>
        <View style={[styles.box, { overflow: 'hidden', borderRadius: 30, padding: 8, backgroundColor: C[4] }]}>
          <View style={{ flex: 1, overflow: 'hidden', borderRadius: 4, backgroundColor: '#11162a' }}>
            <View style={{ position: 'absolute', left: -10, top: -10, width: 50, height: 50, borderRadius: 25, backgroundColor: C[2] }} />
          </View>
        </View>
        <View style={styles.box}>
          <View style={{ position: 'absolute', left: 60, top: 60, width: 60, height: 60, backgroundColor: C[3] }} />
        </View>
      </View>
      <View style={styles.row}>
        <View style={styles.box}>
          <View style={[styles.chip, { left: 10, top: 10, backgroundColor: C[0], zIndex: 3 }]} />
          <View style={[styles.chip, { left: 30, top: 30, backgroundColor: C[1], zIndex: 1 }]} />
          <View style={[styles.chip, { left: 50, top: 50, backgroundColor: C[2], zIndex: 2 }]} />
        </View>
        <View style={styles.box}>
          <View style={[styles.chip, { left: 20, top: 20, backgroundColor: C[3] }]} />
          <View style={[styles.chip, { left: 40, top: 40, backgroundColor: C[4], zIndex: -1 }]} />
        </View>
        <View style={[styles.box, { backgroundColor: 'transparent' }]}>
          <View style={{ width: 60, height: 60, backgroundColor: '#2a3150' }}>
            <View style={{ position: 'absolute', left: 40, top: 40, width: 50, height: 50, borderRadius: 25, backgroundColor: C[1] }} />
          </View>
        </View>
        <View style={[styles.box, { width: 216, overflow: 'hidden', flexDirection: 'row', gap: 8, padding: 10, borderRadius: 12 }]}>
          {C.map((c, i) => (
            <View key={c} style={{ width: 60, height: 80, borderRadius: 8, backgroundColor: c, marginLeft: i === 0 ? -30 : 0 }} />
          ))}
        </View>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#11162a', padding: 30, gap: 40 },
  row: { flexDirection: 'row', gap: 26 },
  box: { width: 100, height: 100, backgroundColor: '#2a3150' },
  chip: { position: 'absolute', width: 40, height: 40, borderRadius: 6 },
});
