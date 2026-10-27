// /examples/<id>/full: one example screen filling the window, scaled to fit
// (letterboxed), under a slim bar with its title, prev / next and a close
// button. It is a <Modal>, so it covers the site's nav bar and keeps Tab and
// D-pad focus inside it. Esc / B close it unless the screen used the key
// itself (a screen closing its own panel); [ ] switch screens, and so do
// LB / RB when the screen does not take them.
import React, { useEffect, useRef } from 'react';
import { Animated, Easing, Modal, Pressable, StyleSheet, Text, View } from 'react-native';
import { Button, GITHUB, Key, Stage, useLayout } from '../../components/ui.jsx';
import { colors, font, mono, site } from '../../theme.js';
import { playUI } from '../../sound.js';

export const BAR_HEIGHT = 56;
const pad2 = (n) => String(n).padStart(2, '0');

export function FullScreen({ s, index, total, mountKey, padInside, onStep, onClose }) {
  const { width, height, phone } = useLayout();
  const t = useRef(new Animated.Value(0)).current;
  useEffect(() => {
    Animated.timing(t, { toValue: 1, duration: 380, easing: Easing.out(Easing.cubic), useNativeDriver: true }).start();
  }, []);
  // Each screen is laid out at 1280 x 720 and scaled to fit what is left
  // under the bar.
  const room = { w: width, h: Math.max(1, height - BAR_HEIGHT) };
  const fit = Math.min(room.w / 1280, room.h / 720);
  const stageW = Math.floor(1280 * fit);
  const close = () => {
    playUI('back');
    onClose();
  };
  const f = s.feature;
  return (
    <Modal transparent onRequestClose={close}>
      <View
        style={styles.root}
        onKeyDown={(e) => {
          if (e.key === '[' || e.key === ']') {
            onStep(e.key === '[' ? -1 : 1);
            e.preventDefault();
          }
        }}
      >
        <Animated.View
          style={[styles.bar, { paddingHorizontal: phone ? 10 : 18, transform: [{ translateY: t.interpolate({ inputRange: [0, 1], outputRange: [-BAR_HEIGHT, 0] }) }] }]}
        >
          <View style={[styles.accent, { backgroundColor: s.accent, boxShadow: `0 0 12px ${s.accent}` }]} />
          <View style={{ flexShrink: 1, gap: 1 }}>
            <Text style={styles.title} numberOfLines={1}>
              {s.title}
            </Text>
            {f && !phone ? (
              <Text style={styles.sub} numberOfLines={1}>
                {f.title}
              </Text>
            ) : null}
          </View>
          <Text style={styles.count}>
            {pad2(index + 1)} / {pad2(total)}
          </Text>
          <View style={{ flex: 1 }} />
          <BarButton label="‹" accessibilityLabel="Previous screen" onPress={() => onStep(-1)} />
          <BarButton label="›" accessibilityLabel="Next screen" onPress={() => onStep(1)} />
          {phone || width < 900 ? null : (
            <Button small kind="quiet" icon="</>" label="Source" href={`${GITHUB}/blob/main/site/app/screens/${s.id}.jsx`} self="center" />
          )}
          <View style={styles.sep} />
          <Pressable
            onPress={close}
            accessibilityRole="button"
            accessibilityLabel="Close full screen"
            style={styles.close}
            hoveredStyle={styles.closeHot}
            focusVisibleStyle={styles.focus}
            pressedStyle={{ transform: [{ scale: 0.94 }] }}
            transition={{ duration: 120, pressed: 40 }}
          >
            <Text style={styles.closeX}>✕</Text>
            {phone ? null : <Text style={styles.closeText}>Close</Text>}
            {phone ? null : <Key>Esc</Key>}
          </Pressable>
        </Animated.View>
        <View
          style={styles.body}
          onGamepadButton={() => {
            padInside.current = true;
          }}
        >
          <Animated.View style={{ opacity: t, transform: [{ scale: t.interpolate({ inputRange: [0, 1], outputRange: [0.96, 1] }) }] }}>
            <Stage width={stageW} radius={phone ? 0 : 6} style={{ backgroundColor: '#03050b' }}>
              <s.component key={s.id + ':' + mountKey} active />
            </Stage>
          </Animated.View>
          {room.h > room.w * 1.2 ? <Text style={styles.rotate}>Turn your phone sideways for a bigger screen</Text> : null}
        </View>
      </View>
    </Modal>
  );
}

function BarButton({ label, accessibilityLabel, onPress }) {
  return (
    <Pressable
      onPress={onPress}
      focusable={false}
      accessibilityLabel={accessibilityLabel}
      style={styles.arrow}
      hoveredStyle={styles.closeHot}
      pressedStyle={{ transform: [{ scale: 0.92 }] }}
      transition={{ duration: 120, pressed: 40 }}
    >
      <Text style={styles.arrowText}>{label}</Text>
    </Pressable>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#010207' },
  bar: {
    height: BAR_HEIGHT,
    flexDirection: 'row',
    alignItems: 'center',
    gap: 12,
    backgroundColor: 'rgba(7, 10, 20, 0.96)',
    borderBottomWidth: 1,
    borderBottomColor: site.stroke,
    zIndex: 1,
  },
  accent: { width: 3, height: 24, borderRadius: 2 },
  title: { fontFamily: font, fontWeight: '700', fontSize: 16, color: colors.text },
  sub: { fontFamily: font, fontSize: 12, color: colors.textMute },
  count: { fontFamily: mono, fontWeight: '700', fontSize: 12, color: colors.textMute },
  arrow: {
    width: 36,
    height: 36,
    borderRadius: 10,
    borderWidth: 1,
    borderColor: site.strokeStrong,
    alignItems: 'center',
    justifyContent: 'center',
    backgroundColor: 'rgba(255,255,255,0.03)',
    cursor: 'pointer',
  },
  arrowText: { fontFamily: font, fontWeight: '600', fontSize: 22, lineHeight: 24, color: colors.text, marginTop: -3 },
  sep: { width: 1, height: 24, backgroundColor: site.stroke },
  close: {
    height: 36,
    minWidth: 36,
    paddingHorizontal: 10,
    borderRadius: 10,
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'center',
    gap: 8,
    borderWidth: 1,
    borderColor: site.strokeStrong,
    backgroundColor: 'rgba(255,255,255,0.04)',
    cursor: 'pointer',
  },
  closeHot: { backgroundColor: 'rgba(255,255,255,0.09)', borderColor: 'rgba(148,170,255,0.45)' },
  focus: { borderColor: colors.cyan },
  closeX: { fontFamily: font, fontWeight: '700', fontSize: 14, color: colors.text },
  closeText: { fontFamily: font, fontWeight: '600', fontSize: 14, color: colors.text },
  rotate: { position: 'absolute', bottom: 28, fontFamily: font, fontWeight: '600', fontSize: 13, color: colors.textMute },
  body: { flex: 1, alignItems: 'center', justifyContent: 'center', overflow: 'hidden' },
});
