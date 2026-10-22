// Starter examples for the playground. Plain React Native code: each one
// runs unchanged in React Native.
export const EXAMPLES = [
  {
    id: 'health',
    label: 'Health bar',
    code: `import React, { useEffect, useRef, useState } from 'react';
import { Animated, Pressable, StyleSheet, Text, View } from 'react-native';

const MAX = 120;

export default function App() {
  const [hp, setHp] = useState(84);
  const fill = useRef(new Animated.Value(hp / MAX)).current;
  const ghost = useRef(new Animated.Value(hp / MAX)).current;

  useEffect(() => {
    // The bar springs to the new value; the red "damage ghost" lags behind.
    Animated.spring(fill, { toValue: hp / MAX, speed: 14, bounciness: 9, useNativeDriver: true }).start();
    Animated.timing(ghost, { toValue: hp / MAX, duration: 600, delay: 380, useNativeDriver: true }).start();
  }, [hp]);

  const change = (d) => setHp((v) => Math.max(0, Math.min(MAX, v + d)));
  const low = hp < MAX * 0.3;

  return (
    <View style={styles.hud}>
      <View style={styles.row}>
        <Text style={styles.label}>VANGUARD</Text>
        <Text style={[styles.value, low && { color: '#ff4d6a' }]}>{hp} / {MAX}</Text>
      </View>
      <View style={styles.track}>
        <Animated.View style={[styles.bar, styles.ghost, { transform: [{ scaleX: ghost }] }]} />
        <Animated.View style={[styles.bar, low && styles.low, { transform: [{ scaleX: fill }] }]} />
      </View>
      <View style={styles.row}>
        <Button label="Hit  -18" color="#ff4d6a" onPress={() => change(-18)} />
        <Button label="Heal  +25" color="#4ade80" onPress={() => change(25)} />
      </View>
    </View>
  );
}

function Button({ label, color, onPress }) {
  return (
    <Pressable onPress={onPress} style={({ pressed, hovered, focused }) => [
      styles.button, { borderColor: color },
      (hovered || focused) && { backgroundColor: color + '22' },
      pressed && { transform: [{ scale: 0.95 }] },
    ]}>
      <Text style={[styles.buttonText, { color }]}>{label}</Text>
    </Pressable>
  );
}

const styles = StyleSheet.create({
  hud: { width: 340, padding: 20, gap: 14, borderRadius: 16, backgroundColor: 'rgba(14,20,36,0.92)',
    borderWidth: 1, borderColor: 'rgba(148,170,255,0.2)' },
  row: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center', gap: 12 },
  label: { color: '#a3aecb', fontSize: 12, fontWeight: '700', letterSpacing: 2 },
  value: { color: '#eef2ff', fontSize: 16, fontWeight: '700' },
  track: { height: 14, borderRadius: 7, overflow: 'hidden', backgroundColor: '#0b1020' },
  bar: { position: 'absolute', left: 0, top: 0, bottom: 0, width: '100%', transformOrigin: 'left',
    experimental_backgroundImage: 'linear-gradient(90deg, #13803f, #4ade80)' },
  ghost: { experimental_backgroundImage: 'linear-gradient(90deg, #9e1730, #ff4d6a)' },
  low: { experimental_backgroundImage: 'linear-gradient(90deg, #9e1730, #ff9a3c)' },
  button: { flex: 1, height: 42, borderRadius: 10, borderWidth: 1, alignItems: 'center', justifyContent: 'center' },
  buttonText: { fontSize: 14, fontWeight: '700' },
});
`,
  },
  {
    id: 'menu',
    label: 'Gamepad menu',
    code: `import React, { useRef, useState } from 'react';
import { Animated, Pressable, StyleSheet, Text, View, useGamepad } from 'react-native';

const ITEMS = ['Continue', 'New Game', 'Load', 'Settings', 'Quit'];

export default function App() {
  const [chosen, setChosen] = useState('nothing yet');
  const pad = useGamepad();
  return (
    <View style={styles.menu}>
      <Text style={styles.title}>AETHERFALL</Text>
      {ITEMS.map((label) => (
        <Item key={label} label={label} onPress={() => setChosen(label)} />
      ))}
      <Text style={styles.hint}>
        {pad.connected ? 'Controller connected: D-pad + A' : 'Click, or Tab / arrows + Enter'}
      </Text>
      <Text style={styles.hint}>Selected: {chosen}</Text>
    </View>
  );
}

function Item({ label, onPress }) {
  const focus = useRef(new Animated.Value(0)).current;
  const spring = (v) => Animated.spring(focus, { toValue: v, speed: 18, bounciness: 10, useNativeDriver: true }).start();
  return (
    <Pressable onPress={onPress} onFocus={() => spring(1)} onBlur={() => spring(0)}
      onHoverIn={() => spring(1)} onHoverOut={() => spring(0)}>
      <Animated.View style={[styles.item, {
        transform: [{ translateX: focus.interpolate({ inputRange: [0, 1], outputRange: [0, 14] }) }],
      }]}>
        <Animated.View style={[styles.glow, { opacity: focus }]} />
        <Animated.View style={[styles.bar, { transform: [{ scaleY: focus }] }]} />
        <Text style={styles.label}>{label}</Text>
      </Animated.View>
    </Pressable>
  );
}

const styles = StyleSheet.create({
  menu: { width: 300, gap: 6 },
  title: { color: '#f2c35b', fontSize: 26, fontWeight: '700', letterSpacing: 6, marginBottom: 12 },
  item: { height: 48, justifyContent: 'center', paddingHorizontal: 20, borderRadius: 10 },
  glow: { position: 'absolute', left: 0, right: 0, top: 0, bottom: 0, borderRadius: 10,
    experimental_backgroundImage: 'linear-gradient(90deg, rgba(242,195,91,0.28), rgba(242,195,91,0))' },
  bar: { position: 'absolute', left: 0, top: 8, bottom: 8, width: 3, borderRadius: 2, backgroundColor: '#f2c35b' },
  label: { color: '#eef2ff', fontSize: 18, fontWeight: '600', letterSpacing: 1 },
  hint: { color: '#66718f', fontSize: 13, marginTop: 8 },
});
`,
  },
  {
    id: 'card',
    label: 'Loot card',
    code: `import React, { useEffect, useRef, useState } from 'react';
import { Animated, Easing, Pressable, StyleSheet, Text, View } from 'react-native';

const RARITY = { name: 'LEGENDARY', color: '#ffb23f' };

export default function App() {
  const [open, setOpen] = useState(false);
  const pulse = useRef(new Animated.Value(0)).current;
  const flip = useRef(new Animated.Value(0)).current;

  useEffect(() => {
    const loop = Animated.loop(Animated.sequence([
      Animated.timing(pulse, { toValue: 1, duration: 900, easing: Easing.inOut(Easing.sin), useNativeDriver: true }),
      Animated.timing(pulse, { toValue: 0, duration: 900, easing: Easing.inOut(Easing.sin), useNativeDriver: true }),
    ]));
    loop.start();
    return () => loop.stop();
  }, []);

  useEffect(() => {
    Animated.spring(flip, { toValue: open ? 1 : 0, speed: 10, bounciness: 12, useNativeDriver: true }).start();
  }, [open]);

  const scale = flip.interpolate({ inputRange: [0, 0.5, 1], outputRange: [1, 0.86, 1.06] });
  const rotate = flip.interpolate({ inputRange: [0, 1], outputRange: ['0deg', '360deg'] });

  return (
    <Pressable onPress={() => setOpen((o) => !o)}>
      <Animated.View style={[styles.glow, { opacity: pulse, borderColor: RARITY.color }]} />
      <Animated.View style={[styles.card, { borderColor: RARITY.color, transform: [{ scale }, { rotate }] }]}>
        <Text style={[styles.rarity, { color: RARITY.color }]}>{RARITY.name}</Text>
        <View style={styles.gem} />
        <Text style={styles.name}>{open ? 'Sunforged Blade' : '? ? ?'}</Text>
        <Text style={styles.stats}>{open ? '+42 ATK   +12% CRIT' : 'Press to reveal'}</Text>
      </Animated.View>
    </Pressable>
  );
}

const styles = StyleSheet.create({
  card: { width: 220, height: 300, borderRadius: 18, borderWidth: 2, alignItems: 'center',
    justifyContent: 'center', gap: 14, backgroundColor: '#121a2e',
    experimental_backgroundImage: 'radial-gradient(circle at 50% 35%, rgba(255,178,63,0.35), rgba(18,26,46,0) 60%)' },
  glow: { position: 'absolute', left: -10, top: -10, right: -10, bottom: -10, borderRadius: 26,
    borderWidth: 2, boxShadow: '0 0 40px rgba(255,178,63,0.7)' },
  rarity: { fontSize: 12, fontWeight: '700', letterSpacing: 3 },
  gem: { width: 72, height: 72, borderRadius: 14, transform: [{ rotate: '45deg' }],
    experimental_backgroundImage: 'linear-gradient(135deg, #ffe29a, #ff9a3c 60%, #a8741f)',
    boxShadow: '0 10px 30px rgba(255,154,60,0.5)' },
  name: { color: '#eef2ff', fontSize: 20, fontWeight: '700', marginTop: 12 },
  stats: { color: '#a3aecb', fontSize: 13, fontWeight: '600', letterSpacing: 1 },
});
`,
  },
  {
    id: 'hello',
    label: 'Hello',
    code: `import React, { useState } from 'react';
import { Pressable, Text, View } from 'react-native';

export default function App() {
  const [count, setCount] = useState(0);
  return (
    <View style={{ alignItems: 'center', gap: 16 }}>
      <Text style={{ color: '#eef2ff', fontSize: 28, fontWeight: '700' }}>
        Hello from QuickJS
      </Text>
      <Pressable
        onPress={() => setCount(count + 1)}
        style={({ pressed, hovered }) => ({
          paddingHorizontal: 22, paddingVertical: 12, borderRadius: 12,
          backgroundColor: pressed ? '#d99a32' : hovered ? '#ffe29a' : '#f2c35b',
        })}
      >
        <Text style={{ color: '#1a1206', fontSize: 16, fontWeight: '700' }}>
          Clicked {count} times
        </Text>
      </Pressable>
    </View>
  );
}
`,
  },
];
