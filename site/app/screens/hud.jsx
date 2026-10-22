// Interactive combat HUD (1280×720): the examples/screens/hud.jsx reference
// running a small combat simulation. Health / mana bars drain with a trailing
// damage ghost and regenerate, abilities have real clock-wipe cooldowns
// (keys 1-4 / Q / R, clicks, gamepad X Y A LT / LB / RB), buffs count down and
// expire, the boss loses phases, kill feed entries slide in, minimap pings
// pulse and damage numbers float. Clicking the scene (or RT) fires the
// weapon. Left alone for a few seconds the HUD plays itself.
import React, { memo, useCallback, useEffect, useRef, useState } from 'react';
import { Animated, Easing, Host, Image, LayoutAnimation, Pressable, StyleSheet, Text, View, useHostMessage } from 'react-native';
import { colors, font } from '../../../examples/theme.js';

export const feature = {
  title: 'Native Animated driver',
  points: [
    'Bars, cooldown clock-wipes, damage numbers, minimap pings and screen effects are Animated values with useNativeDriver: the runtime advances them in C++ every frame; JS only reacts to game events.',
    'The combat sim updates React state a few times a second (hits, buff timers); everything in between is native interpolation.',
    'Health shows a trailing damage ghost and an incoming-heal preview; buffs blink before they expire and leave with LayoutAnimation.',
    'Key caps switch to gamepad glyphs the moment a pad is used. The embedder can push hits with the host message "hud:hit" and hears "hud:cast".',
  ],
  keys: ['1 2 3 4 / X Y A LT: abilities', 'Q / LB: potion', 'R / RB: ultimate (when charged)', 'Click / Space / RT: fire', 'Idle for 6 s: the HUD plays itself'],
};

const ND = true;

const IMG = {
  bg: require('../../../examples/assets/bg-loading.jpg'),
  portrait: require('../../../examples/assets/portrait-mage.png'),
  minimap: require('../../../examples/assets/minimap.png'),
  sword: require('../../../examples/assets/item-sword.png'),
  shield: require('../../../examples/assets/item-shield.png'),
  bow: require('../../../examples/assets/item-bow.png'),
  axe: require('../../../examples/assets/item-axe.png'),
  potion: require('../../../examples/assets/item-potion-health.png'),
  gem: require('../../../examples/assets/item-gem.png'),
  haste: require('../../../examples/assets/buff-haste.png'),
  ward: require('../../../examples/assets/buff-ward.png'),
  regen: require('../../../examples/assets/buff-regen.png'),
  poison: require('../../../examples/assets/buff-poison.png'),
  fire: require('../../../examples/assets/buff-fire.png'),
  frost: require('../../../examples/assets/buff-frost.png'),
  wolf: require('../../../examples/assets/crest-wolf.png'),
  flame: require('../../../examples/assets/crest-flame.png'),
};

const HP_MAX = 1600;
const MP_MAX = 900;
const BOSS_MAX = 240000;

// Abilities: key, gamepad glyph, cooldown (ms), mana cost, damage range.
const ABILITIES = [
  { key: '1', pad: 'X', padBtn: 2, icon: IMG.sword, cd: 2200, cost: 0, dmg: [1600, 2400], name: 'Stormblade', proc: true },
  { key: '2', pad: 'Y', padBtn: 3, icon: IMG.shield, cd: 12000, cost: 80, dmg: null, name: 'Aegis Ward' },
  { key: '3', pad: 'A', padBtn: 0, icon: IMG.bow, cd: 6000, cost: 60, dmg: [2600, 3400], name: 'Galeshot' },
  { key: '4', pad: 'LT', padBtn: 6, icon: IMG.axe, cd: 9000, cost: 140, dmg: [4800, 6400], name: 'Thunder Cleave' },
  { key: 'Q', pad: 'LB', padBtn: 4, icon: IMG.potion, cd: 8000, cost: 0, dmg: null, name: 'Healing Draught', potion: true },
  { key: 'R', pad: 'RB', padBtn: 5, icon: IMG.gem, cd: 0, cost: 0, dmg: [14000, 18000], name: 'Skyfall', ultimate: true },
];
const KEY_TO_SLOT = { 1: 0, 2: 1, 3: 2, 4: 3, q: 4, r: 5 };

const BUFF_DEFS = {
  haste: { icon: IMG.haste },
  ward: { icon: IMG.ward },
  regen: { icon: IMG.regen },
  poison: { icon: IMG.poison, debuff: true },
};

const NAMES = ['Ashlight', 'nightjar', 'Mireille', 'pyrite', 'GRIMWALD', 'Okonkwo', 'tessellate', 'Brannoc', 'Valkyrie_07', 'Sir Reginald'];
const WEAPONS = [IMG.sword, IMG.bow, IMG.axe, IMG.gem];

const useValue = (v) => useRef(new Animated.Value(v)).current;
const rnd = (a, b) => a + Math.random() * (b - a);
const fmt = (n) => String(Math.round(n)).replace(/\B(?=(\d{3})+(?!\d))/g, ',');

// ---------------------------------------------------------------------------
// Resource bar with a trailing damage ghost (white) / incoming heal (green).

function useBar(frac) {
  const fill = useValue(frac);
  const ghost = useValue(frac);
  const last = useRef(frac);
  const set = useCallback((next) => {
    const prev = last.current;
    last.current = next;
    if (next < prev) {
      Animated.timing(fill, { toValue: next, duration: 120, easing: Easing.out(Easing.quad), useNativeDriver: ND }).start();
      ghost.stopAnimation();
      Animated.timing(ghost, { toValue: next, duration: 520, delay: 420, easing: Easing.inOut(Easing.quad), useNativeDriver: ND }).start();
    } else if (next > prev) {
      ghost.stopAnimation();
      ghost.setValue(next);
      Animated.timing(fill, { toValue: next, duration: 650, easing: Easing.out(Easing.cubic), useNativeDriver: ND }).start();
    }
  }, []);
  return { fill, ghost, set };
}

const Bar = memo(function Bar({ bar, fill, height = 14, ticks = 0, label, ghostColor = 'rgba(255,255,255,0.55)' }) {
  return (
    <View style={[styles.barTrack, { height }]}>
      <Animated.View style={[styles.barFill, { backgroundColor: ghostColor, transform: [{ scaleX: bar.ghost }] }]} />
      <Animated.View style={[styles.barFill, { experimental_backgroundImage: fill, transform: [{ scaleX: bar.fill }] }]}>
        <View style={styles.barSheen} />
      </Animated.View>
      {Array.from({ length: ticks }, (_, i) => (
        <View key={i} style={[styles.tick, { left: `${((i + 1) / (ticks + 1)) * 100}%` }]} />
      ))}
      {label ? (
        <View style={styles.barLabelWrap}>
          <Text style={styles.barLabel}>{label}</Text>
        </View>
      ) : null}
    </View>
  );
});

// ---------------------------------------------------------------------------
// Clock-wipe cooldown: two clipped half-planes rotating about the slot centre
// cover the remaining fraction `p` (1 -> 0), mirrored so it unwinds clockwise.

const SWEEP_S = 54;
const SWEEP_D = 84;
function Sweep({ p, size = SWEEP_S, radius = 8 }) {
  const D = SWEEP_D * (size / SWEEP_S);
  const half = size / 2;
  const right = p.interpolate({ inputRange: [0, 0.5, 1], outputRange: ['0deg', '180deg', '180deg'] });
  const left = p.interpolate({ inputRange: [0, 0.5, 1], outputRange: ['0deg', '0deg', '180deg'] });
  const edge = p.interpolate({ inputRange: [0, 1], outputRange: ['0deg', '360deg'] });
  return (
    <View pointerEvents="none" style={[StyleSheet.absoluteFill, { borderRadius: radius, overflow: 'hidden', transform: [{ scaleX: -1 }] }]}>
      <View style={{ position: 'absolute', left: half, top: 0, width: half, height: size, overflow: 'hidden' }}>
        <Animated.View
          style={[
            styles.sweepPiece,
            { left: -D / 2, top: (size - D) / 2, width: D / 2, height: D, transform: [{ translateX: D / 4 }, { rotate: right }, { translateX: -D / 4 }] },
          ]}
        />
      </View>
      <View style={{ position: 'absolute', left: 0, top: 0, width: half, height: size, overflow: 'hidden' }}>
        <Animated.View
          style={[
            styles.sweepPiece,
            { left: half, top: (size - D) / 2, width: D / 2, height: D, transform: [{ translateX: -D / 4 }, { rotate: left }, { translateX: D / 4 }] },
          ]}
        />
      </View>
      <Animated.View
        style={[
          styles.sweepEdge,
          {
            left: half - 1,
            top: half - D / 2,
            height: D / 2,
            opacity: p.interpolate({ inputRange: [0, 0.01, 1], outputRange: [0, 1, 1] }),
            transform: [{ translateY: D / 4 }, { rotate: edge }, { translateY: -D / 4 }],
          },
        ]}
      />
    </View>
  );
}

