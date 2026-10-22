// Side-by-side spring comparison (jsui vs react-native-web in Chromium):
// a menu item hover (slide + scale, as in interactions.jsx) and a spring-in
// panel with text, rendered frame by frame with tests/reference/frames.js:
//
//   node tests/reference/frames.js examples/showcase/spring-wiggle.jsx --out <dir>
//
// Frame i shows time i / 240 s (4x slow motion at 60 fps; every 4th frame
// is real time). Values follow the analytic React Native spring, so both
// renderers show exactly the same transforms.
import { useEffect, useState } from 'react';
import { View, Text, StyleSheet } from 'react-native';

export const meta = { width: 520, height: 300, frames: 864, title: 'Spring wiggle comparison' };

const FPS = 240;

// Analytic RN spring from 0 to 1 (v0 = 0) after `t` seconds.
function spring(t, stiffness, damping, mass) {
  if (t <= 0) return 0;
  const w0 = Math.sqrt(stiffness / mass);
  const zeta = damping / (2 * Math.sqrt(stiffness * mass));
  const env = Math.exp(-zeta * w0 * t);
  if (zeta < 1) {
    const w1 = w0 * Math.sqrt(1 - zeta * zeta);
    return 1 - env * (((zeta * w0) / w1) * Math.sin(w1 * t) + Math.cos(w1 * t));
  }
  return 1 - env * (1 + w0 * t);
}

const HOVER_IN = 0.15, HOVER_OUT = 1.1, PANEL_IN = 1.9, PANEL_OUT = 3.0;

export function values(frame) {
  const t = frame / FPS;
  const hover = t < HOVER_OUT ? spring(t - HOVER_IN, 340, 20, 0.9) : 1 - spring(t - HOVER_OUT, 340, 20, 0.9);
  const panel = t < PANEL_OUT ? spring(t - PANEL_IN, 220, 15, 1) : Math.max(0, 1 - spring(t - PANEL_OUT, 320, 26, 1));
  return { hover, panel };
}

const ITEMS = ['Continue', 'New game', 'Settings'];

export default function App() {
  const [frame, setFrame] = useState(typeof globalThis.__FRAME === 'number' ? globalThis.__FRAME : 120);
  useEffect(() => {
    globalThis.__setFrame = setFrame;
  }, []);
  const { hover, panel } = values(frame);
  return (
    <View style={styles.root}>
      {ITEMS.map((label, i) => {
        const h = i === 0 ? hover : 0;
        return (
          <View key={label} style={[styles.item, { top: 40 + 64 * i, transform: [{ translateX: 12 * h }, { scale: 1 + 0.035 * h }] }]}>
            <View style={[StyleSheet.absoluteFill, styles.glow, { opacity: h }]} />
            <Text style={styles.itemText}>{label}</Text>
            <Text style={styles.itemHint}>{i === 0 ? 'Chapter 4 · The Ember Gate' : i === 1 ? 'Start a fresh campaign' : 'Audio, video, controls'}</Text>
          </View>
        );
      })}
      {panel > 0.001 ? (
        <View
          style={[
            styles.panel,
            {
              opacity: Math.min(1, panel / 0.6),
              transform: [{ translateY: 60 * (1 - panel) }, { scale: 0.82 + 0.18 * panel }],
            },
          ]}
        >
          <Text style={styles.kicker}>EXPEDITION READY</Text>
          <Text style={styles.title}>The Ember Gate</Text>
          <Text style={styles.body}>4 heroes · Heroic · Recommended power 900. Rewards: Cinderforged cache, 1,200 XP.</Text>
          <View style={styles.button}>
            <Text style={styles.buttonText}>Deploy squad</Text>
          </View>
        </View>
      ) : null}
    </View>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#0b1020' },
  item: {
    position: 'absolute',
    left: 24,
    width: 240,
    height: 50,
    justifyContent: 'center',
    paddingHorizontal: 18,
    borderRadius: 10,
    backgroundColor: 'rgba(30, 42, 80, 0.9)',
    borderWidth: 1,
    borderColor: 'rgba(148, 170, 255, 0.18)',
  },
  glow: { borderRadius: 10, borderWidth: 1, borderColor: 'rgba(242, 195, 91, 0.9)', backgroundColor: 'rgba(242, 195, 91, 0.10)' },
  itemText: { fontFamily: 'Inter', fontWeight: '600', fontSize: 15, letterSpacing: 1.4, textTransform: 'uppercase', color: '#e2e8f8' },
  itemHint: { fontFamily: 'Inter', fontWeight: '500', fontSize: 11, color: '#f2c35b', marginTop: 2, letterSpacing: 0.4 },
  panel: {
    position: 'absolute',
    left: 290,
    top: 40,
    width: 210,
    padding: 16,
    borderRadius: 16,
    backgroundColor: '#161e36',
    borderWidth: 1,
    borderColor: 'rgba(148, 170, 255, 0.25)',
  },
  kicker: { fontFamily: 'Inter', fontWeight: '700', fontSize: 10, letterSpacing: 1.2, color: '#ff9b54' },
  title: { fontFamily: 'Inter', fontWeight: '700', fontSize: 20, color: '#ffffff', marginTop: 4 },
  body: { fontFamily: 'Inter', fontSize: 12, lineHeight: 17, color: '#aab4d0', marginTop: 6 },
  button: { marginTop: 14, height: 34, borderRadius: 8, backgroundColor: '#f2c35b', alignItems: 'center', justifyContent: 'center' },
  buttonText: { fontFamily: 'Inter', fontWeight: '700', fontSize: 13, letterSpacing: 0.6, color: '#1a1206' },
});
