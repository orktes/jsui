// Frame-by-frame fixture for text vs. box alignment under animated
// transforms (tests/reference/frames.js). Two boxes with text, driven by the
// analytic React Native spring (the curve Animated.spring follows):
//   top:    translateX 0 -> 12 + scale 1 -> 1.035 (menu item hover)
//   bottom: translateY 20 -> 0 + scale 0.82 -> 1 (modal spring-in)
// The frame index comes from globalThis.__FRAME (and __setFrame on the web),
// so both renderers show exactly the same values. Colors are fixed by the
// motion check (tests/reference/lib/motion.js): #0000ff boxes, white text,
// black background.
import { useEffect, useState } from 'react';
import { View, Text, StyleSheet } from 'react-native';

export const meta = {
  width: 360,
  height: 220,
  frames: 72,
  maxMismatch: 1.5,
  title: 'Spring text alignment (animated)',
  motion: {
    regions: [
      { name: 'menu hover', x: 20, y: 15, w: 300, h: 80 },
      { name: 'modal spring', x: 20, y: 105, w: 300, h: 112 },
    ],
  },
};

// Analytic RN spring (stiffness / damping / mass, from 0 to 1, v0 = 0).
export function spring(t, { stiffness = 340, damping = 20, mass = 0.9 } = {}) {
  const w0 = Math.sqrt(stiffness / mass);
  const zeta = damping / (2 * Math.sqrt(stiffness * mass));
  const env = Math.exp(-zeta * w0 * t);
  if (zeta < 1) {
    const w1 = w0 * Math.sqrt(1 - zeta * zeta);
    return 1 - env * (((zeta * w0) / w1) * Math.sin(w1 * t) + Math.cos(w1 * t));
  }
  return 1 - env * (1 + w0 * t);
}

const SLOW = 2; // slow motion: 72 frames cover 0.6 s of spring time
export function values(i) {
  const t = i / 60 / SLOW;
  const hover = spring(t);
  const modal = spring(t, { stiffness: 220, damping: 15, mass: 1 });
  return { tx: 12 * hover, s1: 1 + 0.035 * hover, s2: 0.82 + 0.18 * modal, ty: 20 * (1 - modal) };
}

export default function App() {
  const [frame, setFrame] = useState(typeof globalThis.__FRAME === 'number' ? globalThis.__FRAME : meta.frames - 1);
  useEffect(() => {
    globalThis.__setFrame = setFrame;
  }, []);
  const v = values(frame);
  return (
    <View style={styles.root}>
      <View style={[styles.box, styles.menu, { transform: [{ translateX: v.tx }, { scale: v.s1 }] }]}>
        <Text style={styles.menuText}>CONTINUE</Text>
      </View>
      <View style={[styles.box, styles.panel, { transform: [{ translateY: v.ty }, { scale: v.s2 }] }]}>
        <Text style={styles.panelText}>Deploy squad</Text>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#000000' },
  box: { position: 'absolute', left: 40, justifyContent: 'center', paddingHorizontal: 18 },
  menu: { top: 30, width: 220, height: 50, backgroundColor: '#0000ff' },
  panel: { top: 120, width: 240, height: 64, backgroundColor: '#0000ff' },
  menuText: { fontFamily: 'Inter', fontWeight: '600', fontSize: 15, letterSpacing: 1.4, color: '#ffffff' },
  panelText: { fontFamily: 'Inter', fontWeight: '700', fontSize: 20, color: '#ffffff' },
});