const AbilitySlot = memo(function AbilitySlot({ a, index, sweep, secs, ready, usable, count, charge, padMode, onUse, fx }) {
  const glow = fx.glow;
  const isUlt = a.ultimate;
  return (
    <Pressable focusable={false} onPressIn={() => onUse(index, 'mouse')} style={{ cursor: 'pointer' }}>
      {({ hovered }) => (
        <Animated.View
          style={[
            styles.slot,
            a.proc && ready && styles.slotReady,
            isUlt && styles.slotUlt,
            isUlt && charge >= 1 && styles.slotUltReady,
            hovered && !isUlt && styles.slotHover,
            { transform: [{ scale: fx.scale }, { translateX: fx.shake }] },
          ]}
        >
          {isUlt ? (
            <Animated.View
              style={[
                styles.ultFill,
                { transform: [{ translateY: fx.charge.interpolate({ inputRange: [0, 1], outputRange: [66, 0] }) }] },
              ]}
            />
          ) : null}
          <Image source={a.icon} style={[styles.slotIcon, !usable && !isUlt && { opacity: 0.55 }]} />
          {!isUlt ? <Sweep p={sweep} /> : null}
          {secs ? (
            <View style={styles.cdTextWrap} pointerEvents="none">
              <Text style={styles.cdText}>{secs}</Text>
            </View>
          ) : null}
          {count != null ? <Text style={styles.count}>{count}</Text> : null}
          {isUlt && charge < 1 ? <Text style={styles.ultPct}>{Math.floor(charge * 100)}%</Text> : null}
          <Animated.View pointerEvents="none" style={[StyleSheet.absoluteFill, { borderRadius: isUlt ? 33 : 8, backgroundColor: '#fff', opacity: fx.flash }]} />
          {isUlt ? null : (
            <View style={[styles.keycap, padMode && styles.keycapPad]}>
              <Text style={[styles.keyText, padMode && { color: PAD_COLORS[a.pad] || colors.textDim }]}>{padMode ? a.pad : a.key}</Text>
            </View>
          )}
          {isUlt ? (
            <View style={[styles.keycap, styles.keycapUlt, padMode && styles.keycapPad]}>
              <Text style={styles.keyText}>{padMode ? a.pad : a.key}</Text>
            </View>
          ) : null}
          {a.proc ? <Animated.View pointerEvents="none" style={[StyleSheet.absoluteFill, styles.procGlow, { opacity: ready ? glow : 0 }]} /> : null}
        </Animated.View>
      )}
    </Pressable>
  );
});
const PAD_COLORS = { A: '#5fe08a', LT: colors.textDim, X: '#6bb8ff', Y: '#ffd36b', LB: colors.textDim, RB: colors.textDim };

// ---------------------------------------------------------------------------

const Buff = memo(function Buff({ id, secs, stacks, blink }) {
  const def = BUFF_DEFS[id];
  return (
    <View style={styles.buff}>
      <Animated.View style={{ opacity: secs <= 3 ? blink : 1 }}>
        <Image source={def.icon} style={[styles.buffIcon, def.debuff && styles.debuffIcon]} />
      </Animated.View>
      {stacks ? (
        <View style={styles.stack}>
          <Text style={styles.stackText}>{stacks}</Text>
        </View>
      ) : null}
      <Text style={[styles.buffTime, def.debuff && { color: '#ff9aa8' }]}>{secs}s</Text>
    </View>
  );
});

function FloatText({ item, onDone }) {
  const t = useValue(0);
  useEffect(() => {
    Animated.timing(t, { toValue: 1, duration: item.dur || 1000, easing: Easing.out(Easing.cubic), useNativeDriver: ND }).start(() => onDone(item.id));
  }, []);
  return (
    <Animated.Text
      pointerEvents="none"
      style={[
        styles.floatText,
        { left: item.x, top: item.y, fontSize: item.size || 22, color: item.color },
        {
          opacity: t.interpolate({ inputRange: [0, 0.1, 0.7, 1], outputRange: [0, 1, 1, 0] }),
          transform: [
            { translateY: t.interpolate({ inputRange: [0, 1], outputRange: [0, item.rise || -70] }) },
            { translateX: t.interpolate({ inputRange: [0, 1], outputRange: [0, item.dx || 0] }) },
            { scale: t.interpolate({ inputRange: [0, 0.12, 0.3, 1], outputRange: [0.4, item.crit ? 1.5 : 1.2, 1, 0.9] }) },
          ],
        },
      ]}
    >
      {item.text}
    </Animated.Text>
  );
}

const KillEntry = memo(function KillEntry({ e }) {
  const t = useValue(0);
  useEffect(() => {
    Animated.spring(t, { toValue: 1, stiffness: 260, damping: 20, useNativeDriver: ND }).start();
  }, []);
  return (
    <Animated.View
      style={[
        styles.kill,
        e.me && styles.killMe,
        { opacity: t, transform: [{ translateX: t.interpolate({ inputRange: [0, 1], outputRange: [-40, 0] }) }] },
      ]}
    >
      <Text style={[styles.killName, { color: e.ally ? '#7fb4ff' : '#ff7f8f' }]}>{e.a}</Text>
      <Image source={e.w} style={styles.killIcon} />
      {e.head ? <View style={styles.headshot} /> : null}
      <Text style={[styles.killName, { color: e.ally ? '#ff7f8f' : '#7fb4ff' }]}>{e.b}</Text>
    </Animated.View>
  );
});

function Ping({ left, top, color, active, delay }) {
  const t = useValue(0);
  useEffect(() => {
    if (!active) return undefined;
    const anim = Animated.loop(
      Animated.sequence([
        Animated.delay(delay),
        Animated.timing(t, { toValue: 1, duration: 1400, easing: Easing.out(Easing.quad), useNativeDriver: ND }),
        Animated.timing(t, { toValue: 0, duration: 0, useNativeDriver: ND }),
        Animated.delay(800),
      ]),
    );
    anim.start();
    return () => anim.stop();
  }, [active]);
  return (
    <>
      <Animated.View
        style={[
          styles.pingRing,
          { left: left - 6, top: top - 6, borderColor: color },
          { opacity: t.interpolate({ inputRange: [0, 0.1, 1], outputRange: [0, 0.9, 0] }), transform: [{ scale: t.interpolate({ inputRange: [0, 1], outputRange: [0.5, 2.6] }) }] },
        ]}
      />
      <View style={[styles.ping, { left, top, backgroundColor: color }]} />
    </>
  );
}

