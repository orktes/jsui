// Native-driver loops: the ambient motion every game UI needs (spinners,
// pulses, shimmer, loaders). Each is started once from JS and then runs in
// the C++ runtime; "Pause" stops them all.
import React, { useEffect, useRef, useState } from 'react';
import { Animated, Easing, StyleSheet, Text, View } from 'react-native';
import { colors, font, mono, site } from '../../theme.js';
import { DemoButton, DemoCard } from './controls.jsx';

const useV = (v = 0) => useRef(new Animated.Value(v)).current;
const native = { useNativeDriver: true };

function useLoops(running, make) {
  useEffect(() => {
    if (!running) return undefined;
    const anims = make();
    anims.forEach((a) => a.start());
    return () => anims.forEach((a) => a.stop());
  }, [running]); // eslint-disable-line react-hooks/exhaustive-deps
}

function Spinner({ running }) {
  const r = useV();
  const r2 = useV();
  useLoops(running, () => [
    Animated.loop(Animated.timing(r, { toValue: 1, duration: 1100, easing: Easing.linear, ...native })),
    Animated.loop(Animated.timing(r2, { toValue: 1, duration: 1700, easing: Easing.inOut(Easing.cubic), ...native })),
  ]);
  const spin = (v, dir = 1) => ({ transform: [{ rotate: v.interpolate({ inputRange: [0, 1], outputRange: ['0deg', `${360 * dir}deg`] }) }] });
  return (
    <View style={styles.center}>
      <Animated.View style={[styles.ring, { borderTopColor: colors.gold, borderRightColor: 'rgba(242,195,91,0.35)' }, spin(r)]} />
      <Animated.View style={[styles.ring2, { borderBottomColor: colors.cyan, borderLeftColor: 'rgba(79,209,255,0.35)' }, spin(r2, -1)]} />
      <View style={styles.core} />
    </View>
  );
}

function Pulse({ running }) {
  const clock = useV();
  // One clock; each ring is the same clock phase-shifted with Animated.modulo.
  useLoops(running, () => [Animated.loop(Animated.timing(clock, { toValue: 1, duration: 2400, easing: Easing.linear, ...native }))]);
  const vals = useRef([0, 1, 2].map((i) => Animated.modulo(Animated.add(clock, i / 3), 1))).current;
  return (
    <View style={styles.center}>
      {vals.map((v, i) => (
        <Animated.View
          key={i}
          style={[
            styles.pulseRing,
            {
              opacity: v.interpolate({ inputRange: [0, 0.1, 1], outputRange: [0, 0.9, 0] }),
              transform: [{ scale: v.interpolate({ inputRange: [0, 1], outputRange: [0.4, 2.1] }) }],
            },
          ]}
        />
      ))}
      <View style={styles.pulseCore}>
        <Text style={styles.pulseText}>!</Text>
      </View>
    </View>
  );
}

function Shimmer({ running }) {
  const s = useV();
  useLoops(running, () => [Animated.loop(Animated.timing(s, { toValue: 1, duration: 1400, easing: Easing.inOut(Easing.quad), ...native }))]);
  const x = s.interpolate({ inputRange: [0, 1], outputRange: [-120, 260] });
  return (
    <View style={styles.skel}>
      <View style={{ flexDirection: 'row', gap: 10, alignItems: 'center' }}>
        <View style={[styles.skelBlock, { width: 36, height: 36, borderRadius: 18 }]} />
        <View style={{ flex: 1, gap: 7 }}>
          <View style={[styles.skelBlock, { width: '80%', height: 9 }]} />
          <View style={[styles.skelBlock, { width: '50%', height: 9 }]} />
        </View>
      </View>
      <View style={[styles.skelBlock, { height: 9 }]} />
      <View style={[styles.skelBlock, { height: 9, width: '70%' }]} />
      <Animated.View pointerEvents="none" style={[styles.shine, { transform: [{ translateX: x }, { skewX: '-18deg' }] }]} />
    </View>
  );
}

