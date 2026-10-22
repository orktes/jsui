import React from 'react';
import { StyleSheet, Text, View } from 'react-native';
import { colors, font, site } from '../theme.js';
import { Column, GITHUB, useLayout } from './ui.jsx';
import { Logo } from './NavBar.jsx';
import { Button } from './ui.jsx';

export function Footer() {
  const { phone } = useLayout();
  return (
    <View style={styles.wrap}>
      <Column style={[styles.row, phone && { flexDirection: 'column', alignItems: 'flex-start' }]}>
        <View style={{ gap: 10, flexShrink: 1 }}>
          <Logo size={16} />
          <Text style={styles.note}>
            This entire site is a JSUI app: React Native code running on QuickJS, laid out by Yoga and painted by Skia,
            compiled to WebAssembly and drawn into a single canvas.
          </Text>
        </View>
        <Button small kind="ghost" label="View source" href={GITHUB + '/tree/main/site'} />
      </Column>
    </View>
  );
}

const styles = StyleSheet.create({
  wrap: { marginTop: 96, paddingVertical: 36, borderTopWidth: 1, borderTopColor: site.stroke, backgroundColor: 'rgba(0,0,0,0.25)' },
  row: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', gap: 24 },
  note: { fontFamily: font, fontSize: 13, lineHeight: 20, color: colors.textMute, maxWidth: 560 },
});
