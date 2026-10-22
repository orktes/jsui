// "How it works": the path from JSX to your engine's texture, drawn with
// Views. A single looping native value sweeps a highlight along the stages.
import React, { useEffect, useRef } from 'react';
import { Animated, Easing, StyleSheet, Text, View } from 'react-native';
import { colors, font, mono, site } from '../../theme.js';
import { useLayout } from '../../components/ui.jsx';

const LANES = [
  {
    title: 'Build time',
    note: 'on your machine',
    color: colors.cyan,
    nodes: [
      ['Your JSX', "import { View } from 'react-native'"],
      ['@jsui/cli', 'esbuild · react-native → @jsui/react'],
      ['bundle.js', 'one file + assets'],
    ],
  },
  {
    title: 'Script',
    note: 'inside the runtime',
    color: colors.purple,
    nodes: [
      ['QuickJS', 'ES2023 interpreter, timers, fetch'],
      ['React reconciler', 'react-reconciler + @jsui/react'],
    ],
  },
  {
    title: 'Native core',
    note: 'C++20',
    color: colors.gold,
    nodes: [
      ['Node tree', 'styles, events, focus, Animated'],
      ['Yoga', 'flexbox layout, like React Native'],
      ['Skia', 'CPU raster or GPU (Ganesh)'],
    ],
  },
  {
    title: 'Your engine',
    note: 'through the C API',
    color: colors.green,
    nodes: [['Texture', 'RGBA pixels or your GL framebuffer']],
  },
];

const TOTAL = LANES.reduce((n, l) => n + l.nodes.length, 0);

function Node({ label, sub, color, index, sweep }) {
  const hot = sweep.interpolate({ inputRange: [index - 1, index, index + 1.2], outputRange: [0, 1, 0], extrapolate: 'clamp' });
  return (
    <View style={styles.node}>
      <Animated.View pointerEvents="none" style={[StyleSheet.absoluteFill, styles.nodeHot, { borderColor: color, boxShadow: `0 0 22px ${color}55`, opacity: hot }]} />
      <View style={[styles.nodeDot, { backgroundColor: color }]} />
      <View style={{ flex: 1, gap: 2 }}>
        <Text style={styles.nodeLabel}>{label}</Text>
        <Text style={styles.nodeSub}>{sub}</Text>
      </View>
    </View>
  );
}

export function Pipeline() {
  const { width } = useLayout();
  const stacked = width < 1000;
  const sweep = useRef(new Animated.Value(-1)).current;
  useEffect(() => {
    const loop = Animated.loop(
      Animated.sequence([
        Animated.timing(sweep, { toValue: TOTAL + 1, duration: 3600, easing: Easing.linear, useNativeDriver: true }),
        Animated.delay(600),
      ]),
    );
    loop.start();
    return () => loop.stop();
  }, [sweep]);
  let i = 0;
  return (
    <View style={[styles.wrap, stacked && { flexDirection: 'column' }]}>
      {LANES.map((lane, li) => (
        <React.Fragment key={lane.title}>
          {li > 0 ? (
            <View style={[styles.link, stacked && styles.linkStacked]}>
              <Text style={[styles.linkArrow, stacked && { transform: [{ rotate: '90deg' }] }]}>→</Text>
            </View>
          ) : null}
          <View style={[styles.lane, { flex: lane.nodes.length === 1 ? 0.8 : 1 }, stacked && { flex: 0 }]}>
            <View style={styles.laneHead}>
              <Text style={[styles.laneTitle, { color: lane.color }]}>{lane.title}</Text>
              <Text style={styles.laneNote}>{lane.note}</Text>
            </View>
            {lane.nodes.map(([label, sub], ni) => {
              const idx = i++;
              return (
                <React.Fragment key={label}>
                  {ni > 0 ? <View style={[styles.wire, { backgroundColor: lane.color + '55' }]} /> : null}
                  <Node label={label} sub={sub} color={lane.color} index={idx} sweep={sweep} />
                </React.Fragment>
              );
            })}
            {lane.title === 'Your engine' ? (
              <View style={styles.targets}>
                {['Godot 4 GDExtension', 'Any C / C++ engine', 'Browser · WASM + canvas'].map((t) => (
                  <Text key={t} style={styles.target}>
                    {t}
                  </Text>
                ))}
              </View>
            ) : null}
          </View>
        </React.Fragment>
      ))}
    </View>
  );
}

const styles = StyleSheet.create({
  wrap: { flexDirection: 'row', alignItems: 'stretch' },
  lane: { padding: 14, gap: 0, borderRadius: 16, borderWidth: 1, borderColor: site.stroke, backgroundColor: 'rgba(10,15,29,0.75)' },
  laneHead: { flexDirection: 'row', alignItems: 'baseline', justifyContent: 'space-between', marginBottom: 12, gap: 8 },
  laneTitle: { fontFamily: font, fontWeight: '700', fontSize: 12, letterSpacing: 1.6, textTransform: 'uppercase' },
  laneNote: { fontFamily: font, fontSize: 11, color: colors.textMute },
  link: { width: 30, alignItems: 'center', justifyContent: 'center' },
  linkStacked: { width: 'auto', height: 30 },
  linkArrow: { fontFamily: font, fontSize: 18, color: colors.textMute },
  wire: { width: 2, height: 14, marginLeft: 17, borderRadius: 1 },
  node: { flexDirection: 'row', alignItems: 'center', gap: 12, paddingVertical: 10, paddingHorizontal: 12, borderRadius: 11, borderWidth: 1, borderColor: site.stroke, backgroundColor: 'rgba(255,255,255,0.03)' },
  nodeHot: { borderRadius: 11, borderWidth: 1, backgroundColor: 'rgba(255,255,255,0.04)' },
  nodeDot: { width: 10, height: 10, borderRadius: 5 },
  nodeLabel: { fontFamily: font, fontWeight: '700', fontSize: 14, color: colors.text },
  nodeSub: { fontFamily: mono, fontSize: 11, lineHeight: 15, color: colors.textMute },
  targets: { marginTop: 12, gap: 6 },
  target: { fontFamily: font, fontWeight: '500', fontSize: 12, color: colors.textDim, paddingLeft: 4 },
});
