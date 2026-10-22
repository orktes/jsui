// Feature: text. Inter in all shipped weights; sizes, letterSpacing,
// lineHeight, textTransform, alignment, decoration, shadows, ellipsis and
// nested spans.
import React from 'react';
import { StyleSheet, Text, View } from 'react-native';

export const meta = { width: 720, height: 520, maxMismatch: 1.5, title: 'Text' };

const F = 'Inter';

export default function App() {
  return (
    <View style={styles.root}>
      <View style={styles.col}>
        <Text style={[styles.t, { fontWeight: '400' }]}>Regular 400 — The quick brown fox</Text>
        <Text style={[styles.t, { fontWeight: '500' }]}>Medium 500 — The quick brown fox</Text>
        <Text style={[styles.t, { fontWeight: '600' }]}>SemiBold 600 — The quick brown fox</Text>
        <Text style={[styles.t, { fontWeight: '700' }]}>Bold 700 — The quick brown fox</Text>
        <Text style={[styles.t, { fontWeight: 'bold' }]}>Bold keyword — jumps over 1234567890</Text>
        <Text style={[styles.t, { fontStyle: 'italic' }]}>Italic — lazy dogs and wizards</Text>
        <View style={styles.sizes}>
          <Text style={[styles.t, { fontSize: 10 }]}>10</Text>
          <Text style={[styles.t, { fontSize: 14 }]}>14</Text>
          <Text style={[styles.t, { fontSize: 20 }]}>20</Text>
          <Text style={[styles.t, { fontSize: 32, fontWeight: '700' }]}>32</Text>
          <Text style={[styles.t, { fontSize: 48, fontWeight: '700', color: '#f2c35b' }]}>48</Text>
        </View>
        <Text style={[styles.t, { letterSpacing: 4, textTransform: 'uppercase', fontWeight: '600', color: '#4fd1ff' }]}>letter spacing 4</Text>
        <Text style={[styles.t, { letterSpacing: -0.8, fontSize: 22, fontWeight: '700' }]}>Tight tracking -0.8</Text>
        <Text style={[styles.t, { textTransform: 'capitalize' }]}>capitalize every word here</Text>
        <Text style={[styles.t, { textTransform: 'lowercase' }]}>LOWERCASE ME PLEASE</Text>
        <Text style={[styles.t, { textDecorationLine: 'underline', color: '#4fd1ff' }]}>Underlined link text</Text>
        <Text style={[styles.t, { textDecorationLine: 'line-through', color: '#a3aecb' }]}>Struck-through price 2,000</Text>
        <Text style={[styles.t, { fontSize: 24, fontWeight: '700', textShadowColor: '#ff4d6a', textShadowOffset: { width: 2, height: 2 }, textShadowRadius: 0 }]}>Hard text shadow</Text>
        <Text style={[styles.t, { fontSize: 24, fontWeight: '700', textShadowColor: 'rgba(79,209,255,0.9)', textShadowOffset: { width: 0, height: 0 }, textShadowRadius: 10 }]}>Glow text shadow</Text>
      </View>
      <View style={styles.col}>
        {/* Padding lives on wrapper Views: react-native-web's line clamp
            leaks the next line into a Text's own padding. */}
        <View style={styles.box}>
          <Text style={[styles.t, { textAlign: 'left' }]}>Left aligned</Text>
        </View>
        <View style={styles.box}>
          <Text style={[styles.t, { textAlign: 'center' }]}>Center aligned</Text>
        </View>
        <View style={styles.box}>
          <Text style={[styles.t, { textAlign: 'right' }]}>Right aligned</Text>
        </View>
        <View style={styles.box}>
          <Text style={styles.t} numberOfLines={1}>
            A single line that is far too long to fit inside this box gets an ellipsis
          </Text>
        </View>
        <View style={styles.box}>
          <Text style={[styles.t, { lineHeight: 20 }]} numberOfLines={2}>
            Two lines at most: the remaining words of this sentence are truncated with an ellipsis at the end of line two.
          </Text>
        </View>
        <View style={styles.box}>
          <Text style={[styles.t, { lineHeight: 26, color: '#a3aecb' }]}>
            A wrapped paragraph with a 26px line height. The traveller crossed the causeway at dusk, lantern held high against the tide.
          </Text>
        </View>
        <View style={styles.box}>
          <Text style={styles.t}>
            Nested <Text style={{ fontWeight: '700', color: '#f2c35b' }}>bold gold</Text>, <Text style={{ fontStyle: 'italic' }}>italic</Text>,{' '}
            <Text style={{ fontSize: 22, color: '#4ade80' }}>bigger</Text> and <Text style={{ fontSize: 11, color: '#ff4d6a' }}>smaller</Text> spans.
          </Text>
        </View>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, flexDirection: 'row', backgroundColor: '#11162a', padding: 20, gap: 20 },
  col: { flex: 1, gap: 6 },
  t: { fontFamily: F, fontSize: 15, color: '#eef2ff' },
  sizes: { flexDirection: 'row', alignItems: 'baseline', gap: 12 },
  box: { padding: 8, backgroundColor: '#1d2442', borderRadius: 4 },
});
