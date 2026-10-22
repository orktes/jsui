// Feature: gradients via experimental_backgroundImage. Shape-only.
// Row 1: linear 180deg (default), 90deg, 45deg, "to bottom right", hard stops.
// Row 2: multi-stop with positions + transparency over a colour, radial circle,
//        radial ellipse at position, gradient + radius + border, stacked layers.
import React from 'react';
import { StyleSheet, View } from 'react-native';

export const meta = { width: 640, height: 360, maxMismatch: 0.5, title: 'Gradients' };

export default function App() {
  return (
    <View style={styles.root}>
      <View style={styles.row}>
        <View style={[styles.box, { experimental_backgroundImage: 'linear-gradient(#ff4d6a, #4f8cff)' }]} />
        <View style={[styles.box, { experimental_backgroundImage: 'linear-gradient(90deg, #f2c35b, #a8741f)' }]} />
        <View style={[styles.box, { experimental_backgroundImage: 'linear-gradient(45deg, #4ade80 0%, #4fd1ff 100%)' }]} />
        <View style={[styles.box, { experimental_backgroundImage: 'linear-gradient(to bottom right, #b26bff, #11162a)' }]} />
        <View style={[styles.box, { experimental_backgroundImage: 'linear-gradient(90deg, #ff4d6a 0%, #ff4d6a 33%, #f2c35b 33%, #f2c35b 66%, #4ade80 66%)' }]} />
      </View>
      <View style={styles.row}>
        <View style={[styles.box, { backgroundColor: '#4f8cff', experimental_backgroundImage: 'linear-gradient(180deg, rgba(0,0,0,0) 0%, rgba(0,0,0,0.8) 60%, #000 100%)' }]} />
        <View style={[styles.box, { experimental_backgroundImage: 'radial-gradient(circle, #fff2c2 0%, #f2c35b 40%, #11162a 75%)' }]} />
        <View style={[styles.box, { experimental_backgroundImage: 'radial-gradient(ellipse at 30% 25%, #4fd1ff 0%, #1477b8 45%, #0b1a33 100%)' }]} />
        <View style={[styles.box, { borderRadius: 24, borderWidth: 4, borderColor: '#eef2ff', experimental_backgroundImage: 'linear-gradient(135deg, #ff9a3c, #9e1730)' }]} />
        <View
          style={[
            styles.box,
            {
              borderRadius: 12,
              experimental_backgroundImage:
                'radial-gradient(circle at 75% 25%, rgba(255,255,255,0.7) 0%, rgba(255,255,255,0) 35%), linear-gradient(180deg, #b26bff, #4f8cff)',
            },
          ]}
        />
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#11162a', padding: 30, gap: 40 },
  row: { flexDirection: 'row', gap: 26 },
  box: { width: 100, height: 100 },
});
