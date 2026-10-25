// Interactive battle pass / store (1280×720) in the style of
// examples/screens/battle-pass.jsx. Featured offers page horizontally
// (pagingEnabled, LB / RB), the 60-tier reward track snaps tier by tier
// (snapToInterval) with momentum, and both drive parallax art through
// Animated.event on the native driver. Unlocked tiers can be claimed with a
// reward burst, XP keeps trickling in from challenges and levels the pass.
import React, { memo, useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { Animated, Easing, Image, Pressable, StyleSheet, Text, View, useInteraction } from 'react-native';
import { colors, font, rarity } from '../../../examples/theme.js';

export const feature = {
  title: 'ScrollView snapping + scroll-driven parallax',
  points: [
    'The reward track is a horizontal ScrollView with snapToInterval: drag, fling (momentum) or wheel and it always settles on a tier.',
    'Featured offers page with pagingEnabled; LB / RB and the page dots scroll programmatically with scrollTo.',
    'Animated.event maps contentOffset.x onto native values: the art layers behind both scrollers, the card art and the track mini-map move without JS per frame.',
    'D-pad / arrow focus glides the focused tier into view; unlocked tiers claim with a reward burst while challenge XP fills the pass live.',
  ],
  keys: ['Drag / wheel / fling: scroll', 'Arrows / D-pad: move focus', 'Enter / A: claim, buy', 'LB RB / Q E: offer pages', 'Y: claim all', 'X: jump to current tier'],
};

const ND = true;
const ASSET = {
  ember: require('../../../examples/assets/card-ember.png'),
  tide: require('../../../examples/assets/card-tide.png'),
  verdant: require('../../../examples/assets/card-verdant.png'),
  void: require('../../../examples/assets/card-void.png'),
};
const ICONS = {
  axe: require('../../../examples/assets/item-axe.png'),
  'potion-health': require('../../../examples/assets/item-potion-health.png'),
  'potion-mana': require('../../../examples/assets/item-potion-mana.png'),
  scroll: require('../../../examples/assets/item-scroll.png'),
  ring: require('../../../examples/assets/item-ring.png'),
  gem: require('../../../examples/assets/item-gem.png'),
  helmet: require('../../../examples/assets/item-helmet.png'),
  boots: require('../../../examples/assets/item-boots.png'),
  amulet: require('../../../examples/assets/item-amulet.png'),
  sword: require('../../../examples/assets/item-sword.png'),
  shield: require('../../../examples/assets/item-shield.png'),
  bow: require('../../../examples/assets/item-bow.png'),
};

const OFFERS = [
  { id: 'cinder', art: 'ember', icon: 'axe', name: 'Cinderforged Bundle', tier: 'legendary', price: 1400, old: 2000, discount: '-30%', tag: 'Featured' },
  { id: 'tide', art: 'tide', icon: 'potion-mana', name: 'Tidecaller Elixirs', tier: 'rare', price: 450, tag: 'Daily' },
  { id: 'verdant', art: 'verdant', icon: 'bow', name: 'Verdant Longbow', tier: 'epic', owned: true },
  { id: 'void', art: 'void', icon: 'gem', name: 'Voidheart Relic', tier: 'epic', price: 900, locked: 'Reach tier 50' },
  { id: 'brazier', art: 'ember', icon: 'amulet', name: "Founder's Brazier", tier: 'legendary', price: 2400, tag: 'Limited' },
  { id: 'warplate', art: 'void', icon: 'helmet', name: 'Ashen Warplate', tier: 'epic', price: 1100 },
  { id: 'aegis', art: 'tide', icon: 'shield', name: 'Tideguard Aegis', tier: 'rare', price: 600 },
  { id: 'treads', art: 'verdant', icon: 'boots', name: 'Wildroot Treads', tier: 'uncommon', price: 250, tag: 'Daily' },
];

// Track: 60 tiers; free reward on every other tier, premium on all; milestones every 5.
const ROT_FREE = ['potion-health', 'scroll', 'gem', 'potion-mana'];
const ROT_PREM = ['ring', 'helmet', 'boots', 'amulet', 'sword', 'shield', 'bow', 'gem', 'axe', 'scroll'];
const TIERS = Array.from({ length: 60 }, (_, i) => {
  const n = i + 1;
  return { n, free: n % 2 === 1 ? ROT_FREE[(n >> 1) % 4] : null, prem: ROT_PREM[n % 10], milestone: n % 5 === 0 };
});

const PAGE_W = 1280; // one page = the full screen width; cards sit inside 48 px gutters
const CARD_W = (1184 - 60) / 4;
const COL = 92;
const TRACK_W = 1184 - 84;
const CONTENT_W = TIERS.length * COL;
const XP_PER_TIER = 10000;

const useValue = (v) => useRef(new Animated.Value(v)).current;
const fmt = (n) => String(Math.round(n)).replace(/\B(?=(\d{3})+(?!\d))/g, ',');

// ---------------------------------------------------------------------------

function Burst({ color }) {
  const t = useValue(0);
  useEffect(() => {
    Animated.timing(t, { toValue: 1, duration: 900, easing: Easing.out(Easing.cubic), useNativeDriver: ND }).start();
  }, []);
  return (
    <View pointerEvents="none" style={styles.burst}>
      <Animated.View
        style={[
          styles.burstRing,
          { borderColor: color, opacity: t.interpolate({ inputRange: [0, 1], outputRange: [0.9, 0] }), transform: [{ scale: t.interpolate({ inputRange: [0, 1], outputRange: [0.3, 2.4] }) }] },
        ]}
      />
      {Array.from({ length: 12 }, (_, i) => {
        const a = (i / 12) * Math.PI * 2;
        const d = 54 + (i % 3) * 14;
        return (
          <Animated.View
            key={i}
            style={[
              styles.spark,
              {
                backgroundColor: i % 2 ? color : '#fff6dc',
                opacity: t.interpolate({ inputRange: [0, 0.2, 1], outputRange: [0, 1, 0] }),
                transform: [
                  { translateX: t.interpolate({ inputRange: [0, 1], outputRange: [0, Math.cos(a) * d] }) },
                  { translateY: t.interpolate({ inputRange: [0, 1], outputRange: [0, Math.sin(a) * d] }) },
                  { rotate: '45deg' },
                  { scale: t.interpolate({ inputRange: [0, 1], outputRange: [1.4, 0.3] }) },
                ],
              },
            ]}
          />
        );
      })}
    </View>
  );
}

const OfferCard = memo(function OfferCard({ o, index, scrollX, state, onPress, gemsOk }) {
  const r = rarity[o.tier];
  const lift = useValue(0);
  const shake = useValue(0);
  // Press feedback from the runtime's interaction state (no re-render).
  const ix = useInteraction();
  const pressScale = useMemo(() => ix.pressed.interpolate({ inputRange: [0, 1], outputRange: [1, 0.97] }), [ix]);
  const x0 = Math.floor(index / 4) * PAGE_W + (index % 4) * (CARD_W + 20) - 48;
  const owned = state === 'owned';
  const confirming = state === 'confirm';
  const press = () => {
    const res = onPress(o);
    if (res === 'deny') {
      Animated.sequence([
        Animated.timing(shake, { toValue: 6, duration: 45, useNativeDriver: ND }),
        Animated.timing(shake, { toValue: -6, duration: 70, useNativeDriver: ND }),
        Animated.timing(shake, { toValue: 3, duration: 60, useNativeDriver: ND }),
        Animated.timing(shake, { toValue: 0, duration: 60, useNativeDriver: ND }),
      ]).start();
    }
  };
  return (
    <Pressable
      onHoverIn={() => Animated.spring(lift, { toValue: 1, stiffness: 300, damping: 20, useNativeDriver: ND }).start()}
      onHoverOut={() => Animated.spring(lift, { toValue: 0, stiffness: 300, damping: 20, useNativeDriver: ND }).start()}
      onFocus={() => Animated.spring(lift, { toValue: 1, stiffness: 300, damping: 20, useNativeDriver: ND }).start()}
      onBlur={() => Animated.spring(lift, { toValue: 0, stiffness: 300, damping: 20, useNativeDriver: ND }).start()}
      onPress={press}
      group
      interaction={ix}
      style={{ flex: 1, cursor: 'pointer' }}
    >
      <Animated.View
        style={[
          styles.card,
          { borderColor: r.color, boxShadow: `0 16px 36px rgba(0,0,0,0.55), 0 0 22px ${r.glow}` },
          {
            transform: [
              { translateY: lift.interpolate({ inputRange: [0, 1], outputRange: [0, -6] }) },
              { translateX: shake },
              { scale: pressScale },
            ],
          },
        ]}
        groupFocusedStyle={{ boxShadow: `0 16px 36px rgba(0,0,0,0.55), 0 0 0 3px rgba(255,243,207,0.85), 0 0 30px ${r.glow}` }}
      >
        <View style={styles.cardArt}>
          <Animated.Image
            source={ASSET[o.art]}
            resizeMode="cover"
            style={[
              styles.cardArtImg,
              {
                transform: [
                  { translateX: scrollX.interpolate({ inputRange: [x0 - PAGE_W, x0 + PAGE_W], outputRange: [60, -60] }) },
                  { scale: lift.interpolate({ inputRange: [0, 1], outputRange: [1, 1.06] }) },
                ],
              },
            ]}
          />
          <View style={styles.artFade} />
          <Animated.Image
            source={ICONS[o.icon]}
            style={[
              styles.cardIcon,
              {
                transform: [
                  { translateX: scrollX.interpolate({ inputRange: [x0 - PAGE_W, x0 + PAGE_W], outputRange: [-24, 24] }) },
                  { translateY: lift.interpolate({ inputRange: [0, 1], outputRange: [0, -6] }) },
                  { rotate: lift.interpolate({ inputRange: [0, 1], outputRange: ['-8deg', '-2deg'] }) },
                ],
              },
            ]}
          />
          {o.tag ? (
            <View style={[styles.tag, { backgroundColor: o.tag === 'Daily' ? colors.cyan : o.tag === 'Limited' ? colors.purple : colors.gold }]}>
              <Text style={styles.tagText}>{o.tag}</Text>
            </View>
          ) : null}
          {o.discount ? (
            <View style={styles.ribbonWrap}>
              <View style={styles.ribbon}>
                <Text style={styles.ribbonText}>{o.discount}</Text>
              </View>
            </View>
          ) : null}
        </View>
        <View style={styles.cardBody}>
          <Text style={[styles.cardRarity, { color: r.color }]}>{r.label}</Text>
          <Text style={styles.cardName} numberOfLines={1}>
            {o.name}
          </Text>
          {owned ? (
            <View style={[styles.priceBtn, styles.ownedBtn]}>
              <View style={styles.tick} />
              <Text style={[styles.priceText, { color: colors.green }]}>Owned</Text>
            </View>
          ) : confirming ? (
            <View style={[styles.priceBtn, gemsOk ? styles.confirmBtn : styles.denyBtn]}>
              <Text style={[styles.priceText, { color: gemsOk ? '#2a1500' : '#fff' }]}>{gemsOk ? `Confirm · ${fmt(o.price)}` : 'Not enough gems'}</Text>
            </View>
          ) : (
            <View style={styles.priceBtn}>
              <View style={styles.gemSmall} />
              <Text style={styles.priceText}>{fmt(o.price)}</Text>
              {o.old ? <Text style={styles.oldPrice}>{fmt(o.old)}</Text> : null}
            </View>
          )}
        </View>
        {o.locked ? (
          <View style={styles.lockOverlay}>
            <View style={styles.lockBody}>
              <View style={styles.lockShackle} />
            </View>
            <Text style={styles.lockText}>{o.locked}</Text>
          </View>
        ) : null}
        {state === 'owned-new' || owned ? (state === 'owned-new' ? <Burst color={r.color} /> : null) : null}
      </Animated.View>
    </Pressable>
  );
});

function Reward({ name, premium, claimed, claimable, pulse }) {
  if (!name) return <View style={[styles.reward, styles.rewardEmpty]} />;
  return (
    <View style={[styles.reward, premium && styles.rewardPremium, claimed && { opacity: 0.45 }]}>
      {claimable ? <Animated.View pointerEvents="none" style={[StyleSheet.absoluteFill, styles.claimGlow, { opacity: pulse }]} /> : null}
      <Image source={ICONS[name]} style={styles.rewardIcon} />
      {claimed ? (
        <View style={styles.claimed}>
          <View style={[styles.tick, { width: 4, height: 8, marginTop: -2, borderRightWidth: 1.5, borderBottomWidth: 1.5 }]} />
        </View>
      ) : null}
      {!claimed && !claimable ? (
        <View style={styles.rewardLock}>
          <View style={styles.miniLock} />
        </View>
      ) : null}
    </View>
  );
}

const TierCol = memo(function TierCol({ t, state, current, burstKey, pulse, onPress, autoFocus }) {
  const claimed = state === 'claimed';
  const claimable = state === 'claimable';
  return (
    <Pressable autoFocus={autoFocus} onPress={() => onPress(t.n)} group style={{ width: COL, cursor: 'pointer' }}>
      <View
        style={[styles.tierCol, current && styles.tierColCurrent]}
        groupHoveredStyle={styles.tierColHot}
        groupFocusedStyle={[styles.tierColHot, styles.tierColFocus]}
        groupPressedStyle={styles.pressed96}
      >
        <View style={[styles.claimPill, !claimable && { opacity: 0 }]}>
          <Text style={styles.claimPillText}>Claim</Text>
        </View>
        <Reward name={t.free} claimed={claimed} claimable={claimable} pulse={pulse} />
        <Reward name={t.prem} premium claimed={claimed} claimable={claimable} pulse={pulse} />
        <View style={[styles.tierNode, claimed && styles.tierNodeDone, claimable && styles.tierNodeReady, current && styles.tierNodeCurrent, t.milestone && styles.tierNodeMilestone]}>
          <Text style={[styles.tierNodeText, (current || claimed || claimable) && { color: '#1a1206' }]}>{t.n}</Text>
        </View>
        {burstKey ? <Burst key={burstKey} color={colors.gold} /> : null}
      </View>
    </Pressable>
  );
});

/** "Refreshes in hh:mm:ss": a native seconds value counting down (one
 * linear timing per day), formatted by the runtime. */
function Countdown({ active, start }) {
  const v = useValue(start);
  useEffect(() => {
    if (!active) return undefined;
    let anim = null;
    const run = () => {
      const from = v.__getValue();
      anim = Animated.timing(v, { toValue: 0, duration: from * 1000, easing: Easing.linear, useNativeDriver: ND });
      anim.start(({ finished }) => {
        if (!finished) return;
        v.setValue(24 * 3600);
        run();
      });
    };
    run();
    return () => anim && anim.stop();
  }, [active]);
  const t = useMemo(() => {
    // Whole seconds left, as the 1 s ticks showed them.
    const secs = Animated.derive([v], (x) => Math.ceil(x - 1e-6));
    const two = { decimals: 0, minDigits: 2 };
    return {
      h: Animated.format(Animated.derive([secs], (x) => Math.floor(x / 3600)), two),
      m: Animated.format(Animated.derive([secs], (x) => Math.floor(x / 60) % 60), two),
      s: Animated.format(Animated.derive([secs], (x) => x % 60), two),
    };
  }, [v]);
  return (
    <Text style={styles.refresh}>
      Refreshes in <Animated.Text text={t.h} />:<Animated.Text text={t.m} />:<Animated.Text text={t.s} />
    </Text>
  );
}

// ---------------------------------------------------------------------------

export default function BattlePass({ active = true }) {
  const [tier, setTier] = useState(37);
  const [xp, setXp] = useState(6420);
  const [claimed, setClaimed] = useState(34); // tiers 1..claimed are claimed
  const [extraClaimed, setExtraClaimed] = useState({});
  const [bursts, setBursts] = useState({});
  const [gems, setGems] = useState(2400);
  const [offerState, setOfferState] = useState({ verdant: 'owned' });
  const [page, setPage] = useState(0);
  const [toast, setToast] = useState(null);

  const offersX = useValue(0);
  const trackX = useValue((37 - 4) * COL);
  const xpFill = useValue(0);
  const lineFill = useValue(0);
  const pulse = useValue(0.4);
  const toastT = useValue(0);
  const tierPop = useValue(1);
  const offersRef = useRef(null);
  const trackRef = useRef(null);
  const confirmTimer = useRef(null);

  const isClaimed = (n) => n <= claimed || !!extraClaimed[n];
  const stateOf = (n) => (isClaimed(n) ? 'claimed' : n <= tier ? 'claimable' : 'locked');

  const say = (text, color = colors.gold) => {
    setToast({ text, color });
    toastT.setValue(0);
    Animated.sequence([
      Animated.spring(toastT, { toValue: 1, stiffness: 260, damping: 18, useNativeDriver: ND }),
      Animated.delay(1600),
      Animated.timing(toastT, { toValue: 0, duration: 260, useNativeDriver: ND }),
    ]).start();
  };

  // Header XP bar and the track's progress line follow tier + XP.
  const progress = (tier - 1 + xp / XP_PER_TIER) * COL + COL / 2;
  useEffect(() => {
    Animated.timing(xpFill, { toValue: xp / XP_PER_TIER, duration: 900, easing: Easing.out(Easing.cubic), useNativeDriver: ND }).start();
    Animated.timing(lineFill, { toValue: progress / CONTENT_W, duration: 1100, easing: Easing.out(Easing.cubic), useNativeDriver: ND }).start();
  }, [xp, tier]);

  // Ambient: claimable pulse + challenge XP trickling in.
  useEffect(() => {
    if (!active) return undefined;
    const l = Animated.loop(
      Animated.sequence([
        Animated.timing(pulse, { toValue: 1, duration: 800, easing: Easing.inOut(Easing.sin), useNativeDriver: ND }),
        Animated.timing(pulse, { toValue: 0.35, duration: 800, easing: Easing.inOut(Easing.sin), useNativeDriver: ND }),
      ]),
    );
    l.start();
    const CHALLENGES = ['Deal 50,000 fire damage', 'Win a ranked match', 'Open 3 rift chests', 'Revive 5 allies', 'Defeat Vharos'];
    let k = 0;
    const id = setInterval(() => {
      const gain = 900 + (k % 3) * 400;
      say(`Challenge complete · ${CHALLENGES[k++ % CHALLENGES.length]} · +${fmt(gain)} XP`, colors.orange);
      setXp((x) => {
        const next = x + gain;
        if (next >= XP_PER_TIER) {
          setTier((t) => Math.min(60, t + 1));
          tierPop.setValue(1.5);
          Animated.spring(tierPop, { toValue: 1, stiffness: 200, damping: 8, useNativeDriver: ND }).start();
          return next - XP_PER_TIER;
        }
        return next;
      });
    }, 7000);
    return () => {
      l.stop();
      clearInterval(id);
    };
  }, [active]);

  const burst = (n) => setBursts((b) => Object.assign({}, b, { [n]: Date.now() + Math.random() }));

  // One stable handler for all 60 tier columns (they are memo): it reads the
  // latest state through a ref, so an XP tick does not re-render the track.
  const latest = useRef(null);
  latest.current = { stateOf, tier, xp };
  const onTier = useCallback(
    (n) => {
      const { stateOf, tier, xp } = latest.current;
      const st = stateOf(n);
      if (st === 'claimable') {
        setExtraClaimed((c) => Object.assign({}, c, { [n]: true }));
        burst(n);
        const t = TIERS[n - 1];
        say(`Tier ${n} claimed · ${t.free ? 'free + ' : ''}premium reward`, colors.goldLight);
      } else if (st === 'claimed') say(`Tier ${n} already claimed`, colors.textDim);
      else say(`Tier ${n} unlocks in ${fmt((n - tier) * XP_PER_TIER - xp)} XP`, colors.textDim);
    },
    [],
  );

  const claimAll = () => {
    const open = TIERS.filter((t) => stateOf(t.n) === 'claimable').map((t) => t.n);
    if (!open.length) return say('Nothing to claim yet', colors.textDim);
    jumpTo(open[0]);
    open.forEach((n, i) =>
      setTimeout(() => {
        setExtraClaimed((c) => Object.assign({}, c, { [n]: true }));
        burst(n);
      }, 250 + i * 220),
    );
    say(`Claimed ${open.length} tier${open.length > 1 ? 's' : ''}`, colors.goldLight);
  };

  const jumpTo = (n) => {
    const r = trackRef.current;
    const x = Math.max(0, Math.min(CONTENT_W - TRACK_W, (n - 4) * COL));
    if (r && r.scrollTo) r.scrollTo({ x, animated: true });
  };
  const goPage = (p) => {
    const n = Math.max(0, Math.min(1, p));
    setPage(n);
    const r = offersRef.current;
    if (r && r.scrollTo) r.scrollTo({ x: n * PAGE_W, animated: true });
  };

  const onOffer = useCallback(
    (o) => {
      const st = offerState[o.id];
      if (o.locked) {
        say(`${o.name} · ${o.locked}`, colors.red);
        return 'deny';
      }
      if (st === 'owned' || st === 'owned-new') {
        say(`${o.name} is already in your armory`, colors.green);
        return 'ok';
      }
      if (st === 'confirm') {
        if (gems < o.price) {
          say('Not enough gems', colors.red);
          return 'deny';
        }
        setGems((g) => g - o.price);
        setOfferState((s) => Object.assign({}, s, { [o.id]: 'owned-new' }));
        say(`Purchased ${o.name}`, rarity[o.tier].color);
        return 'ok';
      }
      clearTimeout(confirmTimer.current);
      setOfferState((s) => {
        const next = Object.assign({}, s);
        Object.keys(next).forEach((k) => next[k] === 'confirm' && delete next[k]);
        next[o.id] = 'confirm';
        return next;
      });
      confirmTimer.current = setTimeout(() => setOfferState((s) => (s[o.id] === 'confirm' ? Object.assign({}, s, { [o.id]: undefined }) : s)), 3000);
      return 'ok';
    },
    [offerState, gems],
  );
  useEffect(() => () => clearTimeout(confirmTimer.current), []);

  const onKeyDown = (e) => {
    const k = (e.key || '').toLowerCase();
    if (k === 'q' || k === 'pageup') goPage(page - 1);
    else if (k === 'e' || k === 'pagedown') goPage(page + 1);
    else if (k === 'y') claimAll();
    else if (k === 'x') jumpTo(tier);
    else return;
    e.preventDefault();
  };
  const onGamepadButton = (e) => {
    if (!e.pressed) return;
    const b = e.buttonIndex;
    if (b === 4) goPage(page - 1);
    else if (b === 5) goPage(page + 1);
    else if (b === 3) claimAll();
    else if (b === 2) jumpTo(tier);
    else return;
    e.preventDefault();
  };

  // Visible tier range: derived natively from the scroll value (no listener,
  // no render while scrolling).
  const range = useMemo(() => {
    const first = Animated.derive([trackX], (x) => Math.max(1, Math.round(x / COL) + 1));
    const span = Math.floor(TRACK_W / COL) - 1;
    return {
      first: Animated.format(first, { decimals: 0 }),
      last: Animated.format(Animated.derive([first], (f) => Math.min(60, f + span)), { decimals: 0 }),
    };
  }, [trackX]);
  const onTrackSettle = () => {};

  const claimableCount = TIERS.filter((t) => stateOf(t.n) === 'claimable').length;
  const thumbW = (TRACK_W / CONTENT_W) * 300;

  return (
    <View style={styles.root} onKeyDown={onKeyDown} onGamepadButton={onGamepadButton}>
      <View style={styles.header}>
        <View>
          <Text style={styles.kicker}>Season 4 · Emberfall</Text>
          <Text style={styles.title}>Battle Pass</Text>
        </View>
        <View style={styles.premium}>
          <Text style={styles.premiumText}>Premium</Text>
        </View>
        <View style={{ flex: 1 }} />
        <View style={styles.gems}>
          <View style={styles.gemSmall} />
          <Text style={styles.gemsText}>{fmt(gems)}</Text>
        </View>
        <View style={styles.tierBox}>
          <Text style={styles.tierLabel}>Tier</Text>
          <Animated.Text style={[styles.tierNum, { transform: [{ scale: tierPop }] }]}>{tier}</Animated.Text>
          <Text style={styles.tierMax}>/ 60</Text>
        </View>
        <View style={styles.xpBox}>
          <View style={styles.xpTop}>
            <Text style={styles.xpText}>
              {fmt(xp)} / {fmt(XP_PER_TIER)} XP
            </Text>
            <Text style={styles.timer}>Ends in 12d 4h</Text>
          </View>
          <View style={styles.xpTrack}>
            <Animated.View style={[styles.xpFill, { transform: [{ scaleX: xpFill }] }]} />
          </View>
        </View>
      </View>

      <View style={styles.sectionRow}>
        <Text style={styles.section}>Featured Offers</Text>
        <View style={styles.pager}>
          <PagerBtn label="LB" onPress={() => goPage(page - 1)} disabled={page === 0} />
          {[0, 1].map((p) => (
            <Pressable key={p} focusable={false} onPress={() => goPage(p)} hitSlop={6}>
              <View style={[styles.dot, page === p && styles.dotActive]} />
            </Pressable>
          ))}
          <PagerBtn label="RB" onPress={() => goPage(page + 1)} disabled={page === 1} />
        </View>
        <View style={{ flex: 1 }} />
        <Countdown active={active} start={6 * 3600 + 41 * 60 + 12} />
      </View>
      <Animated.ScrollView
        ref={offersRef}
        horizontal
        pagingEnabled
        showsHorizontalScrollIndicator={false}
        style={styles.offers}
        scrollEventThrottle={16}
        onScroll={Animated.event([{ nativeEvent: { contentOffset: { x: offersX } } }], { useNativeDriver: ND })}
        onMomentumScrollEnd={(e) => setPage(Math.round(e.nativeEvent.contentOffset.x / PAGE_W))}
      >
        {[0, 1].map((p) => (
          <View key={p} style={styles.page}>
            {OFFERS.slice(p * 4, p * 4 + 4).map((o, i) => (
              <OfferCard
                key={o.id}
                o={o}
                index={p * 4 + i}
                scrollX={offersX}
                state={offerState[o.id]}
                onPress={onOffer}
                gemsOk={gems >= (o.price || 0)}
              />
            ))}
          </View>
        ))}
      </Animated.ScrollView>

      <View style={styles.sectionRow}>
        <Text style={styles.section}>Rewards Track</Text>
        <Text style={styles.rangeText}>
          Tiers <Animated.Text text={range.first} />–<Animated.Text text={range.last} />
        </Text>
        <View style={{ flex: 1 }} />
        <SmallBtn glyph="X" color="#6bb8ff" label="Current tier" onPress={() => jumpTo(tier)} />
        <SmallBtn glyph="Y" color="#ffd36b" label={claimableCount ? `Claim all (${claimableCount})` : 'Claim all'} onPress={claimAll} hot={claimableCount > 0} />
      </View>
      <View style={styles.track}>
        <View style={styles.trackArt} pointerEvents="none">
          <Animated.View
            style={{
              flexDirection: 'row',
              width: 2400,
              height: '100%',
              transform: [{ translateX: trackX.interpolate({ inputRange: [0, CONTENT_W], outputRange: [0, -CONTENT_W * 0.22], extrapolate: 'clamp' }) }],
            }}
          >
            {['ember', 'tide', 'verdant', 'void', 'ember', 'tide'].map((a, i) => (
              <Image key={i} source={ASSET[a]} style={{ width: 400, height: '100%' }} resizeMode="cover" />
            ))}
          </Animated.View>
          <View style={styles.trackArtFade} />
        </View>
        <View style={styles.trackLabels}>
          <Text style={styles.trackLabel}>Free</Text>
          <Text style={[styles.trackLabel, { color: colors.gold }]}>Premium</Text>
        </View>
        <Animated.ScrollView
          ref={trackRef}
          horizontal
          snapToInterval={COL}
          decelerationRate="fast"
          showsHorizontalScrollIndicator={false}
          contentOffset={{ x: (37 - 4) * COL, y: 0 }}
          scrollEventThrottle={16}
          onScroll={Animated.event([{ nativeEvent: { contentOffset: { x: trackX } } }], { useNativeDriver: ND })}
          onMomentumScrollEnd={onTrackSettle}
          onScrollEndDrag={onTrackSettle}
          style={{ width: TRACK_W }}
          contentContainerStyle={{ paddingRight: TRACK_W - COL }}
        >
          <View style={styles.lineTrack} pointerEvents="none">
            <Animated.View style={[styles.lineFill, { transform: [{ scaleX: lineFill }] }]} />
          </View>
          {TIERS.map((t) => (
            <TierCol key={t.n} t={t} state={stateOf(t.n)} current={t.n === tier} burstKey={bursts[t.n]} pulse={pulse} onPress={onTier} autoFocus={active && t.n === 35} />
          ))}
        </Animated.ScrollView>
        <View style={styles.edgeFade} pointerEvents="none" />
      </View>
      <View style={styles.miniMap}>
        <Text style={styles.miniLabel}>1</Text>
        <View style={styles.miniTrack}>
          <View style={[styles.miniDone, { width: ((tier - 1) / 59) * 300 }]} />
          {[10, 20, 30, 40, 50].map((m) => (
            <View key={m} style={[styles.miniMark, { left: ((m - 1) / 59) * 300 }]} />
          ))}
          <Animated.View
            style={[
              styles.miniThumb,
              { width: thumbW, transform: [{ translateX: trackX.interpolate({ inputRange: [0, CONTENT_W - COL], outputRange: [0, 300 - thumbW * 0.1], extrapolate: 'clamp' }) }] },
            ]}
          />
        </View>
        <Text style={styles.miniLabel}>60</Text>
      </View>

      {toast ? (
        <Animated.View
          pointerEvents="none"
          style={[styles.toast, { borderColor: toast.color, opacity: toastT, transform: [{ translateY: toastT.interpolate({ inputRange: [0, 1], outputRange: [16, 0] }) }] }]}
        >
          <View style={[styles.toastDot, { backgroundColor: toast.color }]} />
          <Text style={styles.toastText}>{toast.text}</Text>
        </Animated.View>
      ) : null}
    </View>
  );
}

function PagerBtn({ label, onPress, disabled }) {
  return (
    <Pressable focusable={false} onPress={onPress} disabled={disabled} group style={{ cursor: 'pointer' }}>
      <View style={[styles.pagerBtn, disabled && { opacity: 0.35 }]} groupHoveredStyle={styles.pagerBtnHot}>
        <Text style={styles.pagerText}>{label}</Text>
      </View>
    </Pressable>
  );
}

function SmallBtn({ glyph, color, label, onPress, hot }) {
  return (
    <Pressable focusable={false} onPress={onPress} group style={{ cursor: 'pointer' }}>
      <View style={[styles.smallBtn, hot && styles.smallBtnHot]} groupHoveredStyle={styles.smallBtnHover} groupPressedStyle={styles.pressed96}>
        <View style={[styles.glyph, { backgroundColor: color }]}>
          <Text style={styles.glyphText}>{glyph}</Text>
        </View>
        <Text style={styles.smallBtnText}>{label}</Text>
      </View>
    </Pressable>
  );
}

const styles = StyleSheet.create({
  root: {
    width: 1280,
    height: 720,
    overflow: 'hidden',
    paddingHorizontal: 48,
    paddingTop: 26,
    backgroundColor: '#0a0712',
    experimental_backgroundImage:
      'radial-gradient(circle at 80% -10%, rgba(255,122,58,0.28) 0%, rgba(10,7,18,0) 55%), linear-gradient(180deg, #120c1e 0%, #07060d 100%)',
  },
  header: { flexDirection: 'row', alignItems: 'center', gap: 18 },
  kicker: { fontFamily: font, fontWeight: '600', fontSize: 12, letterSpacing: 2.4, textTransform: 'uppercase', color: colors.orange },
  title: { fontFamily: font, fontWeight: '700', fontSize: 34, color: '#fff', marginTop: 2 },
  premium: { marginTop: 18, paddingHorizontal: 10, paddingVertical: 4, borderRadius: 5, experimental_backgroundImage: 'linear-gradient(90deg, #ffe29a, #f2a93b)', boxShadow: '0 0 14px rgba(242,169,59,0.55)' },
  premiumText: { fontFamily: font, fontWeight: '700', fontSize: 11, letterSpacing: 1.2, textTransform: 'uppercase', color: '#2a1500' },
  gems: { flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 12, paddingVertical: 7, borderRadius: 16, backgroundColor: 'rgba(255,255,255,0.05)', borderWidth: 1, borderColor: 'rgba(178,107,255,0.4)' },
  gemsText: { fontFamily: font, fontWeight: '700', fontSize: 14, color: '#e2c8ff' },
  tierBox: { flexDirection: 'row', alignItems: 'baseline', gap: 6 },
  tierLabel: { fontFamily: font, fontWeight: '600', fontSize: 13, color: colors.textMute, textTransform: 'uppercase', letterSpacing: 1.5 },
  tierNum: { fontFamily: font, fontWeight: '700', fontSize: 40, color: colors.goldLight, textShadowColor: 'rgba(242,195,91,0.5)', textShadowOffset: { width: 0, height: 0 }, textShadowRadius: 12 },
  tierMax: { fontFamily: font, fontWeight: '600', fontSize: 16, color: colors.textMute },
  xpBox: { width: 300 },
  xpTop: { flexDirection: 'row', justifyContent: 'space-between', marginBottom: 6 },
  xpText: { fontFamily: font, fontWeight: '600', fontSize: 12, color: colors.text },
  timer: { fontFamily: font, fontWeight: '500', fontSize: 12, color: colors.orange },
  xpTrack: { height: 10, borderRadius: 5, backgroundColor: 'rgba(255,255,255,0.08)', borderWidth: 1, borderColor: 'rgba(255,255,255,0.08)', overflow: 'hidden' },
  xpFill: { width: '100%', height: '100%', borderRadius: 5, transformOrigin: 'left', experimental_backgroundImage: 'linear-gradient(90deg, #ff7a3a 0%, #ffd36b 100%)', boxShadow: '0 0 10px rgba(255,160,80,0.8)' },

  sectionRow: { flexDirection: 'row', alignItems: 'center', gap: 14, marginTop: 18, marginBottom: 10, height: 28 },
  section: { fontFamily: font, fontWeight: '700', fontSize: 13, letterSpacing: 2, textTransform: 'uppercase', color: colors.textDim },
  refresh: { fontFamily: font, fontSize: 12, color: colors.textMute },
  rangeText: { fontFamily: font, fontWeight: '600', fontSize: 12, color: colors.textMute },
  pager: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  pagerBtn: { paddingHorizontal: 6, height: 20, borderRadius: 6, borderWidth: 1, borderColor: colors.lineStrong, justifyContent: 'center' },
  pagerBtnHot: { borderColor: colors.gold },
  pagerText: { fontFamily: font, fontWeight: '700', fontSize: 9, color: colors.textDim },
  dot: { width: 8, height: 4, borderRadius: 2, backgroundColor: colors.lineStrong },
  dotActive: { width: 22, backgroundColor: colors.orange },

  offers: { marginHorizontal: -48, flexGrow: 0, height: 262 },
  page: { width: PAGE_W, flexDirection: 'row', gap: 20, paddingVertical: 10, paddingHorizontal: 48 },
  card: { height: 238, borderRadius: 14, borderWidth: 1.5, overflow: 'hidden', backgroundColor: '#100c18' },
  cardArt: { height: 132, alignItems: 'center', justifyContent: 'center', overflow: 'hidden' },
  cardArtImg: { position: 'absolute', left: -70, right: -70, top: 0, bottom: 0, width: undefined, height: 132 },
  artFade: { ...StyleSheet.absoluteFillObject, experimental_backgroundImage: 'linear-gradient(180deg, rgba(16,12,24,0) 50%, rgba(16,12,24,1) 100%)' },
  cardIcon: { width: 88, height: 88 },
  tag: { position: 'absolute', left: 10, top: 10, paddingHorizontal: 8, paddingVertical: 3, borderRadius: 4 },
  tagText: { fontFamily: font, fontWeight: '700', fontSize: 10, letterSpacing: 1, textTransform: 'uppercase', color: '#0b0f1a' },
  ribbonWrap: { position: 'absolute', right: 0, top: 0, width: 90, height: 90, overflow: 'hidden' },
  ribbon: { position: 'absolute', right: -34, top: 18, width: 130, height: 24, alignItems: 'center', justifyContent: 'center', backgroundColor: colors.red, transform: [{ rotate: '45deg' }], boxShadow: '0 2px 6px rgba(0,0,0,0.5)' },
  ribbonText: { fontFamily: font, fontWeight: '700', fontSize: 12, color: '#fff' },
  cardBody: { paddingHorizontal: 14, paddingBottom: 14, paddingTop: 2 },
  cardRarity: { fontFamily: font, fontWeight: '700', fontSize: 10, letterSpacing: 1.6, textTransform: 'uppercase' },
  cardName: { fontFamily: font, fontWeight: '700', fontSize: 16, color: colors.text, marginTop: 4 },
  priceBtn: { marginTop: 10, height: 36, borderRadius: 8, flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: 7, backgroundColor: 'rgba(255,255,255,0.06)', borderWidth: 1, borderColor: 'rgba(255,255,255,0.12)' },
  ownedBtn: { borderColor: 'rgba(74,222,128,0.5)', backgroundColor: 'rgba(74,222,128,0.10)' },
  confirmBtn: { borderColor: colors.goldLight, experimental_backgroundImage: 'linear-gradient(180deg, #ffe29a 0%, #f2c35b 60%, #c9902d 100%)' },
  denyBtn: { borderColor: colors.red, backgroundColor: 'rgba(255,77,106,0.25)' },
  gemSmall: { width: 10, height: 10, borderRadius: 2, transform: [{ rotate: '45deg' }], experimental_backgroundImage: 'linear-gradient(135deg, #d6b8ff, #8a4dff)' },
  priceText: { fontFamily: font, fontWeight: '700', fontSize: 14, color: '#fff' },
  oldPrice: { fontFamily: font, fontWeight: '500', fontSize: 12, color: colors.textMute, textDecorationLine: 'line-through' },
  tick: { width: 6, height: 11, borderRightWidth: 2, borderBottomWidth: 2, borderColor: colors.green, transform: [{ rotate: '45deg' }], marginTop: -3 },
  lockOverlay: { ...StyleSheet.absoluteFillObject, backgroundColor: 'rgba(6,4,12,0.72)', alignItems: 'center', justifyContent: 'center', gap: 10 },
  lockBody: { width: 26, height: 20, borderRadius: 4, backgroundColor: colors.textDim, marginTop: 10 },
  lockShackle: { position: 'absolute', left: 5, top: -12, width: 16, height: 16, borderWidth: 3, borderBottomWidth: 0, borderColor: colors.textDim, borderTopLeftRadius: 8, borderTopRightRadius: 8 },
  lockText: { fontFamily: font, fontWeight: '600', fontSize: 13, color: colors.text },

  track: { flexDirection: 'row', paddingVertical: 6, paddingLeft: 14, borderRadius: 14, backgroundColor: 'rgba(255,255,255,0.03)', borderWidth: 1, borderColor: 'rgba(255,255,255,0.07)', overflow: 'hidden' },
  trackArt: { ...StyleSheet.absoluteFillObject, opacity: 0.22 },
  trackArtFade: { ...StyleSheet.absoluteFillObject, experimental_backgroundImage: 'linear-gradient(180deg, rgba(10,7,18,0.6) 0%, rgba(10,7,18,0.2) 50%, rgba(10,7,18,0.85) 100%)' },
  trackLabels: { width: 70, paddingTop: 52, gap: 53 },
  trackLabel: { fontFamily: font, fontWeight: '700', fontSize: 11, letterSpacing: 1.4, textTransform: 'uppercase', color: colors.textDim },
  edgeFade: { position: 'absolute', right: 0, top: 0, bottom: 0, width: 60, experimental_backgroundImage: 'linear-gradient(90deg, rgba(10,7,18,0) 0%, rgba(10,7,18,0.85) 100%)' },
  lineTrack: { position: 'absolute', left: COL / 2, top: 6 + 18 + 8 + 58 + 8 + 58 + 8 + 14, width: CONTENT_W - COL, height: 3, borderRadius: 2, backgroundColor: 'rgba(255,255,255,0.08)', overflow: 'hidden' },
  lineFill: { position: 'absolute', left: -COL / 2, top: 0, bottom: 0, width: CONTENT_W, transformOrigin: 'left', experimental_backgroundImage: 'linear-gradient(90deg, #a8741f, #ffd36b)' },
  tierCol: { marginHorizontal: 4, alignItems: 'center', gap: 8, paddingVertical: 6, borderRadius: 10, borderWidth: 1, borderColor: 'transparent' },
  tierColCurrent: { backgroundColor: 'rgba(242,195,91,0.10)', borderColor: 'rgba(242,195,91,0.45)' },
  pressed96: { transform: [{ scale: 0.96 }] },
  tierColHot: { backgroundColor: 'rgba(255,255,255,0.05)' },
  tierColFocus: { borderColor: '#fff3cf', boxShadow: '0 0 0 2px rgba(255,178,63,0.35), 0 0 18px rgba(255,178,63,0.4)' },
  claimPill: { height: 18, paddingHorizontal: 8, borderRadius: 9, backgroundColor: colors.gold, justifyContent: 'center', boxShadow: '0 0 10px rgba(242,195,91,0.6)' },
  claimPillText: { fontFamily: font, fontWeight: '700', fontSize: 9, letterSpacing: 1, textTransform: 'uppercase', color: '#2a1500' },
  reward: { width: 58, height: 58, borderRadius: 10, backgroundColor: 'rgba(255,255,255,0.05)', borderWidth: 1, borderColor: 'rgba(255,255,255,0.10)', alignItems: 'center', justifyContent: 'center' },
  rewardEmpty: { backgroundColor: 'transparent', borderColor: 'rgba(255,255,255,0.05)' },
  rewardPremium: { borderColor: 'rgba(242,195,91,0.55)', experimental_backgroundImage: 'linear-gradient(180deg, rgba(242,195,91,0.20), rgba(242,195,91,0.02))' },
  claimGlow: { borderRadius: 9, borderWidth: 2, borderColor: colors.goldLight, boxShadow: '0 0 16px rgba(242,195,91,0.8), inset 0 0 12px rgba(242,195,91,0.5)' },
  rewardIcon: { width: 40, height: 40 },
  rewardLock: { position: 'absolute', right: 3, bottom: 3, width: 14, height: 14, borderRadius: 7, backgroundColor: 'rgba(0,0,0,0.6)', alignItems: 'center', justifyContent: 'center' },
  miniLock: { width: 6, height: 5, borderRadius: 1, backgroundColor: colors.textDim },
  claimed: { position: 'absolute', right: -5, top: -5, width: 18, height: 18, borderRadius: 9, backgroundColor: '#0c2416', borderWidth: 1, borderColor: colors.green, alignItems: 'center', justifyContent: 'center' },
  tierNode: { width: 30, height: 30, borderRadius: 15, alignItems: 'center', justifyContent: 'center', backgroundColor: '#1b1626', borderWidth: 1, borderColor: 'rgba(255,255,255,0.15)' },
  tierNodeDone: { backgroundColor: colors.goldDeep, borderColor: colors.goldDeep },
  tierNodeReady: { backgroundColor: colors.goldLight, borderColor: colors.goldLight },
  tierNodeCurrent: { backgroundColor: colors.gold, borderColor: '#fff3cf', boxShadow: '0 0 14px rgba(242,195,91,0.9)' },
  tierNodeMilestone: { borderColor: colors.purple, borderWidth: 2, boxShadow: '0 0 12px rgba(178,107,255,0.6)' },
  tierNodeText: { fontFamily: font, fontWeight: '700', fontSize: 11, color: colors.textDim },
  burst: { position: 'absolute', left: 0, right: 0, top: 70, height: 0, alignItems: 'center' },
  burstRing: { position: 'absolute', width: 60, height: 60, top: -30, borderRadius: 30, borderWidth: 3 },
  spark: { position: 'absolute', width: 7, height: 7, top: -3, borderRadius: 1 },

  miniMap: { flexDirection: 'row', alignItems: 'center', alignSelf: 'center', gap: 10, marginTop: 10 },
  miniLabel: { fontFamily: font, fontWeight: '600', fontSize: 10, color: colors.textMute },
  miniTrack: { width: 300, height: 6, borderRadius: 3, backgroundColor: 'rgba(255,255,255,0.06)', overflow: 'hidden' },
  miniDone: { position: 'absolute', left: 0, top: 0, bottom: 0, backgroundColor: 'rgba(242,195,91,0.35)' },
  miniMark: { position: 'absolute', top: 0, bottom: 0, width: 1, backgroundColor: 'rgba(178,107,255,0.6)' },
  miniThumb: { position: 'absolute', left: 0, top: 0, bottom: 0, borderRadius: 3, backgroundColor: 'rgba(255,243,207,0.75)' },

  smallBtn: { flexDirection: 'row', alignItems: 'center', gap: 8, height: 28, paddingLeft: 5, paddingRight: 12, borderRadius: 14, borderWidth: 1, borderColor: colors.lineStrong, backgroundColor: 'rgba(255,255,255,0.03)' },
  smallBtnHot: { borderColor: 'rgba(242,195,91,0.7)', backgroundColor: 'rgba(242,195,91,0.10)' },
  smallBtnHover: { borderColor: colors.goldLight, backgroundColor: 'rgba(242,195,91,0.18)' },
  glyph: { width: 18, height: 18, borderRadius: 9, alignItems: 'center', justifyContent: 'center' },
  glyphText: { fontFamily: font, fontWeight: '700', fontSize: 10, color: '#0b0f1a' },
  smallBtnText: { fontFamily: font, fontWeight: '600', fontSize: 12, color: colors.text },

  toast: {
    position: 'absolute',
    left: 48,
    bottom: 16,
    flexDirection: 'row',
    alignItems: 'center',
    gap: 10,
    paddingHorizontal: 16,
    paddingVertical: 9,
    borderRadius: 10,
    borderWidth: 1,
    backgroundColor: 'rgba(14,10,24,0.96)',
    boxShadow: '0 12px 30px rgba(0,0,0,0.6)',
  },
  toastDot: { width: 8, height: 8, borderRadius: 4 },
  toastText: { fontFamily: font, fontWeight: '600', fontSize: 13, color: colors.text },
});
