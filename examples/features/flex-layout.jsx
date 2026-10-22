// Feature: Yoga flex layout. Shape-only.
// Left column: justifyContent variants (row), alignItems variants.
// Right: flexWrap + gap, flexGrow ratios, absolute insets + percentages,
//        aspectRatio, margin auto, zIndex overlap, negative margin.
import React from 'react';
import { StyleSheet, View } from 'react-native';

export const meta = { width: 720, height: 460, maxMismatch: 0.5, title: 'Flex layout' };

const C = ['#ff4d6a', '#f2c35b', '#4ade80', '#4fd1ff', '#b26bff', '#ff9a3c'];
const Dot = ({ i, w = 24, h = 24, style }) => <View style={[{ width: w, height: h, backgroundColor: C[i % C.length], borderRadius: 3 }, style]} />;

export default function App() {
  return (
    <View style={styles.root}>
      <View style={styles.col}>
        {['flex-start', 'center', 'flex-end', 'space-between', 'space-around', 'space-evenly'].map((j) => (
          <View key={j} style={[styles.strip, { justifyContent: j }]}>
            <Dot i={0} />
            <Dot i={1} />
            <Dot i={2} />
          </View>
        ))}
        <View style={[styles.strip, { height: 64, alignItems: 'stretch' }]}>
          <View style={{ width: 24, backgroundColor: C[3] }} />
          <View style={{ alignSelf: 'flex-start' }}>
            <Dot i={4} />
          </View>
          <View style={{ alignSelf: 'center' }}>
            <Dot i={5} />
          </View>
          <View style={{ alignSelf: 'flex-end' }}>
            <Dot i={0} />
          </View>
        </View>
      </View>

      <View style={styles.col}>
        <View style={[styles.panel, { flexDirection: 'row', flexWrap: 'wrap', rowGap: 6, columnGap: 10, padding: 8 }]}>
          {Array.from({ length: 13 }, (_, i) => (
            <Dot key={i} i={i} w={28 + (i % 3) * 10} h={18} />
          ))}
        </View>
        <View style={[styles.panel, { flexDirection: 'row', height: 36, padding: 6, gap: 6 }]}>
          <View style={{ flex: 1, backgroundColor: C[0] }} />
          <View style={{ flex: 2, backgroundColor: C[1] }} />
          <View style={{ flex: 3, backgroundColor: C[2] }} />
          <View style={{ width: 40, backgroundColor: C[3] }} />
        </View>
        <View style={[styles.panel, { height: 120 }]}>
          <View style={{ position: 'absolute', left: 10, top: 10, width: '30%', height: '40%', backgroundColor: C[4] }} />
          <View style={{ position: 'absolute', right: '10%', bottom: 10, width: 60, height: 60, backgroundColor: C[5], zIndex: 2 }} />
          <View style={{ position: 'absolute', right: '15%', bottom: 30, width: 60, height: 60, backgroundColor: C[3], zIndex: 1 }} />
          <View style={{ position: 'absolute', left: '40%', top: 20, height: 80, aspectRatio: 1.5, backgroundColor: C[0], borderRadius: 8 }} />
          <View style={{ position: 'absolute', left: 10, right: '70%', bottom: 10, height: 20, backgroundColor: C[1] }} />
        </View>
        <View style={[styles.panel, { flexDirection: 'row', height: 44, padding: 6, alignItems: 'center' }]}>
          <Dot i={2} />
          <Dot i={3} style={{ marginLeft: 'auto' }} />
          <Dot i={4} style={{ marginLeft: -8 }} />
          <Dot i={5} style={{ marginHorizontal: 20 }} />
        </View>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, flexDirection: 'row', backgroundColor: '#11162a', padding: 20, gap: 20 },
  col: { flex: 1, gap: 10 },
  strip: { flexDirection: 'row', height: 40, padding: 8, backgroundColor: '#1d2442', borderRadius: 4 },
  panel: { backgroundColor: '#1d2442', borderRadius: 4 },
});
