// Interactive live scoreboard (1280×720) in the style of
// examples/screens/scoreboard.jsx. A match runs in the background: kills,
// assists and objective captures update the stats, rows re-sort with
// LayoutAnimation, score deltas pop, the MVP badge follows the top player and
// the first team to 5 captures wins the round. Column headers (or LB / RB)
// change the sort; rows expand on press to show the player's details.
import React, { memo, useCallback, useEffect, useRef, useState } from 'react';
import { Animated, Easing, Image, LayoutAnimation, Pressable, StyleSheet, Text, View } from 'react-native';
import { colors, font } from '../../../examples/theme.js';

export const feature = {
  title: 'LayoutAnimation on live data',
  points: [
    'Every kill, assist and capture is a plain React state update; LayoutAnimation.configureNext makes the runtime animate each row from its old frame to its new one, so the tables re-sort smoothly.',
    'Sorting by another column, expanding a row and the MVP badge moving are the same mechanism: no per-row animation code.',
    'Score deltas, row flashes and the live pulse are native-driver Animated values layered on top.',
    'Rows are focusable: arrows / D-pad move between them and across teams, Enter / A expands one.',
  ],
  keys: ['Click a column / LB RB / Q E: sort', 'Arrows / D-pad: move between players', 'Enter / A / click: expand a row', 'Y / P: pause the live feed', 'X: ready up'],
};

const ND = true;
const crest = {
  wolf: require('../../../examples/assets/crest-wolf.png'),
  star: require('../../../examples/assets/crest-star.png'),
  moon: require('../../../examples/assets/crest-moon.png'),
  tower: require('../../../examples/assets/crest-tower.png'),
  flame: require('../../../examples/assets/crest-flame.png'),
  eye: require('../../../examples/assets/crest-eye.png'),
};
const WEAPON = {
  sword: require('../../../examples/assets/item-sword.png'),
  bow: require('../../../examples/assets/item-bow.png'),
  axe: require('../../../examples/assets/item-axe.png'),
  gem: require('../../../examples/assets/item-gem.png'),
  shield: require('../../../examples/assets/item-shield.png'),
};
const WEAPON_NAMES = { sword: 'Stormforged Blade', bow: 'Whisperwind Recurve', axe: 'Cinderbrand', gem: 'Voidheart Focus', shield: 'Tideguard Aegis' };

const P = (name, team, cr, k, d, a, dmg, score, ping, weapon, extra) => Object.assign({ name, team, crest: cr, k, d, a, dmg, score, ping, weapon, streak: 0 }, extra);
const START = [
  P('Valkyrie_07', 'aurora', 'wolf', 24, 6, 11, 38200, 4120, 4, 'sword', { me: true }),
  P('nightjar', 'aurora', 'moon', 17, 9, 14, 29800, 3380, 4, 'bow'),
  P('Sir Reginald the Unreasonably Persistent', 'aurora', 'tower', 12, 11, 21, 24100, 2960, 3, 'shield'),
  P('pyrite', 'aurora', 'star', 9, 12, 8, 18600, 1870, 2, 'gem'),
  P('Okonkwo', 'aurora', 'eye', 6, 10, 25, 12900, 1790, 4, 'bow'),
  P('Ashlight', 'ember', 'flame', 19, 14, 7, 33500, 3210, 3, 'axe'),
  P('Mireille', 'ember', 'star', 14, 15, 12, 26000, 2650, 4, 'gem'),
  P('GRIMWALD', 'ember', 'tower', 11, 16, 9, 21700, 2080, 1, 'axe', { disconnected: true }),
  P('tessellate', 'ember', 'moon', 8, 17, 15, 17300, 1720, 3, 'sword'),
  P('Brannoc', 'ember', 'wolf', 6, 18, 10, 11200, 1240, 2, 'shield'),
];
const TEAMS = {
  aurora: { name: 'Aurora', accent: '#4f8cff', soft: 'rgba(79,140,255,0.28)', light: '#7fb4ff' },
  ember: { name: 'Ember', accent: '#ff4d6a', soft: 'rgba(255,77,106,0.28)', light: '#ff7f8f' },
};
const SORTS = [
  { key: 'score', label: 'Score' },
  { key: 'k', label: 'K' },
  { key: 'd', label: 'D' },
  { key: 'a', label: 'A' },
  { key: 'dmg', label: 'Damage' },
];
const WIN = 5;

