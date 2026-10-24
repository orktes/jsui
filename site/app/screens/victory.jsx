// Victory (interactive). Feature spotlight: Lottie animations.
//
// An end-of-match screen built around Lottie files (examples/assets/lottie,
// made by make.mjs there) shown with <LottieView>, lottie-react-native's
// component. The runtime plays them itself with Skia's Skottie: a playing
// animation runs on the native frame clock and repaints only its own view,
// with no JS per frame.
//
// - The emblem plays its intro segment once (ref.play(0, 110)), then loops
//   the idle segment (play(110, 230)) from onAnimationFinish.
// - The timeline scrubs it: the slider writes an Animated.Value that is the
//   emblem's `progress` (one bridge call per drag event, none per frame).
// - Team colors recolor it live with colorFilters (keypaths into the file).
// - Reward cards loop small Lottie icons; claiming draws a checkmark whose
//   progress is an Animated.timing on the native driver.
import React, { memo, useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { Animated, Easing, ImageBackground, PanResponder, Pressable, StyleSheet, Text, View } from 'react-native';
import LottieView from 'lottie-react-native';
import { colors, font } from '../../../examples/theme.js';

export const feature = {
  title: 'Lottie animations',
  points: [
    '<LottieView> with lottie-react-native’s API: source={require(\'./anim.json\')}, autoPlay, loop, speed, progress, ref.play(start, end) / pause() / resume()',
    'Played natively by Skia’s Skottie on the runtime clock: each playing animation repaints only its own view and runs no JS per frame',
    'progress can be an Animated value, also on the native driver: the timeline and the claim checkmarks use it',
    'colorFilters recolor named layers and fills live (team colors); one parsed file serves every view',
  ],
  keys: ['Space / Y: pause / resume', 'R / X: replay the intro', '1–4 / LB RB: team color', 'S: speed', 'C / A: claim rewards'],
};

const EMBLEM = require('../../../examples/assets/lottie/victory-emblem.json');
const COIN = require('../../../examples/assets/lottie/coin.json');
const ORB = require('../../../examples/assets/lottie/xp-orb.json');
const GEM = require('../../../examples/assets/lottie/gem.json');
const CHECK = require('../../../examples/assets/lottie/checkmark.json');
const SPINNER = require('../../../examples/assets/lottie/spinner.json');
const BG = require('../../../examples/assets/bg-menu.jpg');

// Emblem segments (frames at 60 fps).
const INTRO_END = 110;
const LOOP_END = 230;

// colorFilters per team: keypaths name solid fills / strokes in the file.
const TEAMS = [
  { id: 'royal', label: 'Royal', swatch: '#6a4cf0', filters: null },
  {
    id: 'crimson',
    label: 'Crimson',
    swatch: '#e0314f',
    filters: [
      { keypath: '**.Shield Fill', color: '#4a0f1f' },
      { keypath: '**.Inner Fill', color: '#c4233f' },
      { keypath: '**.Inner Stroke', color: '#ffb3c0' },
    ],
  },
  {
    id: 'azure',
    label: 'Azure',
    swatch: '#2f8cff',
    filters: [
      { keypath: '**.Shield Fill', color: '#0b2350' },
      { keypath: '**.Inner Fill', color: '#1f6fe0' },
      { keypath: '**.Inner Stroke', color: '#a9d4ff' },
      { keypath: '**.Outline Stroke', color: '#9fe3ff' },
    ],
  },
  {
    id: 'verdant',
    label: 'Verdant',
    swatch: '#22c55e',
    filters: [
      { keypath: '**.Shield Fill', color: '#0c3a22' },
      { keypath: '**.Inner Fill', color: '#16a34a' },
      { keypath: '**.Inner Stroke', color: '#b7f7cf' },
    ],
  },
];
const SPEEDS = [1, 2, 0.5];

const REWARDS = [
  { id: 'coins', label: 'Crowns', amount: '+1,250', source: COIN, accent: colors.gold },
  { id: 'xp', label: 'Battle XP', amount: '+3,400', source: ORB, accent: colors.cyan },
  { id: 'gems', label: 'Void shards', amount: '×3', source: GEM, accent: colors.purple },
];

const STATS = [
  ['Eliminations', '14'],
  ['Assists', '9'],
  ['Damage', '18,420'],
  ['Objective', '3:12'],
];

const TRACK_W = 300;
const RANK_W = 320;

/** Entrance: fades and lifts in after `delay` ms (native driver). */
function useEntrance(delay, active) {
  const t = useRef(new Animated.Value(active ? 0 : 1)).current;
  useEffect(() => {
    if (!active) return;
    t.setValue(0);
    Animated.timing(t, { toValue: 1, duration: 620, delay, easing: Easing.out(Easing.cubic), useNativeDriver: true }).start();
  }, [active]);
  return {
    opacity: t,
    transform: [{ translateY: t.interpolate({ inputRange: [0, 1], outputRange: [18, 0] }) }],
  };
}

/** The emblem timeline: drag to scrub (writes the Animated progress). */
const Timeline = memo(function Timeline({ progress, scrubbing, onScrub, onRelease, accent }) {
  const fromX = useRef(null);
  fromX.current = (x) => {
    const p = Math.max(0, Math.min(1, x / TRACK_W));
    progress.setValue(p);
    onScrub(p);
  };
  const pan = useMemo(
    () =>
      PanResponder.create({
        onStartShouldSetPanResponder: () => true,
        onMoveShouldSetPanResponder: () => true,
        onPanResponderGrant: (e) => fromX.current(e.nativeEvent.locationX - 10),
        onPanResponderMove: (e) => fromX.current(e.nativeEvent.locationX - 10),
        onPanResponderRelease: () => onRelease(),
        onPanResponderTerminate: () => onRelease(),
      }),
    [],
  );
  const introX = (INTRO_END / LOOP_END) * TRACK_W;
  return (
    <View style={styles.timelineHit} {...pan.panHandlers}>
      <View pointerEvents="none" style={styles.timelineTrack}>
        <View style={[styles.timelineIntro, { width: introX }]} />
        <Animated.View
          style={[
            styles.timelineFill,
            { backgroundColor: accent, transform: [{ translateX: progress.interpolate({ inputRange: [0, 1], outputRange: [-TRACK_W, 0] }) }] },
          ]}
        />
      </View>
      <View pointerEvents="none" style={[styles.timelineMark, { left: 10 + introX }]} />
      <Animated.View
        pointerEvents="none"
        style={[
          styles.timelineThumb,
          { borderColor: accent, opacity: scrubbing ? 1 : 0.55 },
          { transform: [{ translateX: progress.interpolate({ inputRange: [0, 1], outputRange: [0, TRACK_W] }) }] },
        ]}
      />
    </View>
  );
});

/** A reward: its icon loops; claimed, a checkmark draws over it (its
 * progress is an Animated.timing on the native driver). */
const RewardCard = memo(function RewardCard({ reward, index, claimed, active }) {
  const enter = useEntrance(500 + index * 140, active);
  const check = useRef(new Animated.Value(claimed ? 1 : 0)).current;
  useEffect(() => {
    if (!claimed) {
      check.setValue(0);
      return;
    }
    Animated.timing(check, { toValue: 1, duration: 900, delay: index * 160, easing: Easing.linear, useNativeDriver: true }).start();
  }, [claimed]);
  return (
    <Animated.View style={[styles.card, { borderColor: reward.accent + '55' }, enter]}>
      <View style={[styles.cardGlow, { backgroundColor: reward.accent }]} />
      <LottieView source={reward.source} autoPlay={active} progress={active ? undefined : 0.2} loop style={styles.cardIcon} />
      <View style={styles.cardText}>
        <Text style={styles.cardLabel}>{reward.label}</Text>
        <Text style={[styles.cardAmount, { color: reward.accent }]}>{reward.amount}</Text>
      </View>
      <View style={styles.cardCheck} pointerEvents="none">
        <LottieView source={CHECK} progress={check} style={styles.cardCheckIcon} />
      </View>
    </Animated.View>
  );
});

/** Rank progress: the bar fills past the threshold and rolls over into
 * the new rank (native driver). */
function RankProgress({ accent, active }) {
  const fill = useRef(new Animated.Value(active ? 0.62 : 0.24)).current;
  const [promoted, setPromoted] = useState(!active);
  useEffect(() => {
    if (!active) return;
    fill.setValue(0.62);
    setPromoted(false);
    Animated.timing(fill, { toValue: 1, duration: 1100, delay: 700, easing: Easing.inOut(Easing.cubic), useNativeDriver: true }).start(({ finished }) => {
      if (!finished) return;
      setPromoted(true);
      fill.setValue(0);
      Animated.timing(fill, { toValue: 0.24, duration: 900, easing: Easing.out(Easing.cubic), useNativeDriver: true }).start();
    });
  }, [active]);
  return (
    <View style={styles.rankBlock}>
      <View style={styles.controlRow}>
        <Text style={styles.controlLabel}>{promoted ? 'Diamond II' : 'Diamond III'}</Text>
        <Text style={styles.controlValue}>{promoted ? '240 / 1,000 RP' : '+3,400 XP'}</Text>
      </View>
      <View style={styles.rankTrack}>
        <Animated.View
          style={[styles.rankFill, { backgroundColor: accent, transform: [{ translateX: fill.interpolate({ inputRange: [0, 1], outputRange: [-RANK_W, 0] }) }] }]}
        />
      </View>
    </View>
  );
}

function Chip({ label, active, color, onPress, hint }) {
  return (
    <Pressable onPress={onPress} style={({ hovered, focused }) => [styles.chip, active && { borderColor: color, backgroundColor: color + '26' }, (hovered || focused) && styles.chipHover]}>
      {color ? <View style={[styles.chipDot, { backgroundColor: color }]} /> : null}
      <Text style={[styles.chipText, active && { color: colors.text }]}>{label}</Text>
      {hint ? <Text style={styles.chipHint}>{hint}</Text> : null}
    </Pressable>
  );
}

export default function Victory({ active = true }) {
  const emblem = useRef(null);
  const [phase, setPhase] = useState(active ? 'intro' : 'still'); // intro | idle | still (thumbnail)
  const [paused, setPaused] = useState(false);
  const [team, setTeam] = useState(0);
  const [speedIx, setSpeedIx] = useState(0);
  const [scrub, setScrub] = useState(null); // frame shown while scrubbing (null: playing)
  const [dragging, setDragging] = useState(false);
  const [claim, setClaim] = useState('idle'); // idle | claiming | claimed
  const progress = useRef(new Animated.Value(0)).current;
  const speed = SPEEDS[speedIx];
  const accent = TEAMS[team].swatch;

  const title = useEntrance(80, active);
  const left = useEntrance(260, active);
  const right = useEntrance(380, active);

  // Start: the intro segment once.
  useEffect(() => {
    if (!active) return;
    const id = setTimeout(() => emblem.current && emblem.current.play(0, INTRO_END), 250);
    return () => clearTimeout(id);
  }, [active]);
  const onFinish = useCallback((cancelled) => {
    if (cancelled) return;
    setPhase((p) => {
      if (p === 'intro') emblem.current && emblem.current.play(INTRO_END, LOOP_END);
      return p === 'intro' ? 'idle' : p;
    });
  }, []);

  const replay = useCallback(() => {
    setScrub(null);
    setPaused(false);
    setPhase('intro');
    emblem.current && emblem.current.play(0, INTRO_END);
  }, []);
  const togglePause = useCallback(() => {
    if (scrub != null) {
      // Leave the scrubbed frame: carry on from there.
      const f = scrub;
      setScrub(null);
      setPaused(false);
      const intro = f < INTRO_END;
      setPhase(intro ? 'intro' : 'idle');
      setTimeout(() => emblem.current && (intro ? emblem.current.play(f, INTRO_END) : emblem.current.play(INTRO_END, LOOP_END)), 0);
      return;
    }
    setPaused((p) => {
      if (emblem.current) p ? emblem.current.resume() : emblem.current.pause();
      return !p;
    });
  }, [scrub]);
  const onScrub = useCallback((p) => {
    setDragging(true);
    setScrub(Math.round(p * LOOP_END));
  }, []);
  const onRelease = useCallback(() => setDragging(false), []);
  const doClaim = useCallback(() => {
    setClaim((c) => {
      if (c !== 'idle') return c;
      setTimeout(() => setClaim('claimed'), 900);
      return 'claiming';
    });
  }, []);

  const handled = (e) => {
    e.preventDefault();
    e.stopPropagation();
  };
  const onKeyDown = (e) => {
    const k = e.key;
    if (k === ' ') togglePause(), handled(e);
    else if (k === 'r' || k === 'R') replay(), handled(e);
    else if (k >= '1' && k <= '4') setTeam(+k - 1), handled(e);
    else if (k === 's' || k === 'S') setSpeedIx((i) => (i + 1) % SPEEDS.length), handled(e);
    else if (k === 'c' || k === 'C') doClaim(), handled(e);
  };
  const onPad = (e) => {
    if (!e.pressed) return;
    if (e.buttonIndex === 0) doClaim(), handled(e);
    else if (e.buttonIndex === 2) replay(), handled(e);
    else if (e.buttonIndex === 3) togglePause(), handled(e);
    else if (e.buttonIndex === 4) setTeam((t) => (t + TEAMS.length - 1) % TEAMS.length), handled(e);
    else if (e.buttonIndex === 5) setTeam((t) => (t + 1) % TEAMS.length), handled(e);
  };

  const still = phase === 'still';
  const status = still ? 'Preview' : scrub != null ? `Frame ${scrub} / ${LOOP_END}` : paused ? 'Paused' : phase === 'intro' ? 'Intro · once' : 'Idle loop';

  return (
    <View focusable autoFocus={active} style={styles.root} onKeyDown={onKeyDown} onGamepadButton={onPad}>
      <ImageBackground source={BG} style={StyleSheet.absoluteFill} resizeMode="cover" />
      <View style={styles.shade} />
      <View style={[styles.burst, { backgroundColor: accent }]} />

      <Animated.View style={[styles.header, title]}>
        <Text style={styles.kicker}>Ranked · Shattered Peaks · 14:32</Text>
        <Text style={styles.title}>VICTORY</Text>
        <View style={styles.rankRow}>
          <View style={[styles.rankDot, { backgroundColor: accent }]} />
          <Text style={styles.rank}>Promoted to Diamond II</Text>
        </View>
      </Animated.View>

      <Animated.View style={[styles.left, left]}>
        <View style={styles.emblemWrap}>
          <LottieView
            ref={emblem}
            source={EMBLEM}
            loop={phase === 'idle'}
            speed={speed}
            progress={still ? 0.62 : scrub != null ? progress : undefined}
            colorFilters={TEAMS[team].filters}
            onAnimationFinish={onFinish}
            style={styles.emblem}
          />
        </View>

        <View style={styles.controls}>
          <RankProgress accent={accent} active={active} />
          <View style={styles.controlRow}>
            <Text style={styles.controlLabel}>Emblem timeline</Text>
            <Text style={styles.controlValue}>{status}</Text>
          </View>
          <Timeline progress={progress} scrubbing={dragging || scrub != null} onScrub={onScrub} onRelease={onRelease} accent={accent} />
          <View style={styles.controlRow}>
            <Chip label={scrub != null ? 'Play' : paused ? 'Resume' : 'Pause'} hint="Space" onPress={togglePause} />
            <Chip label="Replay" hint="R" onPress={replay} />
            <Chip label={`${speed}×`} hint="S" onPress={() => setSpeedIx((i) => (i + 1) % SPEEDS.length)} />
          </View>
          <View style={styles.controlRow}>
            {TEAMS.map((t, i) => (
              <Chip key={t.id} label={t.label} color={t.swatch} active={team === i} onPress={() => setTeam(i)} />
            ))}
          </View>
        </View>
      </Animated.View>

      <Animated.View style={[styles.right, right]}>
        <Text style={styles.panelTitle}>Match rewards</Text>
        {REWARDS.map((r, i) => (
          <RewardCard key={r.id} reward={r} index={i} claimed={claim === 'claimed'} active={active} />
        ))}
        <Pressable
          onPress={doClaim}
          style={({ hovered, focused }) => [styles.claim, { backgroundColor: claim === 'claimed' ? colors.greenDeep : accent }, (hovered || focused) && styles.claimHover]}
        >
          {claim === 'claiming' ? <LottieView source={SPINNER} autoPlay loop style={styles.spinner} /> : null}
          <Text style={styles.claimText}>{claim === 'idle' ? 'Claim all' : claim === 'claiming' ? 'Claiming…' : 'Claimed'}</Text>
          {claim === 'idle' ? <Text style={styles.claimHint}>C / A</Text> : null}
        </Pressable>

      </Animated.View>

      <Animated.View style={[styles.stats, left]}>
        {STATS.map(([k, v]) => (
          <View key={k} style={styles.stat}>
            <Text style={styles.statValue}>{v}</Text>
            <Text style={styles.statLabel}>{k}</Text>
          </View>
        ))}
      </Animated.View>

      <View style={styles.footer} pointerEvents="none">
        <Text style={styles.footerText}>
          <Text style={{ color: accent }}>8</Text> Lottie views · played by Skottie on the runtime clock ·{' '}
          <Text style={{ color: colors.green }}>0</Text> JS per frame
        </Text>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: { width: 1280, height: 720, overflow: 'hidden', backgroundColor: colors.bg0 },
  shade: { ...StyleSheet.absoluteFillObject, backgroundColor: 'rgba(7, 6, 18, 0.78)' },
  burst: { position: 'absolute', left: 60, top: 170, width: 420, height: 420, borderRadius: 210, opacity: 0.2, filter: 'blur(90px)' },
  header: { position: 'absolute', left: 64, top: 44 },
  kicker: { fontFamily: font, fontWeight: '600', fontSize: 13, letterSpacing: 2, textTransform: 'uppercase', color: colors.textDim },
  title: { fontFamily: font, fontWeight: '700', fontSize: 64, letterSpacing: 14, color: colors.goldLight, marginTop: 2, textShadowColor: 'rgba(255, 190, 80, 0.45)', textShadowRadius: 24, textShadowOffset: { width: 0, height: 0 } },
  rankRow: { flexDirection: 'row', alignItems: 'center', marginTop: 4 },
  rankDot: { width: 8, height: 8, borderRadius: 4, marginRight: 8 },
  rank: { fontFamily: font, fontWeight: '600', fontSize: 15, color: colors.text },

  left: { position: 'absolute', left: 40, top: 168, width: 740, flexDirection: 'row' },
  emblemWrap: { width: 400, height: 400 },
  emblem: { width: 400, height: 400 },
  controls: { width: 340, marginLeft: 16, marginTop: 34 },
  rankBlock: { marginBottom: 26 },
  rankTrack: { width: RANK_W, height: 8, borderRadius: 4, backgroundColor: 'rgba(255,255,255,0.1)', overflow: 'hidden' },
  rankFill: { position: 'absolute', left: 0, top: 0, bottom: 0, width: RANK_W, borderRadius: 4 },
  controlRow: { flexDirection: 'row', alignItems: 'center', flexWrap: 'wrap', marginBottom: 12 },
  controlLabel: { fontFamily: font, fontWeight: '600', fontSize: 12, letterSpacing: 1.6, textTransform: 'uppercase', color: colors.textDim, flex: 1 },
  controlValue: { fontFamily: font, fontWeight: '600', fontSize: 13, color: colors.text, fontVariant: ['tabular-nums'] },

  timelineHit: { width: TRACK_W + 20, height: 34, marginLeft: -10, marginBottom: 14, justifyContent: 'center', cursor: 'pointer' },
  timelineTrack: { marginLeft: 10, width: TRACK_W, height: 6, borderRadius: 3, backgroundColor: 'rgba(255,255,255,0.1)', overflow: 'hidden' },
  timelineIntro: { position: 'absolute', left: 0, top: 0, bottom: 0, backgroundColor: 'rgba(255,255,255,0.08)' },
  timelineFill: { position: 'absolute', left: 0, top: 0, bottom: 0, width: TRACK_W, opacity: 0.85 },
  timelineMark: { position: 'absolute', top: 9, width: 2, height: 16, backgroundColor: 'rgba(255,255,255,0.35)', borderRadius: 1 },
  timelineThumb: { position: 'absolute', left: 3, top: 10, width: 14, height: 14, borderRadius: 7, borderWidth: 3, backgroundColor: '#fff' },

  chip: { flexDirection: 'row', alignItems: 'center', height: 30, paddingHorizontal: 9, borderRadius: 8, borderWidth: 1, borderColor: colors.line, backgroundColor: 'rgba(255,255,255,0.04)', marginRight: 5, marginBottom: 6 },
  chipHover: { borderColor: colors.lineStrong, backgroundColor: 'rgba(255,255,255,0.09)' },
  chipDot: { width: 9, height: 9, borderRadius: 5, marginRight: 6 },
  chipText: { fontFamily: font, fontWeight: '600', fontSize: 13, color: colors.textDim },
  chipHint: { fontFamily: font, fontWeight: '600', fontSize: 10, color: colors.textMute, marginLeft: 7 },

  right: { position: 'absolute', right: 64, top: 118, width: 400, backgroundColor: 'rgba(14, 16, 34, 0.86)', borderRadius: 18, borderWidth: 1, borderColor: colors.line, padding: 22, boxShadow: '0 24px 60px rgba(0,0,0,0.55), inset 0 1px 0 rgba(255,255,255,0.06)' },
  panelTitle: { fontFamily: font, fontWeight: '600', fontSize: 12, letterSpacing: 1.8, textTransform: 'uppercase', color: colors.textDim, marginBottom: 14 },
  card: { flexDirection: 'row', alignItems: 'center', height: 92, borderRadius: 14, borderWidth: 1, backgroundColor: 'rgba(255,255,255,0.035)', marginBottom: 10, overflow: 'hidden' },
  cardGlow: { position: 'absolute', left: -30, top: -30, width: 150, height: 150, borderRadius: 75, opacity: 0.14, filter: 'blur(26px)' },
  cardIcon: { width: 92, height: 92, marginLeft: 6 },
  cardText: { flex: 1, marginLeft: 6 },
  cardLabel: { fontFamily: font, fontWeight: '600', fontSize: 13, color: colors.textDim, letterSpacing: 0.5 },
  cardAmount: { fontFamily: font, fontWeight: '700', fontSize: 26, marginTop: 2, fontVariant: ['tabular-nums'] },
  cardCheck: { width: 64, height: 64, marginRight: 14 },
  cardCheckIcon: { width: 64, height: 64 },

  claim: { height: 52, borderRadius: 12, flexDirection: 'row', alignItems: 'center', justifyContent: 'center', marginTop: 6, boxShadow: '0 10px 24px rgba(0,0,0,0.35)' },
  claimHover: { transform: [{ scale: 1.02 }] },
  claimText: { fontFamily: font, fontWeight: '700', fontSize: 16, letterSpacing: 1, color: '#fff' },
  claimHint: { fontFamily: font, fontWeight: '600', fontSize: 11, color: 'rgba(255,255,255,0.7)', marginLeft: 10 },
  spinner: { width: 30, height: 30, marginRight: 8 },

  stats: { position: 'absolute', left: 64, top: 590, width: 660, flexDirection: 'row', borderTopWidth: 1, borderTopColor: colors.line, paddingTop: 16 },
  stat: { flex: 1 },
  statValue: { fontFamily: font, fontWeight: '700', fontSize: 24, color: colors.text, fontVariant: ['tabular-nums'] },
  statLabel: { fontFamily: font, fontWeight: '600', fontSize: 11, letterSpacing: 1.2, textTransform: 'uppercase', color: colors.textMute, marginTop: 3 },

  footer: { position: 'absolute', right: 64, bottom: 30 },
  footerText: { fontFamily: font, fontWeight: '500', fontSize: 12, color: colors.textMute, letterSpacing: 0.4 },
});
