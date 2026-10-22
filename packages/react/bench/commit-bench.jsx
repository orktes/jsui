// Commit-cost benchmark under QuickJS (500 host nodes). Run:
//   npx jsui build packages/react/bench/commit-bench.jsx -o /tmp/bench/bundle.js
//   ./build/jsui-render /tmp/bench/bundle.js -o /tmp/bench/out.png --wait-ms 0
import React, { useState } from 'react';
import { View, Text, StyleSheet, render, flushSync, bridgeStats, Animated } from 'react-native';

const N = 500;
let setTick;
const styles = StyleSheet.create({
  root: { flex: 1, flexDirection: 'row', flexWrap: 'wrap' },
  cell: { width: 30, height: 20, margin: 1 },
});

// Plain component: re-renders whenever the parent does.
function Cell({ hue }) {
  return <View style={[styles.cell, { backgroundColor: `hsl(${hue}, 60%, 50%)` }]} />;
}
// Memoized: only re-renders when its hue changes (typical game UI code).
const MemoCell = React.memo(Cell);

function App() {
  const [state, set] = useState({ tick: 0, mode: 'none', memo: false });
  setTick = set;
  const cells = [];
  const C = state.memo ? MemoCell : Cell;
  for (let i = 0; i < N; i++) {
    // 'all': every cell changes color; 'tenth': 10% change; 'none': same values (new style objects).
    const changed = state.mode === 'all' || (state.mode === 'tenth' && i % 10 === 0);
    const hue = changed ? (i * 7 + state.tick * 13) % 360 : (i * 7) % 360;
    cells.push(<C key={i} hue={hue} />);
  }
  return <View style={styles.root}>{cells}</View>;
}

function time(label, iterations, fn) {
  const before = bridgeStats().calls;
  const t0 = Date.now();
  for (let k = 0; k < iterations; k++) fn(k);
  const ms = (Date.now() - t0) / iterations;
  const calls = (bridgeStats().calls - before) / iterations;
  console.log(`[bench] ${label}: ${ms.toFixed(3)} ms/commit, ${calls} bridge calls/commit`);
}

time('mount 500 views', 5, () => {
  render(null);
  render(<App />);
});
let tick = 1;
for (const memo of [false, true]) {
  flushSync(() => setTick({ tick: tick++, mode: 'none', memo }));
  for (const mode of ['none', 'tenth', 'all']) {
    time(`update ${memo ? 'memo' : 'plain'} (${mode})`, 40, () => flushSync(() => setTick({ tick: tick++, mode, memo })));
  }
}

// Animated: 500 views animated via patchStyle (no React).
const values = [];
function AnimApp() {
  const cells = [];
  for (let i = 0; i < N; i++) {
    if (!values[i]) values[i] = new Animated.Value(0);
    cells.push(<Animated.View key={i} style={[styles.cell, { backgroundColor: 'red', opacity: values[i], transform: [{ translateX: values[i] }] }]} />);
  }
  return <View style={styles.root}>{cells}</View>;
}
render(<AnimApp />);
time('Animated setValue x500 (patchStyle)', 40, (k) => {
  for (let i = 0; i < N; i++) values[i].setValue((k % 10) / 10 + i * 1e-4);
});
