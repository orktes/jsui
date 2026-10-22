// Post-match scoreboard: result banner, two team tables with avatars,
// K/D/A columns, MVP badge, highlighted local player, ping bars.
import React from 'react';
import { Image, StyleSheet, Text, View } from 'react-native';
import { colors, font } from '../theme.js';

export const meta = { width: 1280, height: 720, maxMismatch: 1.5, title: 'Scoreboard' };

const crest = {
  wolf: require('../assets/crest-wolf.png'),
  star: require('../assets/crest-star.png'),
  moon: require('../assets/crest-moon.png'),
  tower: require('../assets/crest-tower.png'),
  flame: require('../assets/crest-flame.png'),
  eye: require('../assets/crest-eye.png'),
};

const AURORA = [
  { name: 'Valkyrie_07', crest: 'wolf', k: 24, d: 6, a: 11, dmg: '38.2k', score: 4120, ping: 4, mvp: true, me: true },
  { name: 'nightjar', crest: 'moon', k: 17, d: 9, a: 14, dmg: '29.8k', score: 3380, ping: 4 },
  { name: 'Sir Reginald the Unreasonably Persistent', crest: 'tower', k: 12, d: 11, a: 21, dmg: '24.1k', score: 2960, ping: 3 },
  { name: 'pyrite', crest: 'star', k: 9, d: 12, a: 8, dmg: '18.6k', score: 1870, ping: 2 },
  { name: 'Okonkwo', crest: 'eye', k: 6, d: 10, a: 25, dmg: '12.9k', score: 1790, ping: 4 },
];
const EMBER = [
  { name: 'Ashlight', crest: 'flame', k: 19, d: 14, a: 7, dmg: '33.5k', score: 3210, ping: 3 },
  { name: 'Mireille', crest: 'star', k: 14, d: 15, a: 12, dmg: '26.0k', score: 2650, ping: 4 },
  { name: 'GRIMWALD', crest: 'tower', k: 11, d: 16, a: 9, dmg: '21.7k', score: 2080, ping: 1, disconnected: true },
  { name: 'tessellate', crest: 'moon', k: 8, d: 17, a: 15, dmg: '17.3k', score: 1720, ping: 3 },
  { name: 'Brannoc', crest: 'wolf', k: 6, d: 18, a: 10, dmg: '11.2k', score: 1240, ping: 2 },
];

function Ping({ level }) {
  return (
    <View style={styles.ping}>
      {[0, 1, 2, 3].map((i) => (
        <View
          key={i}
          style={[styles.pingBar, { height: 4 + i * 3 }, i < level && { backgroundColor: level <= 1 ? colors.red : level === 2 ? colors.orange : colors.green }]}
        />
      ))}
    </View>
  );
}

function Team({ name, players, accent, accentSoft, score, won }) {
  return (
    <View style={[styles.team, { borderTopColor: accent }]}>
      <View style={styles.teamHeader}>
        <View style={[styles.teamDot, { backgroundColor: accent, boxShadow: `0 0 10px ${accent}` }]} />
        <Text style={styles.teamName}>{name}</Text>
        <Text style={[styles.teamResult, { color: won ? colors.green : colors.red }]}>{won ? 'Victory' : 'Defeat'}</Text>
        <View style={{ flex: 1 }} />
        <Text style={[styles.teamScore, { color: accent }]}>{score}</Text>
      </View>
      <View style={styles.headRow}>
        <Text style={[styles.th, { width: 28 }]}>#</Text>
        <Text style={[styles.th, { flex: 1 }]}>Player</Text>
        <Text style={[styles.th, styles.num]}>K</Text>
        <Text style={[styles.th, styles.num]}>D</Text>
        <Text style={[styles.th, styles.num]}>A</Text>
        <Text style={[styles.th, styles.wide]}>Damage</Text>
        <Text style={[styles.th, styles.wide]}>Score</Text>
        <View style={{ width: 30 }} />
      </View>
      {players.map((p, i) => (
        <View key={p.name} style={[styles.row, i % 2 === 1 && styles.rowAlt, p.me && [styles.rowMe, { borderColor: accent, experimental_backgroundImage: `linear-gradient(90deg, ${accentSoft} 0%, rgba(0,0,0,0) 80%)` }], p.disconnected && { opacity: 0.45 }]}>
          <Text style={[styles.rank, { width: 28 }]}>{i + 1}</Text>
          <View style={styles.playerCell}>
            <Image source={crest[p.crest]} style={styles.avatar} />
            <Text style={[styles.player, p.me && { color: '#fff' }]} numberOfLines={1}>
              {p.name}
            </Text>
            {p.mvp ? (
              <View style={styles.mvp}>
                <Text style={styles.mvpText}>MVP</Text>
              </View>
            ) : null}
            {p.disconnected ? <Text style={styles.dc}>Disconnected</Text> : null}
          </View>
          <Text style={[styles.cell, styles.num, { color: '#fff' }]}>{p.k}</Text>
          <Text style={[styles.cell, styles.num]}>{p.d}</Text>
          <Text style={[styles.cell, styles.num]}>{p.a}</Text>
          <Text style={[styles.cell, styles.wide]}>{p.dmg}</Text>
          <Text style={[styles.cell, styles.wide, { color: '#fff', fontWeight: '700' }]}>{p.score.toLocaleString('en-US')}</Text>
          <View style={{ width: 30, alignItems: 'flex-end' }}>
            <Ping level={p.ping} />
          </View>
        </View>
      ))}
    </View>
  );
}

