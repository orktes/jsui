// Spring vs timing: tweak a spring and a timing curve, watch both race and
// see their response curves plotted (computed in JS, drawn with Views).
import React, { memo, useEffect, useMemo, useRef, useState } from 'react';
import { Animated, Easing, StyleSheet, Text, View } from 'react-native';
import { colors, font, mono, site } from '../../theme.js';
import { DemoButton, DemoCard, Segmented, Slider } from './controls.jsx';

const PRESETS = [
  { id: 'default', label: 'Default', stiffness: 170, damping: 26, mass: 1 },
  { id: 'gentle', label: 'Gentle', stiffness: 120, damping: 14, mass: 1 },
  { id: 'wobbly', label: 'Wobbly', stiffness: 180, damping: 9, mass: 1 },
  { id: 'stiff', label: 'Stiff', stiffness: 420, damping: 30, mass: 1 },
  { id: 'heavy', label: 'Heavy', stiffness: 160, damping: 18, mass: 3 },
];

const EASINGS = {
  'out(cubic)': Easing.out(Easing.cubic),
  'inOut(quad)': Easing.inOut(Easing.quad),
  'out(back)': Easing.out(Easing.back(1.7)),
  linear: Easing.linear,
};

const SPAN = 1600; // ms shown on the graph
const SAMPLES = 64;

/** Damped spring x'' = (-k (x - 1) - c x') / m from rest at 0, sampled. */
function springCurve(k, c, m) {
  const out = [];
  let x = 0;
  let v = 0;
  let settle = SPAN;
  const dt = 1;
  for (let t = 0, i = 0; t <= SPAN; t += dt) {
    if (t >= (i * SPAN) / (SAMPLES - 1)) {
      out.push(x);
      i++;
    }
    const a = (-k * (x - 1) - c * v) / m;
    v += (a * dt) / 1000;
    x += (v * dt) / 1000;
    if (Math.abs(x - 1) > 0.005 || Math.abs(v) > 0.05) settle = SPAN;
    else if (settle === SPAN) settle = t;
  }
  while (out.length < SAMPLES) out.push(x);
  return { points: out, settle: settle >= SPAN ? null : settle };
}

const PLOT_LO = -0.15;
const PLOT_HI = 1.45;

const Plot = memo(function Plot({ spring, timing, duration, width, height }) {
  const y = (v) => height - ((v - PLOT_LO) / (PLOT_HI - PLOT_LO)) * height;
  const dotsFor = (pts, color, size) =>
    pts.map((v, i) => (
      <View
        key={i}
        style={{
          position: 'absolute',
          left: (i / (pts.length - 1)) * width - size / 2,
          top: y(v) - size / 2,
          width: size,
          height: size,
          borderRadius: size / 2,
          backgroundColor: color,
        }}
      />
    ));
  const tPts = [];
  for (let i = 0; i < SAMPLES; i++) {
    const t = (i / (SAMPLES - 1)) * SPAN;
    tPts.push(t >= duration ? 1 : timing(t / duration));
  }
  return (
    <View style={{ width, height }}>
      <View style={[styles.gridLine, { top: y(1) }]} />
      <Text style={[styles.gridLabel, { top: y(1) - 16 }]}>target</Text>
      <View style={[styles.gridLine, { top: y(0), backgroundColor: 'rgba(148,170,255,0.08)' }]} />
      {dotsFor(tPts, colors.cyan, 4)}
      {dotsFor(spring, colors.gold, 5)}
    </View>
  );
});

