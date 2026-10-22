// Feature: rounded corners up close, rendered at 4x device scale so every
// anti-aliased edge pixel counts. Shape-only (no text). Covers the cases game
// UIs lean on: mixed border widths and colors meeting at a radius (accent top
// borders on cards and tooltips), outer glows, inset highlights, gradients
// under a rounded border, nested rounded clips and hairline borders.
import React from 'react';
import { StyleSheet, View } from 'react-native';

export const meta = {
  width: 300,
  height: 170,
  scale: 4,
  maxMismatch: 0.5,
  maxTileMismatch: 3,
  tileSize: 8,
  title: 'Corners (4x)',
  regions: [
    { name: 'tooltip top-left corner', x: 16, y: 16, w: 22, h: 22, maxMismatch: 1 },
    { name: 'tooltip top-right corner', x: 92, y: 16, w: 22, h: 22, maxMismatch: 1 },
    { name: 'uneven top-left corner', x: 126, y: 16, w: 26, h: 26, maxMismatch: 1 },
  ],
};

export default function App() {
  return (
    <View style={styles.root}>
      <View style={styles.row}>
        {/* Inventory-style tooltip: thin border with a thick accent top edge and a glow. */}
        <View style={[styles.card, styles.tooltip]}>
          <View style={styles.icon} />
        </View>
        {/* Four different widths and colors meeting at a large radius. */}
        <View style={[styles.card, styles.uneven]} />
        {/* Accent left edge, as on selected menu buttons. */}
        <View style={[styles.card, styles.accentLeft]} />
      </View>
      <View style={styles.row}>
        {/* Gradient fill under a hairline border with an inset highlight. */}
        <View style={[styles.small, styles.gradient]} />
        {/* Nested rounded clips. */}
        <View style={[styles.small, styles.outerClip]}>
          <View style={styles.innerClip}>
            <View style={styles.innerFill} />
          </View>
        </View>
        {/* Hairline border on a pill. */}
        <View style={[styles.small, styles.pill]} />
        {/* Per-corner radii with a bottom-only border. */}
        <View style={[styles.small, styles.perCorner]} />
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#0b0f1c', padding: 16, gap: 16 },
  row: { flexDirection: 'row', gap: 12 },
  card: { width: 98, height: 70 },
  tooltip: {
    borderRadius: 14,
    backgroundColor: 'rgba(16,12,8,0.96)',
    borderWidth: 1,
    borderColor: 'rgba(255,178,63,0.55)',
    borderTopWidth: 3,
    borderTopColor: '#ffb23f',
    boxShadow: '0 8px 20px rgba(0,0,0,0.7), 0 0 12px rgba(255,178,63,0.25)',
    padding: 10,
  },
  icon: {
    width: 34,
    height: 34,
    borderRadius: 10,
    borderWidth: 1.5,
    borderColor: '#ffb23f',
    experimental_backgroundImage: 'radial-gradient(circle at 50% 40%, rgba(255,178,63,0.45) 0%, rgba(40,20,4,0.9) 75%)',
  },
  uneven: {
    borderRadius: 22,
    borderTopWidth: 2,
    borderRightWidth: 6,
    borderBottomWidth: 10,
    borderLeftWidth: 4,
    borderTopColor: '#ff4d6a',
    borderRightColor: '#f2c35b',
    borderBottomColor: '#4ade80',
    borderLeftColor: '#4f8cff',
    backgroundColor: '#1c2340',
  },
  accentLeft: {
    borderRadius: 10,
    borderWidth: 1,
    borderColor: 'rgba(148,170,255,0.25)',
    borderLeftWidth: 4,
    borderLeftColor: '#ffb23f',
    backgroundColor: 'rgba(255,178,63,0.12)',
  },
  small: { width: 60, height: 44 },
  gradient: {
    borderRadius: 12,
    borderWidth: StyleSheet.hairlineWidth,
    borderColor: 'rgba(255,255,255,0.35)',
    experimental_backgroundImage: 'linear-gradient(180deg, #3a4a8c 0%, #151b36 100%)',
    boxShadow: 'inset 0 1px 0 rgba(255,255,255,0.25)',
  },
  outerClip: { borderRadius: 16, overflow: 'hidden', backgroundColor: '#f2c35b', padding: 4 },
  innerClip: { flex: 1, borderRadius: 12, overflow: 'hidden', backgroundColor: '#151d33' },
  innerFill: { width: '50%', height: '100%', backgroundColor: '#4fd1ff' },
  pill: { height: 22, marginTop: 11, borderRadius: 999, borderWidth: 1, borderColor: '#4fd1ff', backgroundColor: 'rgba(79,209,255,0.15)' },
  perCorner: {
    borderTopLeftRadius: 18,
    borderTopRightRadius: 4,
    borderBottomRightRadius: 18,
    borderBottomLeftRadius: 4,
    borderBottomWidth: 3,
    borderBottomColor: '#b26bff',
    backgroundColor: '#2a3150',
  },
});
