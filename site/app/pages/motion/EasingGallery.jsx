// Easing gallery: every curve plotted live, with a ball riding it. One shared
// native clock drives every tile; each ball's height is that clock passed
// through an interpolation with the tile's easing, sampled natively.
import React, { memo, useEffect, useMemo, useRef, useState } from 'react';
import { Animated, Easing, Pressable, StyleSheet, Text, View } from 'react-native';
import { colors, font, mono, site } from '../../theme.js';
import { useLayout } from '../../components/ui.jsx';
import { DemoCard, Segmented } from './controls.jsx';

const BASE = [
  { name: 'linear', fn: Easing.linear, flat: true },
  { name: 'quad', fn: Easing.quad },
  { name: 'cubic', fn: Easing.cubic },
  { name: 'sin', fn: Easing.sin },
  { name: 'circle', fn: Easing.circle },
  { name: 'exp', fn: Easing.exp },
  { name: 'back(1.7)', fn: Easing.back(1.7) },
  { name: 'elastic(1)', fn: Easing.elastic(1) },
  { name: 'bounce', fn: Easing.bounce },
  { name: 'bezier(.7,-.6,.3,1.6)', fn: Easing.bezier(0.7, -0.6, 0.3, 1.6), flat: true, code: 'Easing.bezier(0.7, -0.6, 0.3, 1.6)' },
];

const MODES = [
  { id: 'in', label: 'Easing.in' },
  { id: 'out', label: 'Easing.out' },
  { id: 'inOut', label: 'Easing.inOut' },
];

const LO = -0.45;
const HI = 1.45;
const DOTS = 34;

const Tile = memo(function Tile({ item, mode, clock, width, selected, onPick }) {
  const fn = item.flat ? item.fn : mode === 'in' ? Easing.in(item.fn) : mode === 'out' ? Easing.out(item.fn) : Easing.inOut(item.fn);
  const H = 92;
  const W = width - 28;
  const y = (v) => H - ((v - LO) / (HI - LO)) * H;
  const dots = [];
  for (let i = 0; i < DOTS; i++) {
    const t = i / (DOTS - 1);
    dots.push(<View key={i} style={[styles.dot, { left: t * W - 1.5, top: y(fn(t)) - 1.5 }]} />);
  }
  const ball = useMemo(
    () => ({
      x: clock.interpolate({ inputRange: [0, 1], outputRange: [-6, W - 6] }),
      y: clock.interpolate({ inputRange: [0, 1], outputRange: [y(0) - 6, y(1) - 6], easing: fn }),
      bar: clock.interpolate({ inputRange: [0, 1], outputRange: [0, 1], easing: fn }),
    }),
    [clock, fn, W],
  );
  return (
    <Pressable onPress={onPick} style={{ width, cursor: 'pointer' }}>
      {({ hovered, focused }) => (
        <View style={[styles.tile, (hovered || selected) && styles.tileHot, focused && { borderColor: colors.cyan }]}>
          <View style={{ height: H, width: W }}>
            <View style={[styles.base, { top: y(0) }]} />
            <View style={[styles.base, { top: y(1), backgroundColor: 'rgba(242,195,91,0.22)' }]} />
            {dots}
            <Animated.View style={[styles.ball, { transform: [{ translateX: ball.x }, { translateY: ball.y }] }]} />
          </View>
          <View style={styles.meter}>
            <Animated.View style={[styles.meterFill, { transform: [{ translateX: ball.bar.interpolate({ inputRange: [0, 1], outputRange: [-W, 0] }) }] }]} />
          </View>
          <Text style={[styles.name, (hovered || selected) && { color: colors.text }]} numberOfLines={1}>
            {item.flat ? item.name : `${mode}(${item.name})`}
          </Text>
        </View>
      )}
    </Pressable>
  );
});

export function EasingGallery() {
  const { width } = useLayout();
  const [mode, setMode] = useState('out');
  const [areaW, setAreaW] = useState(0);
  const [pick, setPick] = useState('elastic(1)');
  const clock = useRef(new Animated.Value(0)).current;
  useEffect(() => {
    const loop = Animated.loop(
      Animated.sequence([
        Animated.timing(clock, { toValue: 1, duration: 1500, easing: Easing.linear, useNativeDriver: true }),
        Animated.delay(700),
      ]),
    );
    loop.start();
    return () => loop.stop();
  }, [clock]);
  const cols = areaW > 820 ? 5 : areaW > 600 ? 4 : areaW > 420 ? 3 : 2;
  const gap = 10;
  const tileW = areaW ? Math.floor((areaW - gap * (cols - 1)) / cols) : 0;
  const sel = BASE.find((b) => b.name === pick) || BASE[0];
  const code = sel.code || `Easing.${sel.name}`;
  const call = sel.flat ? code : `Easing.${mode}(${code})`;
  return (
    <DemoCard
      index={2}
      title="Easing gallery"
      blurb="Every curve in React Native's Easing module, plotted from the same functions the animations use. One looping native clock drives all ten balls; each tile maps it through interpolate({ easing }), which the runtime samples into a curve it evaluates in C++."
      tags={[['Easing', colors.cyan], ['interpolate', colors.purple], ['Animated.loop', colors.green]]}
      codeTitle="easing.jsx"
      demoMin={width >= 1180 ? 1.8 : 1.35}
      code={`
const t = useRef(new Animated.Value(0)).current;

Animated.loop(Animated.sequence([
  Animated.timing(t, { toValue: 1, duration: 1500,
    easing: Easing.linear, useNativeDriver: true }),
  Animated.delay(700),
])).start();

// The ball's height: the same clock, eased.
const y = t.interpolate({
  inputRange: [0, 1],
  outputRange: [92, 0],
  easing: ${call},
});`}
    >
      <Segmented options={MODES} value={mode} onChange={setMode} color={colors.cyan} small />
      <View style={styles.grid} onLayout={(e) => setAreaW(e.nativeEvent.layout.width)}>
        {tileW > 0
          ? BASE.map((b) => (
              <Tile key={b.name + mode + tileW} item={b} mode={mode} clock={clock} width={tileW} selected={pick === b.name} onPick={() => setPick(b.name)} />
            ))
          : null}
      </View>
    </DemoCard>
  );
}

const styles = StyleSheet.create({
  grid: { flexDirection: 'row', flexWrap: 'wrap', gap: 10, minHeight: 140 },
  tile: { padding: 14, paddingBottom: 10, gap: 10, borderRadius: 12, borderWidth: 1, borderColor: site.stroke, backgroundColor: 'rgba(255,255,255,0.02)' },
  tileHot: { borderColor: 'rgba(79,209,255,0.5)', backgroundColor: 'rgba(79,209,255,0.06)' },
  base: { position: 'absolute', left: 0, right: 0, height: 1, backgroundColor: 'rgba(148,170,255,0.12)' },
  dot: { position: 'absolute', width: 3, height: 3, borderRadius: 1.5, backgroundColor: 'rgba(79,209,255,0.55)' },
  ball: { position: 'absolute', left: 0, top: 0, width: 12, height: 12, borderRadius: 6, backgroundColor: colors.goldLight, boxShadow: '0 0 12px rgba(242,195,91,0.9)' },
  meter: { height: 4, borderRadius: 2, backgroundColor: 'rgba(148,170,255,0.12)', overflow: 'hidden' },
  meterFill: { height: 4, width: '100%', borderRadius: 2, backgroundColor: colors.gold },
  name: { fontFamily: mono, fontSize: 11, color: colors.textDim },
});
