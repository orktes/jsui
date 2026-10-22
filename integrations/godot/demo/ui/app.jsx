// Godot demo UI: the interactions showcase (examples/showcase/interactions.jsx,
// an AETHERFALL hub with hover springs, a paging carousel, a momentum list, a
// TextInput, LayoutAnimation chips, a spring modal and gamepad focus) plus a
// small "Godot bridge" strip in the bottom bar that talks to the game through
// JsuiView:
//   * Godot -> JS shared state: main.gd keeps `godot.stats` (version, fps,
//     frame) up to date with set_state(); useHostState renders it, including
//     the value set before the UI started. main.gd also owns `player`, which
//     the showcase's profile chip reads (name, level, credits).
//   * JS -> Godot call: the Ping button awaits Host.call('godot.ping', {count});
//     main.gd answers through call_requested -> resolve_call and pays out
//     credits with patch_state('player', ...).
//   * Godot -> JS call: main.gd's call_ui('ui.flash', {text}, callback) runs
//     the useHostHandler below, which shows a toast and answers.
//   * JS -> Godot messages: `ui:ready` on mount, and the showcase's
//     `showcase:event` reports (hover, press, focus, scroll, text...) that
//     main.gd's self-test checks.
// The strip (and the toast above it) sit in the empty left half of the
// showcase's bottom bar, so they cover none of the showcase's controls.
import React, { useEffect, useRef, useState } from 'react';
import {
  Animated,
  AppRegistry,
  Easing,
  Host,
  Pressable,
  StyleSheet,
  Text,
  View,
  useHostHandler,
  useHostState,
  useWindowDimensions,
} from 'react-native';
import { useSafeAreaInsets } from 'react-native-safe-area-context';
import Showcase from '../../../../examples/showcase/interactions.jsx';
import { colors, font } from '../../../../examples/theme.js';

function Toast() {
  const [text, setText] = useState(null);
  const t = useRef(new Animated.Value(0)).current;
  const timer = useRef(null);
  useHostHandler('ui.flash', ({ text: msg }) => {
    setText(msg);
    if (timer.current) clearTimeout(timer.current);
    Animated.spring(t, { toValue: 1, stiffness: 260, damping: 18, useNativeDriver: true }).start();
    timer.current = setTimeout(() => {
      Animated.timing(t, { toValue: 0, duration: 260, easing: Easing.in(Easing.quad), useNativeDriver: true }).start();
    }, 2600);
    return { shown: true };
  });
  if (!text) return null;
  return (
    <Animated.View
      pointerEvents="none"
      style={[
        styles.toast,
        { opacity: t, transform: [{ translateY: t.interpolate({ inputRange: [0, 1], outputRange: [12, 0] }) }] },
      ]}
    >
      <View style={styles.toastDot} />
      <Text style={styles.toastText}>{`Godot says: ${text}`}</Text>
    </Animated.View>
  );
}

function Bridge() {
  const stats = useHostState('godot.stats', null);
  const [pings, setPings] = useState(0);
  const [pong, setPong] = useState(null);
  const pingRef = useRef(null);
  useEffect(() => Host.postMessage('ui:ready', {}), []);
  // Where the Ping button is (logical px), for main.gd's self-test.
  const onPingLayout = () =>
    pingRef.current?.measureInWindow((x, y, width, height) => Host.postMessage('demo:anchors', { ping: { x, y, width, height } }));

  const ping = () => {
    const count = pings + 1;
    setPings(count);
    Host.call('godot.ping', { count }, { timeout: 5000 }).then(setPong, (e) => setPong({ error: e.message }));
  };

  return (
    <View style={styles.bridge}>
      <View style={styles.godotDot} />
      <Text style={styles.label}>GODOT</Text>
      <Text style={styles.line}>{stats ? `${stats.version} · ${stats.fps} fps` : 'waiting…'}</Text>
      <View style={styles.sep} />
      <Pressable
        ref={pingRef}
        onLayout={onPingLayout}
        onPress={ping}
        style={({ pressed, focused, hovered }) => [
          styles.button,
          (focused || hovered) && styles.buttonActive,
          pressed && styles.buttonPressed,
        ]}
      >
        <Text style={styles.buttonText}>Ping</Text>
      </Pressable>
      <Text style={styles.line}>
        {pong ? (pong.error ? `failed: ${pong.error}` : `pong #${pong.count} · +${pong.reward ?? 0} credits`) : 'no pong yet'}
      </Text>
    </View>
  );
}

