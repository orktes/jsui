// Boxes with text at fractional transform values, rendered frame by frame:
// translateX / translateY in 1/8 px steps (0.25, 0.5, 0.75 ...), a scale ramp
// in 0.0065 steps (1.013 at frame 2) and a menu-hover style slide + scale.
// The motion check (tests/reference/lib/motion.js) requires each text to
// move rigidly with its box, and each box to sit where Chrome puts it: a
// renderer that pixel-snaps boxes after the transform, or snaps the text
// baseline on the device while the box moves smoothly, makes the text
// wiggle inside its box. Colors are fixed by the check: #0000ff boxes,
// white text, black background.
import { useEffect, useState } from 'react';
import { View, Text, StyleSheet } from 'react-native';

export const meta = {
  width: 320,
  height: 290,
  frames: 17,
  maxMismatch: 1.5,
  title: 'Fractional transforms (animated)',
  motion: {
    regions: [
      { name: 'translateX', x: 20, y: 8, w: 280, h: 64 },
      { name: 'scale', x: 20, y: 78, w: 280, h: 64 },
      { name: 'translateY', x: 20, y: 148, w: 280, h: 64 },
      { name: 'slide+scale', x: 20, y: 218, w: 280, h: 64 },
    ],
  },
};

export default function App() {
  const [i, setFrame] = useState(typeof globalThis.__FRAME === 'number' ? globalThis.__FRAME : 2);
  useEffect(() => {
    globalThis.__setFrame = setFrame;
  }, []);
  const rows = [
    [{ translateX: 0.125 * i }],
    [{ scale: 1 + 0.0065 * i }],
    [{ translateY: 0.125 * i }],
    [{ translateX: 0.75 * i }, { scale: 1 + 0.0022 * i }],
  ];
  return (
    <View style={styles.root}>
      {rows.map((transform, k) => (
        <View key={k} style={[styles.box, { top: 20 + 70 * k, transform }]}>
          <Text style={k % 2 ? styles.label : styles.caps}>{k % 2 ? 'Deploy squad' : 'CONTINUE'}</Text>
        </View>
      ))}
    </View>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#000000' },
  box: { position: 'absolute', left: 40, width: 200, height: 40, justifyContent: 'center', paddingHorizontal: 16, backgroundColor: '#0000ff' },
  caps: { fontFamily: 'Inter', fontWeight: '600', fontSize: 15, letterSpacing: 1.4, color: '#ffffff' },
  label: { fontFamily: 'Inter', fontWeight: '700', fontSize: 17, color: '#ffffff' },
});