const Minimap = memo(function Minimap({ active }) {
  const look = useValue(0);
  const [clock, setClock] = useState(21 * 60 + 40);
  const [pos, setPos] = useState([482, 1290]);
  useEffect(() => {
    if (!active) return undefined;
    const anim = Animated.loop(
      Animated.sequence([
        Animated.timing(look, { toValue: 1, duration: 3200, easing: Easing.inOut(Easing.sin), useNativeDriver: ND }),
        Animated.timing(look, { toValue: -1, duration: 6400, easing: Easing.inOut(Easing.sin), useNativeDriver: ND }),
        Animated.timing(look, { toValue: 0, duration: 3200, easing: Easing.inOut(Easing.sin), useNativeDriver: ND }),
      ]),
    );
    anim.start();
    const id = setInterval(() => {
      setClock((c) => (c + 1) % 1440);
      setPos((p) => [p[0] + Math.round(rnd(-3, 4)), p[1] + Math.round(rnd(-2, 5))]);
    }, 2500);
    return () => {
      anim.stop();
      clearInterval(id);
    };
  }, [active]);
  const rot = look.interpolate({ inputRange: [-1, 1], outputRange: ['-28deg', '28deg'] });
  const pad = (n) => String(n).padStart(2, '0');
  return (
    <View style={styles.mapWrap}>
      <View style={styles.mapFrame}>
        <View style={styles.mapClip}>
          <Image source={IMG.minimap} style={styles.mapImage} />
          <Animated.View style={[styles.fovPivot, { transform: [{ rotate: rot }] }]}>
            <View style={styles.fov} />
          </Animated.View>
          <Animated.View style={[styles.playerArrow, { transform: [{ rotate: '45deg' }] }]} />
          <Ping left={52} top={40} color={colors.gold} active={active} delay={0} />
          <Ping left={128} top={112} color={colors.red} active={active} delay={700} />
          <Ping left={40} top={120} color={colors.cyan} active={active} delay={1400} />
        </View>
        <View style={styles.north}>
          <Text style={styles.northText}>N</Text>
        </View>
      </View>
      <Text style={styles.zone}>Ashen Reach</Text>
      <Text style={styles.zoneSub}>
        {pos[0]}, {pos[1]} · {pad(Math.floor(clock / 60))}:{pad(clock % 60)}
      </Text>
    </View>
  );
});

// ---------------------------------------------------------------------------

function initialGame() {
  return {
    t: 0,
    hp: 1240,
    mp: 610,
    energy: 72,
    boss: BOSS_MAX * 0.58,
    bossDead: 0,
    xp: 0.64,
    level: 42,
    ammo: 24,
    reserve: 180,
    reloadUntil: 0,
    burstUntil: 0,
    potions: 5,
    ult: 0.62,
    braziers: 2,
    // Cooldowns in game ms: end time + total. Matches the reference frame.
    cds: ABILITIES.map((a, i) => (i === 1 ? { end: 7800, total: 12000 } : i === 3 ? { end: 2700, total: 9000 } : { end: 0, total: a.cd })),
    buffs: [
      { id: 'haste', end: 12000 },
      { id: 'ward', end: 45000, stacks: 3 },
      { id: 'regen', end: 8000 },
      { id: 'poison', end: 4000 },
    ],
    feed: [],
    nextBossHit: 1500,
    nextPoison: 14000,
    nextKill: 1800,
    nextAi: 5000,
    lastInput: -99999,
    feedId: 0,
  };
}

