// Feature: international text covered by Inter itself (Latin Extended,
// Vietnamese, Greek, Cyrillic), UTF-8 punctuation and symbols, and
// right-to-left direction with Latin text. Scripts outside Inter (CJK,
// Arabic) are left out on purpose: they would compare system fallback fonts.
import React from 'react';
import { StyleSheet, Text, View } from 'react-native';

export const meta = { width: 640, height: 420, maxMismatch: 1.5, title: 'International text' };

const LINES = [
  ['Deutsch', 'Größenverhältnisse · Straße · Übermäßig'],
  ['Français', 'Œuvre déjà commencée — « à bientôt »'],
  ['Polski', 'Zażółć gęślą jaźń'],
  ['Tiếng Việt', 'Trải nghiệm tuyệt vời của người chơi'],
  ['Ελληνικά', 'Ξεσκεπάζω την ψυχοφθόρα βδελυγμία'],
  ['Русский', 'Съешь же ещё этих мягких французских булок'],
  ['Symbols', '→ ← ↑ ↓ ★ ✓ … € £ ¥ ° ± × ÷ ≤ ≥'],
];

export default function App() {
  return (
    <View style={styles.root}>
      {LINES.map(([lang, text]) => (
        <View key={lang} style={styles.row}>
          <Text style={styles.lang}>{lang}</Text>
          <Text style={styles.text}>{text}</Text>
        </View>
      ))}
      <View style={[styles.row, { flexDirection: 'row-reverse' }]}>
        <Text style={styles.lang}>row-reverse</Text>
        <Text style={[styles.text, { textAlign: 'right' }]}>Aligned to the end ←</Text>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#11162a', padding: 24, gap: 10 },
  row: { flexDirection: 'row', alignItems: 'center', gap: 16, paddingVertical: 8, paddingHorizontal: 12, borderRadius: 6, backgroundColor: '#1d2442' },
  lang: { width: 110, fontFamily: 'Inter', fontWeight: '600', fontSize: 12, letterSpacing: 1, textTransform: 'uppercase', color: '#a3aecb' },
  text: { flex: 1, fontFamily: 'Inter', fontSize: 17, color: '#eef2ff' },
});