const useValue = (v) => useRef(new Animated.Value(v)).current;
const fmt = (n) => String(Math.round(n)).replace(/\B(?=(\d{3})+(?!\d))/g, ',');
const kfmt = (n) => (n / 1000).toFixed(1) + 'k';
const pick = (arr) => arr[Math.floor(Math.random() * arr.length)];
const spring = { duration: 520, update: { type: 'spring', springDamping: 0.75 }, create: { type: 'easeInEaseOut', property: 'opacity' }, delete: { type: 'easeInEaseOut', property: 'opacity' } };

// ---------------------------------------------------------------------------

function Ping({ level }) {
  return (
    <View style={styles.ping}>
      {[0, 1, 2, 3].map((i) => (
        <View key={i} style={[styles.pingBar, { height: 4 + i * 3 }, i < level && { backgroundColor: level <= 1 ? colors.red : level === 2 ? colors.orange : colors.green }]} />
      ))}
    </View>
  );
}

const Row = memo(function Row({ p, rank, mvp, alt, expanded, sortKey, onToggle, autoFocus, rowRef }) {
  const team = TEAMS[p.team];
  const flash = useValue(0);
  const delta = useValue(0);
  const badge = useValue(mvp ? 1 : 0);
  const prev = useRef(p.score);
  const [gain, setGain] = useState(0);
  useEffect(() => {
    const d = p.score - prev.current;
    prev.current = p.score;
    if (d <= 0) return;
    setGain(d);
    flash.setValue(1);
    Animated.timing(flash, { toValue: 0, duration: 1100, easing: Easing.out(Easing.quad), useNativeDriver: ND }).start();
    delta.setValue(0);
    Animated.timing(delta, { toValue: 1, duration: 1200, easing: Easing.out(Easing.cubic), useNativeDriver: ND }).start();
  }, [p.score]);
  useEffect(() => {
    if (mvp) {
      badge.setValue(0);
      Animated.spring(badge, { toValue: 1, stiffness: 260, damping: 10, useNativeDriver: ND }).start();
    }
  }, [mvp]);
  const hl = (key) => (sortKey === key ? styles.cellSorted : null);
  return (
    <Pressable ref={rowRef} autoFocus={autoFocus} onPress={() => onToggle(p.name)} style={{ cursor: 'pointer' }}>
      {({ hovered, focused }) => (
        <View
          style={[
            styles.row,
            alt && styles.rowAlt,
            p.me && [styles.rowMe, { borderColor: team.accent, experimental_backgroundImage: `linear-gradient(90deg, ${team.soft} 0%, rgba(0,0,0,0) 80%)` }],
            hovered && styles.rowHover,
            focused && [styles.rowFocus, { borderColor: team.light }],
            p.disconnected && { opacity: 0.45 },
          ]}
        >
          <Animated.View pointerEvents="none" style={[StyleSheet.absoluteFill, { backgroundColor: team.accent, opacity: flash.interpolate({ inputRange: [0, 1], outputRange: [0, 0.22] }) }]} />
          <View style={styles.rowMain}>
            <Text style={[styles.rank, { width: 28 }, rank === 1 && { color: team.light }]}>{rank}</Text>
            <View style={styles.playerCell}>
              <Image source={crest[p.crest]} style={styles.avatar} />
              <Text style={[styles.player, p.me && { color: '#fff' }]} numberOfLines={1}>
                {p.name}
              </Text>
              {mvp ? (
                <Animated.View style={[styles.mvp, { transform: [{ scale: badge }] }]}>
                  <Text style={styles.mvpText}>MVP</Text>
                </Animated.View>
              ) : null}
              {p.streak >= 3 ? (
                <View style={styles.streak}>
                  <Text style={styles.streakText}>{p.streak} streak</Text>
                </View>
              ) : null}
              {p.disconnected ? <Text style={styles.dc}>Disconnected</Text> : null}
            </View>
            <Text style={[styles.cell, styles.num, { color: '#fff' }, hl('k')]}>{p.k}</Text>
            <Text style={[styles.cell, styles.num, hl('d')]}>{p.d}</Text>
            <Text style={[styles.cell, styles.num, hl('a')]}>{p.a}</Text>
            <Text style={[styles.cell, styles.wide, hl('dmg')]}>{kfmt(p.dmg)}</Text>
            <View style={styles.wide}>
              <Text style={[styles.cell, { textAlign: 'right', color: '#fff', fontWeight: '700' }, hl('score')]}>{fmt(p.score)}</Text>
              <Animated.Text
                pointerEvents="none"
                style={[
                  styles.gain,
                  {
                    opacity: delta.interpolate({ inputRange: [0, 0.1, 0.7, 1], outputRange: [0, 1, 1, 0] }),
                    transform: [{ translateY: delta.interpolate({ inputRange: [0, 1], outputRange: [4, -16] }) }],
                  },
                ]}
              >
                +{gain}
              </Animated.Text>
            </View>
            <View style={{ width: 30, alignItems: 'flex-end' }}>
              <Ping level={p.disconnected ? 0 : p.ping} />
            </View>
          </View>
          {expanded ? (
            <View style={styles.details}>
              <Image source={WEAPON[p.weapon]} style={styles.detailIcon} />
              <View style={{ width: 150 }}>
                <Text style={styles.detailLabel}>Favourite weapon</Text>
                <Text style={styles.detailValue} numberOfLines={1}>
                  {WEAPON_NAMES[p.weapon]}
                </Text>
              </View>
              <View style={{ width: 70 }}>
                <Text style={styles.detailLabel}>K / D</Text>
                <Text style={styles.detailValue}>{(p.k / Math.max(1, p.d)).toFixed(2)}</Text>
              </View>
              <View style={{ flex: 1 }}>
                <Text style={styles.detailLabel}>Damage share</Text>
                <View style={styles.detailTrack}>
                  <View style={[styles.detailFill, { width: `${Math.min(100, (p.dmg / 60000) * 100)}%`, backgroundColor: team.accent }]} />
                </View>
              </View>
            </View>
          ) : null}
        </View>
      )}
    </Pressable>
  );
});

