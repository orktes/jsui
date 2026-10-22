// Notifications (interactive). Feature spotlight: LayoutAnimation, timers
// and host messages.
//
// Toasts, achievements, loot drops, party invites and system warnings spawn
// on their own and on demand (dock buttons or keys 1-6), stack with
// LayoutAnimation, auto-dismiss on a visible timer that pauses while hovered,
// and can be dismissed, accepted or declined. Every toast is reported to the
// embedding page with Host.postMessage, and the page can push its own with
// jsui.postMessage('notify', {...}) (useHostMessage); the bridge log shows
// both directions. A kill feed, compass and "area discovered" banners keep
// the scene moving.
import React, { memo, useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { Animated, Easing, Host, Image, ImageBackground, LayoutAnimation, Pressable, StyleSheet, Text, View, useHostMessage } from 'react-native';

export const feature = {
  title: 'LayoutAnimation, timers & host messages',
  points: [
    'New toasts push the stack down and dismissed ones close the gap with LayoutAnimation (create / update / delete)',
    'Each toast has a native-driver countdown bar that pauses while you hover or focus it',
    'Every toast is sent to the embedding page with Host.postMessage; the page pushes its own with jsui.postMessage(\'notify\', …) (try it in the browser console)',
  ],
  keys: ['1–6 / dock: spawn a notification', 'X · Y: accept · decline the newest invite', "Console: jsui.postMessage('notify', { title: 'Hi' })"],
};
import { colors, font, rarity } from '../../../examples/theme.js';

const A = {
  bg: require('../../../examples/assets/bg-loading.jpg'),
  bow: require('../../../examples/assets/item-bow.png'),
  sword: require('../../../examples/assets/item-sword.png'),
  axe: require('../../../examples/assets/item-axe.png'),
  gem: require('../../../examples/assets/item-gem.png'),
  amulet: require('../../../examples/assets/item-amulet.png'),
  ring: require('../../../examples/assets/item-ring.png'),
  helmet: require('../../../examples/assets/item-helmet.png'),
  shield: require('../../../examples/assets/item-shield.png'),
  scroll: require('../../../examples/assets/item-scroll.png'),
  moon: require('../../../examples/assets/crest-moon.png'),
  wolf: require('../../../examples/assets/crest-wolf.png'),
  flame: require('../../../examples/assets/crest-flame.png'),
  star: require('../../../examples/assets/crest-star.png'),
};

const ACHIEVEMENTS = [
  { title: 'Starfall Cartographer', body: 'Discover every observatory in Ashen Reach.', pts: 50, icon: A.gem },
  { title: 'Not Today', body: 'Survive a hit that would have killed you.', pts: 25, icon: A.shield },
  { title: 'Tide Turner', body: 'Relight 20 drowned lanterns.', pts: 30, icon: A.scroll },
  { title: 'Crowned in Ash', body: 'Defeat the Cinder King without healing.', pts: 100, icon: A.helmet },
];
const LOOT = [
  { title: 'Heart of the Drowned Star', body: 'Amulet · Item level 480', icon: A.amulet, r: 'legendary' },
  { title: 'Gloamstring Recurve', body: 'Bow · Item level 462', icon: A.bow, r: 'epic' },
  { title: 'Band of Quiet Tides', body: 'Ring · Item level 455', icon: A.ring, r: 'rare' },
  { title: 'Emberbrand', body: 'Sword · Item level 486', icon: A.sword, r: 'legendary' },
];
const FRIENDS = [
  { name: 'nightjar', crest: A.moon },
  { name: 'Mireille', crest: A.star },
  { name: 'Okonkwo', crest: A.wolf },
  { name: 'pyrite', crest: A.flame },
];
const SYSTEM = [
  { title: 'Server restart in 5 minutes', body: 'Finish your encounter — progress is saved at the next checkpoint.' },
  { title: 'High latency detected', body: 'Your connection to EU-North is unstable (182 ms).' },
  { title: 'Double XP weekend has started', body: 'All experience gains are doubled until Monday 06:00.' },
];
const AREAS = [
  { title: 'The Drowned Observatory', sub: 'Recommended level 44 – 48' },
  { title: 'Lanternfall Quay', sub: 'Recommended level 40 – 43' },
  { title: 'Hollow of the Choir', sub: 'Recommended level 48 – 50' },
];
const KILLERS = ['Valkyrie_07', 'Mireille', 'Okonkwo', 'nightjar', 'pyrite', 'Ashlight', 'GRIMWALD', 'Thessaly'];
const WEAPONS = [A.bow, A.sword, A.axe];

const DURATION = { achievement: 6500, loot: 6000, invite: 24000, system: 8000, online: 4000 };
const MAX_TOASTS = 5;
const pick = (arr, n) => arr[n % arr.length];

const stackAnim = () =>
  LayoutAnimation.configureNext({
    duration: 320,
    create: { type: 'easeInEaseOut', property: 'opacity', duration: 200 },
    update: { type: 'spring', springDamping: 0.78 },
    delete: { type: 'easeInEaseOut', property: 'opacity', duration: 180 },
  });

// --- toasts ---------------------------------------------------------------------

function useTimer(duration, active, onEnd) {
  const left = useRef(new Animated.Value(1)).current;
  const paused = useRef(false);
  const run = useCallback(() => {
    const rem = left.__getValue();
    Animated.timing(left, { toValue: 0, duration: Math.max(1, rem * duration), easing: Easing.linear, useNativeDriver: true }).start(({ finished }) => finished && onEnd());
  }, [duration, onEnd]);
  useEffect(() => {
    if (!active || paused.current) return undefined;
    run();
    return () => left.stopAnimation();
  }, [active]);
  const pause = () => {
    paused.current = true;
    left.stopAnimation();
  };
  const resume = () => {
    paused.current = false;
    if (active) run();
  };
  return { left, pause, resume };
}

const Toast = memo(function Toast({ t, active, onRemove, onAccept }) {
  const enter = useRef(new Animated.Value(0)).current;
  const glow = useRef(new Animated.Value(0)).current;
  const shine = useRef(new Animated.Value(0)).current;
  const leaving = useRef(false);
  const [state, setState] = useState('open'); // open | accepted | declined
  const [secs, setSecs] = useState(Math.round(DURATION.invite / 1000));

  const dismiss = useCallback(() => {
    if (leaving.current) return;
    leaving.current = true;
    Animated.timing(enter, { toValue: 0, duration: 240, easing: Easing.in(Easing.cubic), useNativeDriver: true }).start(() => onRemove(t.id));
  }, [t.id]);
  const timer = useTimer(DURATION[t.kind], active && state === 'open', dismiss);

  useEffect(() => {
    Animated.spring(enter, { toValue: 1, stiffness: 220, damping: 20, mass: 0.9, useNativeDriver: true }).start();
    if (t.kind === 'achievement') Animated.timing(shine, { toValue: 1, duration: 1100, delay: 250, easing: Easing.inOut(Easing.quad), useNativeDriver: true }).start();
  }, []);
  useEffect(() => {
    if (t.kind !== 'loot' || t.data.r !== 'legendary' || !active) return undefined;
    const l = Animated.loop(Animated.sequence([
      Animated.timing(glow, { toValue: 1, duration: 800, easing: Easing.inOut(Easing.sin), useNativeDriver: true }),
      Animated.timing(glow, { toValue: 0, duration: 800, easing: Easing.inOut(Easing.sin), useNativeDriver: true }),
    ]));
    l.start();
    return () => l.stop();
  }, [active]);
  // Invite countdown (text only; the bar is native).
  useEffect(() => {
    if (t.kind !== 'invite' || state !== 'open' || !active) return undefined;
    const id = setInterval(() => setSecs(Math.max(0, Math.ceil(timer.left.__getValue() * DURATION.invite / 1000))), 500);
    return () => clearInterval(id);
  }, [state, active]);
  useEffect(() => {
    if (t.respond) respond(t.respond);
  }, [t.respond]);

  const respond = (ok) => {
    if (state !== 'open') return;
    timer.pause();
    stackAnim();
    setState(ok ? 'accepted' : 'declined');
    if (ok) onAccept(t.data.name);
    setTimeout(dismiss, ok ? 1800 : 700);
  };

  const accent = { achievement: colors.gold, loot: t.data.r && rarity[t.data.r].color, invite: colors.cyan, system: t.data.host ? colors.cyan : colors.orange, online: colors.green }[t.kind];
  const anim = {
    opacity: enter,
    transform: [{ translateX: enter.interpolate({ inputRange: [0, 1], outputRange: [420, 0] }) }, { scale: enter.interpolate({ inputRange: [0, 1], outputRange: [0.92, 1] }) }],
  };
  const bar = (
    <View style={styles.timerTrack} pointerEvents="none">
      <Animated.View style={[styles.timerFill, { backgroundColor: accent, transform: [{ scaleX: timer.left }] }]} />
    </View>
  );

  if (t.kind === 'online') {
    return (
      <Animated.View style={[{ alignSelf: 'flex-end' }, anim]}>
        <Pressable onPress={dismiss} onHoverIn={timer.pause} onHoverOut={timer.resume} style={({ hovered, focused }) => [styles.mini, (hovered || focused) && styles.hot]}>
          <View style={styles.onlineDot} />
          <Image source={t.data.crest} style={styles.miniAvatar} />
          <Text style={styles.miniText}>
            <Text style={{ fontWeight: '700', color: colors.text }}>{t.data.name}</Text> is now online
          </Text>
        </Pressable>
      </Animated.View>
    );
  }

  const body =
    t.kind === 'achievement' ? (
      <>
        <View style={styles.trophy}>
          <Image source={t.data.icon} style={{ width: 40, height: 40 }} />
        </View>
        <View style={{ flex: 1 }}>
          <Text style={[styles.kicker, { color: colors.gold }]}>Achievement Unlocked</Text>
          <Text style={styles.toastTitle}>{t.data.title}</Text>
          <Text style={styles.toastBody}>{t.data.body}</Text>
        </View>
        <Text style={styles.points}>+{t.data.pts}</Text>
      </>
    ) : t.kind === 'loot' ? (
      <>
        <View style={[styles.lootIcon, { borderColor: accent, experimental_backgroundImage: `radial-gradient(circle at 50% 40%, ${rarity[t.data.r].glow} 0%, rgba(10,10,20,0.9) 75%)` }]}>
          <Image source={t.data.icon} style={{ width: 40, height: 40 }} />
        </View>
        <View style={{ flex: 1 }}>
          <Text style={[styles.kicker, { color: accent }]}>{rarity[t.data.r].label} Drop</Text>
          <Text style={[styles.toastTitle, { color: accent }]} numberOfLines={1}>{t.data.title}</Text>
          <Text style={styles.toastBody}>{t.data.body}</Text>
        </View>
      </>
    ) : t.kind === 'invite' ? (
      <>
        <Image source={t.data.crest} style={styles.avatar} />
        <View style={{ flex: 1 }}>
          {state === 'open' ? (
            <>
              <Text style={styles.toastTitle}>
                <Text style={{ color: colors.cyan }}>{t.data.name}</Text> invited you to a party
              </Text>
              <View style={styles.actions}>
                <Pressable onPress={() => respond(true)} style={({ hovered, focused, pressed }) => [styles.action, styles.accept, (hovered || focused) && styles.acceptHot, pressed && { transform: [{ scale: 0.95 }] }]}>
                  <Text style={styles.acceptText}>Accept</Text>
                  <Text style={styles.actionGlyph}>X</Text>
                </Pressable>
                <Pressable onPress={() => respond(false)} style={({ hovered, focused, pressed }) => [styles.action, (hovered || focused) && styles.declineHot, pressed && { transform: [{ scale: 0.95 }] }]}>
                  <Text style={styles.declineText}>Decline</Text>
                  <Text style={[styles.actionGlyph, { color: colors.textMute }]}>Y</Text>
                </Pressable>
                <Text style={styles.timeout}>0:{String(secs).padStart(2, '0')}</Text>
              </View>
            </>
          ) : (
            <>
              <Text style={[styles.kicker, { color: state === 'accepted' ? colors.green : colors.textMute }]}>{state === 'accepted' ? 'Party joined' : 'Invite declined'}</Text>
              <Text style={styles.toastTitle}>{state === 'accepted' ? `You joined ${t.data.name}'s party` : `Maybe next time, ${t.data.name}`}</Text>
            </>
          )}
        </View>
      </>
    ) : (
      <>
        <View style={[styles.warnIcon, t.data.host && { backgroundColor: colors.cyan }]}>
          <Text style={styles.warnGlyph}>{t.data.host ? '⇄' : '!'}</Text>
        </View>
        <View style={{ flex: 1 }}>
          {t.data.host ? <Text style={[styles.kicker, { color: colors.cyan }]}>From the host page</Text> : null}
          <Text style={styles.toastTitle}>{t.data.title}</Text>
          <Text style={styles.toastBody} numberOfLines={1}>{t.data.body}</Text>
        </View>
      </>
    );

  const kindStyle = t.data.host ? styles.hostToast : t.kind === 'achievement' ? styles.achievement : t.kind === 'loot' ? [styles.loot, { borderColor: accent + '88', borderLeftColor: accent }] : t.kind === 'system' ? styles.system : null;
  const inner = (hovered, focused) => (
    <View style={[styles.toast, { borderLeftColor: accent }, kindStyle, state === 'accepted' && styles.accepted, (hovered || focused) && styles.hot]}>
      {t.kind === 'loot' && t.data.r === 'legendary' ? (
        <Animated.View pointerEvents="none" style={[styles.legendGlow, { opacity: glow }]} />
      ) : null}
      {t.kind === 'achievement' ? (
        <View pointerEvents="none" style={styles.shineClip}>
          <Animated.View style={[styles.shine, { transform: [{ translateX: shine.interpolate({ inputRange: [0, 1], outputRange: [-120, 460] }) }, { skewX: '-20deg' }] }]} />
        </View>
      ) : null}
      {body}
      {state === 'open' ? bar : null}
    </View>
  );

  return (
    <Animated.View style={anim}>
      {t.kind === 'invite' ? (
        <Pressable focusable={false} onHoverIn={timer.pause} onHoverOut={timer.resume} style={{ cursor: 'default' }}>
          {({ hovered }) => inner(hovered, false)}
        </Pressable>
      ) : (
        <Pressable onPress={dismiss} onHoverIn={timer.pause} onHoverOut={timer.resume} onFocus={timer.pause} onBlur={timer.resume} style={{ cursor: 'pointer' }}>
          {({ hovered, focused }) => inner(hovered, focused)}
        </Pressable>
      )}
    </Animated.View>
  );
});

// --- kill feed ---------------------------------------------------------------------

const FeedRow = memo(function FeedRow({ f, index }) {
  const t = useRef(new Animated.Value(0)).current;
  useEffect(() => {
    Animated.spring(t, { toValue: 1, stiffness: 260, damping: 22, useNativeDriver: true }).start();
  }, []);
  return (
    <Animated.View
      style={[
        styles.feedRow,
        f.mine && styles.feedMine,
        { opacity: Animated.multiply(t, 1 - index * 0.17), transform: [{ translateX: t.interpolate({ inputRange: [0, 1], outputRange: [-40, 0] }) }] },
      ]}
    >
      <Text style={[styles.feedName, { color: '#7fb4ff' }]}>{f.a}</Text>
      <Image source={f.weapon} style={styles.feedIcon} />
      {f.headshot ? <View style={styles.headshot} /> : null}
      <Text style={[styles.feedName, { color: '#ff7f8f' }]}>{f.b}</Text>
    </Animated.View>
  );
});

// --- banner --------------------------------------------------------------------------

function Banner({ b, onDone }) {
  const t = useRef(new Animated.Value(0)).current;
  const rule = useRef(new Animated.Value(0)).current;
  useEffect(() => {
    t.setValue(0);
    rule.setValue(0);
    Animated.sequence([
      Animated.parallel([
        Animated.timing(rule, { toValue: 1, duration: 700, easing: Easing.out(Easing.cubic), useNativeDriver: true }),
        Animated.timing(t, { toValue: 1, duration: 900, delay: 200, easing: Easing.out(Easing.cubic), useNativeDriver: true }),
      ]),
      Animated.delay(2600),
      Animated.parallel([
        Animated.timing(t, { toValue: 0, duration: 600, useNativeDriver: true }),
        Animated.timing(rule, { toValue: 0, duration: 700, useNativeDriver: true }),
      ]),
    ]).start(({ finished }) => finished && onDone());
  }, [b.id]);
  return (
    <View style={styles.banner} pointerEvents="none">
      <Animated.View style={[styles.bannerRule, { opacity: rule, transform: [{ scaleX: rule }] }]}>
        <View style={[styles.rule, { experimental_backgroundImage: 'linear-gradient(90deg, rgba(255,226,154,0), rgba(255,226,154,0.9))' }]} />
        <View style={styles.diamond} />
        <View style={[styles.rule, { experimental_backgroundImage: 'linear-gradient(90deg, rgba(255,226,154,0.9), rgba(255,226,154,0))' }]} />
      </Animated.View>
      <Animated.Text style={[styles.bannerKicker, { opacity: t, letterSpacing: 5 }]}>Area Discovered</Animated.Text>
      <Animated.Text style={[styles.bannerTitle, { opacity: t, transform: [{ translateY: t.interpolate({ inputRange: [0, 1], outputRange: [14, 0] }) }, { scale: t.interpolate({ inputRange: [0, 1], outputRange: [1.06, 1] }) }] }]}>
        {b.title}
      </Animated.Text>
      <Animated.Text style={[styles.bannerSub, { opacity: t }]}>{b.sub}</Animated.Text>
    </View>
  );
}

// --- compass (ambient: the player slowly looks around) -----------------------------------

const Compass = memo(function Compass({ active }) {
  const v = useRef(new Animated.Value(0)).current;
  useEffect(() => {
    if (!active) return undefined;
    const l = Animated.loop(Animated.sequence([
      Animated.timing(v, { toValue: 1, duration: 9000, easing: Easing.inOut(Easing.sin), useNativeDriver: true }),
      Animated.timing(v, { toValue: 0, duration: 9000, easing: Easing.inOut(Easing.sin), useNativeDriver: true }),
    ]));
    l.start();
    return () => l.stop();
  }, [active]);
  const marks = useMemo(() => {
    const out = [];
    const names = { 0: 'N', 90: 'E', 180: 'S', 270: 'W', 45: 'NE', 135: 'SE', 225: 'SW', 315: 'NW' };
    for (let d = -180; d <= 540; d += 15) out.push({ d, label: names[((d % 360) + 360) % 360] });
    return out;
  }, []);
  return (
    <View style={styles.compass} pointerEvents="none">
      <Animated.View style={[styles.compassStrip, { transform: [{ translateX: v.interpolate({ inputRange: [0, 1], outputRange: [-160, -520] }) }] }]}>
        {marks.map((m) => (
          <View key={m.d} style={[styles.compassMark, { left: (m.d + 180) * 2.4 }]}>
            {m.label ? (
              <Text style={[styles.compassText, m.label.length === 1 && styles.compassMain]}>{m.label}</Text>
            ) : (
              <View style={styles.compassTick} />
            )}
          </View>
        ))}
        <View style={[styles.compassPin, { left: (60 + 180) * 2.4 }]} />
      </Animated.View>
      <View style={styles.compassCenter} />
    </View>
  );
});

// --- dock ------------------------------------------------------------------------------

const DOCK = [
  { kind: 'achievement', label: 'Achievement', color: colors.gold },
  { kind: 'loot', label: 'Loot', color: rarity.legendary.color },
  { kind: 'invite', label: 'Invite', color: colors.cyan },
  { kind: 'system', label: 'System', color: colors.orange },
  { kind: 'online', label: 'Friend', color: colors.green },
  { kind: 'area', label: 'Area', color: colors.goldLight },
];

function DockButton({ d, n, onPress }) {
  return (
    <Pressable onPress={onPress} style={{ cursor: 'pointer' }}>
      {({ hovered, focused, pressed }) => (
        <View style={[styles.dockBtn, (hovered || focused) && { borderColor: d.color, backgroundColor: d.color + '22' }, pressed && { transform: [{ scale: 0.94 }] }]}>
          <View style={[styles.dockDot, { backgroundColor: d.color }]} />
          <Text style={[styles.dockText, (hovered || focused) && { color: '#fff' }]}>{d.label}</Text>
          <Text style={styles.dockKey}>{n}</Text>
        </View>
      )}
    </Pressable>
  );
}

// --- screen -------------------------------------------------------------------------------

export default function Notifications({ active = true }) {
  const [toasts, setToasts] = useState([]);
  const [feed, setFeed] = useState([]);
  const [banner, setBanner] = useState(null);
  const [auto, setAuto] = useState(true);
  const [party, setParty] = useState([]);
  const seq = useRef(0);
  const counters = useRef({}).current;

  const nextOf = (kind) => (counters[kind] = (counters[kind] || 0) + 1) - 1;

  const [log, setLog] = useState([]);
  const logRef = useRef(0);
  const addLog = useCallback((dir, name, payload) => {
    stackAnim();
    setLog((cur) => [{ id: ++logRef.current, dir, name, text: JSON.stringify(payload) }, ...cur].slice(0, 3));
  }, []);

  const spawn = useCallback((kind, custom) => {
    const n = nextOf(kind);
    if (kind === 'area') {
      const a = pick(AREAS, n);
      setBanner({ id: ++seq.current, ...a });
      Host.postMessage('notification', { kind, title: a.title });
      addLog('out', 'notification', { kind, title: a.title });
      return;
    }
    const data = custom || (kind === 'achievement' ? pick(ACHIEVEMENTS, n) : kind === 'loot' ? pick(LOOT, n) : kind === 'system' ? pick(SYSTEM, n) : pick(FRIENDS, n + (kind === 'online' ? 3 : 0)));
    if (!custom) {
      const title = data.title || data.name;
      Host.postMessage('notification', { kind, title });
      addLog('out', 'notification', { kind, title });
    }
    stackAnim();
    setToasts((cur) => {
      const next = [{ id: ++seq.current, kind, data }, ...cur];
      return next.slice(0, MAX_TOASTS);
    });
  }, []);

  const remove = useCallback((id) => {
    stackAnim();
    setToasts((cur) => cur.filter((t) => t.id !== id));
  }, []);
  // The embedding page can push notifications: jsui.postMessage('notify', {...}).
  useHostMessage('notify', (payload) => {
    let p = payload;
    if (typeof p === 'string') {
      try {
        p = JSON.parse(p);
      } catch (e) {
        p = { title: p };
      }
    }
    p = p || {};
    addLog('in', 'notify', p);
    spawn('system', { host: true, title: String(p.title || 'Hello from the page'), body: String(p.body || 'Sent with jsui.postMessage(\'notify\', …)') });
  });

  const onAccept = useCallback((name) => {
    Host.postMessage('party', { joined: name });
    addLog('out', 'party', { joined: name });
    stackAnim();
    setParty((p) => (p.includes(name) ? p : [...p, name].slice(-3)));
  }, []);
  const respondNewest = (ok) => {
    setToasts((cur) => {
      const inv = cur.find((t) => t.kind === 'invite' && !t.respond);
      return inv ? cur.map((t) => (t === inv ? { ...t, respond: ok } : t)) : cur;
    });
  };

  // Opening sequence, then a steady trickle while auto is on.
  useEffect(() => {
    if (!active) return undefined;
    const ids = [
      setTimeout(() => spawn('system'), 300),
      setTimeout(() => spawn('invite'), 900),
      setTimeout(() => spawn('loot'), 1500),
      setTimeout(() => spawn('achievement'), 2100),
    ];
    return () => ids.forEach(clearTimeout);
  }, []);
  useEffect(() => {
    if (!active || !auto) return undefined;
    const order = ['online', 'achievement', 'loot', 'area', 'system', 'online', 'loot', 'invite'];
    let i = 0;
    const id = setInterval(() => spawn(order[i++ % order.length]), 4200);
    return () => clearInterval(id);
  }, [active, auto]);
  useEffect(() => {
    if (!active) return undefined;
    let n = 0;
    const tick = () => {
      n++;
      const a = KILLERS[(n * 3) % KILLERS.length];
      let b = KILLERS[(n * 5 + 1) % KILLERS.length];
      if (b === a) b = KILLERS[(n * 5 + 2) % KILLERS.length];
      stackAnim();
      setFeed((cur) => [{ id: n, a, b, weapon: WEAPONS[n % 3], mine: a === 'Valkyrie_07', headshot: n % 3 === 0 }, ...cur].slice(0, 4));
    };
    tick();
    const id = setInterval(tick, 2600);
    return () => clearInterval(id);
  }, [active]);

  const handled = (e) => {
    e.preventDefault();
    e.stopPropagation();
  };
  const onKeyDown = (e) => {
    const k = e.key;
    if (k >= '1' && k <= '6') spawn(DOCK[+k - 1].kind), handled(e);
    else if (k === 'x' || k === 'X') respondNewest(true), handled(e);
    else if (k === 'y' || k === 'Y') respondNewest(false), handled(e);
  };
  const onPad = (e) => {
    if (!e.pressed) return;
    if (e.buttonIndex === 2) respondNewest(true), handled(e);
    else if (e.buttonIndex === 3) respondNewest(false), handled(e);
  };

  return (
    <View style={styles.root} onKeyDown={onKeyDown} onGamepadButton={onPad}>
      <ImageBackground source={A.bg} style={StyleSheet.absoluteFill} resizeMode="cover" />
      <View style={styles.shade} />

      <Compass active={active} />

      <View style={styles.feed} pointerEvents="none">
        {feed.map((f, i) => (
          <FeedRow key={f.id} f={f} index={i} />
        ))}
      </View>

      <View style={styles.bridge} pointerEvents="none">
        <Text style={styles.bridgeTitle}>Host bridge</Text>
        {log.length ? (
          log.map((l) => (
            <Text key={l.id} style={styles.bridgeLine} numberOfLines={1}>
              <Text style={{ color: l.dir === 'out' ? colors.gold : colors.cyan }}>{l.dir === 'out' ? '→ page ' : '← page '}</Text>
              {l.name} {l.text}
            </Text>
          ))
        ) : (
          <Text style={styles.bridgeLine}>waiting for messages…</Text>
        )}
      </View>

      {party.length ? (
        <View style={styles.party} pointerEvents="none">
          <Text style={styles.partyTitle}>Party</Text>
          {['Valkyrie_07', ...party].map((p) => (
            <View key={p} style={styles.partyRow}>
              <View style={[styles.onlineDot, { width: 6, height: 6 }]} />
              <Text style={styles.partyName}>{p}</Text>
            </View>
          ))}
        </View>
      ) : null}

      {banner ? <Banner b={banner} onDone={() => setBanner(null)} /> : null}

      <View style={styles.stack}>
        {toasts.map((t) => (
          <Toast key={t.id} t={t} active={active} onRemove={remove} onAccept={onAccept} />
        ))}
      </View>

      <View style={styles.dock}>
        <Text style={styles.dockTitle}>Trigger</Text>
        {DOCK.map((d, i) => (
          <DockButton key={d.kind} d={d} n={i + 1} onPress={() => spawn(d.kind)} />
        ))}
        <View style={styles.dockSep} />
        <Pressable onPress={() => setAuto((a) => !a)} style={{ cursor: 'pointer' }} autoFocus={active}>
          {({ hovered, focused }) => (
            <View style={[styles.dockBtn, (hovered || focused) && { borderColor: colors.cyan }]}>
              <View style={[styles.autoSwitch, auto && styles.autoOn]}>
                <View style={[styles.autoKnob, auto && { transform: [{ translateX: 12 }], backgroundColor: '#fff' }]} />
              </View>
              <Text style={[styles.dockText, (hovered || focused) && { color: '#fff' }]}>Auto</Text>
            </View>
          )}
        </Pressable>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: { width: 1280, height: 720, overflow: 'hidden', backgroundColor: '#0b1a20' },
  shade: { ...StyleSheet.absoluteFillObject, backgroundColor: 'rgba(3,8,12,0.35)' },

  feed: { position: 'absolute', left: 28, top: 28, gap: 6, alignItems: 'flex-start' },
  feedRow: { flexDirection: 'row', alignItems: 'center', gap: 8, height: 30, paddingHorizontal: 12, borderRadius: 6, backgroundColor: 'rgba(0,0,0,0.5)' },
  feedMine: { borderWidth: 1, borderColor: 'rgba(255,255,255,0.55)', backgroundColor: 'rgba(79,140,255,0.25)' },
  feedName: { fontFamily: font, fontWeight: '700', fontSize: 13 },
  feedIcon: { width: 22, height: 22, transform: [{ scaleX: -1 }] },
  headshot: { width: 8, height: 8, borderRadius: 4, backgroundColor: colors.red, borderWidth: 1, borderColor: '#fff' },

  party: { position: 'absolute', left: 28, top: 190, padding: 12, borderRadius: 10, backgroundColor: 'rgba(0,0,0,0.45)', gap: 6, minWidth: 150 },
  partyTitle: { fontFamily: font, fontWeight: '700', fontSize: 10, letterSpacing: 1.6, textTransform: 'uppercase', color: colors.cyan },
  partyRow: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  partyName: { fontFamily: font, fontWeight: '600', fontSize: 13, color: colors.text },

  banner: { position: 'absolute', top: 430, left: 0, right: 0, alignItems: 'center' },
  bannerRule: { flexDirection: 'row', alignItems: 'center', gap: 12, width: 420 },
  rule: { flex: 1, height: 1 },
  diamond: { width: 8, height: 8, backgroundColor: colors.goldLight, transform: [{ rotate: '45deg' }] },
  bannerKicker: { fontFamily: font, fontWeight: '600', fontSize: 13, textTransform: 'uppercase', color: colors.goldLight, marginTop: 14 },
  bannerTitle: {
    fontFamily: font,
    fontWeight: '700',
    fontSize: 44,
    letterSpacing: 1,
    color: '#fff',
    marginTop: 6,
    textShadowColor: 'rgba(0,0,0,0.75)',
    textShadowOffset: { width: 0, height: 3 },
    textShadowRadius: 14,
  },
  bannerSub: { fontFamily: font, fontStyle: 'italic', fontSize: 15, color: colors.textDim, marginTop: 6 },

  stack: { position: 'absolute', right: 24, top: 24, width: 380, gap: 10, alignItems: 'stretch' },
  toast: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 12,
    padding: 12,
    paddingBottom: 14,
    borderRadius: 10,
    borderLeftWidth: 4,
    backgroundColor: 'rgba(10,14,26,0.92)',
    boxShadow: '0 12px 30px rgba(0,0,0,0.55)',
    overflow: 'hidden',
  },
  hot: { boxShadow: '0 12px 30px rgba(0,0,0,0.55), 0 0 0 1px rgba(255,255,255,0.35)' },
  timerTrack: { position: 'absolute', left: 0, right: 0, bottom: 0, height: 3, backgroundColor: 'rgba(255,255,255,0.06)' },
  timerFill: { position: 'absolute', left: 0, top: 0, bottom: 0, right: 0, transformOrigin: 'left', opacity: 0.85 },
  achievement: {
    experimental_backgroundImage: 'linear-gradient(100deg, rgba(242,195,91,0.22) 0%, rgba(10,14,26,0.92) 55%)',
    boxShadow: '0 12px 30px rgba(0,0,0,0.55), 0 0 0 1px rgba(242,195,91,0.35)',
  },
  shineClip: { ...StyleSheet.absoluteFillObject, overflow: 'hidden', borderRadius: 10 },
  shine: { position: 'absolute', top: -10, bottom: -10, width: 60, backgroundColor: 'rgba(255,240,200,0.18)' },
  trophy: {
    width: 52,
    height: 52,
    borderRadius: 26,
    alignItems: 'center',
    justifyContent: 'center',
    borderWidth: 2,
    borderColor: colors.gold,
    experimental_backgroundImage: 'radial-gradient(circle, rgba(242,195,91,0.45) 0%, rgba(40,26,6,0.95) 70%)',
    boxShadow: '0 0 16px rgba(242,195,91,0.5)',
  },
  points: { fontFamily: font, fontWeight: '700', fontSize: 18, color: colors.goldLight },
  loot: { borderWidth: 1, borderLeftWidth: 4 },
  legendGlow: { ...StyleSheet.absoluteFillObject, borderRadius: 10, experimental_backgroundImage: 'linear-gradient(100deg, rgba(255,178,63,0.25) 0%, rgba(255,178,63,0) 60%)' },
  lootIcon: { width: 52, height: 52, borderRadius: 10, alignItems: 'center', justifyContent: 'center', borderWidth: 1.5 },
  kicker: { fontFamily: font, fontWeight: '700', fontSize: 10, letterSpacing: 1.6, textTransform: 'uppercase' },
  toastTitle: { fontFamily: font, fontWeight: '600', fontSize: 15, color: colors.text, marginTop: 2 },
  toastBody: { fontFamily: font, fontSize: 12, color: colors.textDim, marginTop: 2 },
  avatar: { width: 44, height: 44, borderRadius: 22, borderWidth: 2, borderColor: colors.cyan },
  actions: { flexDirection: 'row', alignItems: 'center', gap: 8, marginTop: 8 },
  action: { height: 28, flexDirection: 'row', alignItems: 'center', gap: 6, paddingHorizontal: 12, borderRadius: 6, borderWidth: 1, borderColor: colors.lineStrong, cursor: 'pointer' },
  accept: { backgroundColor: colors.cyan, borderColor: colors.cyan },
  acceptHot: { boxShadow: '0 0 0 2px rgba(10,14,26,1), 0 0 0 4px rgba(79,209,255,0.8)' },
  declineHot: { borderColor: '#fff', backgroundColor: 'rgba(255,255,255,0.08)' },
  acceptText: { fontFamily: font, fontWeight: '700', fontSize: 12, color: '#04121c' },
  declineText: { fontFamily: font, fontWeight: '600', fontSize: 12, color: colors.textDim },
  actionGlyph: { fontFamily: font, fontWeight: '700', fontSize: 10, color: '#04121c', opacity: 0.6 },
  timeout: { marginLeft: 'auto', fontFamily: font, fontWeight: '600', fontSize: 12, color: colors.textMute },
  accepted: { borderLeftColor: colors.green, backgroundColor: 'rgba(8,30,18,0.94)' },
  system: { backgroundColor: 'rgba(30,18,6,0.92)' },
  warnIcon: { width: 32, height: 32, borderRadius: 6, backgroundColor: colors.orange, alignItems: 'center', justifyContent: 'center', transform: [{ rotate: '45deg' }], marginHorizontal: 4 },
  warnGlyph: { fontFamily: font, fontWeight: '700', fontSize: 18, color: '#2a1300', transform: [{ rotate: '-45deg' }] },
  mini: { flexDirection: 'row', alignItems: 'center', gap: 8, paddingLeft: 12, paddingRight: 14, paddingVertical: 6, borderRadius: 18, backgroundColor: 'rgba(10,14,26,0.86)', cursor: 'pointer' },
  miniAvatar: { width: 20, height: 20, borderRadius: 10 },
  onlineDot: { width: 8, height: 8, borderRadius: 4, backgroundColor: colors.green, boxShadow: '0 0 8px rgba(74,222,128,0.9)' },
  miniText: { fontFamily: font, fontSize: 12, color: colors.textDim },

  hostToast: { backgroundColor: 'rgba(6,22,32,0.94)', boxShadow: '0 12px 30px rgba(0,0,0,0.55), 0 0 0 1px rgba(79,209,255,0.45), 0 0 24px rgba(79,209,255,0.25)' },
  bridge: { position: 'absolute', left: 28, bottom: 84, width: 430, padding: 10, borderRadius: 10, backgroundColor: 'rgba(0,0,0,0.5)', borderWidth: 1, borderColor: colors.line, gap: 3 },
  bridgeTitle: { fontFamily: font, fontWeight: '700', fontSize: 10, letterSpacing: 1.6, textTransform: 'uppercase', color: colors.textMute, marginBottom: 2 },
  bridgeLine: { fontFamily: 'JetBrains Mono', fontSize: 11, lineHeight: 16, color: colors.textDim },
  compass: {
    position: 'absolute',
    left: 440,
    top: 22,
    width: 400,
    height: 34,
    overflow: 'hidden',
    experimental_backgroundImage: 'linear-gradient(90deg, rgba(0,0,0,0) 0%, rgba(0,0,0,0.45) 25%, rgba(0,0,0,0.45) 75%, rgba(0,0,0,0) 100%)',
  },
  compassStrip: { position: 'absolute', left: 0, top: 0, bottom: 0, width: 2000 },
  compassMark: { position: 'absolute', top: 0, bottom: 0, width: 30, marginLeft: -15, alignItems: 'center', justifyContent: 'center' },
  compassText: { fontFamily: font, fontWeight: '600', fontSize: 11, color: colors.textDim },
  compassMain: { fontWeight: '700', fontSize: 14, color: '#fff' },
  compassTick: { width: 1, height: 8, backgroundColor: 'rgba(255,255,255,0.4)' },
  compassPin: { position: 'absolute', top: 4, width: 8, height: 8, marginLeft: -4, backgroundColor: colors.gold, transform: [{ rotate: '45deg' }], boxShadow: '0 0 8px rgba(242,195,91,0.9)' },
  compassCenter: { position: 'absolute', left: 199, bottom: 0, width: 2, height: 8, backgroundColor: colors.gold },

  dock: {
    position: 'absolute',
    left: 28,
    bottom: 24,
    flexDirection: 'row',
    alignItems: 'center',
    gap: 6,
    padding: 6,
    paddingLeft: 14,
    borderRadius: 14,
    backgroundColor: 'rgba(6,10,20,0.82)',
    borderWidth: 1,
    borderColor: colors.line,
    boxShadow: '0 16px 40px rgba(0,0,0,0.5)',
  },
  dockTitle: { fontFamily: font, fontWeight: '700', fontSize: 10, letterSpacing: 1.6, textTransform: 'uppercase', color: colors.textMute, marginRight: 4 },
  dockBtn: { flexDirection: 'row', alignItems: 'center', gap: 8, height: 36, paddingHorizontal: 12, borderRadius: 9, borderWidth: 1, borderColor: 'transparent' },
  dockDot: { width: 8, height: 8, borderRadius: 4 },
  dockText: { fontFamily: font, fontWeight: '600', fontSize: 13, color: colors.textDim },
  dockKey: { fontFamily: font, fontWeight: '700', fontSize: 10, color: colors.textMute, borderWidth: 1, borderColor: colors.lineStrong, borderRadius: 4, paddingHorizontal: 4, paddingVertical: 1 },
  dockSep: { width: 1, height: 22, backgroundColor: colors.line, marginHorizontal: 4 },
  autoSwitch: { width: 28, height: 16, borderRadius: 8, padding: 2, backgroundColor: 'rgba(255,255,255,0.12)' },
  autoOn: { backgroundColor: colors.cyanDeep },
  autoKnob: { width: 12, height: 12, borderRadius: 6, backgroundColor: '#8a93ad' },
});