export default function Hud({ active = true }) {
  const g = useRef(null);
  if (!g.current) g.current = initialGame();
  const [, setTick] = useState(0);
  const bump = useCallback(() => setTick((n) => (n + 1) | 0), []);
  const [floats, setFloats] = useState([]);
  const [padMode, setPadMode] = useState(false);
  const [msg, setMsg] = useState(null);
  const [banner, setBanner] = useState(null);

  const hp = useBar(g.current.hp / HP_MAX);
  const mp = useBar(g.current.mp / MP_MAX);
  const en = useBar(g.current.energy / 100);
  const boss = useBar(g.current.boss / BOSS_MAX);
  const xp = useBar(g.current.xp);
  const sweeps = useRef(ABILITIES.map((a, i) => new Animated.Value(i === 1 ? 0.65 : i === 3 ? 0.3 : 0))).current;
  const fx = useRef(
    ABILITIES.map(() => ({
      scale: new Animated.Value(1),
      flash: new Animated.Value(0),
      shake: new Animated.Value(0),
      glow: new Animated.Value(0.6),
      charge: new Animated.Value(0.62),
    })),
  ).current;
  const blink = useValue(1);
  const hurt = useValue(0);
  const low = useValue(0);
  const shakeX = useValue(0);
  const spread = useValue(0);
  const hit = useValue(0);
  const ultFlash = useValue(0);
  const msgT = useValue(0);
  const bannerT = useValue(0);
  const lvl = useValue(1);
  const reload = useValue(0);
  const floatId = useRef(0);

  const addFloat = useCallback((f) => {
    const id = ++floatId.current;
    setFloats((list) => (list.length > 10 ? list.slice(-10) : list).concat(Object.assign({ id }, f)));
  }, []);
  const removeFloat = useCallback((id) => setFloats((list) => list.filter((f) => f.id !== id)), []);

  const flashMsg = (text) => {
    setMsg(text);
    msgT.setValue(1);
    Animated.timing(msgT, { toValue: 0, duration: 1300, delay: 500, useNativeDriver: ND }).start();
  };
  const showBanner = (b) => {
    setBanner(b);
    bannerT.setValue(0);
    Animated.sequence([
      Animated.spring(bannerT, { toValue: 1, stiffness: 220, damping: 16, useNativeDriver: ND }),
      Animated.delay(1600),
      Animated.timing(bannerT, { toValue: 0, duration: 400, useNativeDriver: ND }),
    ]).start();
  };

  // Sweep for one slot from its current remaining fraction to 0.
  const runSweep = (i) => {
    const s = g.current;
    const cd = s.cds[i];
    const rem = Math.max(0, cd.end - s.t);
    sweeps[i].stopAnimation();
    if (rem <= 0) {
      sweeps[i].setValue(0);
      return;
    }
    sweeps[i].setValue(rem / cd.total);
    Animated.timing(sweeps[i], { toValue: 0, duration: rem, easing: Easing.linear, useNativeDriver: ND }).start();
  };

  const syncBars = () => {
    const s = g.current;
    hp.set(Math.max(0, s.hp) / HP_MAX);
    mp.set(s.mp / MP_MAX);
    en.set(s.energy / 100);
    boss.set(s.boss / BOSS_MAX);
  };

  const gainXp = (amount) => {
    const s = g.current;
    s.xp += amount;
    if (s.xp >= 1) {
      s.xp = 1;
      xp.set(1);
      setTimeout(() => {
        s.xp = 0.02;
        s.level += 1;
        xp.fill.setValue(0);
        xp.ghost.setValue(0);
        xp.set(0.02);
        lvl.setValue(1.8);
        Animated.spring(lvl, { toValue: 1, stiffness: 200, damping: 9, useNativeDriver: ND }).start();
        showBanner({ kicker: 'Level up', title: `Level ${s.level}`, color: colors.goldLight });
        bump();
      }, 700);
    } else xp.set(s.xp);
  };

  const damageBoss = (amount, crit, from) => {
    const s = g.current;
    if (s.bossDead) return;
    s.boss = Math.max(0, s.boss - amount);
    s.ult = Math.min(1, s.ult + amount / 60000);
    fx[5].charge.stopAnimation();
    Animated.timing(fx[5].charge, { toValue: s.ult, duration: 400, useNativeDriver: ND }).start();
    hit.setValue(1);
    Animated.timing(hit, { toValue: 0, duration: 260, useNativeDriver: ND }).start();
    addFloat({
      x: 620 + rnd(-60, 60) + (from || 0),
      y: 300 + rnd(-30, 20),
      text: fmt(amount) + (crit ? '!' : ''),
      color: crit ? '#ffd36b' : '#fff',
      size: crit ? 30 : 22,
      crit,
      dx: rnd(-30, 30),
    });
    gainXp(amount / 90000);
    if (s.boss <= 0) {
      s.bossDead = s.t + 4200;
      showBanner({ kicker: 'Boss defeated', title: 'Vharos, the Cinder Wyrm', color: '#ffb38a' });
      s.braziers = 4;
      const kill = { id: ++s.feedId, a: 'Seren Ashvale', b: 'Vharos', w: IMG.gem, ally: true, me: true, at: s.t };
      LayoutAnimation.configureNext(LayoutAnimation.create(260, 'easeInEaseOut', 'opacity'));
      s.feed = s.feed.concat(kill).slice(-4);
      gainXp(0.3);
    }
  };

  const hurtPlayer = (amount) => {
    const s = g.current;
    const ward = s.buffs.find((b) => b.id === 'ward');
    if (ward) {
      amount = Math.round(amount * 0.5);
      ward.stacks -= 1;
      if (ward.stacks <= 0) {
        LayoutAnimation.configureNext(LayoutAnimation.create(240, 'easeInEaseOut', 'opacity'));
        s.buffs = s.buffs.filter((b) => b !== ward);
      }
    }
    s.hp -= amount;
    addFloat({ x: 150 + rnd(0, 140), y: 66, text: '-' + amount, color: '#ff6b81', size: 16, rise: -34, dur: 900 });
    hurt.setValue(1);
    Animated.timing(hurt, { toValue: 0, duration: 600, easing: Easing.out(Easing.quad), useNativeDriver: ND }).start();
    if (amount > 90) {
      Animated.sequence([
        Animated.timing(shakeX, { toValue: 5, duration: 40, useNativeDriver: ND }),
        Animated.timing(shakeX, { toValue: -4, duration: 50, useNativeDriver: ND }),
        Animated.timing(shakeX, { toValue: 2, duration: 50, useNativeDriver: ND }),
        Animated.timing(shakeX, { toValue: 0, duration: 60, useNativeDriver: ND }),
      ]).start();
    }
    if (s.hp <= 0) {
      // Second wind: back to full instead of a game over.
      s.hp = HP_MAX;
      s.buffs = s.buffs.filter((b) => !BUFF_DEFS[b.id].debuff);
      showBanner({ kicker: 'Second wind', title: 'Revived', color: colors.green });
    }
  };

  const addBuff = (id, ms, stacks) => {
    const s = g.current;
    const existing = s.buffs.find((b) => b.id === id);
    if (existing) {
      existing.end = s.t + ms;
      if (stacks) existing.stacks = stacks;
    } else {
      LayoutAnimation.configureNext(LayoutAnimation.create(240, 'easeInEaseOut', 'opacity'));
      s.buffs = s.buffs.concat({ id, end: s.t + ms, stacks });
    }
  };

  const pop = (i, ok) => {
    const f = fx[i];
    if (ok) {
      f.scale.setValue(0.86);
      Animated.spring(f.scale, { toValue: 1, stiffness: 420, damping: 12, useNativeDriver: ND }).start();
      f.flash.setValue(0.75);
      Animated.timing(f.flash, { toValue: 0, duration: 380, useNativeDriver: ND }).start();
    } else {
      Animated.sequence([
        Animated.timing(f.shake, { toValue: 5, duration: 40, useNativeDriver: ND }),
        Animated.timing(f.shake, { toValue: -5, duration: 60, useNativeDriver: ND }),
        Animated.timing(f.shake, { toValue: 3, duration: 50, useNativeDriver: ND }),
        Animated.timing(f.shake, { toValue: 0, duration: 50, useNativeDriver: ND }),
      ]).start();
    }
  };

  const useAbility = useCallback((i, source) => {
    const s = g.current;
    if (source !== 'ai') s.lastInput = s.t;
    const a = ABILITIES[i];
    const cd = s.cds[i];
    if (a.ultimate) {
      if (s.ult < 1) {
        pop(i, false);
        if (source !== 'ai') flashMsg('Skyfall is not charged');
        return bump();
      }
      s.ult = 0;
      fx[5].charge.stopAnimation();
      Animated.timing(fx[5].charge, { toValue: 0, duration: 600, useNativeDriver: ND }).start();
      pop(i, true);
      ultFlash.setValue(1);
      Animated.timing(ultFlash, { toValue: 0, duration: 900, easing: Easing.out(Easing.quad), useNativeDriver: ND }).start();
      Animated.sequence([
        Animated.timing(shakeX, { toValue: 9, duration: 40, useNativeDriver: ND }),
        Animated.timing(shakeX, { toValue: -8, duration: 60, useNativeDriver: ND }),
        Animated.timing(shakeX, { toValue: 5, duration: 60, useNativeDriver: ND }),
        Animated.timing(shakeX, { toValue: 0, duration: 80, useNativeDriver: ND }),
      ]).start();
      damageBoss(Math.round(rnd(a.dmg[0], a.dmg[1])), true);
      syncBars();
      return bump();
    }
    if (cd.end > s.t) {
      pop(i, false);
      if (source !== 'ai') flashMsg(`${a.name} is not ready`);
      return bump();
    }
    if (a.potion && s.potions <= 0) {
      pop(i, false);
      flashMsg('No potions left');
      return bump();
    }
    if (s.mp < a.cost) {
      pop(i, false);
      if (source !== 'ai') flashMsg('Not enough mana');
      return bump();
    }
    s.mp -= a.cost;
    const haste = s.buffs.some((b) => b.id === 'haste');
    cd.total = Math.round(a.cd * (haste ? 0.8 : 1));
    cd.end = s.t + cd.total;
    runSweep(i);
    pop(i, true);
    spread.setValue(1);
    Animated.spring(spread, { toValue: 0, stiffness: 300, damping: 14, useNativeDriver: ND }).start();
    if (a.dmg) {
      const crit = Math.random() < 0.22;
      const amount = Math.round(rnd(a.dmg[0], a.dmg[1]) * (crit ? 1.8 : 1));
      damageBoss(amount, crit);
      if (i === 2) addBuff('haste', 12000);
      if (i === 3) s.energy = Math.min(100, s.energy + 18);
    }
    if (i === 1) addBuff('ward', 45000, 3);
    if (source !== 'ai') Host.postMessage('hud:cast', { ability: a.name, key: a.key });
    if (a.potion) {
      s.potions -= 1;
      s.hp = Math.min(HP_MAX, s.hp + 520);
      addBuff('regen', 8000);
      addFloat({ x: 200 + rnd(0, 80), y: 66, text: '+520', color: '#7cf0a4', size: 18, rise: -34, dur: 1000 });
    }
    syncBars();
    bump();
  }, []);

  // The embedding game can push damage in: Host message "hud:hit" (amount).
  const activeRef = useRef(active);
  activeRef.current = active;
  useHostMessage('hud:hit', (amount) => {
    if (!activeRef.current) return;
    hurtPlayer(Math.max(1, Math.round(Number(amount) || 120)));
    syncBars();
    bump();
  });

  const fire = useCallback((source) => {
    const s = g.current;
    if (source !== 'ai') s.lastInput = s.t;
    if (s.reloadUntil > s.t || s.burstUntil > s.t) return;
    if (s.ammo <= 0) return;
    const shots = Math.min(3, s.ammo);
    s.ammo -= shots;
    s.burstUntil = s.t + 260;
    spread.setValue(1);
    Animated.spring(spread, { toValue: 0, stiffness: 380, damping: 12, useNativeDriver: ND }).start();
    for (let k = 0; k < shots; k++) {
      const crit = Math.random() < 0.15;
      setTimeout(() => damageBoss(Math.round(rnd(260, 380) * (crit ? 2 : 1)), crit, (k - 1) * 30), k * 80);
    }
    if (s.ammo <= 0) startReload();
    setTimeout(() => {
      syncBars();
      bump();
    }, shots * 80);
    bump();
  }, []);

  const startReload = () => {
    const s = g.current;
    if (s.reloadUntil > s.t || s.ammo >= 30) return;
    if (s.reserve <= 0) s.reserve = 180;
    s.reloadUntil = s.t + 1600;
    reload.setValue(0);
    Animated.timing(reload, { toValue: 1, duration: 1600, easing: Easing.linear, useNativeDriver: ND }).start();
  };

  // Game loop: advances game time only while active (so timers pause with the screen).
  useEffect(() => {
    if (!active) return undefined;
    const s = g.current;
    ABILITIES.forEach((_, i) => runSweep(i));
    const loops = [
      Animated.loop(
        Animated.sequence([
          Animated.timing(blink, { toValue: 0.25, duration: 260, useNativeDriver: ND }),
          Animated.timing(blink, { toValue: 1, duration: 260, useNativeDriver: ND }),
        ]),
      ),
      Animated.loop(
        Animated.sequence([
          Animated.timing(fx[0].glow, { toValue: 1, duration: 700, easing: Easing.inOut(Easing.sin), useNativeDriver: ND }),
          Animated.timing(fx[0].glow, { toValue: 0.35, duration: 700, easing: Easing.inOut(Easing.sin), useNativeDriver: ND }),
        ]),
      ),
      Animated.loop(
        Animated.sequence([
          Animated.timing(low, { toValue: 1, duration: 600, easing: Easing.inOut(Easing.sin), useNativeDriver: ND }),
          Animated.timing(low, { toValue: 0, duration: 600, easing: Easing.inOut(Easing.sin), useNativeDriver: ND }),
        ]),
      ),
    ];
    loops.forEach((l) => l.start());
    let last = Date.now();
    const STEP = 250;
    const id = setInterval(() => {
      const now = Date.now();
      const dt = Math.min(500, now - last);
      last = now;
      const before = s.t;
      s.t += dt;
      let dirty = false;
      const sec = (t) => Math.floor(t / 1000);
      const tickSecond = sec(s.t) !== sec(before);

      // Cooldown labels change once per second.
      s.cds.forEach((cd, i) => {
        if (cd.end > before && cd.end <= s.t) {
          fx[i].flash.setValue(0.55);
          Animated.timing(fx[i].flash, { toValue: 0, duration: 450, useNativeDriver: ND }).start();
          fx[i].scale.setValue(1.1);
          Animated.spring(fx[i].scale, { toValue: 1, stiffness: 300, damping: 10, useNativeDriver: ND }).start();
          dirty = true;
        }
        if (cd.end > before && Math.ceil((cd.end - s.t) / 1000) !== Math.ceil((cd.end - before) / 1000)) dirty = true;
      });

      // Buffs / debuffs.
      const live = s.buffs.filter((b) => b.end > s.t);
      if (live.length !== s.buffs.length) {
        LayoutAnimation.configureNext(LayoutAnimation.create(260, 'easeInEaseOut', 'opacity'));
        s.buffs = live;
        dirty = true;
      }
      if (tickSecond) {
        dirty = true;
        if (s.buffs.some((b) => b.id === 'poison')) {
          s.hp -= 18;
          addFloat({ x: 300 + rnd(0, 20), y: 140, text: '-18', color: '#8ef06b', size: 13, rise: -24, dur: 800 });
        }
        if (s.buffs.some((b) => b.id === 'regen')) s.hp += 24;
        s.hp = Math.min(HP_MAX, s.hp + 6);
        s.mp = Math.min(MP_MAX, s.mp + 22);
        s.energy = Math.min(100, s.energy + 2.5);
        if (s.hp <= 0) hurtPlayer(0);
        syncBars();
      }

      // Boss swings.
      if (!s.bossDead && s.t >= s.nextBossHit) {
        s.nextBossHit = s.t + rnd(1400, 2400);
        hurtPlayer(Math.round(rnd(50, 150)));
        syncBars();
        dirty = true;
      }
      if (!s.bossDead && s.t >= s.nextPoison) {
        s.nextPoison = s.t + rnd(12000, 18000);
        addBuff('poison', 6000);
        dirty = true;
      }
      if (s.bossDead && s.t >= s.bossDead) {
        s.bossDead = 0;
        s.boss = BOSS_MAX;
        s.braziers = 0;
        boss.fill.setValue(0);
        boss.ghost.setValue(0);
        boss.set(1);
        dirty = true;
      }
      // Braziers tick up as the fight goes on.
      if (tickSecond && !s.bossDead && sec(s.t) % 9 === 0 && s.braziers < 4) {
        LayoutAnimation.configureNext(LayoutAnimation.create(260, 'easeInEaseOut', 'opacity'));
        s.braziers += 1;
        dirty = true;
      }

      // Kill feed.
      if (s.t >= s.nextKill) {
        s.nextKill = s.t + rnd(2600, 4800);
        const a = NAMES[Math.floor(rnd(0, NAMES.length))];
        let b = NAMES[Math.floor(rnd(0, NAMES.length))];
        if (b === a) b = 'Mireille';
        LayoutAnimation.configureNext(LayoutAnimation.create(260, 'easeInEaseOut', 'opacity'));
        s.feed = s.feed.concat({ id: ++s.feedId, a, b, w: WEAPONS[Math.floor(rnd(0, WEAPONS.length))], ally: Math.random() < 0.55, head: Math.random() < 0.3, at: s.t }).slice(-4);
        dirty = true;
      }
      const feed = s.feed.filter((e) => s.t - e.at < 9000);
      if (feed.length !== s.feed.length) {
        LayoutAnimation.configureNext(LayoutAnimation.create(260, 'easeInEaseOut', 'opacity'));
        s.feed = feed;
        dirty = true;
      }

      // Reload finished.
      if (s.reloadUntil && s.t >= s.reloadUntil) {
        const take = Math.min(30 - s.ammo, s.reserve);
        s.ammo += take;
        s.reserve -= take;
        s.reloadUntil = 0;
        dirty = true;
      }

      // Demo mode: after 6 s without input the HUD plays itself.
      if (s.t - s.lastInput > 6000 && s.t >= s.nextAi && !s.bossDead) {
        s.nextAi = s.t + rnd(900, 1700);
        if (s.hp < HP_MAX * 0.38 && s.cds[4].end <= s.t && s.potions > 0) useAbility(4, 'ai');
        else if (s.ult >= 1 && Math.random() < 0.5) useAbility(5, 'ai');
        else {
          const ready = [0, 1, 2, 3].filter((k) => s.cds[k].end <= s.t && s.mp >= ABILITIES[k].cost && (k !== 1 || !s.buffs.some((b) => b.id === 'ward')));
          if (ready.length && Math.random() < 0.75) useAbility(ready[Math.floor(rnd(0, ready.length))], 'ai');
          else fire('ai');
        }
        if (s.potions <= 0) s.potions = 5;
      }
      if (dirty) bump();
    }, STEP);
    return () => {
      clearInterval(id);
      loops.forEach((l) => l.stop());
      sweeps.forEach((v) => v.stopAnimation());
    };
  }, [active]);

  const onKeyDown = (e) => {
    const k = (e.key || '').toLowerCase();
    if (k in KEY_TO_SLOT) {
      setPadMode(false);
      useAbility(KEY_TO_SLOT[k], 'key');
      e.preventDefault();
    } else if (k === 'f' || k === ' ' || k === 'enter') {
      setPadMode(false);
      fire('key');
      e.preventDefault();
    } else if (k === 't') {
      startReload();
      bump();
    }
  };
  const onGamepadButton = (e) => {
    if (!e.pressed) return;
    const slot = ABILITIES.findIndex((a) => a.padBtn === e.buttonIndex);
    if (slot >= 0) {
      setPadMode(true);
      useAbility(slot, 'pad');
      e.preventDefault();
    } else if (e.buttonIndex === 7) {
      setPadMode(true);
      fire('pad');
      e.preventDefault();
    }
  };

  const s = g.current;
  const hpPct = s.hp / HP_MAX;
  const bossPct = Math.ceil((s.boss / BOSS_MAX) * 100);
  const phase = bossPct > 66 ? 'Phase I' : bossPct > 33 ? 'Phase II' : 'Phase III';
  const reloading = s.reloadUntil > s.t;

  return (
    <Pressable
      style={styles.root}
      autoFocus={active}
      onPressIn={() => {
        setPadMode(false);
        fire('mouse');
      }}
      onKeyDown={onKeyDown}
      onGamepadButton={onGamepadButton}
    >
      <Animated.View style={[StyleSheet.absoluteFill, { transform: [{ translateX: shakeX }, { scale: 1.02 }] }]}>
        <Image source={IMG.bg} style={{ width: 1280, height: 720 }} resizeMode="cover" />
      </Animated.View>
      <View pointerEvents="none" style={styles.vignette} />
      <Animated.View pointerEvents="none" style={[styles.hurt, { opacity: hurt }]} />
      <Animated.View pointerEvents="none" style={[styles.hurt, { opacity: hpPct < 0.35 ? low.interpolate({ inputRange: [0, 1], outputRange: [0.25, 0.7] }) : 0 }]} />
      <Animated.View pointerEvents="none" style={[styles.ultFlash, { opacity: ultFlash }]} />

      {/* Player frame */}
      <View style={styles.player}>
        <View style={styles.portraitRing}>
          <Image source={IMG.portrait} style={styles.portrait} />
          <Animated.View style={[styles.levelBadge, { transform: [{ scale: lvl }] }]}>
            <Text style={styles.levelText}>{s.level}</Text>
          </Animated.View>
        </View>
        <View style={styles.playerBars}>
          <View style={styles.nameRow}>
            <Text style={styles.playerName}>Seren Ashvale</Text>
            <Text style={styles.playerClass}>Stormcaller</Text>
          </View>
          <Bar
            bar={hp}
            height={18}
            ticks={3}
            label={`${fmt(Math.max(0, s.hp))} / ${fmt(HP_MAX)}`}
            fill="linear-gradient(180deg, #ff7a8c 0%, #e0344f 55%, #a3132c 100%)"
            ghostColor={s.buffs.some((b) => b.id === 'regen') ? 'rgba(124,240,164,0.6)' : 'rgba(255,255,255,0.5)'}
          />
          <View style={{ height: 5 }} />
          <Bar bar={mp} height={12} label={`${fmt(s.mp)} / ${fmt(MP_MAX)}`} fill="linear-gradient(180deg, #7cc4ff 0%, #2f7bf0 60%, #1a4fb0 100%)" />
          <View style={{ height: 5 }} />
          <Bar bar={en} height={5} fill="linear-gradient(90deg, #ffd36b, #f2a93b)" />
          <View style={styles.buffs}>
            {s.buffs.map((b) => (
              <Buff key={b.id} id={b.id} secs={Math.max(0, Math.ceil((b.end - s.t) / 1000))} stacks={b.stacks} blink={blink} />
            ))}
          </View>
        </View>
      </View>

      {/* Kill feed */}
      <View style={styles.feed} pointerEvents="none">
        {s.feed.map((e) => (
          <KillEntry key={e.id} e={e} />
        ))}
      </View>

      {/* Boss */}
      <View style={styles.boss} pointerEvents="none">
        <View style={styles.bossHeader}>
          <View style={styles.skullDot} />
          <Text style={styles.bossName}>Vharos, the Cinder Wyrm</Text>
          <Text style={styles.bossPhase}>{s.bossDead ? 'Defeated' : phase}</Text>
        </View>
        <View style={styles.bossTrack}>
          <Animated.View style={[styles.bossGhost, { transform: [{ scaleX: boss.ghost }] }]} />
          <Animated.View style={[styles.bossFill, { transform: [{ scaleX: boss.fill }] }]} />
          <View style={[styles.phaseMark, { left: '66%' }]} />
          <View style={[styles.phaseMark, { left: '33%' }]} />
        </View>
        <Text style={styles.bossPct}>{s.bossDead ? 'Respawning…' : `${bossPct}%`}</Text>
      </View>

      <Minimap active={active} />

      {/* Quest tracker */}
      <View style={styles.quests} pointerEvents="none">
        <Text style={styles.questKicker}>Main Quest</Text>
        <Text style={styles.questTitle}>Embers of the Old King</Text>
        <Objective done text="Reach the Cinder Gate" />
        <Objective done={s.braziers >= 4} text="Extinguish the braziers" count={`${s.braziers}/4`} />
        <Objective done={!!s.bossDead} text="Defeat Vharos before the tide of flame consumes the bridge" />
      </View>

      {/* Crosshair */}
      <Animated.View
        pointerEvents="none"
        style={[styles.crosshair, { transform: [{ scale: spread.interpolate({ inputRange: [0, 1], outputRange: [1, 1.7] }) }] }]}
      >
        <View style={[styles.chLine, { width: 2, height: 10, left: 13, top: 0 }]} />
        <View style={[styles.chLine, { width: 2, height: 10, left: 13, bottom: 0 }]} />
        <View style={[styles.chLine, { width: 10, height: 2, top: 13, left: 0 }]} />
        <View style={[styles.chLine, { width: 10, height: 2, top: 13, right: 0 }]} />
        <View style={styles.chDot} />
      </Animated.View>
      <Animated.View pointerEvents="none" style={[styles.hitMarker, { opacity: hit, transform: [{ rotate: '45deg' }, { scale: hit.interpolate({ inputRange: [0, 1], outputRange: [0.8, 1.15] }) }] }]}>
        <View style={[styles.hmLine, { left: 0, top: 13, width: 8, height: 2 }]} />
        <View style={[styles.hmLine, { right: 0, top: 13, width: 8, height: 2 }]} />
        <View style={[styles.hmLine, { top: 0, left: 13, width: 2, height: 8 }]} />
        <View style={[styles.hmLine, { bottom: 0, left: 13, width: 2, height: 8 }]} />
      </Animated.View>

      {/* Floating combat text */}
      {floats.map((f) => (
        <FloatText key={f.id} item={f} onDone={removeFloat} />
      ))}

      {/* Error line + banner */}
      <Animated.View pointerEvents="none" style={[styles.msg, { opacity: msgT }]}>
        <Text style={styles.msgText}>{msg}</Text>
      </Animated.View>
      {banner ? (
        <Animated.View
          pointerEvents="none"
          style={[
            styles.banner,
            { opacity: bannerT, transform: [{ scale: bannerT.interpolate({ inputRange: [0, 1], outputRange: [0.85, 1] }) }] },
          ]}
        >
          <Text style={[styles.bannerKicker, { color: banner.color }]}>{banner.kicker}</Text>
          <Text style={styles.bannerTitle}>{banner.title}</Text>
          <View style={[styles.bannerRule, { backgroundColor: banner.color }]} />
        </Animated.View>
      ) : null}

      {/* Ability bar */}
      <View style={styles.abilities}>
        {ABILITIES.map((a, i) => {
          const cd = s.cds[i];
          const rem = cd.end - s.t;
          const ready = rem <= 0;
          return (
            <AbilitySlot
              key={a.key}
              a={a}
              index={i}
              sweep={sweeps[i]}
              secs={!a.ultimate && rem > 0 ? String(Math.ceil(rem / 1000)) : null}
              ready={ready}
              usable={ready && s.mp >= a.cost}
              count={a.potion ? s.potions : null}
              charge={a.ultimate ? s.ult : 0}
              padMode={padMode}
              onUse={useAbility}
              fx={fx[i]}
            />
          );
        })}
      </View>
      <View style={styles.xp} pointerEvents="none">
        <Animated.View style={[styles.xpFill, { transform: [{ scaleX: xp.fill }] }]} />
      </View>

      {/* Ammo */}
      <View style={styles.ammo} pointerEvents="none">
        <Text style={styles.weapon}>Thornspitter Mk II</Text>
        <View style={styles.ammoRow}>
          <Text style={[styles.ammoMag, s.ammo <= 6 && { color: '#ff8a9a' }]}>{s.ammo}</Text>
          <Text style={styles.ammoReserve}>/ {s.reserve}</Text>
        </View>
        <View style={styles.rounds}>
          {Array.from({ length: 30 }, (_, i) => (
            <View key={i} style={[styles.round, i >= s.ammo && styles.roundSpent]} />
          ))}
          {reloading ? <Animated.View style={[styles.reloadFill, { transform: [{ scaleX: reload }] }]} /> : null}
        </View>
        <Text style={styles.fireMode}>{reloading ? 'Reloading…' : padMode ? 'Burst · RT to fire' : 'Burst · Click to fire'}</Text>
      </View>
    </Pressable>
  );
}