function Dots({ running }) {
  const vals = [useV(), useV(), useV(), useV()];
  useLoops(running, () => [
    Animated.loop(
      Animated.stagger(
        110,
        vals.map((v) =>
          Animated.sequence([
            Animated.timing(v, { toValue: 1, duration: 260, easing: Easing.out(Easing.quad), ...native }),
            Animated.timing(v, { toValue: 0, duration: 420, easing: Easing.bounce, ...native }),
          ]),
        ),
      ),
    ),
  ]);
  const tint = [colors.gold, colors.orange, colors.red, colors.purple];
  return (
    <View style={[styles.center, { flexDirection: 'row', gap: 9 }]}>
      {vals.map((v, i) => (
        <Animated.View
          key={i}
          style={[
            styles.bdot,
            { backgroundColor: tint[i] },
            { transform: [{ translateY: v.interpolate({ inputRange: [0, 1], outputRange: [8, -16] }) }, { scale: v.interpolate({ inputRange: [0, 1], outputRange: [1, 1.15] }) }] },
          ]}
        />
      ))}
    </View>
  );
}

function Orbit({ running }) {
  const r = useV();
  const b = useV();
  useLoops(running, () => [
    Animated.loop(Animated.timing(r, { toValue: 1, duration: 3200, easing: Easing.linear, ...native })),
    Animated.loop(
      Animated.sequence([
        Animated.timing(b, { toValue: 1, duration: 1200, easing: Easing.inOut(Easing.sin), ...native }),
        Animated.timing(b, { toValue: 0, duration: 1200, easing: Easing.inOut(Easing.sin), ...native }),
      ]),
    ),
  ]);
  const rot = r.interpolate({ inputRange: [0, 1], outputRange: ['0deg', '360deg'] });
  return (
    <View style={styles.center}>
      <View style={styles.orbitPath} />
      <Animated.View style={[styles.orbitArm, { transform: [{ rotate: rot }] }]}>
        <View style={[styles.moon, { backgroundColor: colors.cyan, left: -7 }]} />
        <View style={[styles.moon, { backgroundColor: colors.purple, right: -7, width: 10, height: 10, borderRadius: 5, top: 39 }]} />
      </Animated.View>
      <Animated.View
        style={[
          styles.planet,
          {
            opacity: b.interpolate({ inputRange: [0, 1], outputRange: [0.75, 1] }),
            transform: [{ scale: b.interpolate({ inputRange: [0, 1], outputRange: [0.92, 1.08] }) }],
          },
        ]}
      />
    </View>
  );
}

function Bar({ running }) {
  const t = useV();
  useLoops(running, () => [Animated.loop(Animated.timing(t, { toValue: 1, duration: 2600, easing: Easing.linear, ...native }))]);
  // Ease in-out over the first 85 %, then hold full before the loop restarts.
  const p = t.interpolate({ inputRange: [0, 0.15, 0.45, 0.7, 0.85, 1], outputRange: [0, 0.06, 0.5, 0.92, 1, 1] });
  return (
    <View style={[styles.center, { paddingHorizontal: 18, gap: 10, alignItems: 'stretch' }]}>
      <View style={styles.barTrack}>
        <Animated.View style={[styles.barFill, { transform: [{ scaleX: p }] }]} />
      </View>
      <Animated.Text style={[styles.barLabel, { opacity: p.interpolate({ inputRange: [0, 0.9, 1], outputRange: [0.5, 0.8, 1] }) }]}>Loading shaders</Animated.Text>
    </View>
  );
}

const TILES = [
  ['Spinner', 'rotate · linear', Spinner],
  ['Pulse rings', 'scale + opacity · modulo', Pulse],
  ['Skeleton shimmer', 'translateX + skewX', Shimmer],
  ['Bounce loader', 'stagger · Easing.bounce', Dots],
  ['Orbit', 'rotate an arm', Orbit],
  ['Progress', 'scaleX · transformOrigin', Bar],
];

export function Loops() {
  const [running, setRunning] = useState(true);
  return (
    <DemoCard
      index={3}
      title="Native-driver loops"
      blurb="Ambient motion is where a game UI spends most of its frames. These loops are started once from JS with useNativeDriver: true; after that the C++ runtime steps them every tick, writes the transforms itself and repaints only the pixels that moved. Not one line of JS runs per frame."
      tags={[['useNativeDriver: true', colors.green], ['Animated.loop', colors.gold], ['incremental repaint', colors.cyan]]}
      codeTitle="spinner.jsx"
      code={`
const spin = useRef(new Animated.Value(0)).current;

useEffect(() => {
  const loop = Animated.loop(
    Animated.timing(spin, {
      toValue: 1, duration: 1100,
      easing: Easing.linear,
      useNativeDriver: true,   // runs in C++
    }),
  );
  loop.start();
  return () => loop.stop();
}, []);

const rotate = spin.interpolate({
  inputRange: [0, 1],
  outputRange: ['0deg', '360deg'],
});
<Animated.View style={[styles.ring, { transform: [{ rotate }] }]} />`}
    >
      <View style={styles.tiles}>
        {TILES.map(([name, hint, C]) => (
          <View key={name} style={styles.tile}>
            <View style={styles.tileStage}>
              <C running={running} />
            </View>
            <Text style={styles.tileName}>{name}</Text>
            <Text style={styles.tileHint}>{hint}</Text>
          </View>
        ))}
      </View>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 12, flexWrap: 'wrap' }}>
        <DemoButton primary={!running} label={running ? 'Pause loops' : 'Resume loops'} icon={running ? '■' : '▶'} onPress={() => setRunning((r) => !r)} />
        <Text style={styles.note}>JS runs only at loop boundaries, never per frame.</Text>
      </View>
    </DemoCard>
  );
}

