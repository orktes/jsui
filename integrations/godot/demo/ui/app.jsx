// Godot demo UI: the main-menu example screen plus a small "bridge" card
// that talks to the game through JsuiView:
//   * JS -> Godot: `ui:ready` on mount
//   * Godot -> JS: `godot:stats` on ready and every second (useHostMessage)
//   * JS -> Godot: `ui:ping` when the button is pressed (Host.postMessage);
//     main.gd answers with `godot:pong`.
import React, { useEffect, useState } from 'react';
import { AppRegistry, Host, Pressable, StyleSheet, Text, View, useHostMessage } from 'react-native';
import MainMenu from '../../../../examples/screens/main-menu.jsx';
import { colors, font } from '../../../../examples/theme.js';

function Bridge() {
  const [stats, setStats] = useState(null);
  const [pings, setPings] = useState(0);
  const [pong, setPong] = useState(null);
  useHostMessage('godot:stats', setStats);
  useHostMessage('godot:pong', setPong);
  // Tell the game we are listening (messages sent before this are dropped).
  useEffect(() => Host.postMessage('ui:ready', {}), []);

  return (
    <View style={styles.card}>
      <Text style={styles.label}>GODOT BRIDGE</Text>
      <Text style={styles.line}>
        {stats ? `Godot ${stats.version} · ${stats.fps} fps · frame ${stats.frame}` : 'waiting for godot:stats…'}
      </Text>
      <Text style={styles.line}>{pong ? `pong #${pong.count} at ${pong.time.toFixed(1)} s` : 'no pong yet'}</Text>
      <Pressable
        autoFocus
        onPress={() => {
          const count = pings + 1;
          setPings(count);
          Host.postMessage('ui:ping', { count });
        }}
        style={({ pressed, focused, hovered }) => [
          styles.button,
          (focused || hovered) && styles.buttonActive,
          pressed && styles.buttonPressed,
        ]}
      >
        <Text style={styles.buttonText}>Ping Godot</Text>
      </Pressable>
    </View>
  );
}

function App() {
  return (
    <View style={styles.root}>
      <MainMenu />
      <Bridge />
    </View>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1 },
  card: {
    position: 'absolute',
    right: 32,
    top: 104,
    width: 300,
    padding: 16,
    gap: 6,
    borderRadius: 12,
    backgroundColor: 'rgba(10, 14, 28, 0.82)',
    borderWidth: 1,
    borderColor: colors.line,
    boxShadow: '0 8px 24px rgba(0,0,0,0.45)',
  },
  label: { fontFamily: font, fontWeight: '700', fontSize: 11, letterSpacing: 2, color: colors.gold },
  line: { fontFamily: font, fontWeight: '500', fontSize: 13, color: colors.textDim },
  button: {
    marginTop: 6,
    height: 38,
    borderRadius: 8,
    alignItems: 'center',
    justifyContent: 'center',
    backgroundColor: 'rgba(148, 170, 255, 0.12)',
    borderWidth: 1,
    borderColor: 'rgba(148, 170, 255, 0.25)',
  },
  buttonActive: { borderColor: 'rgba(242, 195, 91, 0.85)', backgroundColor: 'rgba(242, 195, 91, 0.18)' },
  buttonPressed: { transform: [{ scale: 0.97 }] },
  buttonText: { fontFamily: font, fontWeight: '700', fontSize: 14, letterSpacing: 1, color: colors.text },
});

AppRegistry.registerComponent('App', () => App);
AppRegistry.runApplication('App', {});
