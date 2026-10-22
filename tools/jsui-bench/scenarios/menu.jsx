// jsui-bench "menu": a main menu whose buttons spring on hover
// (Animated.spring, native driver, transform + opacity). The bench moves the
// mouse up and down across the buttons, so hover springs keep running.
import React, { useRef } from 'react';
import { Animated, AppRegistry, Pressable, StyleSheet, Text, View } from 'react-native';

const ITEMS = ['Continue', 'New Game', 'Load Game', 'Multiplayer', 'Armory', 'Settings', 'Credits', 'Quit'];

function MenuButton({ label }) {
  const hover = useRef(new Animated.Value(0)).current;
  const to = (v) => Animated.spring(hover, { toValue: v, stiffness: 300, damping: 18, mass: 1, useNativeDriver: true }).start();
  const scale = hover.interpolate({ inputRange: [0, 1], outputRange: [1, 1.05] });
  const translateX = hover.interpolate({ inputRange: [0, 1], outputRange: [0, 14] });
  return (
    <Pressable onHoverIn={() => to(1)} onHoverOut={() => to(0)} onPress={() => {}}>
      <Animated.View style={[styles.button, { transform: [{ translateX }, { scale }] }]}>
        <Animated.View style={[StyleSheet.absoluteFill, styles.glow, { opacity: hover }]} />
        <Text style={styles.label}>{label}</Text>
        <Text style={styles.hint}>Press A</Text>
      </Animated.View>
    </Pressable>
  );
}

function App() {
  return (
    <View style={styles.root}>
      <Text style={styles.title}>EMBERFALL</Text>
      <View style={styles.menu}>
        {ITEMS.map((l) => (
          <MenuButton key={l} label={l} />
        ))}
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#070a14', padding: 48 },
  title: { color: '#f2c35b', fontSize: 48, fontWeight: '800', letterSpacing: 6, marginBottom: 32 },
  menu: { width: 360, gap: 10 },
  button: {
    height: 52,
    borderRadius: 10,
    paddingHorizontal: 20,
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    backgroundColor: 'rgba(26, 36, 64, 0.9)',
    borderWidth: 1,
    borderColor: 'rgba(148, 170, 255, 0.32)',
    boxShadow: '0 4px 12px rgba(0,0,0,0.5)',
  },
  glow: { borderRadius: 10, backgroundColor: 'rgba(242, 195, 91, 0.18)', borderWidth: 1, borderColor: '#f2c35b' },
  label: { color: '#eef2ff', fontSize: 18, fontWeight: '600' },
  hint: { color: '#66718f', fontSize: 12 },
});

AppRegistry.registerComponent('bench', () => App);