export function SpringLab() {
  const [k, setK] = useState(180);
  const [c, setC] = useState(9);
  const [m, setM] = useState(1);
  const [preset, setPreset] = useState('wobbly');
  const [duration, setDuration] = useState(500);
  const [ease, setEase] = useState('out(cubic)');
  const [plotW, setPlotW] = useState(0);
  const [laneW, setLaneW] = useState(0);
  const sx = useRef(new Animated.Value(0)).current;
  const tx = useRef(new Animated.Value(0)).current;
  const head = useRef(new Animated.Value(0)).current;
  const dir = useRef(0);

  const curve = useMemo(() => springCurve(k, c, m), [k, c, m]);
  const zeta = c / (2 * Math.sqrt(k * m));

  const run = (flip) => {
    if (flip) dir.current = dir.current ? 0 : 1;
    const to = dir.current;
    if (!flip) {
      // Replay from the far side so a tweak is always visible.
      sx.setValue(1 - to);
      tx.setValue(1 - to);
    }
    Animated.spring(sx, { toValue: to, stiffness: k, damping: c, mass: m, useNativeDriver: true }).start();
    Animated.timing(tx, { toValue: to, duration, easing: EASINGS[ease], useNativeDriver: true }).start();
    head.setValue(0);
    Animated.timing(head, { toValue: 1, duration: SPAN, easing: Easing.linear, useNativeDriver: true }).start();
  };
  const first = useRef(true);
  useEffect(() => {
    if (first.current) {
      first.current = false;
      return undefined;
    }
    const id = setTimeout(() => run(false), 140);
    return () => clearTimeout(id);
  }, [k, c, m, duration, ease]); // eslint-disable-line react-hooks/exhaustive-deps

  const pick = (id) => {
    const p = PRESETS.find((x) => x.id === id);
    setPreset(id);
    setK(p.stiffness);
    setC(p.damping);
    setM(p.mass);
  };
  const custom = (fn) => (v) => {
    setPreset(null);
    fn(v);
  };

  const PUCK = 34;
  const travel = Math.max(0, laneW - PUCK - 8);
  const lane = (value, color, label, note) => (
    <View style={{ gap: 6 }}>
      <View style={styles.laneHead}>
        <View style={[styles.swatch, { backgroundColor: color }]} />
        <Text style={styles.laneLabel}>{label}</Text>
        <Text style={styles.laneNote}>{note}</Text>
      </View>
      <View style={styles.lane} onLayout={(e) => setLaneW(e.nativeEvent.layout.width)}>
        <View style={[styles.laneMark, { left: 4 + PUCK / 2 }]} />
        <View style={[styles.laneMark, { left: 4 + travel + PUCK / 2 }]} />
        <Animated.View
          style={[
            styles.puck,
            { width: PUCK, height: PUCK, borderRadius: PUCK / 2, backgroundColor: color, boxShadow: `0 0 18px ${color}88` },
            { transform: [{ translateX: value.interpolate({ inputRange: [0, 1], outputRange: [0, travel] }) }] },
          ]}
        />
      </View>
    </View>
  );

  return (
    <DemoCard
      index={1}
      title="Spring vs timing"
      blurb="A timing animation follows a fixed curve for a fixed duration. A spring is physics: stiffness pulls, damping resists, mass adds inertia, and it can be interrupted mid-flight without a jolt because it keeps its velocity. Drag the sliders and watch both race."
      tags={[['Animated.spring', colors.gold], ['Animated.timing', colors.cyan], ['useNativeDriver', colors.green]]}
      codeTitle="spring-vs-timing.jsx"
      code={`
const x = useRef(new Animated.Value(0)).current;

// Physics: settles when it settles.
Animated.spring(x, {
  toValue: 1,
  stiffness: ${k}, damping: ${c}, mass: ${m},
  useNativeDriver: true,
}).start();

// Curve: always exactly ${duration} ms.
Animated.timing(x, {
  toValue: 1,
  duration: ${duration},
  easing: Easing.${ease},
  useNativeDriver: true,
}).start();`}
    >
      {lane(sx, colors.gold, 'Spring', curve.settle != null ? `settles ≈ ${Math.round(curve.settle)} ms · ζ ${zeta.toFixed(2)}` : `still moving at ${SPAN} ms · ζ ${zeta.toFixed(2)}`)}
      {lane(tx, colors.cyan, 'Timing', `${duration} ms · ${ease}`)}

      <View style={styles.plotWrap} onLayout={(e) => setPlotW(e.nativeEvent.layout.width - 24)}>
        {plotW > 0 ? <Plot spring={curve.points} timing={EASINGS[ease]} duration={duration} width={plotW} height={130} /> : <View style={{ height: 130 }} />}
        <Animated.View
          pointerEvents="none"
          style={[
            styles.playhead,
            { transform: [{ translateX: head.interpolate({ inputRange: [0, 1], outputRange: [0, Math.max(0, plotW)] }) }] },
            { opacity: head.interpolate({ inputRange: [0, 0.02, 0.95, 1], outputRange: [0, 1, 1, 0] }) },
          ]}
        />
        <View style={styles.axis}>
          <Text style={styles.axisText}>0 ms</Text>
          <Text style={styles.axisText}>{SPAN / 2} ms</Text>
          <Text style={styles.axisText}>{SPAN} ms</Text>
        </View>
      </View>

      <View style={styles.controls}>
        <View style={{ flex: 1, minWidth: 230, gap: 12 }}>
          <Segmented options={PRESETS} value={preset} onChange={pick} color={colors.gold} small />
          <Slider label="Stiffness" value={k} min={20} max={600} step={5} onChange={custom(setK)} />
          <Slider label="Damping" value={c} min={2} max={80} step={1} onChange={custom(setC)} />
          <Slider label="Mass" value={m} min={0.2} max={5} step={0.1} onChange={custom(setM)} format={(v) => v.toFixed(1)} />
        </View>
        <View style={{ flex: 1, minWidth: 230, gap: 12 }}>
          <Segmented options={Object.keys(EASINGS)} value={ease} onChange={setEase} color={colors.cyan} small />
          <Slider label="Duration" value={duration} min={100} max={1500} step={20} onChange={setDuration} color={colors.cyan} format={(v) => `${v} ms`} />
          <View style={{ flex: 1 }} />
          <View style={{ flexDirection: 'row', gap: 10, flexWrap: 'wrap' }}>
            <DemoButton primary label="Play" icon="▶" onPress={() => run(true)} />
            <DemoButton label="Interrupt" icon="↺" color={colors.cyan} onPress={() => {
              // Retarget mid-flight: the spring keeps its velocity, timing restarts its curve.
              run(true);
              setTimeout(() => run(true), 220);
            }} />
          </View>
        </View>
      </View>
    </DemoCard>
  );
}