const Objective = memo(function Objective({ done, text, count }) {
  return (
    <View style={styles.objective}>
      <View style={[styles.check, done && styles.checkDone]} />
      <Text style={[styles.objText, done && styles.objDone]} numberOfLines={1}>
        {text}
      </Text>
      {count && !done ? <Text style={styles.objCount}>{count}</Text> : null}
    </View>
  );
});

const textShadow = { textShadowColor: 'rgba(0,0,0,0.85)', textShadowOffset: { width: 0, height: 1 }, textShadowRadius: 3 };

const styles = StyleSheet.create({
  root: { width: 1280, height: 720, overflow: 'hidden', backgroundColor: '#0c1a20', cursor: 'crosshair' },
  vignette: {
    ...StyleSheet.absoluteFillObject,
    experimental_backgroundImage: 'radial-gradient(ellipse at center, rgba(0,0,0,0) 45%, rgba(0,0,0,0.65) 100%)',
  },
  hurt: {
    ...StyleSheet.absoluteFillObject,
    experimental_backgroundImage: 'radial-gradient(ellipse at center, rgba(160,0,20,0) 50%, rgba(200,10,40,0.55) 100%)',
  },
  ultFlash: {
    ...StyleSheet.absoluteFillObject,
    experimental_backgroundImage: 'radial-gradient(circle at 50% 50%, rgba(214,184,255,0.75) 0%, rgba(138,77,255,0.35) 40%, rgba(40,10,80,0) 80%)',
  },

  player: { position: 'absolute', left: 28, top: 26, flexDirection: 'row', alignItems: 'flex-start', gap: 14 },
  portraitRing: {
    width: 84,
    height: 84,
    borderRadius: 42,
    padding: 3,
    experimental_backgroundImage: 'linear-gradient(160deg, #ffe29a 0%, #c9902d 50%, #6e4512 100%)',
    boxShadow: '0 6px 18px rgba(0,0,0,0.6), 0 0 18px rgba(242,195,91,0.35)',
  },
  portrait: { width: 78, height: 78, borderRadius: 39 },
  levelBadge: {
    position: 'absolute',
    right: -4,
    bottom: -2,
    minWidth: 30,
    height: 24,
    paddingHorizontal: 6,
    borderRadius: 12,
    backgroundColor: '#1a1206',
    borderWidth: 2,
    borderColor: colors.gold,
    alignItems: 'center',
    justifyContent: 'center',
  },
  levelText: { fontFamily: font, fontWeight: '700', fontSize: 12, color: colors.goldLight },
  playerBars: { width: 300, paddingTop: 4 },
  nameRow: { flexDirection: 'row', alignItems: 'baseline', gap: 8, marginBottom: 6 },
  playerName: { fontFamily: font, fontWeight: '700', fontSize: 16, color: colors.text, ...textShadow },
  playerClass: { fontFamily: font, fontWeight: '500', fontSize: 12, color: colors.cyan, ...textShadow },

  barTrack: {
    borderRadius: 4,
    overflow: 'hidden',
    backgroundColor: 'rgba(6,8,14,0.78)',
    borderWidth: 1,
    borderColor: 'rgba(0,0,0,0.9)',
    boxShadow: '0 2px 6px rgba(0,0,0,0.5)',
  },
  barFill: { position: 'absolute', left: 0, top: 0, bottom: 0, width: '100%', transformOrigin: 'left' },
  barSheen: { position: 'absolute', left: 0, right: 0, top: 0, height: '45%', backgroundColor: 'rgba(255,255,255,0.18)' },
  tick: { position: 'absolute', top: 0, bottom: 0, width: 1, backgroundColor: 'rgba(0,0,0,0.45)' },
  barLabelWrap: { ...StyleSheet.absoluteFillObject, alignItems: 'center', justifyContent: 'center' },
  barLabel: { fontFamily: font, fontWeight: '700', fontSize: 10, color: '#fff', letterSpacing: 0.5, ...textShadow },

  buffs: { flexDirection: 'row', gap: 6, marginTop: 10, height: 46 },
  buff: { width: 34, alignItems: 'center' },
  buffIcon: { width: 30, height: 30, borderRadius: 7, borderWidth: 1, borderColor: 'rgba(255,255,255,0.35)' },
  debuffIcon: { borderColor: 'rgba(255,107,129,0.8)' },
  stack: {
    position: 'absolute',
    right: -2,
    top: -4,
    width: 15,
    height: 15,
    borderRadius: 8,
    backgroundColor: '#10162a',
    borderWidth: 1,
    borderColor: colors.cyan,
    alignItems: 'center',
    justifyContent: 'center',
  },
  stackText: { fontFamily: font, fontWeight: '700', fontSize: 9, color: colors.cyan },
  buffTime: { fontFamily: font, fontWeight: '600', fontSize: 10, color: colors.text, marginTop: 2, ...textShadow },

  feed: { position: 'absolute', left: 28, top: 190, width: 330, gap: 6 },
  kill: {
    flexDirection: 'row',
    alignItems: 'center',
    alignSelf: 'flex-start',
    gap: 8,
    height: 28,
    paddingHorizontal: 10,
    borderRadius: 6,
    backgroundColor: 'rgba(6,10,20,0.6)',
    borderLeftWidth: 2,
    borderLeftColor: 'rgba(255,255,255,0.25)',
  },
  killMe: { borderLeftColor: colors.gold, backgroundColor: 'rgba(60,40,8,0.7)' },
  killName: { fontFamily: font, fontWeight: '700', fontSize: 12, ...textShadow },
  killIcon: { width: 18, height: 18 },
  headshot: { width: 6, height: 6, borderRadius: 3, backgroundColor: colors.red, marginLeft: -4 },

  boss: { position: 'absolute', top: 26, left: 450, width: 380, alignItems: 'center' },
  bossHeader: { flexDirection: 'row', alignItems: 'center', gap: 8, marginBottom: 6 },
  skullDot: { width: 8, height: 8, borderRadius: 2, backgroundColor: colors.red, transform: [{ rotate: '45deg' }], boxShadow: '0 0 8px rgba(255,77,106,0.9)' },
  bossName: { fontFamily: font, fontWeight: '700', fontSize: 15, letterSpacing: 1.2, textTransform: 'uppercase', color: '#ffe3d6', ...textShadow },
  bossPhase: { fontFamily: font, fontWeight: '600', fontSize: 11, color: colors.orange, ...textShadow },
  bossTrack: {
    width: '100%',
    height: 12,
    borderRadius: 2,
    backgroundColor: 'rgba(10,4,6,0.85)',
    borderWidth: 1,
    borderColor: 'rgba(255,140,90,0.45)',
    overflow: 'hidden',
    boxShadow: '0 0 16px rgba(255,90,60,0.35)',
  },
  bossGhost: { position: 'absolute', left: 0, top: 0, bottom: 0, width: '100%', transformOrigin: 'left', backgroundColor: 'rgba(255,220,190,0.75)' },
  bossFill: {
    position: 'absolute',
    left: 0,
    top: 0,
    bottom: 0,
    width: '100%',
    transformOrigin: 'left',
    experimental_backgroundImage: 'linear-gradient(90deg, #7a0f1e 0%, #e0344f 70%, #ff8a5c 100%)',
  },
  phaseMark: { position: 'absolute', top: 0, bottom: 0, width: 2, backgroundColor: 'rgba(255,230,200,0.7)' },
  bossPct: { fontFamily: font, fontWeight: '600', fontSize: 11, color: colors.textDim, marginTop: 4, ...textShadow },

  mapWrap: { position: 'absolute', right: 28, top: 22, alignItems: 'center' },
  mapFrame: {
    width: 188,
    height: 188,
    borderRadius: 94,
    padding: 5,
    experimental_backgroundImage: 'linear-gradient(180deg, #3a4462 0%, #151b2e 100%)',
    boxShadow: '0 10px 30px rgba(0,0,0,0.6), inset 0 1px 0 rgba(255,255,255,0.15)',
  },
  mapClip: { flex: 1, borderRadius: 89, overflow: 'hidden', borderWidth: 2, borderColor: 'rgba(0,0,0,0.7)' },
  mapImage: { position: 'absolute', left: -40, top: -30, width: 256, height: 256, opacity: 0.92 },
  fovPivot: { position: 'absolute', left: 88 - 76, top: 88 - 76, width: 152, height: 152 },
  fov: {
    position: 'absolute',
    left: 49 - 12,
    top: 30 - 12,
    width: 76,
    height: 76,
    borderTopLeftRadius: 76,
    experimental_backgroundImage: 'linear-gradient(135deg, rgba(255,255,255,0.28), rgba(255,255,255,0))',
    transform: [{ rotate: '45deg' }],
  },
  playerArrow: {
    position: 'absolute',
    left: 82,
    top: 82,
    width: 12,
    height: 12,
    backgroundColor: '#fff',
    borderTopLeftRadius: 2,
    boxShadow: '0 0 6px rgba(255,255,255,0.9)',
  },
  ping: { position: 'absolute', width: 9, height: 9, borderRadius: 5, borderWidth: 2, borderColor: 'rgba(0,0,0,0.6)' },
  pingRing: { position: 'absolute', width: 21, height: 21, borderRadius: 11, borderWidth: 2 },
  north: {
    position: 'absolute',
    top: -4,
    left: 82,
    width: 24,
    height: 24,
    borderRadius: 12,
    backgroundColor: '#151b2e',
    borderWidth: 1,
    borderColor: colors.gold,
    alignItems: 'center',
    justifyContent: 'center',
  },
  northText: { fontFamily: font, fontWeight: '700', fontSize: 11, color: colors.gold },
  zone: { fontFamily: font, fontWeight: '700', fontSize: 14, color: colors.text, marginTop: 8, letterSpacing: 1, textTransform: 'uppercase', ...textShadow },
  zoneSub: { fontFamily: font, fontWeight: '500', fontSize: 11, color: colors.textDim, marginTop: 2, ...textShadow },

  quests: {
    position: 'absolute',
    right: 28,
    top: 270,
    width: 250,
    paddingVertical: 12,
    paddingHorizontal: 14,
    borderLeftWidth: 3,
    borderLeftColor: colors.gold,
    experimental_backgroundImage: 'linear-gradient(90deg, rgba(8,12,24,0.75) 0%, rgba(8,12,24,0.2) 100%)',
    borderTopRightRadius: 8,
    borderBottomRightRadius: 8,
  },
  questKicker: { fontFamily: font, fontWeight: '700', fontSize: 10, letterSpacing: 1.6, textTransform: 'uppercase', color: colors.gold },
  questTitle: { fontFamily: font, fontWeight: '700', fontSize: 15, color: colors.text, marginTop: 3, marginBottom: 8, ...textShadow },
  objective: { flexDirection: 'row', alignItems: 'center', gap: 8, marginTop: 5 },
  check: { width: 10, height: 10, borderRadius: 2, borderWidth: 1.5, borderColor: colors.textDim },
  checkDone: { backgroundColor: colors.green, borderColor: colors.green },
  objText: { flex: 1, fontFamily: font, fontWeight: '500', fontSize: 12, color: colors.text, ...textShadow },
  objDone: { color: colors.textMute, textDecorationLine: 'line-through' },
  objCount: { fontFamily: font, fontWeight: '700', fontSize: 12, color: colors.gold },

  crosshair: { position: 'absolute', left: 626, top: 346, width: 28, height: 28 },
  chLine: { position: 'absolute', backgroundColor: 'rgba(255,255,255,0.9)', borderRadius: 1, boxShadow: '0 0 3px rgba(0,0,0,0.8)' },
  chDot: { position: 'absolute', left: 12, top: 12, width: 4, height: 4, borderRadius: 2, backgroundColor: colors.red },
  hitMarker: { position: 'absolute', left: 626, top: 346, width: 28, height: 28 },
  hmLine: { position: 'absolute', backgroundColor: '#fff', boxShadow: '0 0 4px rgba(255,80,80,0.9)' },
  floatText: { position: 'absolute', fontFamily: font, fontWeight: '700', letterSpacing: -0.5, ...textShadow, textShadowRadius: 4 },

  msg: { position: 'absolute', left: 340, right: 340, bottom: 128, alignItems: 'center' },
  msgText: { fontFamily: font, fontWeight: '700', fontSize: 15, color: '#ff6b81', letterSpacing: 0.5, ...textShadow },
  banner: { position: 'absolute', left: 340, right: 340, top: 160, alignItems: 'center' },
  bannerKicker: { fontFamily: font, fontWeight: '700', fontSize: 13, letterSpacing: 4, textTransform: 'uppercase', ...textShadow },
  bannerTitle: { fontFamily: font, fontWeight: '700', fontSize: 34, color: '#fff6dc', marginTop: 4, letterSpacing: 1, ...textShadow, textShadowRadius: 12 },
  bannerRule: { width: 160, height: 2, marginTop: 10, borderRadius: 1, opacity: 0.8 },

  abilities: { position: 'absolute', bottom: 34, left: 0, right: 0, flexDirection: 'row', justifyContent: 'center', alignItems: 'flex-end', gap: 10 },
  slot: {
    width: 58,
    height: 58,
    borderRadius: 10,
    backgroundColor: 'rgba(10,14,26,0.85)',
    borderWidth: 2,
    borderColor: 'rgba(148,170,255,0.25)',
    overflow: 'hidden',
    alignItems: 'center',
    justifyContent: 'center',
    boxShadow: '0 6px 14px rgba(0,0,0,0.55)',
  },
  slotHover: { borderColor: 'rgba(200,215,255,0.6)' },
  slotReady: { borderColor: colors.goldLight, boxShadow: '0 0 0 2px rgba(242,195,91,0.35), 0 0 18px rgba(242,195,91,0.6)' },
  slotUlt: {
    width: 66,
    height: 66,
    marginBottom: -4,
    borderRadius: 33,
    borderColor: colors.purple,
    experimental_backgroundImage: 'radial-gradient(circle at 50% 40%, rgba(178,107,255,0.25) 0%, rgba(30,12,60,0.95) 70%)',
    boxShadow: '0 0 12px rgba(178,107,255,0.45)',
  },
  slotUltReady: {
    borderColor: '#e2c8ff',
    experimental_backgroundImage: 'radial-gradient(circle at 50% 40%, rgba(178,107,255,0.55) 0%, rgba(30,12,60,0.95) 70%)',
    boxShadow: '0 0 22px rgba(178,107,255,0.8), 0 0 0 2px rgba(178,107,255,0.35)',
  },
  ultFill: { position: 'absolute', left: 0, right: 0, top: 0, height: 66, backgroundColor: 'rgba(178,107,255,0.35)' },
  ultPct: { position: 'absolute', bottom: 6, fontFamily: font, fontWeight: '700', fontSize: 10, color: '#e2c8ff', ...textShadow },
  procGlow: { borderRadius: 8, boxShadow: 'inset 0 0 14px rgba(255,226,154,0.9)' },
  slotIcon: { width: 42, height: 42 },
  sweepPiece: { position: 'absolute', backgroundColor: 'rgba(2,4,10,0.8)' },
  sweepEdge: { position: 'absolute', width: 2, backgroundColor: 'rgba(255,236,190,0.85)', boxShadow: '0 0 6px rgba(255,210,120,0.9)' },
  cdTextWrap: { ...StyleSheet.absoluteFillObject, alignItems: 'center', justifyContent: 'center' },
  cdText: { fontFamily: font, fontWeight: '700', fontSize: 18, color: '#fff', ...textShadow },
  count: { position: 'absolute', right: 5, bottom: 2, fontFamily: font, fontWeight: '700', fontSize: 12, color: '#fff', ...textShadow },
  keycap: {
    position: 'absolute',
    left: 3,
    top: 3,
    minWidth: 16,
    height: 16,
    borderRadius: 4,
    paddingHorizontal: 3,
    backgroundColor: 'rgba(0,0,0,0.7)',
    alignItems: 'center',
    justifyContent: 'center',
  },
  keycapPad: { borderRadius: 8 },
  keycapUlt: { left: 8, top: 6 },
  keyText: { fontFamily: font, fontWeight: '700', fontSize: 10, color: colors.textDim },
  xp: { position: 'absolute', left: 390, right: 390, bottom: 18, height: 4, borderRadius: 2, backgroundColor: 'rgba(0,0,0,0.6)', overflow: 'hidden' },
  xpFill: { height: '100%', width: '100%', transformOrigin: 'left', experimental_backgroundImage: 'linear-gradient(90deg, #8a4dff, #c39bff)' },

  ammo: { position: 'absolute', right: 36, bottom: 30, alignItems: 'flex-end' },
  weapon: { fontFamily: font, fontWeight: '600', fontSize: 12, letterSpacing: 1.4, textTransform: 'uppercase', color: colors.textDim, ...textShadow },
  ammoRow: { flexDirection: 'row', alignItems: 'baseline', gap: 6, marginTop: 2 },
  ammoMag: { fontFamily: font, fontWeight: '700', fontSize: 52, color: '#fff', letterSpacing: -1, ...textShadow },
  ammoReserve: { fontFamily: font, fontWeight: '600', fontSize: 20, color: colors.textDim, ...textShadow },
  rounds: { flexDirection: 'row', gap: 2, marginTop: 2 },
  round: { width: 4, height: 12, borderRadius: 1, backgroundColor: colors.goldLight },
  roundSpent: { backgroundColor: 'rgba(255,255,255,0.18)' },
  reloadFill: { position: 'absolute', left: 0, right: 0, top: 0, bottom: 0, transformOrigin: 'left', backgroundColor: 'rgba(255,226,154,0.55)', borderRadius: 1 },
  fireMode: { fontFamily: font, fontWeight: '500', fontSize: 11, color: colors.textMute, marginTop: 6, ...textShadow },
});