export default function App() {
  return (
    <View style={styles.root}>
      <View style={styles.banner}>
        <Text style={styles.mode}>Ranked · Conquest · Ashen Reach</Text>
        <View style={styles.scoreLine}>
          <Text style={[styles.big, { color: '#7fb4ff' }]}>3</Text>
          <View style={styles.vs}>
            <Text style={styles.result}>Victory</Text>
            <Text style={styles.duration}>18:42</Text>
          </View>
          <Text style={[styles.big, { color: '#ff7f8f' }]}>1</Text>
        </View>
        <View style={styles.rewards}>
          <View style={styles.rewardChip}>
            <Text style={styles.rewardValue}>+28</Text>
            <Text style={styles.rewardLabel}>Rank points</Text>
          </View>
          <View style={styles.rewardChip}>
            <Text style={styles.rewardValue}>+1,250</Text>
            <Text style={styles.rewardLabel}>XP</Text>
          </View>
          <View style={styles.rewardChip}>
            <Text style={[styles.rewardValue, { color: colors.goldLight }]}>+340</Text>
            <Text style={styles.rewardLabel}>Gold</Text>
          </View>
        </View>
      </View>
      <View style={styles.tables}>
        <Team name="Aurora" players={AURORA} accent="#4f8cff" accentSoft="rgba(79,140,255,0.28)" score="3" won />
        <Team name="Ember" players={EMBER} accent="#ff4d6a" accentSoft="rgba(255,77,106,0.28)" score="1" />
      </View>
      <View style={styles.perf}>
        {[
          ['Accuracy', '41.8%', 0.42, colors.cyan],
          ['Headshots', '19', 0.63, colors.gold],
          ['Objective time', '3:12', 0.55, colors.green],
          ['Healing done', '6.4k', 0.3, colors.purple],
        ].map(([label, value, pct, c]) => (
          <View key={label} style={styles.perfCard}>
            <Text style={styles.perfLabel}>{label}</Text>
            <Text style={styles.perfValue}>{value}</Text>
            <View style={styles.perfTrack}>
              <View style={[styles.perfFill, { width: `${pct * 100}%`, backgroundColor: c }]} />
            </View>
          </View>
        ))}
        <View style={styles.continueBtn}>
          <Text style={styles.continueText}>Continue</Text>
          <View style={styles.continueKey}>
            <Text style={styles.continueKeyText}>A</Text>
          </View>
        </View>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: {
    flex: 1,
    paddingHorizontal: 40,
    paddingTop: 28,
    backgroundColor: '#070a14',
    experimental_backgroundImage:
      'radial-gradient(circle at 25% 0%, rgba(79,140,255,0.22) 0%, rgba(7,10,20,0) 50%), radial-gradient(circle at 75% 0%, rgba(255,77,106,0.16) 0%, rgba(7,10,20,0) 50%)',
  },
  banner: { alignItems: 'center' },
  mode: { fontFamily: font, fontWeight: '600', fontSize: 12, letterSpacing: 2.2, textTransform: 'uppercase', color: colors.textMute },
  scoreLine: { flexDirection: 'row', alignItems: 'center', gap: 34, marginTop: 4 },
  big: { fontFamily: font, fontWeight: '700', fontSize: 64, textShadowColor: 'rgba(0,0,0,0.6)', textShadowOffset: { width: 0, height: 4 }, textShadowRadius: 12 },
  vs: { alignItems: 'center' },
  result: {
    fontFamily: font,
    fontWeight: '700',
    fontSize: 30,
    letterSpacing: 8,
    textTransform: 'uppercase',
    color: colors.goldLight,
    textShadowColor: 'rgba(242,195,91,0.6)',
    textShadowOffset: { width: 0, height: 0 },
    textShadowRadius: 18,
  },
  duration: { fontFamily: font, fontWeight: '500', fontSize: 13, color: colors.textDim, marginTop: 2 },
  rewards: { flexDirection: 'row', gap: 12, marginTop: 10 },
  rewardChip: {
    flexDirection: 'row',
    alignItems: 'baseline',
    gap: 6,
    paddingHorizontal: 14,
    paddingVertical: 6,
    borderRadius: 16,
    backgroundColor: 'rgba(255,255,255,0.05)',
    borderWidth: 1,
    borderColor: colors.line,
  },
  rewardValue: { fontFamily: font, fontWeight: '700', fontSize: 14, color: colors.green },
  rewardLabel: { fontFamily: font, fontWeight: '500', fontSize: 12, color: colors.textDim },

  tables: { flexDirection: 'row', gap: 24, marginTop: 22 },
  team: {
    flex: 1,
    borderRadius: 12,
    borderTopWidth: 3,
    backgroundColor: 'rgba(14,20,36,0.85)',
    paddingBottom: 8,
    overflow: 'hidden',
    boxShadow: '0 20px 40px rgba(0,0,0,0.5)',
  },
  teamHeader: { flexDirection: 'row', alignItems: 'center', gap: 10, paddingHorizontal: 16, paddingTop: 14, paddingBottom: 10 },
  teamDot: { width: 10, height: 10, borderRadius: 5 },
  teamName: { fontFamily: font, fontWeight: '700', fontSize: 18, color: colors.text, letterSpacing: 1, textTransform: 'uppercase' },
  teamResult: { fontFamily: font, fontWeight: '600', fontSize: 12, letterSpacing: 1.2, textTransform: 'uppercase' },
  teamScore: { fontFamily: font, fontWeight: '700', fontSize: 26 },
  headRow: { flexDirection: 'row', alignItems: 'center', height: 30, paddingHorizontal: 16, borderBottomWidth: 1, borderBottomColor: colors.line },
  th: { fontFamily: font, fontWeight: '600', fontSize: 11, letterSpacing: 1.2, textTransform: 'uppercase', color: colors.textMute },
  num: { width: 36, textAlign: 'center' },
  wide: { width: 64, textAlign: 'right' },
  row: { flexDirection: 'row', alignItems: 'center', height: 50, paddingHorizontal: 16, borderWidth: 1, borderColor: 'transparent' },
  rowAlt: { backgroundColor: 'rgba(255,255,255,0.025)' },
  rowMe: { borderLeftWidth: 3 },
  rank: { fontFamily: font, fontWeight: '700', fontSize: 13, color: colors.textMute },
  playerCell: { flex: 1, flexDirection: 'row', alignItems: 'center', gap: 10, paddingRight: 8 },
  avatar: { width: 30, height: 30, borderRadius: 15 },
  player: { flexShrink: 1, fontFamily: font, fontWeight: '600', fontSize: 14, color: colors.text },
  mvp: { paddingHorizontal: 6, paddingVertical: 2, borderRadius: 4, backgroundColor: colors.gold, boxShadow: '0 0 10px rgba(242,195,91,0.6)' },
  mvpText: { fontFamily: font, fontWeight: '700', fontSize: 10, letterSpacing: 0.8, color: '#2a1500' },
  dc: { fontFamily: font, fontStyle: 'italic', fontSize: 11, color: colors.red },
  cell: { fontFamily: font, fontWeight: '500', fontSize: 14, color: colors.textDim },
  perf: { flexDirection: 'row', gap: 14, marginTop: 22, alignItems: 'stretch' },
  perfCard: { flex: 1, padding: 14, borderRadius: 12, backgroundColor: 'rgba(255,255,255,0.04)', borderWidth: 1, borderColor: colors.line },
  perfLabel: { fontFamily: font, fontWeight: '600', fontSize: 11, letterSpacing: 1.3, textTransform: 'uppercase', color: colors.textMute },
  perfValue: { fontFamily: font, fontWeight: '700', fontSize: 24, color: colors.text, marginTop: 4 },
  perfTrack: { height: 4, borderRadius: 2, backgroundColor: 'rgba(255,255,255,0.08)', marginTop: 10, overflow: 'hidden' },
  perfFill: { height: '100%', borderRadius: 2 },
  continueBtn: {
    width: 220,
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'center',
    gap: 12,
    borderRadius: 12,
    experimental_backgroundImage: 'linear-gradient(180deg, #ffe29a 0%, #f2c35b 50%, #c9902d 100%)',
    boxShadow: '0 10px 24px rgba(242,195,91,0.3), inset 0 1px 0 rgba(255,255,255,0.6)',
  },
  continueText: { fontFamily: font, fontWeight: '700', fontSize: 17, letterSpacing: 1.5, textTransform: 'uppercase', color: '#2a1a03' },
  continueKey: { width: 24, height: 24, borderRadius: 12, backgroundColor: 'rgba(42,26,3,0.85)', alignItems: 'center', justifyContent: 'center' },
  continueKeyText: { fontFamily: font, fontWeight: '700', fontSize: 12, color: '#5fe08a' },
  ping: { flexDirection: 'row', alignItems: 'flex-end', gap: 2, height: 14 },
  pingBar: { width: 3, borderRadius: 1, backgroundColor: 'rgba(255,255,255,0.15)' },
});
