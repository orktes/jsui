// Feature: transforms and opacity. Shape-only.
// Row 1: rotate, scale, translate, rotate+scale order, transformOrigin.
// Row 2: rotated parent with children, skewX, scaleX mirror, group opacity
//        over overlapping children, nested opacity.
import React from 'react';
import { StyleSheet, View } from 'react-native';

export const meta = { width: 640, height: 380, maxMismatch: 0.5, maxTileMismatch: 3, title: 'Transforms & opacity' };

function Cell({ children }) {
  return <View style={styles.cell}>{children}</View>;
}

export default function App() {
  return (
    <View style={styles.root}>
      <View style={styles.row}>
        <Cell>
          <View style={[styles.box, { transform: [{ rotate: '30deg' }] }]} />
        </Cell>
        <Cell>
          <View style={[styles.box, { transform: [{ scale: 0.6 }], backgroundColor: '#4fd1ff' }]} />
        </Cell>
        <Cell>
          <View style={[styles.box, { transform: [{ translateX: 18 }, { translateY: -14 }], backgroundColor: '#4ade80' }]} />
        </Cell>
        <Cell>
          <View style={[styles.box, { transform: [{ translateX: 20 }, { rotate: '45deg' }, { scale: 0.7 }], backgroundColor: '#b26bff' }]} />
        </Cell>
        <Cell>
          <View style={[styles.box, { transformOrigin: 'top left', transform: [{ rotate: '20deg' }], backgroundColor: '#ff9a3c' }]} />
        </Cell>
      </View>
      <View style={styles.row}>
        <Cell>
          <View style={[styles.box, { transform: [{ rotate: '-15deg' }], padding: 12, gap: 8, backgroundColor: '#2a3150' }]}>
            <View style={{ height: 14, borderRadius: 7, backgroundColor: '#f2c35b' }} />
            <View style={{ height: 14, width: '60%', borderRadius: 7, backgroundColor: '#ff4d6a' }} />
          </View>
        </Cell>
        <Cell>
          <View style={[styles.box, { transform: [{ skewX: '-15deg' }], backgroundColor: '#4f8cff' }]} />
        </Cell>
        <Cell>
          <View style={[styles.box, { transform: [{ scaleX: -1 }], backgroundColor: 'transparent' }]}>
            <View style={{ width: '50%', height: '100%', backgroundColor: '#ff4d6a' }} />
          </View>
        </Cell>
        <Cell>
          <View style={{ width: 80, height: 80, opacity: 0.5 }}>
            <View style={{ position: 'absolute', left: 0, top: 0, width: 56, height: 56, backgroundColor: '#ff4d6a' }} />
            <View style={{ position: 'absolute', right: 0, bottom: 0, width: 56, height: 56, backgroundColor: '#4fd1ff' }} />
          </View>
        </Cell>
        <Cell>
          <View style={[styles.box, { opacity: 0.6, backgroundColor: '#f2c35b', padding: 16 }]}>
            <View style={{ flex: 1, opacity: 0.5, backgroundColor: '#11162a' }} />
          </View>
        </Cell>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#11162a', padding: 20, gap: 30 },
  row: { flexDirection: 'row', gap: 20 },
  cell: { width: 104, height: 150, alignItems: 'center', justifyContent: 'center', backgroundColor: '#181f38', borderRadius: 6 },
  box: { width: 80, height: 80, backgroundColor: '#f2c35b' },
});
