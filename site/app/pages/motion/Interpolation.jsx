// Interpolation: one Animated.Value drives color, rotation, scale and radius
// through interpolate(). Scrub it with the slider or let it auto-play.
import React, { useEffect, useRef, useState } from 'react';
import { Animated, Easing, StyleSheet, Text, View } from 'react-native';
import { colors, font, mono, site } from '../../theme.js';
import { DemoButton, DemoCard, Slider } from './controls.jsx';

const TICKS = 13;

export function Interpolation() {
  const p = useRef(new Animated.Value(0.35)).current;
  const [v, setV] = useState(0.35);
  const [auto, setAuto] = useState(true);
  useEffect(() => {
    if (!auto) return undefined;
    p.setValue(0);
    const loop = Animated.loop(
      Animated.sequence([
        Animated.timing(p, { toValue: 1, duration: 2400, easing: Easing.inOut(Easing.cubic), useNativeDriver: true }),
        Animated.delay(400),
        Animated.timing(p, { toValue: 0, duration: 1600, easing: Easing.inOut(Easing.cubic), useNativeDriver: true }),
        Animated.delay(300),
      ]),
    );
    loop.start();
    return () => loop.stop();
  }, [auto, p]);

  const scrub = (x) => {
    if (auto) setAuto(false);
    setV(x);
    p.setValue(x);
  };

  const color = p.interpolate({ inputRange: [0, 0.5, 1], outputRange: ['#4f8cff', '#b26bff', '#ff9a3c'] });
  const glow = p.interpolate({ inputRange: [0, 0.5, 1], outputRange: ['rgba(79,140,255,0.25)', 'rgba(178,107,255,0.35)', 'rgba(255,154,60,0.55)'] });
  const needle = p.interpolate({ inputRange: [0, 1], outputRange: ['-120deg', '120deg'] });
  const spin = p.interpolate({ inputRange: [0, 1], outputRange: ['0deg', '225deg'] });
  const scale = p.interpolate({ inputRange: [0, 0.8, 1], outputRange: [0.8, 1.05, 1.18] });
  const radius = p.interpolate({ inputRange: [0, 1], outputRange: [8, 46] });

  return (
    <DemoCard
      index={4}
      title="Interpolation"
      blurb="One value, many outputs. interpolate() maps a number onto colors, angles, scales and radii, with multi-stop ranges and clamping. On the native driver colors are interpolated per channel in C++, exactly like the JS driver would."
      tags={[['interpolate', colors.purple], ['color', colors.orange], ['rotate', colors.cyan]]}
      codeTitle="interpolate.jsx"
      code={`
const charge = useRef(new Animated.Value(0)).current;

const color = charge.interpolate({
  inputRange:  [0, 0.5, 1],
  outputRange: ['#4f8cff', '#b26bff', '#ff9a3c'],
});
const needle = charge.interpolate({
  inputRange:  [0, 1],
  outputRange: ['-120deg', '120deg'],
});
const scale = charge.interpolate({
  inputRange:  [0, 0.8, 1],
  outputRange: [0.8, 1.05, 1.18],
});

<Animated.View style={{ backgroundColor: color,
  transform: [{ scale }] }} />`}
    >
      <View style={styles.row}>
        <View style={styles.cell}>
          <View style={styles.gauge}>
            {Array.from({ length: TICKS }, (_, i) => {
              const a = -120 + (240 * i) / (TICKS - 1);
              const hot = i / (TICKS - 1) > 0.75;
              return (
                <View key={i} style={[styles.tickArm, { transform: [{ rotate: `${a}deg` }] }]}>
                  <View style={[styles.tick, hot && { backgroundColor: colors.orange }, i % 3 === 0 && { height: 12 }]} />
                </View>
              );
            })}
            <Animated.View style={[styles.needleArm, { transform: [{ rotate: needle }] }]}>
              <Animated.View style={[styles.needle, { backgroundColor: color }]} />
            </Animated.View>
            <View style={styles.hub} />
          </View>
          <Text style={styles.cap}>rotate · '-120deg' → '120deg'</Text>
        </View>

        <View style={styles.cell}>
          <View style={styles.orbBox}>
            <Animated.View style={[styles.halo, { backgroundColor: glow, transform: [{ scale }] }]} />
            <Animated.View style={[styles.orbRing, { borderTopColor: color, borderRightColor: color, transform: [{ rotate: spin }] }]} />
            <Animated.View style={[styles.orb, { backgroundColor: color, transform: [{ scale }] }]}>
              <View style={styles.orbShine} />
            </Animated.View>
          </View>
          <Text style={styles.cap}>backgroundColor · scale</Text>
        </View>

        <View style={styles.cell}>
          <View style={styles.orbBox}>
            <Animated.View style={[styles.morph, { borderRadius: radius, borderColor: color, transform: [{ rotate: spin }] }]} />
          </View>
          <Text style={styles.cap}>borderRadius · borderColor</Text>
        </View>
      </View>

      <View style={{ flexDirection: 'row', alignItems: 'flex-end', gap: 14, flexWrap: 'wrap' }}>
        <Slider
          style={{ flex: 1, minWidth: 220 }}
          label={auto ? 'Charge (auto-playing; drag to take over)' : 'Charge'}
          value={v}
          min={0}
          max={1}
          step={0.01}
          color={colors.purple}
          onChange={scrub}
          format={(x) => (auto ? 'auto' : x.toFixed(2))}
        />
        <DemoButton label={auto ? 'Auto: on' : 'Auto: off'} icon={auto ? '■' : '▶'} color={colors.purple} onPress={() => setAuto((a) => !a)} />
      </View>
    </DemoCard>
  );
}

const styles = StyleSheet.create({
  row: { flexDirection: 'row', flexWrap: 'wrap', gap: 10 },
  cell: { flexGrow: 1, flexBasis: 160, alignItems: 'center', gap: 8, paddingVertical: 12, borderRadius: 12, borderWidth: 1, borderColor: site.stroke, backgroundColor: 'rgba(255,255,255,0.02)' },
  cap: { fontFamily: mono, fontSize: 11, color: colors.textMute },
  gauge: { width: 130, height: 130, alignItems: 'center', justifyContent: 'center' },
  tickArm: { position: 'absolute', width: 4, height: 120, alignItems: 'center' },
  tick: { width: 3, height: 8, borderRadius: 1.5, backgroundColor: 'rgba(148,170,255,0.4)' },
  needleArm: { position: 'absolute', width: 6, height: 110, alignItems: 'center' },
  needle: { width: 4, height: 52, borderRadius: 2, boxShadow: '0 0 8px rgba(255,255,255,0.35)' },
  hub: { width: 16, height: 16, borderRadius: 8, backgroundColor: '#eef2ff', borderWidth: 3, borderColor: site.cardSolid },
  orbBox: { width: 130, height: 130, alignItems: 'center', justifyContent: 'center' },
  halo: { position: 'absolute', width: 120, height: 120, borderRadius: 60 },
  orbRing: { position: 'absolute', width: 96, height: 96, borderRadius: 48, borderWidth: 3, borderColor: 'rgba(148,170,255,0.1)' },
  orb: { width: 60, height: 60, borderRadius: 30, overflow: 'hidden', borderWidth: 2, borderColor: 'rgba(255,255,255,0.5)' },
  orbShine: { position: 'absolute', left: 8, top: 6, width: 24, height: 16, borderRadius: 12, backgroundColor: 'rgba(255,255,255,0.45)' },
  morph: { width: 92, height: 92, borderWidth: 4, backgroundColor: 'rgba(148,170,255,0.06)' },
});
