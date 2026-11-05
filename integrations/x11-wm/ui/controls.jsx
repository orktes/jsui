// Small touch controls shared by the shell's panels: icon buttons, toggle
// tiles, a finger-sized slider and the panel card.
import React, { memo, useRef, useState } from 'react';
import { PanResponder, Pressable, StyleSheet, Text, View } from 'react-native';
import { Icon } from './icons.jsx';
import { font, touch, useTheme } from './theme.js';

export const IconButton = memo(function IconButton({ icon, onPress, size = touch.button, iconSize = 24, active, label, style, anchor, color }) {
  const t = useTheme();
  return (
    <Pressable
      ref={anchor && anchor.ref}
      onLayout={anchor && anchor.onLayout}
      onPress={onPress}
      accessibilityLabel={label}
      style={({ pressed }) => [
        { width: size, height: size, borderRadius: size / 2, alignItems: 'center', justifyContent: 'center' },
        active && { backgroundColor: t.accent },
        pressed && { backgroundColor: active ? t.accentStrong : t.panelLine },
        style,
      ]}
    >
      <Icon name={icon} size={iconSize} color={color || (active ? t.accentText : t.text)} />
    </Pressable>
  );
});

export const Card = memo(function Card({ children, style }) {
  const t = useTheme();
  return (
    <View
      onStartShouldSetResponder={() => true}
      style={[
        styles.card,
        { backgroundColor: t.panel, borderColor: t.panelLine, boxShadow: `0px 20px 60px ${t.shadow}` },
        style,
      ]}
    >
      {children}
    </View>
  );
});

export const ToggleTile = memo(function ToggleTile({ icon, label, sub, on, onPress, anchor }) {
  const t = useTheme();
  return (
    <Pressable
      ref={anchor && anchor.ref}
      onLayout={anchor && anchor.onLayout}
      onPress={onPress}
      style={({ pressed }) => [
        styles.tile,
        { backgroundColor: on ? t.accent : t.panelRaised, transform: [{ scale: pressed ? 0.97 : 1 }] },
      ]}
    >
      <View style={[styles.tileIcon, { backgroundColor: on ? 'rgba(255,255,255,0.18)' : t.panelLine }]}>
        <Icon name={icon} size={22} color={on ? t.accentText : t.text} />
      </View>
      <View style={{ flex: 1 }}>
        <Text numberOfLines={1} style={[styles.tileLabel, { color: on ? t.accentText : t.text }]}>{label}</Text>
        <Text numberOfLines={1} style={[styles.tileSub, { color: on ? 'rgba(255,255,255,0.8)' : t.textMute }]}>
          {sub != null ? sub : on ? 'On' : 'Off'}
        </Text>
      </View>
    </Pressable>
  );
});

// A wide slider for fingers: drag anywhere on the track.
export const Slider = memo(function Slider({ icon, value, onChange, anchor }) {
  const t = useTheme();
  const [width, setWidth] = useState(1);
  const state = useRef({ width: 1, onChange });
  state.current.width = width;
  state.current.onChange = onChange;
  const pan = useRef(null);
  if (!pan.current) {
    const set = (x) => state.current.onChange(Math.max(0, Math.min(1, x / state.current.width)));
    pan.current = PanResponder.create({
      onStartShouldSetPanResponder: () => true,
      onMoveShouldSetPanResponder: () => true,
      onPanResponderTerminationRequest: () => false,
      onPanResponderGrant: (e) => set(e.nativeEvent.locationX),
      onPanResponderMove: (e, g) => set(g.moveX - state.current.left),
    });
  }
  const trackRef = useRef(null);
  return (
    <View style={styles.sliderRow}>
      <Icon name={icon} size={24} color={t.textDim} />
      <View
        ref={(r) => {
          trackRef.current = r;
          if (anchor) anchor.ref.current = r;
        }}
        onLayout={(e) => {
          setWidth(e.nativeEvent.layout.width);
          if (trackRef.current && trackRef.current.measureInWindow)
            trackRef.current.measureInWindow((x) => {
              state.current.left = x;
            });
          if (anchor) anchor.onLayout();
        }}
        {...pan.current.panHandlers}
        style={[styles.track, { backgroundColor: t.panelRaised }]}
      >
        <View style={[styles.fill, { width: Math.max(28, value * width), backgroundColor: t.accent }]}>
          <View style={styles.knob} />
        </View>
      </View>
    </View>
  );
});

export function Label({ children, style }) {
  const t = useTheme();
  return <Text style={[{ fontFamily: font, color: t.textDim, fontSize: 14, fontWeight: '600', letterSpacing: 0.4 }, style]}>{children}</Text>;
}

const styles = StyleSheet.create({
  card: { borderRadius: 26, borderWidth: 1, padding: 18, overflow: 'hidden' },
  tile: { flexDirection: 'row', alignItems: 'center', height: 68, borderRadius: 20, paddingHorizontal: 12, flexBasis: '48%', flexGrow: 1 },
  tileIcon: { width: 40, height: 40, borderRadius: 20, alignItems: 'center', justifyContent: 'center', marginRight: 10 },
  tileLabel: { fontFamily: font, fontSize: 15, fontWeight: '600' },
  tileSub: { fontFamily: font, fontSize: 13, marginTop: 1 },
  sliderRow: { flexDirection: 'row', alignItems: 'center', height: 56 },
  track: { flex: 1, height: 44, borderRadius: 22, marginLeft: 14, overflow: 'hidden', justifyContent: 'center' },
  fill: { height: 44, borderRadius: 22, justifyContent: 'center', alignItems: 'flex-end' },
  knob: { width: 20, height: 20, borderRadius: 10, backgroundColor: '#fff', marginRight: 12 },
});
