// Self-driving markers (same React work in JSUI and react-native-web): 300
// markers whose positions/HP live in React state, updated every frame by rAF
// (markers.jsx: 100). docs/PERFORMANCE.md "Web":
//   node web/test/frame-bench.mjs --real-gpu --renderers webgpu,chrome --settle 1500 web/test/bench-apps/markers300.jsx
import React, { memo, useEffect, useState } from 'react';
import { AppRegistry, StyleSheet, View } from 'react-native';

const N = 300;
const Marker = memo(function Marker({ x, y, hp }) {
  return (
    <View style={[styles.marker, { transform: [{ translateX: x }, { translateY: y }] }]}>
      <View style={styles.bar}>
        <View style={[styles.fill, { transform: [{ scaleX: hp / 100 }] }]} />
      </View>
    </View>
  );
});
function pos(i, t) {
  return [600 + 500 * Math.sin(t * 0.0011 + i * 0.37), 340 + 300 * Math.cos(t * 0.0013 + i * 0.53), 50 + 50 * Math.sin(t * 0.002 + i)];
}
export default function App() {
  const [t, setT] = useState(0);
  useEffect(() => {
    let raf = requestAnimationFrame(function step(now) { setT(now); raf = requestAnimationFrame(step); });
    return () => cancelAnimationFrame(raf);
  }, []);
  const out = [];
  for (let i = 0; i < N; i++) { const [x, y, hp] = pos(i, t); out.push(<Marker key={i} x={x} y={y} hp={hp} />); }
  return <View style={styles.root}>{out}</View>;
}
AppRegistry.registerComponent('Markers', () => App);
const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#0b0f1a' },
  marker: { position: 'absolute', left: 0, top: 0, width: 48, height: 14, padding: 3, borderRadius: 3, backgroundColor: '#0d1322cc' },
  bar: { flex: 1, borderRadius: 2, backgroundColor: '#3a1d24', overflow: 'hidden' },
  fill: { flex: 1, backgroundColor: '#ef476f' },
});
