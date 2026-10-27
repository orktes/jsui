// /features/motion: an Animated + LayoutAnimation playground.
import React from 'react';
import { StyleSheet, Text, View } from 'react-native';
import { colors, font, mono, site, text } from '../theme.js';
import { Column, Reveal, SectionHead, useLayout } from '../components/ui.jsx';
import { SpringLab } from './motion/SpringLab.jsx';
import { EasingGallery } from './motion/EasingGallery.jsx';
import { Loops } from './motion/Loops.jsx';
import { Interpolation } from './motion/Interpolation.jsx';
import { LayoutLab } from './motion/LayoutLab.jsx';
import { Fling } from './motion/Fling.jsx';

function DriverDiagram() {
  const { phone } = useLayout();
  const step = (label, sub, color) => (
    <View style={[styles.step, { borderColor: color + '55' }]}>
      <Text style={[styles.stepLabel, { color }]}>{label}</Text>
      <Text style={styles.stepSub}>{sub}</Text>
    </View>
  );
  const arrow = <Text style={[styles.arrow, phone && { transform: [{ rotate: '90deg' }] }]}>→</Text>;
  return (
    <View style={styles.diagram}>
      <View style={[styles.flow, phone && { flexDirection: 'column', alignItems: 'stretch' }]}>
        {step('JS, once', 'Animated.spring(v, { useNativeDriver: true }).start()', colors.cyan)}
        {arrow}
        {step('C++ runtime, every tick', 'steps springs, timings, decays, loops and interpolations', colors.gold)}
        {arrow}
        {step('Painter', 'writes transform / opacity / colors, repaints only damaged rects', colors.green)}
      </View>
      <Text style={styles.diagramNote}>JS hears back only when an animation ends (or a value listener asks). Everything below runs that way unless the code says otherwise.</Text>
    </View>
  );
}

export function MotionPage() {
  const { phone } = useLayout();
  const gap = phone ? 18 : 28;
  return (
    <Column style={{ paddingTop: phone ? 36 : 64, paddingBottom: 40 }}>
      <Reveal>
        <SectionHead
          eyebrow="Motion lab"
          title="Animated, the React Native way. Running in C++."
          lead="JSUI ships React Native's Animated and LayoutAnimation APIs with both drivers. Animations started with useNativeDriver: true run in the C++ runtime without touching JS each frame, so a busy game loop never makes your UI stutter. Everything on this page is interactive."
        />
      </Reveal>
      <Reveal delay={120} style={{ marginTop: phone ? 24 : 36 }}>
        <DriverDiagram />
      </Reveal>
      <View style={{ gap, marginTop: phone ? 32 : 56 }}>
        <SpringLab />
        <EasingGallery />
        <Loops />
        <Interpolation />
        <LayoutLab />
        <Fling />
      </View>
    </Column>
  );
}

const styles = StyleSheet.create({
  diagram: { gap: 14, padding: 20, borderRadius: 16, borderWidth: 1, borderColor: site.stroke, backgroundColor: 'rgba(10,15,29,0.7)' },
  flow: { flexDirection: 'row', alignItems: 'center', gap: 12 },
  step: { flex: 1, gap: 4, padding: 14, borderRadius: 12, borderWidth: 1, backgroundColor: 'rgba(255,255,255,0.02)' },
  stepLabel: { fontFamily: font, fontWeight: '700', fontSize: 14 },
  stepSub: { fontFamily: mono, fontSize: 12, lineHeight: 18, color: colors.textDim },
  arrow: { fontFamily: font, fontSize: 20, color: colors.textMute, textAlign: 'center' },
  diagramNote: { ...text.small, lineHeight: 19 },
});