const styles = StyleSheet.create({
  laneHead: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  swatch: { width: 10, height: 10, borderRadius: 3 },
  laneLabel: { fontFamily: font, fontWeight: '700', fontSize: 13, color: colors.text },
  laneNote: { fontFamily: mono, fontSize: 12, color: colors.textMute, flex: 1, textAlign: 'right' },
  lane: { height: 46, borderRadius: 23, backgroundColor: 'rgba(148,170,255,0.06)', borderWidth: 1, borderColor: site.stroke, justifyContent: 'center', paddingHorizontal: 4 },
  laneMark: { position: 'absolute', top: 8, bottom: 8, width: 1, backgroundColor: 'rgba(148,170,255,0.18)' },
  puck: { borderWidth: 2, borderColor: 'rgba(255,255,255,0.75)' },
  plotWrap: { padding: 12, paddingBottom: 6, borderRadius: 12, backgroundColor: 'rgba(0,0,0,0.25)', borderWidth: 1, borderColor: site.stroke, gap: 6 },
  gridLine: { position: 'absolute', left: 0, right: 0, height: 1, backgroundColor: 'rgba(242,195,91,0.25)' },
  gridLabel: { position: 'absolute', right: 0, fontFamily: mono, fontSize: 10, color: colors.textMute },
  playhead: { position: 'absolute', left: 12, top: 8, height: 138, width: 2, borderRadius: 1, backgroundColor: 'rgba(255,255,255,0.7)', boxShadow: '0 0 8px rgba(255,255,255,0.6)' },
  axis: { flexDirection: 'row', justifyContent: 'space-between' },
  axisText: { fontFamily: mono, fontSize: 10, color: colors.textMute },
  controls: { flexDirection: 'row', flexWrap: 'wrap', gap: 22 },
});
