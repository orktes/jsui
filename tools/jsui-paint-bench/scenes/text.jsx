// Paint bench: text-heavy panels (quest log, lore, chat, stats table) with
// three small animations: a clock whose text changes every frame, a news
// ticker sliding horizontally (native-driven transform over text) and a
// pulsing "NEW" badge (opacity). 1280 x 720.
import React, { useEffect, useRef, useState } from 'react';
import { Animated, AppRegistry, Easing, StyleSheet, Text, View } from 'react-native';

const LORE =
  'Long before the first tide, the Emberfall was a single flame kept by the wardens of the Cinder Gate. ' +
  'When the gate broke, the flame scattered into a thousand embers, and every ember became a city. ' +
  'Some still burn bright; others are cold ash that only remembers the warmth. Travellers say you can ' +
  'hear the old wardens in the wind that blows through the ruined arches at dusk.';
const QUESTS = [
  ['The Cinder Gate', 'Find the three wardens and relight the brazier at the top of the gate.'],
  ['Ashes to Ashes', 'Collect 12 ember shards from the cold cities of the eastern marches.'],
  ['A Tide of Glass', 'Escort the glassmaker across the salt flats before the tide turns.'],
  ['Wolves at the Door', 'Drive the pack away from the mill and report to the reeve.'],
  ['The Last Lantern', 'Carry the lantern through the Hollow without letting it go out.'],
];
const CHAT = Array.from({ length: 14 }, (_, i) => [`Player${(i * 37) % 97}`, ['meet at the bridge before the tide turns', 'anyone up for the raid tonight?', 'lf healer for the hollow', 'gg wp', 'selling ember shards, pm me'][i % 5]]);
const STATS = [['Strength', 42], ['Agility', 31], ['Intellect', 58], ['Stamina', 47], ['Spirit', 22], ['Haste', 12], ['Critical', 19], ['Mastery', 27]];

function Clock() {
  const [t, setT] = useState(0);
  useEffect(() => {
    let raf = 0;
    let start = -1;
    const step = (now) => {  // the frame clock (simulated in the bench)
      if (start < 0) start = now;
      setT(Math.floor(now - start));
      raf = requestAnimationFrame(step);
    };
    raf = requestAnimationFrame(step);
    return () => cancelAnimationFrame(raf);
  }, []);
  const s = Math.floor(t / 1000), ms = t % 1000;
  return <Text style={styles.clock}>{`${String(Math.floor(s / 60)).padStart(2, '0')}:${String(s % 60).padStart(2, '0')}.${String(ms).padStart(3, '0')}`}</Text>;
}

function App() {
  const ticker = useRef(new Animated.Value(0)).current;
  const pulse = useRef(new Animated.Value(0)).current;
  useEffect(() => {
    Animated.loop(Animated.timing(ticker, { toValue: 1, duration: 8000, easing: Easing.linear, useNativeDriver: true })).start();
    Animated.loop(Animated.sequence([
      Animated.timing(pulse, { toValue: 1, duration: 600, useNativeDriver: true }),
      Animated.timing(pulse, { toValue: 0, duration: 600, useNativeDriver: true }),
    ])).start();
  }, []);
  return (
    <View style={styles.root}>
      <View style={styles.top}>
        <Text style={styles.h1}>Journal</Text>
        <Clock />
      </View>
      <View style={styles.cols}>
        <View style={[styles.panel, { flex: 1.2 }]}>
          <View style={styles.titleRow}>
            <Text style={styles.panelTitle}>Quests</Text>
            <Animated.View style={[styles.newBadge, { opacity: pulse }]}>
              <Text style={styles.newText}>NEW</Text>
            </Animated.View>
          </View>
          {QUESTS.map(([q, d]) => (
            <View key={q} style={styles.quest}>
              <Text style={styles.questTitle}>{q}</Text>
              <Text style={styles.questText}>{d}</Text>
            </View>
          ))}
        </View>
        <View style={[styles.panel, { flex: 1.4 }]}>
          <Text style={styles.panelTitle}>Lore</Text>
          <Text style={styles.para}>{LORE}</Text>
          <Text style={styles.para}>{LORE}</Text>
          <Text style={[styles.para, { fontStyle: 'italic', color: '#8f98c0' }]}>— from the Warden's Codex, chapter one</Text>
        </View>
        <View style={{ flex: 1, gap: 16 }}>
          <View style={[styles.panel, { flex: 1 }]}>
            <Text style={styles.panelTitle}>Chat</Text>
            {CHAT.map(([who, msg], i) => (
              <Text key={i} style={styles.chat}>
                <Text style={styles.who}>{`[${who}] `}</Text>
                {msg}
              </Text>
            ))}
          </View>
          <View style={styles.panel}>
            <Text style={styles.panelTitle}>Attributes</Text>
            {STATS.map(([k, v]) => (
              <View key={k} style={styles.stat}>
                <Text style={styles.statKey}>{k}</Text>
                <Text style={styles.statVal}>{v}</Text>
              </View>
            ))}
          </View>
        </View>
      </View>
      <View style={styles.tickerBox}>
        <Animated.Text
          style={[styles.ticker, { transform: [{ translateX: ticker.interpolate({ inputRange: [0, 1], outputRange: [1200, -1400] }) }] }]}
          numberOfLines={1}>
          Server maintenance tonight at 02:00 UTC · Double XP weekend starts Friday · The Cinder Gate raid is now open · Patch 4.2 notes are live
        </Animated.Text>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#0b1020', padding: 24, gap: 16 },
  top: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center' },
  h1: { color: '#fff', fontSize: 28, fontWeight: '800' },
  clock: { color: '#4fd1ff', fontSize: 22, fontWeight: '700', fontVariant: ['tabular-nums'] },
  cols: { flex: 1, flexDirection: 'row', gap: 16 },
  panel: { padding: 16, gap: 8, borderRadius: 14, backgroundColor: 'rgba(24,32,65,0.9)', borderWidth: 1, borderColor: 'rgba(255,255,255,0.1)', boxShadow: '0 8px 24px rgba(0,0,0,0.45)' },
  titleRow: { flexDirection: 'row', alignItems: 'center', gap: 10 },
  panelTitle: { color: '#f2c35b', fontSize: 16, fontWeight: '700', letterSpacing: 1 },
  newBadge: { paddingHorizontal: 6, paddingVertical: 2, borderRadius: 6, backgroundColor: '#ff4d6a' },
  newText: { color: '#fff', fontSize: 11, fontWeight: '800' },
  quest: { gap: 2, paddingVertical: 4 },
  questTitle: { color: '#fff', fontSize: 15, fontWeight: '600' },
  questText: { color: '#aab3d6', fontSize: 13, lineHeight: 18 },
  para: { color: '#cfd6ff', fontSize: 14, lineHeight: 21 },
  chat: { color: '#dde', fontSize: 12, lineHeight: 17 },
  who: { color: '#4ade80', fontWeight: '600' },
  stat: { flexDirection: 'row', justifyContent: 'space-between' },
  statKey: { color: '#9aa3c7', fontSize: 13 },
  statVal: { color: '#fff', fontSize: 13, fontWeight: '700' },
  tickerBox: { height: 34, borderRadius: 8, overflow: 'hidden', backgroundColor: 'rgba(0,0,0,0.45)', justifyContent: 'center' },
  ticker: { color: '#f2c35b', fontSize: 14, fontWeight: '600', width: 1400 },
});

AppRegistry.registerComponent('TextPanels', () => App);
export default App;