function Team({ id, players, score, won, sortKey, onSort, expanded, onToggle, mvpName, active, meRef }) {
  const team = TEAMS[id];
  return (
    <View style={[styles.team, { borderTopColor: team.accent }]}>
      <View style={styles.teamHeader}>
        <View style={[styles.teamDot, { backgroundColor: team.accent, boxShadow: `0 0 10px ${team.accent}` }]} />
        <Text style={styles.teamName}>{team.name}</Text>
        {won != null ? <Text style={[styles.teamResult, { color: won ? colors.green : colors.red }]}>{won ? 'Victory' : 'Defeat'}</Text> : <Text style={[styles.teamResult, { color: colors.textMute }]}>{score >= WIN - 1 ? 'Match point' : 'Capturing'}</Text>}
        <View style={{ flex: 1 }} />
        <Text style={[styles.teamScore, { color: team.accent }]}>{score}</Text>
      </View>
      <View style={styles.headRow}>
        <Text style={[styles.th, { width: 28 }]}>#</Text>
        <Text style={[styles.th, { flex: 1 }]}>Player</Text>
        {[['k', 'K', styles.num], ['d', 'D', styles.num], ['a', 'A', styles.num], ['dmg', 'Damage', styles.wide], ['score', 'Score', styles.wide]].map(([k, l, w]) => (
          <Pressable key={k} focusable={false} onPress={() => onSort(k)} style={[w, { cursor: 'pointer' }]}>
            {({ hovered }) => (
              <View>
                <Text style={[styles.th, { textAlign: w === styles.num ? 'center' : 'right' }, (hovered || sortKey === k) && { color: sortKey === k ? colors.goldLight : colors.text }]}>{l}</Text>
                {sortKey === k ? <View style={[styles.sortBar, w === styles.num ? { alignSelf: 'center' } : { alignSelf: 'flex-end' }]} /> : null}
              </View>
            )}
          </Pressable>
        ))}
        <View style={{ width: 30 }} />
      </View>
      {players.map((p, i) => (
        <Row
          key={p.name}
          p={p}
          rank={i + 1}
          alt={i % 2 === 1}
          mvp={p.name === mvpName}
          expanded={expanded === p.name}
          sortKey={sortKey}
          onToggle={onToggle}
          autoFocus={active && !!p.me}
          rowRef={p.me ? meRef : undefined}
        />
      ))}
    </View>
  );
}

