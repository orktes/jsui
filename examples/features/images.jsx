// Feature: images. resizeMode variants on a non-square source, rounded and
// bordered images, opacity, tintColor, ImageBackground with content.
import React from 'react';
import { Image, ImageBackground, StyleSheet, View } from 'react-native';

export const meta = { width: 640, height: 380, maxMismatch: 0.75, title: 'Images' };

const wide = require('../assets/card-ember.png'); // 320x180
const icon = require('../assets/item-potion-mana.png'); // 128x128, alpha
const tile = require('../assets/buff-frost.png'); // 64x64

export default function App() {
  return (
    <View style={styles.root}>
      <View style={styles.row}>
        <Image source={wide} style={styles.frame} resizeMode="cover" />
        <Image source={wide} style={styles.frame} resizeMode="contain" />
        <Image source={wide} style={styles.frame} resizeMode="stretch" />
        <Image source={icon} style={styles.frame} resizeMode="center" />
        <Image source={tile} style={styles.frame} resizeMode="repeat" />
      </View>
      <View style={styles.row}>
        <Image source={wide} style={[styles.frame, { borderRadius: 50 }]} resizeMode="cover" />
        <Image source={wide} style={[styles.frame, { borderRadius: 16, borderWidth: 4, borderColor: '#f2c35b' }]} resizeMode="cover" />
        <Image source={icon} style={[styles.frame, { opacity: 0.4 }]} />
        <Image source={icon} style={[styles.frame, { tintColor: '#4ade80' }]} />
        <ImageBackground source={wide} style={[styles.frame, { padding: 10, justifyContent: 'flex-end' }]} imageStyle={{ borderRadius: 12 }}>
          <View style={{ height: 20, borderRadius: 4, backgroundColor: 'rgba(0,0,0,0.6)' }} />
        </ImageBackground>
      </View>
      <View style={[styles.row, { alignItems: 'center' }]}>
        {[24, 32, 48, 64, 96].map((s) => (
          <Image key={s} source={icon} style={{ width: s, height: s }} />
        ))}
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#11162a', padding: 20, gap: 16 },
  row: { flexDirection: 'row', gap: 16 },
  frame: { width: 104, height: 104, backgroundColor: '#1d2442' },
});