const styles = StyleSheet.create({
  tiles: { flexDirection: 'row', flexWrap: 'wrap', gap: 10 },
  tile: { flexGrow: 1, flexBasis: 150, gap: 3, padding: 10, borderRadius: 12, borderWidth: 1, borderColor: site.stroke, backgroundColor: 'rgba(255,255,255,0.02)' },
  tileStage: { height: 110, overflow: 'hidden', borderRadius: 8, marginBottom: 6 },
  tileName: { fontFamily: font, fontWeight: '700', fontSize: 13, color: colors.text },
  tileHint: { fontFamily: mono, fontSize: 11, color: colors.textMute },
  center: { flex: 1, alignItems: 'center', justifyContent: 'center' },
  ring: { position: 'absolute', width: 64, height: 64, borderRadius: 32, borderWidth: 5, borderColor: 'rgba(242,195,91,0.08)' },
  ring2: { position: 'absolute', width: 42, height: 42, borderRadius: 21, borderWidth: 4, borderColor: 'rgba(79,209,255,0.08)' },
  core: { width: 10, height: 10, borderRadius: 5, backgroundColor: colors.goldLight, boxShadow: '0 0 12px rgba(242,195,91,0.9)' },
  pulseRing: { position: 'absolute', width: 48, height: 48, borderRadius: 24, borderWidth: 2, borderColor: colors.red },
  pulseCore: { width: 30, height: 30, borderRadius: 15, backgroundColor: colors.red, alignItems: 'center', justifyContent: 'center', boxShadow: '0 0 16px rgba(255,77,106,0.8)' },
  pulseText: { fontFamily: font, fontWeight: '700', fontSize: 16, color: '#fff' },
  skel: { flex: 1, margin: 8, padding: 12, gap: 9, borderRadius: 10, backgroundColor: 'rgba(148,170,255,0.06)', overflow: 'hidden' },
  skelBlock: { borderRadius: 5, backgroundColor: 'rgba(148,170,255,0.14)' },
  shine: { position: 'absolute', top: -20, bottom: -20, width: 70, experimental_backgroundImage: 'linear-gradient(90deg, rgba(255,255,255,0) 0%, rgba(255,255,255,0.14) 50%, rgba(255,255,255,0) 100%)' },
  bdot: { width: 12, height: 12, borderRadius: 6 },
  orbitPath: { position: 'absolute', width: 88, height: 88, borderRadius: 44, borderWidth: 1, borderColor: 'rgba(148,170,255,0.18)' },
  orbitArm: { position: 'absolute', width: 88, height: 88 },
  moon: { position: 'absolute', top: 37, width: 14, height: 14, borderRadius: 7 },
  planet: { width: 34, height: 34, borderRadius: 17, experimental_backgroundImage: 'radial-gradient(circle at 35% 35%, #ffe29a 0%, #f2c35b 40%, #a8741f 100%)', boxShadow: '0 0 20px rgba(242,195,91,0.6)' },
  barTrack: { height: 8, borderRadius: 4, backgroundColor: 'rgba(148,170,255,0.14)', overflow: 'hidden' },
  barFill: { height: 8, borderRadius: 4, transformOrigin: 'left', experimental_backgroundImage: 'linear-gradient(90deg, #4f8cff 0%, #4fd1ff 100%)', boxShadow: '0 0 10px rgba(79,209,255,0.7)' },
  barLabel: { fontFamily: mono, fontSize: 11, color: colors.textDim, textAlign: 'center' },
  note: { fontFamily: font, fontSize: 13, color: colors.textMute },
});