// ---------------------------------------------------------------------------

export default function Scoreboard({ active = true }) {
  const [players, setPlayers] = useState(START);
  const [score, setScore] = useState({ aurora: 3, ember: 1 });
  const [sortKey, setSortKey] = useState('score');
  const [expanded, setExpanded] = useState(null);
  const [paused, setPaused] = useState(false);
  const [clock, setClock] = useState(18 * 60 + 42);
  const [event, setEvent] = useState({ text: 'Aurora captured the Ember Gate', color: TEAMS.aurora.light, id: 0 });
  const [winner, setWinner] = useState(null);
  const [ready, setReady] = useState(false);
  const live = useValue(0);
  const eventT = useValue(1);
  const bigA = useValue(1);
  const bigE = useValue(1);
  const resultT = useValue(0);
  const meRef = useRef(null);
  const state = useRef({ players: START, score: { aurora: 3, ember: 1 }, n: 0, winner: null });

  useEffect(() => {
    if (!active) return undefined;
    const l = Animated.loop(
      Animated.sequence([
        Animated.timing(live, { toValue: 1, duration: 700, easing: Easing.inOut(Easing.sin), useNativeDriver: ND }),
        Animated.timing(live, { toValue: 0, duration: 700, easing: Easing.inOut(Easing.sin), useNativeDriver: ND }),
      ]),
    );
    l.start();
    return () => l.stop();
  }, [active]);

  const announce = (text, color) => {
    setEvent((e) => ({ text, color, id: e.id + 1 }));
    eventT.setValue(0);
    Animated.timing(eventT, { toValue: 1, duration: 420, easing: Easing.out(Easing.cubic), useNativeDriver: ND }).start();
  };

  // The match: one event every ~1.5 s while active and not paused.
  useEffect(() => {
    if (!active || paused) return undefined;
    const tick = setInterval(() => setClock((c) => c + 1), 1000);
    const id = setInterval(() => {
      const s = state.current;
      if (s.winner) return;
      s.n += 1;
      const ps = s.players.map((p) => Object.assign({}, p));
      const live = (team) => ps.filter((p) => p.team === team && !p.disconnected);
      const killTeam = Math.random() < 0.56 ? 'aurora' : 'ember';
      const other = killTeam === 'aurora' ? 'ember' : 'aurora';
      // Better players get more kills.
      const pool = live(killTeam).flatMap((p) => Array(Math.max(1, Math.round(p.k / 5))).fill(p));
      const killer = pick(pool);
      const victim = pick(live(other));
      killer.k += 1;
      killer.streak += 1;
      killer.score += 150 + (killer.streak >= 3 ? 50 : 0);
      killer.dmg += 700 + Math.round(Math.random() * 900);
      victim.d += 1;
      victim.streak = 0;
      const helpers = live(killTeam).filter((p) => p !== killer);
      if (Math.random() < 0.65 && helpers.length) {
        const h = pick(helpers);
        h.a += 1;
        h.score += 50;
        h.dmg += 300 + Math.round(Math.random() * 400);
      }
      if (Math.random() < 0.3) {
        const p = pick(ps);
        if (!p.disconnected) p.ping = Math.max(1, Math.min(4, p.ping + (Math.random() < 0.5 ? -1 : 1)));
      }
      const grim = ps.find((p) => p.name === 'GRIMWALD');
      if (s.n === 4 && grim.disconnected) {
        grim.disconnected = false;
        grim.ping = 3;
      }
      let sc = s.score;
      let text = `${killer.name} eliminated ${victim.name}${killer.streak >= 3 ? ` · ${killer.streak} streak` : ''}`;
      if (s.n % 4 === 0) {
        sc = Object.assign({}, sc, { [killTeam]: sc[killTeam] + 1 });
        text = `${TEAMS[killTeam].name} captured ${pick(['the Ember Gate', 'the Old Bridge', 'the Ashen Spire', 'the Tide Shrine'])}`;
        const big = killTeam === 'aurora' ? bigA : bigE;
        big.setValue(1.5);
        Animated.spring(big, { toValue: 1, stiffness: 220, damping: 9, useNativeDriver: ND }).start();
        if (sc[killTeam] >= WIN) {
          s.winner = killTeam;
          setWinner(killTeam);
          resultT.setValue(0);
          Animated.spring(resultT, { toValue: 1, stiffness: 180, damping: 14, useNativeDriver: ND }).start();
          setTimeout(() => {
            s.winner = null;
            s.score = { aurora: 0, ember: 0 };
            setWinner(null);
            setScore(s.score);
            announce('Next round · first to 5 captures', colors.goldLight);
          }, 4200);
        }
      }
      s.players = ps;
      s.score = sc;
      LayoutAnimation.configureNext(spring);
      setPlayers(ps);
      setScore(sc);
      announce(text, TEAMS[killTeam].light);
    }, 1500);
    return () => {
      clearInterval(id);
      clearInterval(tick);
    };
  }, [active, paused]);

  const sorted = (team) => {
    const list = players.filter((p) => p.team === team);
    return list.slice().sort((a, b) => (sortKey === 'd' ? a.d - b.d : b[sortKey] - a[sortKey]) || b.score - a.score);
  };
  const mvpName = players.reduce((best, p) => (!best || p.score > best.score ? p : best), null).name;

  const onSort = useCallback((k) => {
    LayoutAnimation.configureNext(spring);
    setSortKey(k);
  }, []);
  const cycleSort = (dir) => {
    const i = SORTS.findIndex((s) => s.key === sortKey);
    onSort(SORTS[(i + dir + SORTS.length) % SORTS.length].key);
  };
  const onToggle = useCallback((name) => {
    LayoutAnimation.configureNext({ duration: 300, update: { type: 'easeInEaseOut' }, create: { type: 'easeInEaseOut', property: 'opacity' }, delete: { type: 'easeInEaseOut', property: 'opacity' } });
    setExpanded((e) => (e === name ? null : name));
  }, []);

  const onKeyDown = (e) => {
    const k = (e.key || '').toLowerCase();
    if (k === 'q') cycleSort(-1);
    else if (k === 'e') cycleSort(1);
    else if (k === 'y' || k === 'p') setPaused((x) => !x);
    else if (k === 'x') setReady((r) => !r);
    else return;
    e.preventDefault();
  };
  const onGamepadButton = (e) => {
    if (!e.pressed) return;
    const b = e.buttonIndex;
    if (b === 4) cycleSort(-1);
    else if (b === 5) cycleSort(1);
    else if (b === 3) setPaused((x) => !x);
    else if (b === 2) setReady((r) => !r);
    else return;
    e.preventDefault();
  };

  const me = players.find((p) => p.me);
  const pad = (n) => String(n).padStart(2, '0');
  const perf = [
    ['Accuracy', `${(38 + me.k * 0.15).toFixed(1)}%`, (38 + me.k * 0.15) / 100, colors.cyan],
    ['Kills / deaths', (me.k / Math.max(1, me.d)).toFixed(2), Math.min(1, me.k / Math.max(1, me.d) / 5), colors.gold],
    ['Damage', kfmt(me.dmg), Math.min(1, me.dmg / 60000), colors.green],
    ['Assists', String(me.a), Math.min(1, me.a / 30), colors.purple],
  ];

  return (
    <View style={styles.root} onKeyDown={onKeyDown} onGamepadButton={onGamepadButton}>
      <View style={styles.banner}>
        <View style={styles.modeRow}>
          <Animated.View style={[styles.liveDot, { opacity: paused ? 0.3 : live.interpolate({ inputRange: [0, 1], outputRange: [0.4, 1] }) }]} />
          <Text style={styles.mode}>{paused ? 'Paused' : 'Live'} · Ranked · Conquest · Ashen Reach</Text>
        </View>
        <View style={styles.scoreLine}>
          <Animated.Text style={[styles.big, { color: '#7fb4ff', transform: [{ scale: bigA }] }]}>{score.aurora}</Animated.Text>
          <View style={styles.vs}>
            {winner ? (
              <Animated.Text style={[styles.result, { transform: [{ scale: resultT.interpolate({ inputRange: [0, 1], outputRange: [0.6, 1] }) }], opacity: resultT }]}>
                {winner === 'aurora' ? 'Victory' : 'Defeat'}
              </Animated.Text>
            ) : (
              <Text style={[styles.result, styles.resultLive]}>First to {WIN}</Text>
            )}
            <Text style={styles.duration}>
              {pad(Math.floor(clock / 60))}:{pad(clock % 60)}
            </Text>
          </View>
          <Animated.Text style={[styles.big, { color: '#ff7f8f', transform: [{ scale: bigE }] }]}>{score.ember}</Animated.Text>
        </View>
        <Animated.View style={[styles.eventLine, { opacity: eventT, transform: [{ translateY: eventT.interpolate({ inputRange: [0, 1], outputRange: [8, 0] }) }] }]}>
          <View style={[styles.eventDot, { backgroundColor: event.color }]} />
          <Text style={styles.eventText}>{event.text}</Text>
        </Animated.View>
      </View>

      <View style={styles.tables}>
        <Team id="aurora" players={sorted('aurora')} score={score.aurora} won={winner ? winner === 'aurora' : null} sortKey={sortKey} onSort={onSort} expanded={expanded} onToggle={onToggle} mvpName={mvpName} active={active} meRef={meRef} />
        <Team id="ember" players={sorted('ember')} score={score.ember} won={winner ? winner === 'ember' : null} sortKey={sortKey} onSort={onSort} expanded={expanded} onToggle={onToggle} mvpName={mvpName} active={false} />
      </View>

      <View style={styles.perf}>
        {perf.map(([label, value, pct, c]) => (
          <View key={label} style={styles.perfCard}>
            <Text style={styles.perfLabel}>{label}</Text>
            <Text style={styles.perfValue}>{value}</Text>
            <View style={styles.perfTrack}>
              <View style={[styles.perfFill, { width: `${Math.min(100, pct * 100)}%`, backgroundColor: c }]} />
            </View>
          </View>
        ))}
        <Pressable focusable onPress={() => setReady((r) => !r)} style={{ cursor: 'pointer' }}>
          {({ hovered, focused, pressed }) => (
            <View style={[styles.continueBtn, ready && styles.readyBtn, (hovered || focused) && styles.continueHot, pressed && { transform: [{ scale: 0.97 }] }]}>
              <View>
                <Text style={[styles.continueText, ready && { color: colors.green }]}>{ready ? 'Ready' : 'Ready up'}</Text>
                <Text style={[styles.readyCount, ready && { color: colors.textDim }]}>{ready ? '8 / 10 players ready' : '7 / 10 players ready'}</Text>
              </View>
              <View style={styles.continueKey}>
                <Text style={styles.continueKeyText}>X</Text>
              </View>
            </View>
          )}
        </Pressable>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: {
    width: 1280,
    height: 720,
    overflow: 'hidden',
    paddingHorizontal: 40,
    paddingTop: 24,
    backgroundColor: '#070a14',
    experimental_backgroundImage:
      'radial-gradient(circle at 25% 0%, rgba(79,140,255,0.22) 0%, rgba(7,10,20,0) 50%), radial-gradient(circle at 75% 0%, rgba(255,77,106,0.16) 0%, rgba(7,10,20,0) 50%)',
  },
  banner: { alignItems: 'center' },
  modeRow: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  liveDot: { width: 8, height: 8, borderRadius: 4, backgroundColor: colors.red, boxShadow: '0 0 8px rgba(255,77,106,0.9)' },
  mode: { fontFamily: font, fontWeight: '600', fontSize: 12, letterSpacing: 2.2, textTransform: 'uppercase', color: colors.textMute },
  scoreLine: { flexDirection: 'row', alignItems: 'center', gap: 34, marginTop: 2 },
  big: { fontFamily: font, fontWeight: '700', fontSize: 60, width: 60, textAlign: 'center', textShadowColor: 'rgba(0,0,0,0.6)', textShadowOffset: { width: 0, height: 4 }, textShadowRadius: 12 },
  vs: { alignItems: 'center', width: 260 },
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
  resultLive: { fontSize: 18, letterSpacing: 5, color: colors.textDim, textShadowRadius: 0, paddingVertical: 6 },
  duration: { fontFamily: font, fontWeight: '600', fontSize: 13, color: colors.textDim, marginTop: 2 },
  eventLine: { flexDirection: 'row', alignItems: 'center', gap: 8, marginTop: 8, paddingHorizontal: 14, paddingVertical: 6, borderRadius: 16, backgroundColor: 'rgba(255,255,255,0.05)', borderWidth: 1, borderColor: colors.line },
  eventDot: { width: 6, height: 6, borderRadius: 3 },
  eventText: { fontFamily: font, fontWeight: '600', fontSize: 13, color: colors.text },

  tables: { flexDirection: 'row', gap: 24, marginTop: 16, alignItems: 'flex-start' },
  team: { flex: 1, borderRadius: 12, borderTopWidth: 3, backgroundColor: 'rgba(14,20,36,0.85)', paddingBottom: 8, overflow: 'hidden', boxShadow: '0 20px 40px rgba(0,0,0,0.5)' },
  teamHeader: { flexDirection: 'row', alignItems: 'center', gap: 10, paddingHorizontal: 16, paddingTop: 12, paddingBottom: 8 },
  teamDot: { width: 10, height: 10, borderRadius: 5 },
  teamName: { fontFamily: font, fontWeight: '700', fontSize: 18, color: colors.text, letterSpacing: 1, textTransform: 'uppercase' },
  teamResult: { fontFamily: font, fontWeight: '600', fontSize: 12, letterSpacing: 1.2, textTransform: 'uppercase' },
  teamScore: { fontFamily: font, fontWeight: '700', fontSize: 26 },
  headRow: { flexDirection: 'row', alignItems: 'center', height: 30, paddingHorizontal: 16, borderBottomWidth: 1, borderBottomColor: colors.line },
  th: { fontFamily: font, fontWeight: '600', fontSize: 11, letterSpacing: 1.2, textTransform: 'uppercase', color: colors.textMute },
  sortBar: { position: 'absolute', bottom: -7, width: 16, height: 2, borderRadius: 1, backgroundColor: colors.gold, boxShadow: '0 0 6px rgba(242,195,91,0.8)' },
  num: { width: 36, textAlign: 'center' },
  wide: { width: 64, textAlign: 'right' },
  row: { paddingHorizontal: 16, borderWidth: 1, borderColor: 'transparent', overflow: 'hidden' },
  rowMain: { flexDirection: 'row', alignItems: 'center', height: 48 },
  rowAlt: { backgroundColor: 'rgba(255,255,255,0.025)' },
  rowMe: { borderLeftWidth: 3 },
  rowHover: { backgroundColor: 'rgba(255,255,255,0.06)' },
  rowFocus: { borderRadius: 6, boxShadow: 'inset 0 0 12px rgba(127,180,255,0.18)' },
  rank: { fontFamily: font, fontWeight: '700', fontSize: 13, color: colors.textMute },
  playerCell: { flex: 1, flexDirection: 'row', alignItems: 'center', gap: 10, paddingRight: 8 },
  avatar: { width: 30, height: 30, borderRadius: 15 },
  player: { flexShrink: 1, fontFamily: font, fontWeight: '600', fontSize: 14, color: colors.text },
  mvp: { paddingHorizontal: 6, paddingVertical: 2, borderRadius: 4, backgroundColor: colors.gold, boxShadow: '0 0 10px rgba(242,195,91,0.6)' },
  mvpText: { fontFamily: font, fontWeight: '700', fontSize: 10, letterSpacing: 0.8, color: '#2a1500' },
  streak: { paddingHorizontal: 6, paddingVertical: 2, borderRadius: 4, borderWidth: 1, borderColor: 'rgba(255,154,60,0.6)' },
  streakText: { fontFamily: font, fontWeight: '700', fontSize: 9, letterSpacing: 0.6, textTransform: 'uppercase', color: colors.orange },
  dc: { fontFamily: font, fontStyle: 'italic', fontSize: 11, color: colors.red },
  cell: { fontFamily: font, fontWeight: '500', fontSize: 14, color: colors.textDim },
  cellSorted: { color: colors.goldLight },
  gain: { position: 'absolute', right: 0, top: -6, fontFamily: font, fontWeight: '700', fontSize: 11, color: colors.green },
  details: { flexDirection: 'row', alignItems: 'center', gap: 14, height: 50, marginLeft: 28, paddingBottom: 6 },
  detailIcon: { width: 34, height: 34 },
  detailLabel: { fontFamily: font, fontWeight: '600', fontSize: 10, letterSpacing: 1.2, textTransform: 'uppercase', color: colors.textMute },
  detailValue: { fontFamily: font, fontWeight: '700', fontSize: 13, color: colors.text, marginTop: 2 },
  detailTrack: { height: 6, borderRadius: 3, backgroundColor: 'rgba(255,255,255,0.08)', marginTop: 6, overflow: 'hidden' },
  detailFill: { height: '100%', borderRadius: 3 },

  perf: { position: 'absolute', left: 40, right: 40, bottom: 22, flexDirection: 'row', gap: 14, alignItems: 'stretch' },
  perfCard: { flex: 1, padding: 14, borderRadius: 12, backgroundColor: 'rgba(255,255,255,0.04)', borderWidth: 1, borderColor: colors.line },
  perfLabel: { fontFamily: font, fontWeight: '600', fontSize: 11, letterSpacing: 1.3, textTransform: 'uppercase', color: colors.textMute },
  perfValue: { fontFamily: font, fontWeight: '700', fontSize: 24, color: colors.text, marginTop: 4 },
  perfTrack: { height: 4, borderRadius: 2, backgroundColor: 'rgba(255,255,255,0.08)', marginTop: 10, overflow: 'hidden' },
  perfFill: { height: '100%', borderRadius: 2 },
  continueBtn: {
    width: 220,
    height: '100%',
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'center',
    gap: 14,
    borderRadius: 12,
    borderWidth: 1,
    borderColor: 'transparent',
    experimental_backgroundImage: 'linear-gradient(180deg, #ffe29a 0%, #f2c35b 50%, #c9902d 100%)',
    boxShadow: '0 10px 24px rgba(242,195,91,0.3), inset 0 1px 0 rgba(255,255,255,0.6)',
  },
  continueHot: { boxShadow: '0 12px 30px rgba(242,195,91,0.5), 0 0 0 3px rgba(255,243,207,0.6), inset 0 1px 0 rgba(255,255,255,0.7)' },
  readyBtn: { experimental_backgroundImage: 'none', backgroundColor: 'rgba(74,222,128,0.12)', borderColor: 'rgba(74,222,128,0.6)', boxShadow: '0 0 20px rgba(74,222,128,0.25)' },
  continueText: { fontFamily: font, fontWeight: '700', fontSize: 17, letterSpacing: 1.5, textTransform: 'uppercase', color: '#2a1a03' },
  readyCount: { fontFamily: font, fontWeight: '600', fontSize: 11, color: 'rgba(42,26,3,0.75)', marginTop: 2 },
  continueKey: { width: 24, height: 24, borderRadius: 12, backgroundColor: 'rgba(42,26,3,0.85)', alignItems: 'center', justifyContent: 'center' },
  continueKeyText: { fontFamily: font, fontWeight: '700', fontSize: 12, color: '#6bb8ff' },
  ping: { flexDirection: 'row', alignItems: 'flex-end', gap: 2, height: 14 },
  pingBar: { width: 3, borderRadius: 1, backgroundColor: 'rgba(255,255,255,0.15)' },
});