// The showcase is laid out for a 1280 x 720 stage (the Godot viewport). On a
// smaller screen (a phone in landscape is ~874 x 402) the stage is scaled
// down to fit the safe area and centred in it; in Godot the scale is 1.
const STAGE_W = 1280;
const STAGE_H = 720;

function Stage({ children }) {
  const { width, height } = useWindowDimensions();
  const insets = useSafeAreaInsets();
  const w = width - insets.left - insets.right;
  const h = height - insets.top - insets.bottom;
  const scale = Math.min(1, w / STAGE_W, h / STAGE_H);
  const style = {
    position: 'absolute',
    left: insets.left + (w - STAGE_W) / 2,
    top: insets.top + (h - STAGE_H) / 2,
    width: STAGE_W,
    height: STAGE_H,
  };
  if (scale < 1) style.transform = [{ scale }];
  return <View style={style}>{children}</View>;
}

function App() {
  return (
    // Insets are zero in Godot; on phones (the iOS host) the background runs
    // edge to edge and the UI stays clear of the notch / home indicator.
    <View style={styles.root}>
      <Stage>
        <Showcase />
        <Bridge />
        <Toast />
      </Stage>
    </View>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: colors.bg0 },
  bridge: {
    position: 'absolute',
    left: 24,
    bottom: 9,
    height: 34,
    flexDirection: 'row',
    alignItems: 'center',
    gap: 10,
    paddingLeft: 12,
    paddingRight: 14,
    borderRadius: 17,
    backgroundColor: 'rgba(10, 14, 28, 0.82)',
    borderWidth: 1,
    borderColor: colors.line,
  },
  godotDot: { width: 8, height: 8, borderRadius: 4, backgroundColor: '#478cbf', boxShadow: '0 0 8px rgba(71,140,191,0.9)' },
  label: { fontFamily: font, fontWeight: '700', fontSize: 11, letterSpacing: 2, color: colors.gold },
  line: { fontFamily: font, fontWeight: '500', fontSize: 12, color: colors.textDim },
  sep: { width: 1, height: 16, backgroundColor: colors.line },
  button: {
    height: 24,
    paddingHorizontal: 12,
    borderRadius: 12,
    alignItems: 'center',
    justifyContent: 'center',
    backgroundColor: 'rgba(148, 170, 255, 0.12)',
    borderWidth: 1,
    borderColor: 'rgba(148, 170, 255, 0.25)',
  },
  buttonActive: { borderColor: 'rgba(242, 195, 91, 0.85)', backgroundColor: 'rgba(242, 195, 91, 0.18)' },
  buttonPressed: { transform: [{ scale: 0.94 }] },
  buttonText: { fontFamily: font, fontWeight: '700', fontSize: 11, letterSpacing: 1.2, textTransform: 'uppercase', color: colors.text },
  toast: {
    position: 'absolute',
    left: 24,
    bottom: 56,
    flexDirection: 'row',
    alignItems: 'center',
    gap: 10,
    paddingHorizontal: 18,
    height: 36,
    borderRadius: 18,
    backgroundColor: 'rgba(12, 17, 32, 0.92)',
    borderWidth: 1,
    borderColor: 'rgba(71, 140, 191, 0.6)',
    boxShadow: '0 10px 28px rgba(0,0,0,0.5)',
  },
  toastDot: { width: 8, height: 8, borderRadius: 4, backgroundColor: '#478cbf' },
  toastText: { fontFamily: font, fontWeight: '600', fontSize: 13, color: colors.text },
});

AppRegistry.registerComponent('App', () => App);
AppRegistry.runApplication('App', {});
