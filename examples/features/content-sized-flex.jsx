// Feature: `flex: N` items inside containers that are sized by their content.
// react-native-web makes `flex: 1` `flex: 1 1 0%`; while the container is
// being sized to fit its content, the 0% basis is indefinite and the item is
// sized by its content (Yoga used to lay such text out at zero width, one
// character per line).
// Left: the showcase site's stacked "Write it once" list, a flex: 0 column
//       in an alignItems: 'flex-start' parent, rows of [check, flex: 1 text].
// Right: a fit-content row whose flex: 1 text is shorter than the space, a
//        column item in a flex-start row holding a flex: 1 child, and a
//        shrinkable panel whose flex: 1 text is longer than the space.
// The stacks have auto heights, as on the site (a definite height would make
// the web's 0% basis resolve, and `flex: 0` collapse there).
import React from 'react';
import { StyleSheet, Text, View } from 'react-native';

export const meta = { width: 720, height: 360, maxMismatch: 1.5, title: 'Content-sized flex' };

const POINTS = [
  ['Focus, hover and press states', 'Pressable style functions receive { focused, hovered, pressed }.'],
  ['autoFocus and spatial navigation', 'The D-pad finds the nearest focusable neighbour.'],
  ['Messages both ways', 'Host.postMessage from JS, jsui_post_message from your engine.'],
];

export default function App() {
  return (
    <View style={styles.root}>
      <View style={styles.stack}>
        <View style={[styles.panel, { flex: 0, gap: 8 }]}>
          {POINTS.map(([t, b]) => (
            <View key={t} style={styles.point}>
              <View style={styles.check} />
              <View style={{ flex: 1 }}>
                <Text style={styles.title}>{t}</Text>
                <Text style={styles.body}>{b}</Text>
              </View>
            </View>
          ))}
        </View>
        <View style={styles.after} />
      </View>
      <View style={[styles.stack, { gap: 12 }]}>
        <View style={[styles.panel, styles.point]}>
          <View style={styles.check} />
          <View style={{ flex: 1 }}>
            <Text style={styles.title}>Short label</Text>
          </View>
        </View>
        <View style={{ flexDirection: 'row', alignItems: 'flex-start', gap: 12 }}>
          <View style={[styles.panel, { width: 140 }]}>
            <View style={{ flex: 1, backgroundColor: '#2d3a6e', borderRadius: 4, padding: 6 }}>
              <Text style={styles.body}>A flex: 1 child of an auto-height column.</Text>
            </View>
          </View>
          <View style={[styles.panel, { flexShrink: 1 }]}>
            <View style={styles.point}>
              <View style={styles.check} />
              <Text style={[styles.body, { flex: 1 }]}>flex: 1 text that is far too long to fit the remaining width of this row.</Text>
            </View>
          </View>
        </View>
        <View style={styles.after} />
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, flexDirection: 'row', alignItems: 'flex-start', backgroundColor: '#11162a', padding: 20, gap: 20 },
  stack: { flex: 1, alignItems: 'flex-start', gap: 10 },
  panel: { backgroundColor: '#1d2442', borderRadius: 6, padding: 10 },
  point: { flexDirection: 'row', gap: 10, alignItems: 'flex-start' },
  check: { width: 14, height: 14, marginTop: 2, borderRadius: 7, backgroundColor: '#4ade80' },
  title: { fontSize: 15, fontWeight: '700', color: '#eef2ff' },
  body: { fontSize: 13, lineHeight: 19, color: '#aab4d6' },
  after: { width: 60, height: 12, borderRadius: 3, backgroundColor: '#f2c35b' },
});
