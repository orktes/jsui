// Feature: TextInput (unfocused, so no caret). Values, placeholders with a
// custom color (the default differs on purpose: react-native-web inherits
// the text color, JSUI uses Chrome's gray like native RN's gray),
// secureTextEntry bullets, alignment, letterSpacing,
// textTransform, RTL, a value wider than the box (start edge shown) and a
// multiline input.
import React from 'react';
import { StyleSheet, TextInput, View } from 'react-native';

export const meta = { width: 640, height: 360, maxMismatch: 1.5, title: 'TextInput' };

const F = 'Inter';

export default function App() {
  return (
    <View style={styles.root}>
      <View style={styles.col}>
        <TextInput style={styles.input} value="Player One" />
        <TextInput style={styles.input} placeholder="Search items…" placeholderTextColor="#7d86a8" />
        <TextInput style={[styles.input, { fontSize: 13 }]} placeholder="Smaller placeholder, wider text" placeholderTextColor="rgba(238,242,255,0.5)" />
        <TextInput style={styles.input} value="hunter2" secureTextEntry />
        <TextInput style={[styles.input, { textAlign: 'center' }]} value="Centered" />
        <TextInput style={[styles.input, { textAlign: 'right' }]} defaultValue="Right aligned" />
      </View>
      <View style={styles.col}>
        <TextInput style={[styles.input, { letterSpacing: 3 }]} value="SPACED OUT" />
        <TextInput style={[styles.input, { textTransform: 'uppercase' }]} value="upper case value" />
        <TextInput style={styles.input} value="שלום עולם" />
        <TextInput style={styles.input} value="A value that is much wider than its input box" />
        <TextInput style={[styles.input, styles.multi]} multiline value={'First line\nSecond line'} />
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, flexDirection: 'row', padding: 20, gap: 24, backgroundColor: '#141a2e' },
  col: { gap: 12 },
  input: {
    width: 260,
    height: 36,
    paddingHorizontal: 10,
    borderWidth: 1,
    borderColor: '#3a4468',
    borderRadius: 6,
    backgroundColor: '#1d2442',
    color: '#eef2ff',
    fontFamily: F,
    fontSize: 16,
  },
  multi: { height: 64, paddingVertical: 6 },
});
