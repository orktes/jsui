// Segmented control with a springy sliding indicator. Every segment is a
// focusable Pressable, so it works with mouse, touch, keyboard and gamepad.
import React, { useEffect, useRef, useState } from 'react';
import { Animated, Pressable, ScrollView, StyleSheet, Text, View } from 'react-native';
import { colors, font, site } from '../../theme.js';

export function Segmented({ items, value, onChange, scrollable, style, accent = colors.gold }) {
  const x = useRef(new Animated.Value(0)).current;
  const w = useRef(new Animated.Value(0)).current;
  const [frames, setFrames] = useState({});
  const placed = useRef(false);
  const f = frames[value];

  useEffect(() => {
    if (!f) return;
    if (!placed.current) {
      x.setValue(f.x);
      w.setValue(f.width);
      placed.current = true;
      return;
    }
    Animated.parallel([
      Animated.spring(x, { toValue: f.x, stiffness: 320, damping: 26, mass: 0.9, useNativeDriver: false }),
      Animated.spring(w, { toValue: f.width, stiffness: 320, damping: 26, mass: 0.9, useNativeDriver: false }),
    ]).start();
  }, [value, f && f.x, f && f.width]);

  const body = (
    <View style={[styles.wrap, style]}>
      <Animated.View
        pointerEvents="none"
        style={[styles.indicator, { left: x, width: w, opacity: f ? 1 : 0, borderColor: accent + '66', backgroundColor: accent + '1c' }]}
      />
      {items.map((it) => {
        const active = it.id === value;
        return (
          <Pressable
            key={it.id}
            onPress={() => onChange(it.id)}
            onLayout={(e) => {
              const { x: lx, width } = e.nativeEvent.layout;
              setFrames((s) => (s[it.id] && s[it.id].x === lx && s[it.id].width === width ? s : { ...s, [it.id]: { x: lx, width } }));
            }}
            style={({ hovered, focused }) => [styles.seg, (hovered || focused) && !active && styles.segHot, focused && styles.segFocus]}
          >
            {it.icon ? <Text style={[styles.icon, { color: active ? accent : colors.textMute }]}>{it.icon}</Text> : null}
            <Text style={[styles.label, active && { color: colors.text }]}>{it.label}</Text>
          </Pressable>
        );
      })}
    </View>
  );
  if (!scrollable) return body;
  return (
    <ScrollView horizontal showsHorizontalScrollIndicator={false} style={{ flexGrow: 0 }} contentContainerStyle={{ paddingRight: 4 }}>
      {body}
    </ScrollView>
  );
}

const styles = StyleSheet.create({
  wrap: {
    flexDirection: 'row',
    alignSelf: 'flex-start',
    padding: 4,
    gap: 2,
    borderRadius: 14,
    borderWidth: 1,
    borderColor: site.stroke,
    backgroundColor: 'rgba(10, 15, 29, 0.85)',
  },
  indicator: { position: 'absolute', top: 4, bottom: 4, borderRadius: 10, borderWidth: 1 },
  seg: { height: 36, paddingHorizontal: 14, borderRadius: 10, flexDirection: 'row', alignItems: 'center', gap: 8 },
  segHot: { backgroundColor: 'rgba(255,255,255,0.04)' },
  segFocus: { boxShadow: '0 0 0 2px rgba(79, 209, 255, 0.9)' },
  icon: { fontFamily: font, fontWeight: '700', fontSize: 13 },
  label: { fontFamily: font, fontWeight: '600', fontSize: 14, color: colors.textDim },
});
