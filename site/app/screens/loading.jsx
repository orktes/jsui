// Loading (interactive). Feature spotlight: native-driver ambient motion
// and image loading.
//
// Progress moves through loading phases. During "Streaming textures" the
// bar counts real image assets as they stream in: each tile's Image pops in
// when its onLoad fires. Tips rotate with a crossfade (◀ ▶ / left /
// right), and the spinner, embers, pulses and slow camera drift all loop on
// the native Animated driver. When the zone is ready, any key travels to the
// next zone, which loads again.
import React, { memo, useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { Animated, Easing, Image, ImageBackground, Pressable, StyleSheet, Text, View } from 'react-native';

export const feature = {
  title: 'Native driver & image loading',
  points: [
    'The spinner, 18 drifting embers, pulses and the camera drift all loop with useNativeDriver: the runtime animates them without running JS',
    'The "Streaming textures" phase streams 20 real image assets one by one: each <Image> tile pops in on its onLoad and the bar counts them',
    'Tips crossfade between two layers; zones change behind a fade to black',
  ],
  keys: ['← → / D-pad: previous / next tip', 'Any key / A / click: continue when ready'],
};

const TEXTURES = [
  'item-sword', 'item-axe', 'item-bow', 'item-shield', 'item-helmet', 'item-boots', 'item-ring', 'item-amulet', 'item-gem', 'item-scroll',
  'item-potion-health', 'item-potion-mana', 'buff-fire', 'buff-frost', 'buff-haste', 'buff-poison', 'buff-regen', 'buff-ward', 'crest-wolf', 'crest-flame',
];
const TEX = {
  'item-sword': require('../../../examples/assets/item-sword.png'),
  'item-axe': require('../../../examples/assets/item-axe.png'),
  'item-bow': require('../../../examples/assets/item-bow.png'),
  'item-shield': require('../../../examples/assets/item-shield.png'),
  'item-helmet': require('../../../examples/assets/item-helmet.png'),
  'item-boots': require('../../../examples/assets/item-boots.png'),
  'item-ring': require('../../../examples/assets/item-ring.png'),
  'item-amulet': require('../../../examples/assets/item-amulet.png'),
  'item-gem': require('../../../examples/assets/item-gem.png'),
  'item-scroll': require('../../../examples/assets/item-scroll.png'),
  'item-potion-health': require('../../../examples/assets/item-potion-health.png'),
  'item-potion-mana': require('../../../examples/assets/item-potion-mana.png'),
  'buff-fire': require('../../../examples/assets/buff-fire.png'),
  'buff-frost': require('../../../examples/assets/buff-frost.png'),
  'buff-haste': require('../../../examples/assets/buff-haste.png'),
  'buff-poison': require('../../../examples/assets/buff-poison.png'),
  'buff-regen': require('../../../examples/assets/buff-regen.png'),
  'buff-ward': require('../../../examples/assets/buff-ward.png'),
  'crest-wolf': require('../../../examples/assets/crest-wolf.png'),
  'crest-flame': require('../../../examples/assets/crest-flame.png'),
};

/** One streamed texture: it pops in when its image has loaded (onLoadEnd:
 * onLoad, or onError so a missing file does not stall the bar). */
const Tile = memo(function Tile({ name, onLoad }) {
  const t = useRef(new Animated.Value(0)).current;
  const finish = () => {
    Animated.spring(t, { toValue: 1, stiffness: 300, damping: 14, useNativeDriver: true }).start();
    onLoad();
  };
  return (
    <View style={styles.tile}>
      <Animated.View style={{ opacity: t, transform: [{ scale: t.interpolate({ inputRange: [0, 1], outputRange: [0.4, 1] }) }] }}>
        <Image
          source={TEX[name]}
          style={styles.tileImg}
          onLoadEnd={finish}
        />
      </Animated.View>
    </View>
  );
});
import { colors, font } from '../../../examples/theme.js';

const ZONES = [
  {
    chapter: 'Chapter III',
    name: 'The Sunken Causeway',
    bg: require('../../../examples/assets/bg-loading.jpg'),
    accent: '#f2c27a',
    lore: 'When the Old King fell, the sea climbed the causeway stone by stone. The lanterns still burn beneath the tide, and those who follow their light rarely return the same.',
  },
  {
    chapter: 'Chapter IV',
    name: 'Ninth Spire Observatory',
    bg: require('../../../examples/assets/bg-menu.jpg'),
    accent: '#9fd8ff',
    lore: 'Seren’s order charted the storms from here for three hundred years. The great lens is cracked now, but on clear nights it still turns toward something no one remembers naming.',
  },
  {
    chapter: 'Chapter V',
    name: 'The Cinder Gate',
    bg: require('../../../examples/assets/bg-loading.jpg'),
    accent: '#ff9a6a',
    tint: 'rgba(120,30,0,0.35)',
    lore: 'Ash falls upward at the gate. The Wardens say it is the Choir, still singing; the Wardens no longer say much else.',
  },
];

const PHASES = [
  { label: 'Connecting to realm', total: 3, unit: 'handshakes' },
  { label: 'Streaming textures', total: TEXTURES.length, unit: 'images', real: true },
  { label: 'Compiling shaders', total: 860, unit: '' },
  { label: 'Waking creatures', total: 212, unit: '' },
  { label: 'Finalising', total: 1, unit: '' },
];
// Fraction of the bar each phase owns.
const SHARE = [0.06, 0.38, 0.36, 0.14, 0.06];

const TIPS = [
  ['Hold ', ['LB'], ' while aiming to slow time briefly. Each use drains one segment of focus.'],
  ['Lanterns you relight become fast-travel points, but the tide puts them out again at night.'],
  ['Press ', ['Y'], ' on a fallen ally to share half of your remaining focus with them.'],
  ['Stormcallers deal double damage to targets standing in water. So do you, if you are wet.'],
  ['Parry with ', ['RB'], ' just before a hit lands to stagger even the largest foes.'],
];

const SEGS = 40;

const TipText = ({ tip, accent }) => (
  <Text style={styles.tipText}>
    {tip.map((p, i) => (typeof p === 'string' ? p : <Text key={i} style={[styles.key, { color: accent }]}>[{p[0]}]</Text>))}
  </Text>
);

/** Two stacked layers crossfade between the old and the new tip. */
function Tips({ index, onStep, accent }) {
  const [shown, setShown] = useState({ a: index, b: index });
  const t = useRef(new Animated.Value(1)).current;
  const dir = useRef(1);
  const prev = useRef(index);
  useEffect(() => {
    if (prev.current === index) return;
    dir.current = (index > prev.current && !(prev.current === 0 && index === TIPS.length - 1)) || (prev.current === TIPS.length - 1 && index === 0) ? 1 : -1;
    setShown({ a: prev.current, b: index });
    prev.current = index;
    t.setValue(0);
    Animated.timing(t, { toValue: 1, duration: 520, easing: Easing.out(Easing.cubic), useNativeDriver: true }).start();
  }, [index]);
  const d = dir.current;
  return (
    <View style={styles.tip}>
      <View style={[styles.tipBadge, { backgroundColor: accent }]}>
        <Text style={styles.tipBadgeText}>Tip</Text>
      </View>
      <View style={styles.tipStack}>
        <Animated.View style={[styles.tipLayer, { opacity: t.interpolate({ inputRange: [0, 1], outputRange: [1, 0] }), transform: [{ translateX: t.interpolate({ inputRange: [0, 1], outputRange: [0, -24 * d] }) }] }]}>
          <TipText tip={TIPS[shown.a]} accent={accent} />
        </Animated.View>
        <Animated.View style={[styles.tipLayer, { opacity: t, transform: [{ translateX: t.interpolate({ inputRange: [0, 1], outputRange: [24 * d, 0] }) }] }]}>
          <TipText tip={TIPS[shown.b]} accent={accent} />
        </Animated.View>
      </View>
      <View style={styles.tipNav}>
        <Pressable focusable={false} onPress={() => onStep(-1)} style={({ hovered }) => [styles.tipArrow, hovered && styles.tipArrowHot]}>
          <Text style={styles.tipArrowText}>‹</Text>
        </Pressable>
        <Text style={styles.tipCount}>
          {index + 1}/{TIPS.length}
        </Text>
        <Pressable focusable={false} onPress={() => onStep(1)} style={({ hovered }) => [styles.tipArrow, hovered && styles.tipArrowHot]}>
          <Text style={styles.tipArrowText}>›</Text>
        </Pressable>
      </View>
    </View>
  );
}

/** Drifting embers; each loops on the native driver. */
const Embers = memo(function Embers({ active }) {
  const parts = useMemo(
    () =>
      Array.from({ length: 18 }, (_, i) => ({
        x: (i * 137) % 1280,
        size: 2 + ((i * 7) % 4),
        dur: 7000 + ((i * 1931) % 6000),
        delay: (i * 811) % 5000,
        drift: ((i % 5) - 2) * 26,
        v: new Animated.Value(0),
      })),
    [],
  );
  useEffect(() => {
    if (!active) return undefined;
    const loops = parts.map((p) => {
      p.v.setValue(0);
      return Animated.sequence([
        Animated.delay(p.delay),
        Animated.loop(Animated.timing(p.v, { toValue: 1, duration: p.dur, easing: Easing.linear, useNativeDriver: true })),
      ]);
    });
    loops.forEach((l) => l.start());
    return () => loops.forEach((l) => l.stop());
  }, [active]);
  return (
    <View pointerEvents="none" style={StyleSheet.absoluteFill}>
      {parts.map((p, i) => (
        <Animated.View
          key={i}
          style={[
            styles.ember,
            {
              left: p.x,
              width: p.size,
              height: p.size,
              borderRadius: p.size / 2,
              opacity: p.v.interpolate({ inputRange: [0, 0.1, 0.7, 1], outputRange: [0, 0.9, 0.6, 0] }),
              transform: [
                { translateY: p.v.interpolate({ inputRange: [0, 1], outputRange: [0, -560] }) },
                { translateX: p.v.interpolate({ inputRange: [0, 0.5, 1], outputRange: [0, p.drift, p.drift * 0.4] }) },
              ],
            },
          ]}
        />
      ))}
    </View>
  );
});

const Bar = memo(function Bar({ lit, ready, accent }) {
  return (
    <View style={styles.track}>
      {Array.from({ length: SEGS }, (_, i) => (
        <View key={i} style={[styles.seg, i < lit && { backgroundColor: ready ? accent : '#e0a85c' }, i === lit && !ready && styles.segHead]} />
      ))}
    </View>
  );
});

/** Owns the progress state so its 10 Hz ticks re-render only this panel. */
function ProgressPanel({ active, paused, accent, onReady }) {
  const [progress, setProgress] = useState(0); // 0..1
  const [mounted, setMounted] = useState(0); // texture tiles requested
  const [loaded, setLoaded] = useState(0); // texture tiles whose onLoad fired
  const ready = progress >= 1;
  const texStart = SHARE[0];
  const texEnd = SHARE[0] + SHARE[1];
  const inTextures = progress >= texStart && loaded < TEXTURES.length;
  // Simulated phases: irregular steps (with a stall during shaders).
  useEffect(() => {
    if (!active || ready || paused || inTextures) return undefined;
    const id = setTimeout(() => {
      setProgress((p) => {
        const stall = p > 0.55 && p < 0.6 ? 0.25 : 1;
        const n = Math.min(1, p + (0.004 + Math.random() * 0.018) * stall);
        return p < texStart ? Math.min(n, texStart) : n;
      });
    }, 90);
    return () => clearTimeout(id);
  }, [active, progress, ready, paused, inTextures]);
  // Real phase: mount the next image every 160 ms; the bar follows onLoad.
  useEffect(() => {
    if (!active || paused || !inTextures || mounted >= TEXTURES.length) return undefined;
    const id = setTimeout(() => setMounted((m) => m + 1), 160);
    return () => clearTimeout(id);
  }, [active, paused, inTextures, mounted]);
  const onTile = useCallback(() => setLoaded((n) => n + 1), []);
  useEffect(() => {
    if (loaded) setProgress(texStart + (texEnd - texStart) * (loaded / TEXTURES.length));
  }, [loaded]);
  useEffect(() => {
    if (ready) onReady();
  }, [ready]);

  let acc = 0;
  let phase = PHASES.length - 1;
  let within = 1;
  for (let i = 0; i < SHARE.length; i++) {
    if (progress < acc + SHARE[i]) {
      phase = i;
      within = (progress - acc) / SHARE[i];
      break;
    }
    acc += SHARE[i];
  }
  const ph = PHASES[phase];
  const count = ph.real ? loaded : Math.floor(within * ph.total);
  return (
    <View style={{ flex: 1 }}>
      <View style={styles.tiles} pointerEvents="none">
        <Text style={styles.tilesLabel}>Textures {loaded}/{TEXTURES.length}</Text>
        <View style={styles.tileGrid}>
          {TEXTURES.map((n, i) => (i < mounted ? <Tile key={n} name={n} onLoad={onTile} /> : <View key={n} style={styles.tile} />))}
        </View>
      </View>
      <View style={styles.statusRow}>
        <Text style={styles.status}>{ready ? 'Ready' : ph.label}</Text>
        {!ready && ph.total > 1 ? (
          <Text style={styles.statusCount}>
            {count} / {ph.total} {ph.unit}
          </Text>
        ) : null}
        <View style={{ flex: 1 }} />
        <Text style={[styles.pct, { color: accent }]}>{Math.floor(progress * 100)}%</Text>
      </View>
      <Bar lit={Math.floor(progress * SEGS)} ready={ready} accent={accent} />
    </View>
  );
}

export default function Loading({ active = true }) {
  const [zone, setZone] = useState(0);
  const [ready, setReady] = useState(false);
  const [tip, setTip] = useState(0);
  const [leaving, setLeaving] = useState(false);
  const z = ZONES[zone];

  // Tips rotate on their own.
  useEffect(() => {
    if (!active) return undefined;
    const id = setTimeout(() => setTip((t) => (t + 1) % TIPS.length), 5200);
    return () => clearTimeout(id);
  }, [active, tip]);
  const stepTip = useCallback((d) => setTip((t) => (t + d + TIPS.length) % TIPS.length), []);

  // Motion.
  const spin = useRef(new Animated.Value(0)).current;
  const kb = useRef(new Animated.Value(0)).current;
  const pulse = useRef(new Animated.Value(0)).current;
  const intro = useRef(new Animated.Value(0)).current;
  const readyV = useRef(new Animated.Value(0)).current;
  const black = useRef(new Animated.Value(1)).current;
  useEffect(() => {
    if (!active) return undefined;
    const a = Animated.loop(Animated.timing(spin, { toValue: 1, duration: 1100, easing: Easing.linear, useNativeDriver: true }));
    const b = Animated.loop(Animated.sequence([
      Animated.timing(kb, { toValue: 1, duration: 16000, easing: Easing.inOut(Easing.sin), useNativeDriver: true }),
      Animated.timing(kb, { toValue: 0, duration: 16000, easing: Easing.inOut(Easing.sin), useNativeDriver: true }),
    ]));
    const c = Animated.loop(Animated.sequence([
      Animated.timing(pulse, { toValue: 1, duration: 900, easing: Easing.inOut(Easing.sin), useNativeDriver: true }),
      Animated.timing(pulse, { toValue: 0, duration: 900, easing: Easing.inOut(Easing.sin), useNativeDriver: true }),
    ]));
    a.start();
    b.start();
    c.start();
    return () => {
      a.stop();
      b.stop();
      c.stop();
    };
  }, [active]);
  useEffect(() => {
    intro.setValue(0);
    Animated.timing(black, { toValue: 0, duration: 700, easing: Easing.out(Easing.quad), useNativeDriver: true }).start();
    Animated.timing(intro, { toValue: 1, duration: 1100, delay: 200, easing: Easing.out(Easing.cubic), useNativeDriver: true }).start();
  }, [zone]);
  useEffect(() => {
    Animated.spring(readyV, { toValue: ready ? 1 : 0, stiffness: 180, damping: 16, useNativeDriver: true }).start();
  }, [ready]);

  const proceed = useCallback(() => {
    if (!ready || leaving) return;
    setLeaving(true);
    Animated.timing(black, { toValue: 1, duration: 520, easing: Easing.in(Easing.quad), useNativeDriver: true }).start(() => {
      setZone((zi) => (zi + 1) % ZONES.length);
      setReady(false);
      setLeaving(false);
    });
  }, [ready, leaving]);

  const onKeyDown = (e) => {
    const k = e.key;
    if (k === 'ArrowLeft' || k === 'ArrowRight') {
      stepTip(k === 'ArrowLeft' ? -1 : 1);
      e.preventDefault();
      e.stopPropagation();
    } else if (ready && k !== 'Escape' && k !== 'Tab' && k !== 'Shift' && k !== '[' && k !== ']') {
      proceed();
      e.preventDefault();
      e.stopPropagation();
    }
  };
  const onPad = (e) => {
    if (!e.pressed) return;
    const b = e.buttonIndex;
    if (b === 14 || b === 15) {
      stepTip(b === 14 ? -1 : 1);
      e.preventDefault();
      e.stopPropagation();
    } else if (ready && b !== 1 && b !== 4 && b !== 5 && b !== 12 && b !== 13) {
      proceed();
      e.preventDefault();
      e.stopPropagation();
    }
  };

  const rise = (from) => ({ opacity: intro, transform: [{ translateY: intro.interpolate({ inputRange: [0, 1], outputRange: [from, 0] }) }] });

  return (
    <Pressable
      onPress={proceed}
      onKeyDown={onKeyDown}
      onGamepadButton={onPad}
      autoFocus={active}
      style={[styles.root, { cursor: ready ? 'pointer' : 'default' }]}
    >
      <>
          <Animated.View
            style={[
              StyleSheet.absoluteFill,
              {
                transform: [
                  { scale: kb.interpolate({ inputRange: [0, 1], outputRange: [1.04, 1.12] }) },
                  { translateX: kb.interpolate({ inputRange: [0, 1], outputRange: [-14, 14] }) },
                ],
              },
            ]}
          >
            <ImageBackground source={z.bg} style={StyleSheet.absoluteFill} resizeMode="cover" />
            {z.tint ? <View style={[StyleSheet.absoluteFill, { backgroundColor: z.tint }]} /> : null}
          </Animated.View>
          <Embers active={active} />
          <View style={styles.grade} />

          <Animated.View style={[styles.logo, { opacity: intro }]}>
            <Text style={styles.logoText}>AETHERFALL</Text>
            <Text style={styles.logoSub}>Chronicles of the Shattered Sky</Text>
          </Animated.View>

          <View style={styles.info}>
            <Animated.Text style={[styles.chapter, { color: z.accent }, rise(10)]}>{z.chapter}</Animated.Text>
            <Animated.Text style={[styles.zone, rise(18)]}>{z.name}</Animated.Text>
            <Animated.View style={[styles.zoneRule, { backgroundColor: z.accent, transform: [{ scaleX: intro }] }]} />
            <Animated.Text style={[styles.lore, rise(24)]}>{z.lore}</Animated.Text>
            <Tips index={tip} onStep={stepTip} accent={z.accent} />
          </View>

          <View style={styles.bottom}>
            <View style={styles.spinnerWrap}>
              <Animated.View style={[styles.spinnerFrame, { borderColor: z.accent + '66', transform: [{ rotate: '45deg' }, { scale: pulse.interpolate({ inputRange: [0, 1], outputRange: [0.9, 1.08] }) }] }]} />
              <Animated.View
                style={[
                  styles.spinner,
                  { borderTopColor: z.accent, opacity: readyV.interpolate({ inputRange: [0, 1], outputRange: [1, 0] }), transform: [{ rotate: spin.interpolate({ inputRange: [0, 1], outputRange: ['0deg', '360deg'] }) }] },
                ]}
              />
              <Animated.View style={[styles.spinnerCore, { transform: [{ scale: Animated.add(pulse.interpolate({ inputRange: [0, 1], outputRange: [0.8, 1.2] }), Animated.multiply(readyV, 0.8)) }] }]} />
            </View>
            <ProgressPanel key={zone} active={active} paused={leaving} accent={z.accent} onReady={() => setReady(true)} />
          </View>

          <Animated.View
            pointerEvents="none"
            style={[styles.ready, { opacity: Animated.multiply(readyV, pulse.interpolate({ inputRange: [0, 1], outputRange: [0.55, 1] })), transform: [{ translateY: readyV.interpolate({ inputRange: [0, 1], outputRange: [12, 0] }) }] }]}
          >
            <View style={styles.readyGlyph}>
              <Text style={styles.readyGlyphText}>A</Text>
            </View>
            <Text style={styles.readyText}>Press any key to continue</Text>
          </Animated.View>

          <Animated.View pointerEvents="none" style={[StyleSheet.absoluteFill, { backgroundColor: '#000', opacity: black }]} />
      </>
    </Pressable>
  );
}

const styles = StyleSheet.create({
  root: { width: 1280, height: 720, overflow: 'hidden', backgroundColor: '#0b1a20' },
  grade: {
    ...StyleSheet.absoluteFillObject,
    experimental_backgroundImage:
      'linear-gradient(0deg, rgba(3,8,12,0.95) 0%, rgba(3,8,12,0.55) 35%, rgba(3,8,12,0) 60%), linear-gradient(90deg, rgba(3,8,12,0.6) 0%, rgba(3,8,12,0) 50%)',
  },
  ember: { position: 'absolute', top: 700, backgroundColor: '#ffd9a0', boxShadow: '0 0 8px rgba(255,190,110,0.9)' },
  logo: { position: 'absolute', right: 56, top: 44, alignItems: 'flex-end' },
  logoText: { fontFamily: font, fontWeight: '700', fontSize: 26, letterSpacing: 7, color: '#fff6dc', textShadowColor: 'rgba(242,195,91,0.6)', textShadowOffset: { width: 0, height: 0 }, textShadowRadius: 14 },
  logoSub: { fontFamily: font, fontStyle: 'italic', fontSize: 12, color: colors.textDim, marginTop: 4 },

  info: { position: 'absolute', left: 64, bottom: 150, width: 580 },
  chapter: { fontFamily: font, fontWeight: '600', fontSize: 13, letterSpacing: 4, textTransform: 'uppercase' },
  zone: { fontFamily: font, fontWeight: '700', fontSize: 48, color: '#fff', marginTop: 6, letterSpacing: -0.5, textShadowColor: 'rgba(0,0,0,0.6)', textShadowOffset: { width: 0, height: 2 }, textShadowRadius: 10 },
  zoneRule: { width: 80, height: 3, borderRadius: 2, marginTop: 14, transformOrigin: 'left', boxShadow: '0 0 10px rgba(242,194,122,0.8)' },
  lore: { fontFamily: font, fontSize: 16, lineHeight: 26, color: '#c9d4dc', marginTop: 16, minHeight: 78 },
  tip: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 12,
    marginTop: 22,
    padding: 14,
    borderRadius: 10,
    backgroundColor: 'rgba(8,20,26,0.75)',
    borderWidth: 1,
    borderColor: 'rgba(120,200,220,0.2)',
  },
  tipBadge: { paddingHorizontal: 8, paddingVertical: 3, borderRadius: 4, alignSelf: 'flex-start' },
  tipBadgeText: { fontFamily: font, fontWeight: '700', fontSize: 11, letterSpacing: 1.2, textTransform: 'uppercase', color: '#03141c' },
  tipStack: { flex: 1, height: 42, overflow: 'hidden' },
  tipLayer: { position: 'absolute', left: 0, right: 0, top: 0 },
  tipText: { fontFamily: font, fontSize: 14, lineHeight: 21, color: colors.text },
  key: { fontWeight: '700' },
  tipNav: { flexDirection: 'row', alignItems: 'center', gap: 4 },
  tipArrow: { width: 26, height: 26, borderRadius: 13, alignItems: 'center', justifyContent: 'center', borderWidth: 1, borderColor: 'rgba(255,255,255,0.15)', cursor: 'pointer' },
  tipArrowHot: { borderColor: 'rgba(255,255,255,0.6)', backgroundColor: 'rgba(255,255,255,0.08)' },
  tipArrowText: { fontFamily: font, fontWeight: '700', fontSize: 16, lineHeight: 18, color: colors.text, marginTop: -2 },
  tipCount: { fontFamily: font, fontWeight: '600', fontSize: 11, color: colors.textMute, width: 26, textAlign: 'center' },

  bottom: { position: 'absolute', left: 64, right: 64, bottom: 48, flexDirection: 'row', alignItems: 'center', gap: 22 },
  spinnerWrap: { width: 48, height: 48, alignItems: 'center', justifyContent: 'center' },
  spinnerFrame: { position: 'absolute', width: 34, height: 34, borderWidth: 1 },
  spinner: {
    position: 'absolute',
    width: 44,
    height: 44,
    borderRadius: 22,
    borderWidth: 3,
    borderRightColor: 'rgba(242,194,122,0.5)',
    borderBottomColor: 'rgba(242,194,122,0.12)',
    borderLeftColor: 'rgba(242,194,122,0.12)',
  },
  spinnerCore: { width: 8, height: 8, borderRadius: 4, backgroundColor: '#fff3d6', boxShadow: '0 0 12px rgba(255,220,160,0.9)' },
  statusRow: { flexDirection: 'row', alignItems: 'baseline', gap: 10, marginBottom: 10 },
  status: { fontFamily: font, fontWeight: '600', fontSize: 14, color: colors.text },
  statusCount: { fontFamily: font, fontWeight: '500', fontSize: 12, color: colors.textMute },
  pct: { fontFamily: font, fontWeight: '700', fontSize: 22 },
  track: { flexDirection: 'row', gap: 3, height: 8 },
  seg: { flex: 1, borderRadius: 1, backgroundColor: 'rgba(255,255,255,0.10)' },
  segHead: { backgroundColor: '#fff3d6', boxShadow: '0 0 10px rgba(255,220,160,0.9)' },

  tiles: { position: 'absolute', right: 0, bottom: 64, width: 352, alignItems: 'flex-end' },
  tilesLabel: { fontFamily: font, fontWeight: '700', fontSize: 10, letterSpacing: 1.6, textTransform: 'uppercase', color: colors.textMute, marginBottom: 8 },
  tileGrid: { flexDirection: 'row', flexWrap: 'wrap', gap: 4, justifyContent: 'flex-end' },
  tile: { width: 31, height: 31, borderRadius: 6, backgroundColor: 'rgba(255,255,255,0.05)', borderWidth: 1, borderColor: 'rgba(255,255,255,0.08)', alignItems: 'center', justifyContent: 'center' },
  tileImg: { width: 25, height: 25 },
  ready: { position: 'absolute', left: 0, right: 0, bottom: 96, flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: 10 },
  readyGlyph: { width: 24, height: 24, borderRadius: 12, backgroundColor: colors.green, alignItems: 'center', justifyContent: 'center', boxShadow: '0 0 12px rgba(74,222,128,0.7)' },
  readyGlyphText: { fontFamily: font, fontWeight: '700', fontSize: 12, color: '#04210f' },
  readyText: { fontFamily: font, fontWeight: '600', fontSize: 15, letterSpacing: 2, textTransform: 'uppercase', color: '#fff' },
  focusFrame: { ...StyleSheet.absoluteFillObject, borderWidth: 2, borderColor: 'rgba(79,209,255,0.35)' },
});
